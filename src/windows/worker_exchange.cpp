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
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flitzfs {
namespace {
constexpr ULONG request_magic=0x51575846, reply_magic=0x52575846;
constexpr ULONG sized_request_magic=0x32575846, sized_reply_magic=0x32525946;
struct ChannelProfile {
  WorkerChannelCapacity capacity{kWorkerRequestLimit,kWorkerReplyLimit};
  bool sized=false;
  ULONG input_magic() const noexcept {return sized?sized_request_magic:request_magic;}
  ULONG output_magic() const noexcept {return sized?sized_reply_magic:reply_magic;}
  ULONG version() const noexcept {return sized?2U:0U;}
};
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
  bool intact(ULONG magic,ULONG version=0) const noexcept {
    const auto* c=control();
    return c->magic==magic && c->capacity==capacity && c->reserved==version &&
        std::all_of(bytes()+capacity,bytes()+capacity+guard_bytes,[](auto b){return b==std::byte{0xa7};});
  }
  static Mapping create(const std::wstring& name,std::size_t count,ULONG magic,PSECURITY_DESCRIPTOR acl,ULONG version=0) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),acl,FALSE};
    SetLastError(ERROR_SUCCESS);
    Handle h(CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,
        static_cast<DWORD>(sizeof(Control)+count+guard_bytes),name.c_str()));
    const auto error=GetLastError();
    if(!h.value)failed("worker mapping creation",error);
    require(error!=ERROR_ALREADY_EXISTS,"worker mapping name collision");
    Mapping mapping(std::move(h),FILE_MAP_WRITE,count);
    *mapping.control()={magic,static_cast<ULONG>(count),pending,version};
    std::fill_n(mapping.bytes()+count,guard_bytes,std::byte{0xa7});return mapping;
  }
  static Mapping open(const std::wstring& name,std::size_t count,DWORD access) {
    Handle h(OpenFileMappingW(access,FALSE,name.c_str()));
    if(!h.value)failed("worker mapping open");
    return Mapping(std::move(h),access,count);
  }
};
void headroom(ProcessBudget budget,ChannelProfile profile={}) {
  require(valid_process_budget(budget),"invalid worker process budget");
  if(profile.sized)require(valid_worker_channel_capacity(profile.capacity,budget),
      "invalid sized worker capacity");
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))failed("worker memory headroom");
  // A valid adaptive budget can exceed the legacy fixture's MiB range. Check
  // the complete sum before adding transport copies and reserves.
  const auto overhead=512ULL*1024*1024+3ULL*profile.capacity.request_bytes+
      2ULL*profile.capacity.reply_bytes+2*(sizeof(Control)+guard_bytes)+4ULL*1024*1024;
  require(budget.memory_mib<=((std::numeric_limits<ULONGLONG>::max)()-overhead)/(1024*1024),
      "worker resident accounting overflow");
  const auto needed=static_cast<ULONGLONG>(budget.memory_mib)*1024*1024+
      512ULL*1024*1024+3ULL*profile.capacity.request_bytes+2ULL*profile.capacity.reply_bytes+
      2*(sizeof(Control)+guard_bytes)+4ULL*1024*1024;
  if(memory.ullAvailPhys<needed || memory.ullAvailPageFile<needed)
    throw std::runtime_error("insufficient worker channel memory headroom");
  // The optional large-window route yields at least 15/16 of CURRENT spare
  // physical/commit headroom, including runtime allowance and all parent copies.
  const auto resident=needed-512ULL*1024*1024;
  if(profile.sized && (resident>(memory.ullAvailPhys-512ULL*1024*1024)/(std::max)(16U,budget.memory_share_divisor) ||
      resident>(memory.ullAvailPageFile-512ULL*1024*1024)/(std::max)(16U,budget.memory_share_divisor)))
    throw std::runtime_error("sized worker would consume excessive live headroom");
}
}
bool valid_worker_channel_capacity(WorkerChannelCapacity capacity,ProcessBudget budget) noexcept {
  if(!valid_process_budget(budget) || capacity.request_bytes<kWorkerHeaderBytes ||
      capacity.reply_bytes<kWorkerHeaderBytes ||
      capacity.request_bytes>static_cast<std::size_t>((std::numeric_limits<LONG>::max)()) ||
      capacity.reply_bytes>static_cast<std::size_t>((std::numeric_limits<LONG>::max)()))return false;
  // Caller chooses actual capacities. Checked representation and the enforced
  // child commitment allowance bound copies before mapping or allocation.
  const auto charged=3ULL*capacity.request_bytes+2ULL*capacity.reply_bytes+
      2*(sizeof(Control)+guard_bytes);
  return charged<=std::uint64_t(budget.memory_mib)*1024*1024/2;
}
namespace {
WorkerExchangeResult exchange_unclaimed(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget,
    const std::shared_ptr<ProcessObserver>& observer,ChannelProfile profile) {
  require(request.size()>=kWorkerHeaderBytes && request.size()<=profile.capacity.request_bytes &&
      arguments.size()<=14,"invalid worker request or argument bound");
  headroom(budget,profile);
  const std::vector<std::byte> owned_request(request.begin(),request.end());
  request=owned_request;
  const auto token=random_token();auto command=arguments;
  command.push_back(L"--worker-channel");command.push_back(token);
  (void)windows_child_command(helper.native(),command); // Validate before mappings or launch.
  Local acl;owner_security(acl);
  auto input=Mapping::create(object_name(token,false),profile.capacity.request_bytes,profile.input_magic(),acl.value,profile.version());
  auto output=Mapping::create(object_name(token,true),profile.capacity.reply_bytes,profile.output_magic(),acl.value,profile.version());
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
  result.publication_observed=output.intact(profile.output_magic(),profile.version()) &&
      length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(profile.capacity.reply_bytes);
  if(result.process.cancelled) {result.code=WorkerExchangeCode::cancelled;return result;}
  if(result.process.timed_out) {result.code=WorkerExchangeCode::timed_out;return result;}
  if(result.process.exit_code) {result.code=WorkerExchangeCode::worker_failed;return result;}
  // Wait on child exit is the synchronization boundary; there is no live helper
  // able to mutate this reply. No success is inferred from shared flags alone.
  if(!input.intact(profile.input_magic(),profile.version()) || !output.intact(profile.output_magic(),profile.version()) ||
      input.control()->published!=static_cast<LONG>(request.size()) ||
      std::memcmp(input.bytes(),request.data(),request.size()) ||
      length<static_cast<LONG>(kWorkerHeaderBytes) || length>static_cast<LONG>(profile.capacity.reply_bytes))return result;
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
  const ChannelProfile profile;
  WorkerExchangeResult result;
  std::exception_ptr error;
  std::atomic<bool> done{false}, retired{false};
  std::mutex mutex;
  std::condition_variable ready;
  ExchangeState(std::shared_ptr<ExchangeControl> c,const std::filesystem::path& h,
      const std::vector<std::wstring>& a,std::span<const std::byte> r,ProcessBudget b,ChannelProfile p):
      control(std::move(c)),helper(h),arguments(a),request(r.begin(),r.end()),budget(b),profile(p) {}
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
    state->result=exchange_unclaimed(state->helper,state->arguments,state->request,state->budget,state->observer,state->profile);
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
  return start_impl(helper,arguments,request,{kWorkerRequestLimit,kWorkerReplyLimit},budget,false);
}
WorkerExchangeSession WorkerExchangeSession::start_sized(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,
    WorkerChannelCapacity capacity,ProcessBudget budget) {
  require(valid_worker_channel_capacity(capacity,budget),"invalid sized worker admission");
  return start_impl(helper,arguments,request,capacity,budget,true);
}
WorkerExchangeSession WorkerExchangeSession::start_impl(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,
    WorkerChannelCapacity capacity,ProcessBudget budget,bool sized) {
  const ChannelProfile profile{capacity,sized};
  require(valid_process_budget(budget) && request.size()>=kWorkerHeaderBytes &&
      request.size()<=capacity.request_bytes && arguments.size()<=14,"invalid asynchronous worker admission");
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
  headroom(budget,profile); // Refuse before proportional copies or supervisor creation.
  // All proportional storage is bounded and allocated before a thread exists.
  auto state=std::make_shared<ExchangeState>(control,helper,arguments,request,budget,profile);
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
  return impl_ && impl_->state->retired.load(std::memory_order_acquire) &&
      WaitForSingleObject(impl_->supervisor.value,0)==WAIT_OBJECT_0;
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
  if(state.retired.load(std::memory_order_acquire)) {
    const auto wait=WaitForSingleObject(impl_->supervisor.value,2000);
    if(wait==WAIT_FAILED)failed("retired worker supervisor wait");
    if(wait!=WAIT_OBJECT_0) {
      auto result=state.result;
      if(state.error)std::rethrow_exception(state.error);
      result.code=WorkerExchangeCode::supervisor_exit_unconfirmed;
      result.response.clear();
      return result; // Retain admission until exact supervisor-thread exit.
    }
    // Release the coordinator's confirmed observation handle now, rather than
    // making idle ownership depend on a later admission/status call.
    std::lock_guard control_lock(state.control->mutex);
    state.control->reap();
  }
  if(state.error)std::rethrow_exception(state.error);
  return state.result;
}
WorkerExchangeResult run_worker_exchange(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget) {
  auto session=WorkerExchangeSession::start(helper,arguments,request,budget);
  return session.wait_result();
}

struct WorkerChannel::Impl {
  ChannelProfile profile;
  Mapping input,output;
  std::vector<std::byte> owned;
  explicit Impl(std::wstring_view token,ChannelProfile p={}):profile(p),
      input(Mapping::open(object_name(token,false),p.capacity.request_bytes,FILE_MAP_READ)),
      output(Mapping::open(object_name(token,true),p.capacity.reply_bytes,FILE_MAP_WRITE)) {
    const LONG length=input.control()->published;
    require(input.intact(profile.input_magic(),profile.version()) && output.intact(profile.output_magic(),profile.version()) && output.control()->published==pending &&
        length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(profile.capacity.request_bytes),
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
WorkerChannel WorkerChannel::open_sized(std::wstring_view token,WorkerChannelCapacity capacity) {
  require(valid_token(token),"invalid sized worker channel token");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if(!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr))
    failed("sized worker job admission");
  const auto flags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY;
  require((limits.BasicLimitInformation.LimitFlags&flags)==flags &&
      limits.BasicLimitInformation.ActiveProcessLimit==1 && limits.ProcessMemoryLimit &&
      limits.JobMemoryLimit==limits.ProcessMemoryLimit && limits.ProcessMemoryLimit%(1024*1024)==0,
      "sized channel requires enforced process limits");
  ProcessBudget budget;budget.memory_mib=limits.ProcessMemoryLimit/(1024*1024);
  budget.memory_share_divisor=16;
  require(valid_worker_channel_capacity(capacity,budget),"sized child capacity exceeds job allowance");
  headroom(budget,{capacity,true});
  return WorkerChannel(std::make_unique<Impl>(token,ChannelProfile{capacity,true}));
}
std::span<const std::byte> WorkerChannel::request() const noexcept {
  return impl_?std::span<const std::byte>(impl_->owned):std::span<const std::byte>{};
}
void WorkerChannel::publish(std::span<const std::byte> response) {
  require(impl_ && response.size()>=kWorkerHeaderBytes && response.size()<=impl_->profile.capacity.reply_bytes,
      "invalid worker response bound");
  auto& output=impl_->output;
  const auto profile=impl_->profile;
  require(output.intact(profile.output_magic(),profile.version()),"worker response mapping changed");
  require(InterlockedCompareExchange(&output.control()->published,writing,pending)==pending,
      "worker response already published");
  std::memcpy(output.bytes(),response.data(),response.size());
  InterlockedExchange(&output.control()->published,static_cast<LONG>(response.size()));
}
} // namespace flitzfs
