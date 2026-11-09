#include "worker_exchange.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <process.h>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flitzfs {
namespace {
constexpr ULONG request_magic=0x51575846, reply_magic=0x52575846;
constexpr LONG pending=-1, writing=-2;
constexpr std::size_t guard_bytes=16;
struct Control { ULONG magic, capacity; LONG published; ULONG reserved; };
static_assert(sizeof(Control)==16 && offsetof(Control,published)==8);
struct Handle {
  HANDLE value=nullptr;
  explicit Handle(HANDLE h=nullptr):value(h) {}
  ~Handle() {if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
};
struct View {
  void* value=nullptr;
  explicit View(void* p=nullptr):value(p) {}
  ~View() {if(value) UnmapViewOfFile(value);}
  View(const View&)=delete;
  View& operator=(const View&)=delete;
  View(View&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
};
struct Local { void* value=nullptr;~Local(){if(value)LocalFree(value);} };
[[noreturn]] void failed(const char* operation,DWORD error=GetLastError()) {
  throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
void require(bool ok,const char* why) {if(!ok)throw std::invalid_argument(why);}
bool valid_token(std::wstring_view token) noexcept {
  return token.size()==32 && std::all_of(token.begin(),token.end(),[](wchar_t c){
    return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');});
}
std::wstring object_name(std::wstring_view token,bool response) {
  require(valid_token(token),"invalid worker channel token");
  return L"Local\\FlitzFS.worker."+std::wstring(token)+(response?L".reply":L".request");
}
std::wstring random_token() {
  std::array<UCHAR,16> random{};
  if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
    throw std::runtime_error("worker channel randomness unavailable");
  constexpr wchar_t hex[]=L"0123456789abcdef";
  std::wstring token;token.reserve(32);
  for(auto b:random){token+=hex[b>>4];token+=hex[b&15];}
  return token;
}
void owner_security(Local& descriptor) {
  Handle token;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))failed("worker identity query");
  DWORD length=0;
  GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || !length || length>16384)
    throw std::runtime_error("invalid worker identity size");
  std::vector<std::byte> storage(length);
  if(!GetTokenInformation(token.value,TokenUser,storage.data(),length,&length))failed("worker identity read");
  LPWSTR sid_text=nullptr;
  if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid_text))failed("worker SID encoding");
  Local sid{sid_text};
  const std::wstring identity(sid_text);
  const auto sddl=L"O:"+identity+L"D:P(A;;GA;;;"+identity+L")";
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor.value,&length) ||
      length>4096 || !IsValidSecurityDescriptor(descriptor.value))failed("worker mapping ACL");
}
struct Mapping {
  Handle object;
  View view;
  std::size_t capacity;
  Mapping(Handle h,DWORD access,std::size_t count):object(std::move(h)),
      view(MapViewOfFile(object.value,access,0,0,sizeof(Control)+count+guard_bytes)),capacity(count) {
    if(!view.value)failed("worker mapping view");
  }
  Control* control() const noexcept {return static_cast<Control*>(view.value);}
  std::byte* bytes() const noexcept {return static_cast<std::byte*>(view.value)+sizeof(Control);}
  bool intact(ULONG magic) const noexcept {
    const auto* c=control();
    return c->magic==magic && c->capacity==capacity && !c->reserved &&
        std::all_of(bytes()+capacity,bytes()+capacity+guard_bytes,[](auto b){return b==std::byte{0xa7};});
  }
  static Mapping create(const std::wstring& name,std::size_t count,ULONG magic,PSECURITY_DESCRIPTOR acl) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),acl,FALSE};
    SetLastError(ERROR_SUCCESS);
    Handle h(CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,
        static_cast<DWORD>(sizeof(Control)+count+guard_bytes),name.c_str()));
    const auto error=GetLastError();
    if(!h.value)failed("worker mapping creation",error);
    require(error!=ERROR_ALREADY_EXISTS,"worker mapping name collision");
    Mapping mapping(std::move(h),FILE_MAP_WRITE,count);
    *mapping.control()={magic,static_cast<ULONG>(count),pending,0};
    std::fill_n(mapping.bytes()+count,guard_bytes,std::byte{0xa7});return mapping;
  }
  static Mapping open(const std::wstring& name,std::size_t count,DWORD access) {
    Handle h(OpenFileMappingW(access,FALSE,name.c_str()));
    if(!h.value)failed("worker mapping open");
    return Mapping(std::move(h),access,count);
  }
};
void headroom(ProcessBudget budget) {
  require(valid_process_budget(budget),"invalid worker process budget");
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))failed("worker memory headroom");
  const auto needed=static_cast<ULONGLONG>(budget.memory_mib)*1024*1024+
      512ULL*1024*1024+kWorkerRequestLimit+kWorkerReplyLimit+2*(sizeof(Control)+guard_bytes)+
      2*kWorkerRequestLimit+kWorkerReplyLimit+4ULL*1024*1024;
  if(memory.ullAvailPhys<needed || memory.ullAvailPageFile<needed)
    throw std::runtime_error("insufficient worker channel memory headroom");
}
}
namespace {
WorkerExchangeResult exchange_unclaimed(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget,
    const std::shared_ptr<ProcessObserver>& observer) {
  require(request.size()>=kWorkerHeaderBytes && request.size()<=kWorkerRequestLimit &&
      arguments.size()<=14,"invalid worker request or argument bound");
  headroom(budget);
  const std::vector<std::byte> owned_request(request.begin(),request.end());
  request=owned_request;
  const auto token=random_token();auto command=arguments;
  command.push_back(L"--worker-channel");command.push_back(token);
  (void)windows_child_command(helper.native(),command); // Validate before mappings or launch.
  Local acl;owner_security(acl);
  auto input=Mapping::create(object_name(token,false),kWorkerRequestLimit,request_magic,acl.value);
  auto output=Mapping::create(object_name(token,true),kWorkerReplyLimit,reply_magic,acl.value);
  std::memcpy(input.bytes(),request.data(),request.size());
  input.control()->published=static_cast<LONG>(request.size());
  MemoryBarrier();
  WorkerExchangeResult result;result.channel_token=token;
  result.process=run_bounded_process(helper,command,budget,observer);
  if(!result.process.exit_confirmed) {result.code=WorkerExchangeCode::exit_unconfirmed;return result;}
  // Only inspect publication after confirmed child exit, including failure.
  // This diagnostic never permits timed-out or unsuccessful output to escape.
  MemoryBarrier();
  const LONG length=output.control()->published;
  result.publication_observed=output.intact(reply_magic) &&
      length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(kWorkerReplyLimit);
  if(result.process.cancelled) {result.code=WorkerExchangeCode::cancelled;return result;}
  if(result.process.timed_out) {result.code=WorkerExchangeCode::timed_out;return result;}
  if(result.process.exit_code) {result.code=WorkerExchangeCode::worker_failed;return result;}
  // Wait on child exit is the synchronization boundary; there is no live helper
  // able to mutate this reply. No success is inferred from shared flags alone.
  if(!input.intact(request_magic) || !output.intact(reply_magic) ||
      input.control()->published!=static_cast<LONG>(request.size()) ||
      std::memcmp(input.bytes(),request.data(),request.size()) ||
      length<static_cast<LONG>(kWorkerHeaderBytes) || length>static_cast<LONG>(kWorkerReplyLimit))return result;
  result.response.assign(output.bytes(),output.bytes()+length);
  result.code=WorkerExchangeCode::completed;return result;
}

struct ExchangeControl {
  std::mutex mutex;
  bool busy=false;
  Handle supervisor;
  std::atomic<bool> retired{false}, disabled{false};
  void reap() noexcept {
    if(!busy || !supervisor.value)return;
    const auto wait=WaitForSingleObject(supervisor.value,0);
    if(wait==WAIT_OBJECT_0 && retired.load(std::memory_order_acquire)) {
      if(!CloseHandle(supervisor.value)){disabled.store(true,std::memory_order_release);return;}
      supervisor.value=nullptr;busy=false;
    } else if(wait==WAIT_FAILED || wait==WAIT_OBJECT_0) {
      disabled.store(true,std::memory_order_release);
    }
  }
};
std::shared_ptr<ExchangeControl> exchange_control() {
  // The supervisor captures shared ownership. It never references a destroyed
  // static mutex/control if its caller goes away during process shutdown.
  static const auto control=std::make_shared<ExchangeControl>();
  return control;
}
struct ExchangeState {
  const std::shared_ptr<ExchangeControl> control;
  const std::shared_ptr<ProcessObserver> observer=std::make_shared<ProcessObserver>();
  const std::filesystem::path helper;
  const std::vector<std::wstring> arguments;
  const std::vector<std::byte> request;
  const ProcessBudget budget;
  WorkerExchangeResult result;
  std::exception_ptr error;
  std::atomic<bool> done{false}, retired{false};
  std::mutex mutex;
  std::condition_variable ready;
  ExchangeState(std::shared_ptr<ExchangeControl> c,const std::filesystem::path& h,
      const std::vector<std::wstring>& a,std::span<const std::byte> r,ProcessBudget b):
      control(std::move(c)),helper(h),arguments(a),request(r.begin(),r.end()),budget(b) {}
};
void record_retirement(const std::shared_ptr<ExchangeState>& state,ProcessObservation observation) noexcept {
  if(observation==ProcessObservation::exited || observation==ProcessObservation::not_started)
    state->retired.store(true,std::memory_order_release);
  else
    state->control->disabled.store(true,std::memory_order_release);
  // Admission ALSO requires signalled supervisor-thread exit. The controller
  // reaps that exact thread handle; a foreground poll never joins the thread.
  // Unknown observation disables admission instead of reopening a PID or
  // retrying an invalid wait in a busy loop.
  state->control->retired.store(state->retired.load(std::memory_order_acquire),std::memory_order_release);
}
unsigned __stdcall supervise_exchange(void* opaque) noexcept {
  std::unique_ptr<std::shared_ptr<ExchangeState>> context(
      static_cast<std::shared_ptr<ExchangeState>*>(opaque));
  const auto state=*context;
  context.reset();
  try {
    if(!SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST))
      failed("worker supervisor priority");
    state->result=exchange_unclaimed(state->helper,state->arguments,state->request,state->budget,state->observer);
  } catch(...) {
    state->error=std::current_exception();
    state->result=WorkerExchangeResult{};
    state->result.code=WorkerExchangeCode::supervisor_failed;
    state->result.process.process_id=state->observer->process_id();
  }
  auto observation=state->observer->wait_for_exit();
  if(observation!=ProcessObservation::running)record_retirement(state,observation);
  {
    std::lock_guard lock(state->mutex);
    state->done.store(true,std::memory_order_release);
  }
  state->ready.notify_all();
  // A result deadline is not proof of retirement. Keep exactly this process
  // HANDLE and admission lease until actual signalled exit, even after the
  // request owner has selected CPU or destroyed its session.
  while(observation==ProcessObservation::running) {
    observation=state->observer->wait_for_exit(2000);
    if(observation!=ProcessObservation::running)record_retirement(state,observation);
  }
  return 0;
}
} // namespace
struct WorkerExchangeSession::Impl {
  const std::shared_ptr<ExchangeState> state;
  Handle supervisor;
  explicit Impl(std::shared_ptr<ExchangeState> s):state(std::move(s)) {}
};
WorkerExchangeSession::WorkerExchangeSession(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)) {}
WorkerExchangeSession::~WorkerExchangeSession()=default;
WorkerExchangeSession::WorkerExchangeSession(WorkerExchangeSession&&) noexcept=default;
WorkerExchangeSession& WorkerExchangeSession::operator=(WorkerExchangeSession&&) noexcept=default;
WorkerExchangeSession WorkerExchangeSession::start(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget) {
  require(valid_process_budget(budget) && request.size()>=kWorkerHeaderBytes &&
      request.size()<=kWorkerRequestLimit && arguments.size()<=14,"invalid asynchronous worker admission");
  auto command=arguments;
  command.push_back(L"--worker-channel");command.push_back(std::wstring(32,L'0'));
  (void)windows_child_command(helper.native(),command);
  auto control=exchange_control();
  {
    std::lock_guard lock(control->mutex);
    control->reap();
    if(control->disabled.load(std::memory_order_acquire))
      throw std::runtime_error("worker observation failed; further admission is disabled");
    if(control->busy)throw std::runtime_error("a worker exchange or retirement is already active");
    control->retired.store(false,std::memory_order_release);
    control->busy=true;
  }
  struct Reservation {
    std::shared_ptr<ExchangeControl> control;
    bool transferred=false;
    ~Reservation(){
      if(!transferred){std::lock_guard lock(control->mutex);control->busy=false;}
    }
  } reservation{control};
  headroom(budget); // Refuse before proportional copies or supervisor creation.
  // All proportional storage is bounded and allocated before a thread exists.
  auto state=std::make_shared<ExchangeState>(control,helper,arguments,request,budget);
  auto impl=std::make_unique<Impl>(state);
  auto* context=new std::shared_ptr<ExchangeState>(state);
  const auto thread=_beginthreadex(nullptr,0,supervise_exchange,context,0,nullptr);
  if(!thread) {
    delete context;
    throw std::system_error(errno,std::generic_category(),"worker supervisor creation");
  }
  impl->supervisor.value=reinterpret_cast<HANDLE>(thread);
  reservation.transferred=true;
  {
    std::lock_guard lock(control->mutex);
    HANDLE observed=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),impl->supervisor.value,GetCurrentProcess(),
        &observed,SYNCHRONIZE,FALSE,0)) {
      control->disabled.store(true,std::memory_order_release);
      failed("supervisor retirement handle");
    }
    control->supervisor.value=observed;
  }
  return WorkerExchangeSession(std::move(impl));
}
const WorkerExchangeResult* WorkerExchangeSession::try_result() const noexcept {
  return impl_ && impl_->state->done.load(std::memory_order_acquire)?&impl_->state->result:nullptr;
}
std::exception_ptr WorkerExchangeSession::failure() const noexcept {
  return try_result()?impl_->state->error:std::exception_ptr{};
}
bool WorkerExchangeSession::retirement_confirmed() const noexcept {
  return impl_ && impl_->state->retired.load(std::memory_order_acquire);
}
std::uint32_t WorkerExchangeSession::process_id() const noexcept {
  return impl_?impl_->state->observer->process_id():0;
}
bool WorkerExchangeSession::request_cancel() const noexcept {
  return impl_ && impl_->state->observer->request_stop();
}
bool WorkerExchangeSession::available() noexcept {
  try {
    const auto control=exchange_control();
    std::unique_lock lock(control->mutex,std::try_to_lock);
    if(!lock.owns_lock())return false;
    control->reap();
    return !control->disabled.load(std::memory_order_acquire) && !control->busy;
  } catch(...) {return false;}
}
WorkerExchangeResult WorkerExchangeSession::wait_result() {
  require(static_cast<bool>(impl_),"moved-from worker session");
  auto& state=*impl_->state;
  std::unique_lock lock(state.mutex);
  state.ready.wait(lock,[&]{return state.done.load(std::memory_order_acquire);});
  lock.unlock();
  // Preserve strict synchronous handle accounting for an already retired
  // helper. Never join/wait here when retirement remains unconfirmed.
  if(state.retired.load(std::memory_order_acquire) &&
      WaitForSingleObject(impl_->supervisor.value,INFINITE)!=WAIT_OBJECT_0)
    failed("retired worker supervisor wait");
  if(state.error)std::rethrow_exception(state.error);
  return state.result;
}
WorkerExchangeResult run_worker_exchange(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget) {
  auto session=WorkerExchangeSession::start(helper,arguments,request,budget);
  return session.wait_result();
}

struct WorkerChannel::Impl {
  Mapping input,output;
  std::vector<std::byte> owned;
  explicit Impl(std::wstring_view token):
      input(Mapping::open(object_name(token,false),kWorkerRequestLimit,FILE_MAP_READ)),
      output(Mapping::open(object_name(token,true),kWorkerReplyLimit,FILE_MAP_WRITE)) {
    const LONG length=input.control()->published;
    require(input.intact(request_magic) && output.intact(reply_magic) && output.control()->published==pending &&
        length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(kWorkerRequestLimit),
        "invalid worker channel framing");
    owned.assign(input.bytes(),input.bytes()+length);
  }
};
WorkerChannel::WorkerChannel(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)) {}
WorkerChannel::~WorkerChannel()=default;
WorkerChannel::WorkerChannel(WorkerChannel&&) noexcept=default;
WorkerChannel& WorkerChannel::operator=(WorkerChannel&&) noexcept=default;
WorkerChannel WorkerChannel::open(std::wstring_view token) {
  require(valid_token(token),"invalid worker channel token");
  return WorkerChannel(std::make_unique<Impl>(token));
}
std::span<const std::byte> WorkerChannel::request() const noexcept {
  return impl_?std::span<const std::byte>(impl_->owned):std::span<const std::byte>{};
}
void WorkerChannel::publish(std::span<const std::byte> response) {
  require(impl_ && response.size()>=kWorkerHeaderBytes && response.size()<=kWorkerReplyLimit,
      "invalid worker response bound");
  auto& output=impl_->output;
  require(output.intact(reply_magic),"worker response mapping changed");
  require(InterlockedCompareExchange(&output.control()->published,writing,pending)==pending,
      "worker response already published");
  std::memcpy(output.bytes(),response.data(),response.size());
  InterlockedExchange(&output.control()->published,static_cast<LONG>(response.size()));
}
} // namespace flitzfs
