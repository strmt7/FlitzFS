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
  bool sized=false,prepared=false,windowed=false,repeated_requests=false;
  ULONG input_magic() const noexcept {return sized?sized_request_magic:request_magic;}
  ULONG output_magic() const noexcept {return sized?sized_reply_magic:reply_magic;}
  ULONG version() const noexcept {return repeated_requests?5U:windowed?4U:prepared?3U:sized?2U:0U;}
  std::wstring_view prepared_argument() const noexcept {
    if(repeated_requests)return kPreparedWorkerRequestsArgument;
    return windowed?kPreparedWorkerWindowsArgument:kPreparedWorkerChannelArgument;
  }
};
constexpr LONG pending=-1, writing=-2, discarded=-3;
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
std::wstring prepared_event_name(std::wstring_view token,std::wstring_view suffix) {
  require(valid_token(token),"invalid prepared worker channel token");
  return L"Local\\FlitzFS.worker."+std::wstring(token)+std::wstring(suffix);
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
Handle create_prepared_event(std::wstring_view token,std::wstring_view suffix,PSECURITY_DESCRIPTOR acl) {
  const auto name=prepared_event_name(token,suffix);
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),acl,FALSE};
  Handle event(CreateEventW(&security,TRUE,FALSE,name.c_str()));
  const auto error=GetLastError();
  if(!event.value)failed("prepared worker event creation",error);
  require(error!=ERROR_ALREADY_EXISTS,"prepared worker event name collision");
  return event;
}
Handle open_prepared_event(std::wstring_view token,std::wstring_view suffix,DWORD access) {
  const auto name=prepared_event_name(token,suffix);
  Handle event(OpenEventW(access,FALSE,name.c_str()));
  if(!event.value)failed("prepared worker event open");
  return event;
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
constexpr std::uint64_t channel_reserve=512ULL*1024*1024;

std::uint64_t channel_resident_bytes(WorkerChannelCapacity capacity,ProcessBudget budget,
    bool repeated_requests=false) noexcept {
  // A repeated exchange funds both host snapshots during a changeover.
  const auto overhead=(repeated_requests?4ULL:3ULL)*capacity.request_bytes+2ULL*capacity.reply_bytes+
      2*(sizeof(Control)+guard_bytes)+4ULL*1024*1024;
  if(budget.memory_mib>((std::numeric_limits<std::uint64_t>::max)()-overhead)/(1024*1024))return 0;
  return static_cast<std::uint64_t>(budget.memory_mib)*1024*1024+overhead;
}

std::uint64_t channel_headroom_bytes(std::uint64_t resident,std::uint32_t divisor) noexcept {
  if(!resident || resident>((std::numeric_limits<std::uint64_t>::max)()-channel_reserve)/divisor)return 0;
  return channel_reserve+resident*divisor;
}

void headroom(ProcessBudget budget,ChannelProfile profile={}) {
  require(valid_process_budget(budget),"invalid worker process budget");
  if(profile.sized)require(valid_worker_channel_capacity(profile.capacity,budget),
      "invalid sized worker capacity");
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))failed("worker memory headroom");
  const auto resident=channel_resident_bytes(profile.capacity,budget,profile.repeated_requests);
  const auto minimum=channel_headroom_bytes(resident,1);
  require(minimum!=0,"worker resident accounting overflow");
  const auto available=(std::min)(memory.ullAvailPhys,memory.ullAvailPageFile);
  if(available<minimum)
    throw std::runtime_error("insufficient worker channel memory headroom");
  // The optional large-window route yields at least 15/16 of CURRENT spare
  // physical/commit headroom, including runtime allowance and all parent copies.
  if(profile.sized) {
    const auto required=channel_headroom_bytes(resident,sized_worker_memory_share_divisor(budget));
    if(!required || available<required)throw std::runtime_error("sized worker would consume excessive live headroom");
  }
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
std::uint64_t required_sized_worker_headroom(WorkerChannelCapacity capacity,ProcessBudget budget) noexcept {
  if(!valid_worker_channel_capacity(capacity,budget))return 0;
  return channel_headroom_bytes(channel_resident_bytes(capacity,budget),sized_worker_memory_share_divisor(budget));
}
void append_prepared_reply(std::vector<std::byte>& sequence,std::span<const std::byte> reply,std::size_t capacity) {
  constexpr std::size_t prefix=sizeof(std::uint32_t);
  require(sequence.size()<=capacity && capacity-sequence.size()>=prefix &&
      reply.size()>=kWorkerHeaderBytes && reply.size()<=(std::numeric_limits<std::uint32_t>::max)() &&
      reply.size()<=capacity-sequence.size()-prefix,"prepared reply exceeds its aggregate capacity");
  const auto length=static_cast<std::uint32_t>(reply.size());
  const auto start=sequence.size();
  sequence.resize(start+prefix+reply.size());
  for(unsigned i=0;i<prefix;++i)sequence[start+i]=static_cast<std::byte>(length>>(8*i));
  std::memcpy(sequence.data()+start+prefix,reply.data(),reply.size());
}
std::span<const std::byte> take_prepared_reply(std::span<const std::byte>& sequence) {
  constexpr std::size_t prefix=sizeof(std::uint32_t);
  require(sequence.size()>=prefix,"truncated prepared reply length");
  std::uint32_t length=0;
  for(unsigned i=0;i<prefix;++i)length|=std::uint32_t(std::to_integer<unsigned char>(sequence[i]))<<(8*i);
  require(length>=kWorkerHeaderBytes && length<=sequence.size()-prefix,"invalid prepared reply length");
  const auto reply=sequence.subspan(prefix,length);
  sequence=sequence.subspan(prefix+length);
  return reply;
}
namespace {
WorkerExchangeResult complete_exchange(ProcessResult process,const std::wstring& token,
    const Mapping& input,const Mapping& output,std::span<const std::byte> request,ChannelProfile profile) {
  WorkerExchangeResult result;
  result.process=process;
  result.channel_token=token;
  if(!process.exit_confirmed) {result.code=WorkerExchangeCode::exit_unconfirmed;return result;}
  // A readiness signal never authorizes reply inspection. The exact child
  // exit supplies that boundary, including for cancelled or failed exchanges.
  MemoryBarrier();
  const LONG length=output.control()->published;
  result.publication_observed=output.intact(profile.output_magic(),profile.version()) &&
      length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(profile.capacity.reply_bytes);
  if(process.cancelled) {result.code=WorkerExchangeCode::cancelled;return result;}
  if(process.timed_out) {result.code=WorkerExchangeCode::timed_out;return result;}
  if(process.exit_code) {result.code=WorkerExchangeCode::worker_failed;return result;}
  if(request.empty() || !input.intact(profile.input_magic(),profile.version()) ||
      input.control()->published!=static_cast<LONG>(request.size()) ||
      std::memcmp(input.bytes(),request.data(),request.size()) || !result.publication_observed)return result;
  result.response.assign(output.bytes(),output.bytes()+length);
  result.code=WorkerExchangeCode::completed;
  return result;
}
WorkerExchangeResult exchange_unclaimed(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,const WorkerRequestSnapshot& snapshot,ProcessBudget budget,
    const std::shared_ptr<ProcessObserver>& observer,ChannelProfile profile) {
  const auto request=snapshot.bytes();
  require(request.size()>=kWorkerHeaderBytes && request.size()<=profile.capacity.request_bytes &&
      arguments.size()<=14,"invalid worker request or argument bound");
  headroom(budget,profile);
  const auto token=random_token();auto command=arguments;
  command.push_back(L"--worker-channel");command.push_back(token);
  (void)windows_child_command(helper.native(),command);
  Local acl;owner_security(acl);
  auto input=Mapping::create(object_name(token,false),profile.capacity.request_bytes,profile.input_magic(),acl.value,profile.version());
  auto output=Mapping::create(object_name(token,true),profile.capacity.reply_bytes,profile.output_magic(),acl.value,profile.version());
  std::memcpy(input.bytes(),request.data(),request.size());
  input.control()->published=static_cast<LONG>(request.size());
  MemoryBarrier();
  const auto process=run_bounded_process(helper,command,budget,observer);
  return complete_exchange(process,token,input,output,request,profile);
}

struct PreparedChannel {
  const std::wstring token;
  Mapping input,output;
  Handle ready,supplied,declined;
  SharedWorkerRequest request;
  std::uint32_t permits=0;
  bool accepting=true;
  PreparedChannel(std::wstring key,ChannelProfile profile,PSECURITY_DESCRIPTOR acl):
      token(std::move(key)),
      input(Mapping::create(object_name(token,false),profile.capacity.request_bytes,profile.input_magic(),acl,profile.version())),
      output(Mapping::create(object_name(token,true),profile.capacity.reply_bytes,profile.output_magic(),acl,profile.version())),
      ready(create_prepared_event(token,L".ready",acl)),supplied(create_prepared_event(token,L".supplied",acl)),
      declined(profile.windowed?create_prepared_event(token,L".declined",acl):Handle{}) {}
};
std::unique_ptr<PreparedChannel> make_prepared_channel(ChannelProfile profile) {
  Local acl;owner_security(acl);
  return std::make_unique<PreparedChannel>(random_token(),profile,acl.value);
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
  const SharedWorkerRequest request;
  const std::shared_ptr<const void> pinned_route;
  const ProcessBudget budget;
  const ChannelProfile profile;
  const std::uint32_t continuation_windows;
  std::unique_ptr<PreparedChannel> prepared;
  WorkerExchangeResult result;
  std::exception_ptr error;
  std::atomic<bool> done{false}, retired{false}, prepared_cancelled{false}, prepared_discarded{false};
  std::mutex mutex;
  std::condition_variable ready;
  ExchangeState(std::shared_ptr<ExchangeControl> c,const std::filesystem::path& h,
      const std::vector<std::wstring>& a,SharedWorkerRequest r,ProcessBudget b,ChannelProfile p,
      std::shared_ptr<const void> pins,std::uint32_t windows):
      control(std::move(c)),helper(h),arguments(a),request(std::move(r)),pinned_route(std::move(pins)),
      budget(b),profile(p),continuation_windows(windows),prepared(p.prepared?make_prepared_channel(p):nullptr) {}
  bool stop() noexcept {
    if(profile.prepared)prepared_cancelled.store(true,std::memory_order_release);
    return observer->request_stop();
  }
  bool cancel() noexcept {
    if(!profile.prepared)return stop();
    if(prepared_discarded.load(std::memory_order_acquire))return true;
    try {
      std::unique_lock lock(mutex,std::try_to_lock);
      if(lock && prepared && prepared->accepting && !prepared->request &&
          WaitForSingleObject(prepared->ready.value,0)==WAIT_OBJECT_0 &&
          prepared->input.intact(profile.input_magic(),profile.version()) &&
          prepared->input.control()->published==pending) {
        prepared->accepting=false;
        prepared_cancelled.store(true,std::memory_order_release);
        InterlockedExchange(&prepared->input.control()->published,discarded);
        if(SetEvent(prepared->supplied.value)) {
          prepared_discarded.store(true,std::memory_order_release);
          return true;
        }
      }
    }catch(...) {}
    if(prepared_discarded.load(std::memory_order_acquire))return true;
    return stop();
  }
};
WorkerExchangeResult exchange_prepared(ExchangeState& state) {
  headroom(state.budget,state.profile);
  auto& channel=*state.prepared;
  auto command=state.arguments;
  command.emplace_back(state.profile.prepared_argument());
  command.push_back(channel.token);
  command.push_back(std::to_wstring(state.budget.timeout_ms));
  if(state.prepared_cancelled.load(std::memory_order_acquire)) {
    WorkerExchangeResult cancelled;
    cancelled.channel_token=channel.token;
    cancelled.code=WorkerExchangeCode::cancelled;
    cancelled.process.cancelled=true;
    return cancelled;
  }
  auto process=run_bounded_process(state.helper,command,state.budget,state.observer);
  std::lock_guard lock(state.mutex);
  if(!process.timed_out && state.prepared_cancelled.load(std::memory_order_acquire))
    process.cancelled=true;
  channel.accepting=false;
  const auto request=channel.request && channel.permits==state.continuation_windows?
      channel.request->bytes():std::span<const std::byte>{};
  return complete_exchange(process,channel.token,channel.input,channel.output,request,state.profile);
}
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
    state->result=state->profile.prepared?exchange_prepared(*state):
        exchange_unclaimed(state->helper,state->arguments,*state->request,state->budget,state->observer,state->profile);
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
    if(observation!=ProcessObservation::running)state->prepared.reset();
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
  if(state->profile.prepared) {
    std::lock_guard lock(state->mutex);
    state->prepared.reset();
  }
  return 0;
}
} // namespace
struct WorkerExchangeSession::Impl {
  const std::shared_ptr<ExchangeState> state;
  Handle supervisor;
  explicit Impl(std::shared_ptr<ExchangeState> s):state(std::move(s)) {}
  ~Impl() {
    if(state->profile.prepared && !state->done.load(std::memory_order_acquire))(void)state->cancel();
  }
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
WorkerExchangeSession WorkerExchangeSession::start_sized(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,SharedWorkerRequest request,
    WorkerChannelCapacity capacity,ProcessBudget budget,std::shared_ptr<const void> pinned_route) {
  require(request && valid_worker_channel_capacity(capacity,budget),"invalid sized worker snapshot admission");
  const auto bytes=request->bytes();
  return start_impl(helper,arguments,bytes,capacity,budget,true,std::move(request),std::move(pinned_route));
}
WorkerExchangeSession WorkerExchangeSession::start_prepared_sized(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,WorkerChannelCapacity capacity,ProcessBudget budget,
    std::shared_ptr<const void> pinned_route) {
  require(valid_worker_channel_capacity(capacity,budget),"invalid prepared worker admission");
  return start_impl(helper,arguments,{},capacity,budget,true,{},std::move(pinned_route),true);
}
WorkerExchangeSession WorkerExchangeSession::start_impl(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,
    WorkerChannelCapacity capacity,ProcessBudget budget,bool sized,SharedWorkerRequest snapshot,
    std::shared_ptr<const void> pinned_route,bool prepared,std::uint32_t continuation_windows,bool repeated_requests) {
  const ChannelProfile profile{capacity,sized,prepared,continuation_windows!=0,repeated_requests};
  require(valid_process_budget(budget) && (!profile.windowed || prepared) && (prepared?
      (sized && request.empty() && !snapshot && arguments.size()<=13):
      (request.size()>=kWorkerHeaderBytes && request.size()<=capacity.request_bytes && arguments.size()<=14)),
      "invalid asynchronous worker admission");
  auto command=arguments;
  command.emplace_back(prepared?profile.prepared_argument():std::wstring_view(L"--worker-channel"));
  command.push_back(std::wstring(32,L'0'));
  if(prepared)command.push_back(std::to_wstring(budget.timeout_ms));
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
  if(!prepared && !snapshot)snapshot=std::make_shared<const WorkerRequestSnapshot>(request);
  auto state=std::make_shared<ExchangeState>(control,helper,arguments,std::move(snapshot),budget,profile,
      std::move(pinned_route),continuation_windows);
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
bool WorkerExchangeSession::wait_prepared(std::uint32_t milliseconds) const noexcept {
  if(!impl_ || milliseconds>impl_->state->budget.timeout_ms)return false;
  auto& state=*impl_->state;
  try {
    Handle ready;
    {
      std::lock_guard lock(state.mutex);
      const auto* channel=state.prepared.get();
      if(!channel || !channel->accepting ||
          (channel->request && channel->permits==state.continuation_windows) ||
          state.prepared_cancelled.load(std::memory_order_acquire) || state.done.load(std::memory_order_acquire))
        return false;
      if(!DuplicateHandle(GetCurrentProcess(),channel->ready.value,GetCurrentProcess(),&ready.value,
          SYNCHRONIZE,FALSE,0))return false;
    }
    const HANDLE waits[]{impl_->supervisor.value,ready.value};
    return WaitForMultipleObjects(2,waits,FALSE,milliseconds)==WAIT_OBJECT_0+1;
  }catch(...){return false;}
}
bool WorkerExchangeSession::submit_prepared(SharedWorkerRequest request) noexcept {
  if(!impl_ || !request)return false;
  auto& state=*impl_->state;
  try {
    std::lock_guard lock(state.mutex);
    auto* channel=state.prepared.get();
    if(!channel || !channel->accepting ||
        (channel->request && (!state.profile.repeated_requests || channel->permits>=state.continuation_windows)) ||
        state.prepared_cancelled.load(std::memory_order_acquire) || state.done.load(std::memory_order_acquire) ||
        request->bytes().size()!=state.profile.capacity.request_bytes ||
        WaitForSingleObject(channel->ready.value,0)!=WAIT_OBJECT_0 ||
        state.observer->wait_for_exit()!=ProcessObservation::running)return false;
    auto& input=channel->input;
    const bool continuing=static_cast<bool>(channel->request);
    if(state.profile.repeated_requests)headroom(state.budget,state.profile);
    const auto previous=continuing?channel->request->bytes():std::span<const std::byte>{};
    const bool intact=input.intact(state.profile.input_magic(),state.profile.version());
    if(intact && (continuing?
        (input.control()->published==static_cast<LONG>(previous.size()) &&
            !std::memcmp(input.bytes(),previous.data(),previous.size())):input.control()->published==pending) &&
        (!state.profile.windowed || ResetEvent(channel->ready.value))) {
      const auto bytes=request->bytes();
      channel->request=std::move(request);
      std::memcpy(input.bytes(),bytes.data(),bytes.size());
      InterlockedExchange(&input.control()->published,static_cast<LONG>(bytes.size()));
      if(!state.prepared_cancelled.load(std::memory_order_acquire) && SetEvent(channel->supplied.value)) {
        if(continuing)++channel->permits;
        return true;
      }
    }
    channel->accepting=false;
    (void)state.stop();
  }catch(...) {}
  return false;
}
bool WorkerExchangeSession::permit_window(bool admitted) noexcept {
  if(!impl_)return false;
  auto& state=*impl_->state;
  try {
    std::lock_guard lock(state.mutex);
    auto* channel=state.prepared.get();
    if(!channel || !channel->accepting || !channel->request || channel->permits>=state.continuation_windows ||
        (admitted && state.profile.repeated_requests) ||
        state.prepared_cancelled.load(std::memory_order_acquire) ||
        WaitForSingleObject(channel->ready.value,0)!=WAIT_OBJECT_0 ||
        state.observer->wait_for_exit()!=ProcessObservation::running || !ResetEvent(channel->ready.value))return false;
    if(admitted) {
      if(!SetEvent(channel->supplied.value))return false;
      ++channel->permits;
      return true;
    }
    channel->accepting=false;
    state.prepared_cancelled.store(true,std::memory_order_release);
    state.prepared_discarded.store(true,std::memory_order_release);
    if(!SetEvent(channel->declined.value)) {
      state.prepared_discarded.store(false,std::memory_order_release);
      (void)state.stop();
    }
  }catch(...) {}
  return false;
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
void* WorkerExchangeSession::supervisor_wait_handle() const noexcept {
  return impl_?impl_->supervisor.value:nullptr;
}
std::uint32_t WorkerExchangeSession::process_id() const noexcept {
  return impl_?impl_->state->observer->process_id():0;
}
bool WorkerExchangeSession::request_cancel() const noexcept {
  return impl_ && impl_->state->cancel();
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
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget,
    std::stop_token stop,std::shared_ptr<const void> pinned_route) {
  if(stop.stop_requested()){
    WorkerExchangeResult refused;
    refused.code=WorkerExchangeCode::cancelled;
    refused.process.cancelled=true;
    return refused;
  }
  auto session=WorkerExchangeSession::start_impl(helper,arguments,request,
      {kWorkerRequestLimit,kWorkerReplyLimit},budget,false,{},std::move(pinned_route));
  std::stop_callback cancel(stop,[&]() noexcept {(void)session.request_cancel();});
  return session.wait_result();
}

WorkerExchangeResult WorkerExchangeSession::run_sized(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,SharedWorkerRequest request,
    WorkerChannelCapacity capacity,ProcessBudget budget,std::stop_token stop,
    std::shared_ptr<const void> pinned_route) {
  if(stop.stop_requested()){
    WorkerExchangeResult refused;
    refused.code=WorkerExchangeCode::cancelled;
    refused.process.cancelled=true;
    return refused;
  }
  auto session=start_sized(helper,arguments,std::move(request),capacity,budget,std::move(pinned_route));
  std::stop_callback cancel(stop,[&]() noexcept {(void)session.request_cancel();});
  return session.wait_sized_result();
}

WorkerExchangeResult WorkerExchangeSession::run_prepared_sized(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,SharedWorkerRequest request,WorkerChannelCapacity capacity,
    ProcessBudget budget,std::stop_token stop,const std::function<bool()>& on_ready,
    std::shared_ptr<const void> pinned_route) {
  return run_prepared_windows(helper,arguments,std::move(request),capacity,budget,1,stop,
      on_ready?std::function<bool(std::uint32_t)>([&](std::uint32_t){return on_ready();}):
          std::function<bool(std::uint32_t)>{},std::move(pinned_route));
}
WorkerExchangeResult WorkerExchangeSession::run_prepared_windows(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,SharedWorkerRequest request,WorkerChannelCapacity capacity,
    ProcessBudget budget,std::uint32_t windows,std::stop_token stop,
    const std::function<bool(std::uint32_t)>& admit,std::shared_ptr<const void> pinned_route) {
  if(stop.stop_requested()) {
    WorkerExchangeResult refused;
    refused.code=WorkerExchangeCode::cancelled;
    refused.process.cancelled=true;
    return refused;
  }
  require(request && request->bytes().size()==capacity.request_bytes && admit && windows &&
      windows<=capacity.request_bytes && valid_worker_channel_capacity(capacity,budget),
      "invalid prepared exchange input");
  auto session=start_impl(helper,arguments,{},capacity,budget,true,{},std::move(pinned_route),true,windows-1);
  std::stop_callback cancel(stop,[&]() noexcept {(void)session.request_cancel();});
  try {
    if(!session.wait_prepared(budget.timeout_ms) || stop.stop_requested() ||
        !admit(0) || !session.submit_prepared(std::move(request))) {
      (void)session.request_cancel();
    } else {
      for(std::uint32_t i=1;i<windows;++i) {
        if(session.wait_prepared(budget.timeout_ms) && session.permit_window(!stop.stop_requested() && admit(i)))continue;
        if(!session.try_result())(void)session.request_cancel();
        break;
      }
    }
  }catch(...) {
    (void)session.request_cancel();
    (void)session.wait_sized_result();
    throw;
  }
  return session.wait_sized_result();
}

WorkerExchangeResult WorkerExchangeSession::run_prepared_requests(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,WorkerChannelCapacity capacity,ProcessBudget budget,
    std::uint32_t requests,std::stop_token stop,const std::function<SharedWorkerRequest(std::uint32_t)>& prepare,
    std::shared_ptr<const void> pinned_route) {
  if(stop.stop_requested()) {
    WorkerExchangeResult refused;
    refused.code=WorkerExchangeCode::cancelled;
    refused.process.cancelled=true;
    return refused;
  }
  require(prepare && requests>=2 && requests<=capacity.reply_bytes/kWorkerHeaderBytes &&
      valid_worker_channel_capacity(capacity,budget),"invalid repeated exchange admission");
  auto session=start_impl(helper,arguments,{},capacity,budget,true,{},std::move(pinned_route),true,requests-1,true);
  std::stop_callback cancel(stop,[&]() noexcept {(void)session.request_cancel();});
  try {
    for(std::uint32_t i=0;i<requests;++i) {
      if(!session.wait_prepared(budget.timeout_ms) || stop.stop_requested()) {
        if(!session.try_result())(void)session.request_cancel();
        break;
      }
      auto request=prepare(i);
      if(stop.stop_requested() || !request || !session.submit_prepared(std::move(request))) {
        if(i && !stop.stop_requested())(void)session.permit_window(false);
        (void)session.request_cancel();
        break;
      }
    }
  }catch(...) {
    (void)session.request_cancel();
    (void)session.wait_sized_result();
    throw;
  }
  return session.wait_sized_result();
}

WorkerExchangeResult WorkerExchangeSession::wait_sized_result() {
  auto result=wait_result();
  if(!retirement_confirmed() && result.code!=WorkerExchangeCode::exit_unconfirmed) {
    result.code=WorkerExchangeCode::supervisor_exit_unconfirmed;
    result.response.clear();
  }
  return result;
}

struct WorkerChannel::Impl {
  ChannelProfile profile;
  Mapping input,output;
  Handle ready,supplied,declined;
  std::vector<std::byte> owned;
  std::uint32_t timeout_ms=0,expected_requests=0,received_requests=0;
  bool receive_started=false,received=false,window_declined=false;
  explicit Impl(std::wstring_view token,ChannelProfile p={},std::uint32_t timeout=0,std::uint32_t requests=0):profile(p),
      input(Mapping::open(object_name(token,false),p.capacity.request_bytes,FILE_MAP_READ)),
      output(Mapping::open(object_name(token,true),p.capacity.reply_bytes,FILE_MAP_WRITE)),
      ready(p.prepared?open_prepared_event(token,L".ready",EVENT_MODIFY_STATE):Handle{}),
      supplied(p.prepared?open_prepared_event(token,L".supplied",SYNCHRONIZE|(p.windowed?EVENT_MODIFY_STATE:0)):Handle{}),
      declined(p.windowed?open_prepared_event(token,L".declined",SYNCHRONIZE):Handle{}),
      timeout_ms(timeout),expected_requests(requests) {
    if(p.prepared) {
      require(input.intact(p.input_magic(),p.version()) && output.intact(p.output_magic(),p.version()) &&
          input.control()->published==pending && output.control()->published==pending,
          "invalid prepared worker channel framing");
    } else copy_request();
  }
  void copy_request() {
    const LONG length=input.control()->published;
    require(input.intact(profile.input_magic(),profile.version()) && output.intact(profile.output_magic(),profile.version()) &&
        output.control()->published==pending && length>=static_cast<LONG>(kWorkerHeaderBytes) &&
        length<=static_cast<LONG>(profile.capacity.request_bytes) &&
        (!profile.prepared || length==static_cast<LONG>(profile.capacity.request_bytes)),
        "invalid worker channel framing");
    owned.assign(input.bytes(),input.bytes()+length);
    received=true;
    ++received_requests;
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
namespace {
ChannelProfile sized_child_profile(std::wstring_view token,WorkerChannelCapacity capacity,
    std::uint32_t timeout_ms,bool prepared,bool windowed=false,bool repeated_requests=false) {
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
  budget.timeout_ms=timeout_ms;
  require(valid_worker_channel_capacity(capacity,budget),"sized child capacity exceeds job allowance");
  const ChannelProfile profile{capacity,true,prepared,windowed,repeated_requests};
  headroom(budget,profile);
  return profile;
}
}
WorkerChannel WorkerChannel::open_sized(std::wstring_view token,WorkerChannelCapacity capacity) {
  return WorkerChannel(std::make_unique<Impl>(token,sized_child_profile(token,capacity,ProcessBudget{}.timeout_ms,false)));
}
WorkerChannel WorkerChannel::open_prepared_sized(std::wstring_view token,WorkerChannelCapacity capacity,
    std::uint32_t timeout_ms,bool windowed) {
  return WorkerChannel(std::make_unique<Impl>(token,sized_child_profile(token,capacity,timeout_ms,true,windowed),timeout_ms));
}
WorkerChannel WorkerChannel::open_prepared_requests(std::wstring_view token,WorkerChannelCapacity capacity,
    std::uint32_t timeout_ms,std::uint32_t requests) {
  require(requests>=2 && requests<=capacity.reply_bytes/kWorkerHeaderBytes,"invalid repeated worker count");
  return WorkerChannel(std::make_unique<Impl>(token,
      sized_child_profile(token,capacity,timeout_ms,true,true,true),timeout_ms,requests));
}
bool WorkerChannel::receive_prepared() {
  require(impl_ && impl_->profile.prepared && !impl_->receive_started,"invalid prepared worker receive");
  auto& endpoint=*impl_;
  endpoint.receive_started=true;
  if(!SetEvent(endpoint.ready.value))failed("prepared worker ready signal");
  const auto wait=WaitForSingleObject(endpoint.supplied.value,endpoint.timeout_ms);
  if(wait!=WAIT_OBJECT_0)failed("prepared worker request wait",wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError());
  MemoryBarrier();
  if(endpoint.input.control()->published==discarded) {
    require(endpoint.input.intact(endpoint.profile.input_magic(),endpoint.profile.version()) &&
        endpoint.output.control()->published==pending,"invalid prepared worker discard");
    return false;
  }
  endpoint.copy_request();
  if(endpoint.profile.windowed && !ResetEvent(endpoint.supplied.value))failed("prepared window reset");
  return true;
}
bool WorkerChannel::await_window() {
  require(impl_ && !impl_->profile.repeated_requests,"fixed window requires an immutable request");
  return await_prepared_boundary();
}
bool WorkerChannel::receive_next_prepared() {
  require(impl_ && impl_->profile.repeated_requests && impl_->received_requests<impl_->expected_requests,
      "invalid next prepared request");
  if(!await_prepared_boundary())return false;
  MemoryBarrier();
  impl_->copy_request();
  return true;
}
bool WorkerChannel::await_prepared_boundary() {
  require(impl_ && impl_->profile.windowed && impl_->received && !impl_->window_declined &&
      impl_->output.control()->published==pending,"invalid prepared window request");
  auto& endpoint=*impl_;
  if(!SetEvent(endpoint.ready.value))failed("prepared window ready signal");
  const HANDLE events[]{endpoint.declined.value,endpoint.supplied.value};
  const auto wait=WaitForMultipleObjects(2,events,FALSE,endpoint.timeout_ms);
  if(wait==WAIT_OBJECT_0) {
    endpoint.window_declined=true;
    return false;
  }
  if(wait!=WAIT_OBJECT_0+1)failed("prepared window grant wait",wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError());
  if(!ResetEvent(endpoint.supplied.value))failed("prepared window reset");
  return true;
}
std::span<const std::byte> WorkerChannel::request() const noexcept {
  return impl_?std::span<const std::byte>(impl_->owned):std::span<const std::byte>{};
}
SharedWorkerRequest WorkerChannel::take_request_snapshot() {
  require(impl_ && !impl_->owned.empty(),"worker request snapshot already transferred");
  auto snapshot=std::make_shared<const WorkerRequestSnapshot>(std::move(impl_->owned));
  impl_->owned.clear();
  return snapshot;
}
void WorkerChannel::publish(std::span<const std::byte> response) {
  require(impl_ && impl_->received && !impl_->window_declined &&
      (!impl_->profile.repeated_requests || impl_->received_requests==impl_->expected_requests) &&
      response.size()>=kWorkerHeaderBytes && response.size()<=impl_->profile.capacity.reply_bytes,
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
