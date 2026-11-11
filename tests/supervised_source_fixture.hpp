#pragma once
// Trusted CPU-only test infrastructure. No mount, driver, GPU or data-path API.
#include "bounded_process.hpp"
#include "gpu_probe_budget.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <process.h>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
namespace source_fixture {
// Shared fixture budget is CPU-only despite its historical file/namespace name.
using ControllerBudget=gpu_probe_fixture::Budget;
inline std::atomic<bool> cleanup_ok{true},source_busy{false};
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline void checked(BOOL ok,const char* why){if(!ok){const auto error=GetLastError();
  throw std::system_error(static_cast<int>(error),std::system_category(),why);}}
struct Handle {
  HANDLE value=nullptr;
  explicit Handle(HANDLE h=nullptr) noexcept:value(h){}
  ~Handle(){close();}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)){}
  bool close() noexcept {
    const auto h=std::exchange(value,nullptr);
    if(h&&h!=INVALID_HANDLE_VALUE&&!CloseHandle(h)){cleanup_ok=false;return false;}
    return true;
  }
};
struct Local {
  void* value=nullptr;
  ~Local(){if(value&&LocalFree(value))cleanup_ok=false;}
};
inline bool valid_token(std::wstring_view token) noexcept {
  if(token.size()!=32)return false;
  for(auto c:token)if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;
  return true;
}
inline std::wstring random_token() {
  std::array<UCHAR,16> bytes{};
  require(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"source protocol randomness");
  constexpr wchar_t hex[]=L"0123456789abcdef";
  std::wstring out;out.reserve(32);for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}return out;
}
inline void owner_security(Local& acl) {
  Handle token;checked(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value),"source token query");
  DWORD length=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
  require(GetLastError()==ERROR_INSUFFICIENT_BUFFER&&length&&length<=16384,"source token size");
  std::vector<std::byte> bytes(length);
  checked(GetTokenInformation(token.value,TokenUser,bytes.data(),length,&length),"source token read");
  LPWSTR text=nullptr;
  checked(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text),"source SID");
  Local sid{ text };
  const auto sddl=L"O:"+std::wstring(text)+L"D:P(A;;GA;;;"+std::wstring(text)+L")";
  checked(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,
      &acl.value,&length),"source event ACL");
  require(length<=4096&&IsValidSecurityDescriptor(acl.value),"source event security");
}
enum class Phase:std::size_t {ready,written,ack};
class Events final {
 public:
  explicit Events(bool creator,std::wstring token=random_token()):token_(std::move(token)) {
    require(valid_token(token_),"invalid source protocol token");
    Local acl;if(creator)owner_security(acl);
    SECURITY_ATTRIBUTES security{sizeof(security),acl.value,FALSE};
    for(std::size_t i=0;i<events_.size();++i) {
      const auto name=event_name(token_,static_cast<Phase>(i));
      if(creator) {
        SetLastError(ERROR_SUCCESS);
        events_[i].value=CreateEventW(&security,TRUE,FALSE,name.c_str());
        const auto error=GetLastError();
        checked(events_[i].value!=nullptr,"source event creation");
        require(error!=ERROR_ALREADY_EXISTS,"source event collision");
      } else {
        const auto access=i==static_cast<std::size_t>(Phase::written)?SYNCHRONIZE:EVENT_MODIFY_STATE;
        events_[i].value=OpenEventW(access,FALSE,name.c_str());
        checked(events_[i].value!=nullptr,"source event open");
      }
    }
  }
  Events(const Events&)=delete;Events& operator=(const Events&)=delete;
  const std::wstring& token() const noexcept{return token_;}
  static std::wstring event_name(std::wstring_view token,Phase phase) {
    require(valid_token(token)&&static_cast<std::size_t>(phase)<3,"source event name bound");
    constexpr const wchar_t* suffix[]{L".ready",L".written",L".ack"};
    return L"Local\\FlitzFS.source."+std::wstring(token)+suffix[static_cast<std::size_t>(phase)];
  }
  HANDLE get(Phase phase) const {
    const auto i=static_cast<std::size_t>(phase);require(i<events_.size(),"source event phase");return events_[i].value;
  }
  void signal(Phase phase) const {checked(SetEvent(get(phase)),"source phase signal");}
  void wait_written() const {
    require(WaitForSingleObject(get(Phase::written),5000)==WAIT_OBJECT_0,"source client write phase deadline");
  }
 private:
  std::wstring token_;
  std::array<Handle,3> events_;
};
// Only schedules the existing creation-time/job-bounded launcher. It does not
// duplicate process creation, quoting, caps, termination or exit-code logic.
class Child final {
  struct State {
    const std::filesystem::path executable;
    const std::vector<std::wstring> arguments;
    const flitzfs::ProcessBudget budget;
    const std::shared_ptr<flitzfs::ProcessObserver> observer=std::make_shared<flitzfs::ProcessObserver>();
    Handle ready{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    flitzfs::ProcessResult result;
    std::exception_ptr error;
    std::atomic<bool> done{false},retired{false};
    State(const std::filesystem::path& exe,const std::vector<std::wstring>& args,flitzfs::ProcessBudget limits):
        executable(exe),arguments(args),budget(limits){checked(ready.value!=nullptr,"source result event");}
  };
  std::shared_ptr<State> state_;
  Handle thread_;
  bool reaped_=false;
  static std::shared_ptr<State>& quarantine() {
    // At most one terminal unknown source, retained until this test process
    // exits. No subsequent source admission or VM qualification is permitted.
    static std::shared_ptr<State> value;return value;
  }
  static unsigned __stdcall supervise(void* opaque) noexcept {
    std::unique_ptr<std::shared_ptr<State>> incoming(static_cast<std::shared_ptr<State>*>(opaque));
    const auto state=*incoming;incoming.reset();
    try {
      checked(SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST),"source supervisor priority");
      state->result=flitzfs::run_bounded_process(state->executable,state->arguments,state->budget,state->observer);
    }catch(...) {
      state->error=std::current_exception();state->result.process_id=state->observer->process_id();
    }
    auto observation=state->observer->wait_for_exit();
    const auto retired=[](auto value){return value==flitzfs::ProcessObservation::not_started||
        value==flitzfs::ProcessObservation::exited;};
    state->retired.store(retired(observation),std::memory_order_release);
    state->done.store(true,std::memory_order_release);
    if(!SetEvent(state->ready.value))cleanup_ok=false;
    // CPU/caller failure never discards the exact process lease. No PID reopen,
    // polling spin or replacement source is used while retirement is unknown.
    while(observation==flitzfs::ProcessObservation::running) {
      observation=state->observer->wait_for_exit(2000);
      state->retired.store(retired(observation),std::memory_order_release);
    }
    return 0;
  }
  void reap() {
    require(state_->retired.load(std::memory_order_acquire),"source child retirement unconfirmed");
    require(WaitForSingleObject(thread_.value,2000)==WAIT_OBJECT_0,"source supervisor exit unconfirmed");
    require(thread_.close(),"source supervisor handle close");
    reaped_=true;source_busy.store(false,std::memory_order_release);
  }
 public:
  Child(const std::filesystem::path& executable,const std::vector<std::wstring>& arguments,
      flitzfs::ProcessBudget budget) {
    require(flitzfs::valid_process_budget(budget)&&arguments.size()<=14,"source launch bounds");
    (void)flitzfs::windows_child_command(executable.native(),arguments);
    bool expected=false;
    require(source_busy.compare_exchange_strong(expected,true),"a source or terminal retirement is already active");
    try {
      state_=std::make_shared<State>(executable,arguments,budget);
      auto* context=new std::shared_ptr<State>(state_);
      const auto handle=_beginthreadex(nullptr,0,supervise,context,0,nullptr);
      if(!handle){delete context;throw std::runtime_error("source supervisor creation");}
      thread_.value=reinterpret_cast<HANDLE>(handle);
    }catch(...){source_busy.store(false,std::memory_order_release);throw;}
  }
  ~Child() {
    if(!reaped_&&state_) {
      state_->observer->request_stop();
      if(WaitForSingleObject(thread_.value,2000)==WAIT_OBJECT_0&&
          state_->retired.load(std::memory_order_acquire)&&thread_.close()) {
        reaped_=true;source_busy.store(false,std::memory_order_release);
      }else quarantine()=state_;
    }
  }
  Child(const Child&)=delete;Child& operator=(const Child&)=delete;
  DWORD process_id() const noexcept{return state_->observer->process_id();}
  bool request_cancel() const noexcept{return state_->observer->request_stop();}
  bool retirement_confirmed() const noexcept{return reaped_;}
  HANDLE result_event() const noexcept{return state_->ready.value;}
  void await(Events& protocol,Phase phase) {
    const HANDLE waits[]{protocol.get(phase),result_event()};
    const auto wait=WaitForMultipleObjects(2,waits,FALSE,5000);
    if(wait==WAIT_OBJECT_0)return;
    if(wait==WAIT_OBJECT_0+1||wait==WAIT_TIMEOUT) {
      if(wait==WAIT_TIMEOUT)request_cancel();
      const auto ended=result();
      throw std::runtime_error("source protocol phase="+std::to_string(static_cast<unsigned>(phase))+
          " wait="+std::to_string(wait)+" exit="+std::to_string(ended.exit_code)+
          " confirmed="+std::to_string(ended.exit_confirmed));
    }
    checked(FALSE,"source protocol wait");
  }
  flitzfs::ProcessResult result() {
    if(!reaped_) {
      require(WaitForSingleObject(result_event(),state_->budget.timeout_ms+4000)==WAIT_OBJECT_0&&
          state_->done.load(std::memory_order_acquire),"source result deadline");
      if(state_->retired.load(std::memory_order_acquire))reap();
      else {
        if(state_->error)std::rethrow_exception(state_->error);
        return state_->result; // exit_confirmed remains false; never authorizes a next child.
      }
    }
    if(state_->error)std::rethrow_exception(state_->error);
    return state_->result;
  }
};
} // namespace source_fixture
