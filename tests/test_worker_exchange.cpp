#include "worker_exchange.hpp"
#include "extent_codec_fixture.hpp"
#include "gpu/decode_kernel.hpp"
#include <windows.h>
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <cstdlib>
#include <exception>
#include <process.h>
#include <system_error>

namespace {
using namespace flitzfs;
using namespace extent_fixture;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f) {
  try{f();}catch(const std::exception&){++checks;return;}
  throw std::runtime_error("invalid worker operation accepted");
}
std::filesystem::path self() {
  wchar_t path[512]{};const auto n=GetModuleFileNameW(nullptr,path,512);
  check(n && n<512,"worker test executable path");return std::wstring(path,n);
}
struct RawMapping {
  HANDLE handle=nullptr;void* view=nullptr;
  RawMapping(std::wstring_view token,bool reply,DWORD access) {
    const auto name=L"Local\\FlitzFS.worker."+std::wstring(token)+(reply?L".reply":L".request");
    handle=OpenFileMappingW(access|READ_CONTROL,FALSE,name.c_str());
    check(handle!=nullptr,"fault-injection mapping open");
    view=MapViewOfFile(handle,access,0,0,16+(reply?kWorkerReplyLimit:kWorkerRequestLimit)+16);
    if(!view){CloseHandle(handle);handle=nullptr;throw std::runtime_error("fault-injection mapping view");}
  }
  ~RawMapping(){if(view)UnmapViewOfFile(view);if(handle)CloseHandle(handle);}
};
void check_security(std::wstring_view token) {
  RawMapping map(token,false,FILE_MAP_READ);
  MEMORY_BASIC_INFORMATION memory{};
  check(VirtualQuery(map.view,&memory,sizeof(memory))==sizeof(memory) && memory.Protect==PAGE_READONLY,
      "request is not mapped read-only");
  DWORD flags=0;
  check(GetHandleInformation(map.handle,&flags) && !(flags&HANDLE_FLAG_INHERIT),"mapping handle inheritance");
  PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
  check(GetSecurityInfo(map.handle,SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&sd)==ERROR_SUCCESS,
      "mapping ACL read");
  struct Free {PSECURITY_DESCRIPTOR value;~Free(){LocalFree(value);}} free{sd};
  SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
  check(GetSecurityDescriptorControl(sd,&control,&revision) && (control&SE_DACL_PROTECTED) && acl && acl->AceCount==1,
      "mapping DACL is not protected/single-principal");
  void* ace=nullptr;check(GetAce(acl,0,&ace),"mapping ACE");
  const auto* allowed=static_cast<ACCESS_ALLOWED_ACE*>(ace);
  check(allowed->Header.AceType==ACCESS_ALLOWED_ACE_TYPE,"mapping allow ACE");
  HANDLE process_token=nullptr;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&process_token),"test identity");
  struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{process_token};
  alignas(TOKEN_USER) std::array<std::byte,4096> storage{};DWORD needed=0;
  check(GetTokenInformation(process_token,TokenUser,storage.data(),static_cast<DWORD>(storage.size()),&needed),"test identity read");
  check(EqualSid(const_cast<DWORD*>(&allowed->SidStart),reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid),
      "mapping grants another principal");
}
void check_closed(const WorkerExchangeResult& result) {
  check(result.process.exit_confirmed && result.channel_token.size()==32,"missing completed exchange identity");
  for(bool reply:{false,true}) {
    const auto name=L"Local\\FlitzFS.worker."+result.channel_token+(reply?L".reply":L".request");
    SetLastError(ERROR_SUCCESS);
    const auto handle=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
    const auto error=GetLastError();
    if(handle)CloseHandle(handle);
    check(!handle && error==ERROR_FILE_NOT_FOUND,"worker mapping retained a handle or view");
  }
}
void check_job() {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  check(QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr) &&
      QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu),nullptr),"worker job queries");
  check(limits.ProcessMemoryLimit==128ULL*1024*1024 && limits.JobMemoryLimit==limits.ProcessMemoryLimit &&
      limits.BasicLimitInformation.ActiveProcessLimit==1 &&
      (limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) &&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) && cpu.CpuRate==2000,
      "worker process limits absent");
}
int child(std::wstring_view mode,std::wstring_view token) {
  check_job();
  if(mode==L"exit-empty")return 0;
  if(mode==L"exit-bad")return 17;
  auto channel=WorkerChannel::open(token);const auto input=decode_worker_request(channel.request());
  const auto& batch=input.batch;
  GpuBatchReply reply{input.request_id,Bytes(batch.cpu_reference().size()),{}};
  reply.status.resize(batch.tasks().size(),gpu_detail::pending);
  const unsigned char dummy=0;
  const auto* source=batch.payload().empty()?&dummy:reinterpret_cast<const unsigned char*>(batch.payload().data());
  for(std::size_t i=reply.status.size();i>0;--i)
    reply.status[i-1]=gpu_detail::decode_task(batch.tasks()[i-1],source,static_cast<unsigned>(batch.payload().size()),
        reinterpret_cast<unsigned char*>(reply.bytes.data()),static_cast<unsigned>(reply.bytes.size()));
  if(mode==L"wrong-id")++reply.generation;
  if(mode==L"bad-status")reply.status.back()=gpu_detail::pending;
  if(mode==L"bad-bytes")reply.bytes.back()^=std::byte{1};
  auto frame=encode_worker_reply(reply,mode==L"wrong-backend"?WorkerExecution::hip:WorkerExecution::cpu_semantics);
  if(mode==L"bad-wire")frame[0]=std::byte{0};
  if(mode==L"trailing")frame.push_back(std::byte{0});
  if(mode==L"oversize") {
    frame.resize(kWorkerReplyLimit+1);
    rejects([&]{channel.publish(frame);});return 0;
  }
  if(mode==L"security")check_security(token);
  channel.publish(frame);
  if(mode==L"double") {frame.back()^=std::byte{1};rejects([&]{channel.publish(frame);});}
  if(mode==L"tamper-input") {
    RawMapping map(token,false,FILE_MAP_WRITE);static_cast<std::byte*>(map.view)[16]^=std::byte{1};
  }
  if(mode==L"tamper-guard" || mode==L"tamper-length" || mode==L"tamper-header") {
    RawMapping map(token,true,FILE_MAP_WRITE);
    if(mode==L"tamper-guard")static_cast<std::byte*>(map.view)[16+kWorkerReplyLimit]=std::byte{0};
    if(mode==L"tamper-length")static_cast<LONG*>(map.view)[2]=0x7fffffff;
    if(mode==L"tamper-header")static_cast<DWORD*>(map.view)[3]=1;
  }
  if(mode==L"publish-fail")return 17;
  if(mode==L"publish-hang") {Sleep(10000);return 42;}
  return 0;
}
PreparedGpuBatch mixed() {
  const std::vector<Block> blocks{raw(text("channel sample")),fill(4097,19),pattern(8193),prefix_sample(),prefix_sample(true),huffman()};
  std::vector<GpuBlockView> views;
  for(const auto& b:blocks)views.push_back({b.kind,b.fill,static_cast<std::uint32_t>(b.plain.size()),b.encoded});
  return PreparedGpuBatch::prepare(views);
}


struct ErrorProbeState {std::exception_ptr error;};
unsigned __stdcall error_probe_thread(void* opaque) {
  std::unique_ptr<std::shared_ptr<ErrorProbeState>> owner(
      static_cast<std::shared_ptr<ErrorProbeState>*>(opaque));
  const auto state=*owner;owner.reset();
  try {throw std::system_error(ERROR_FILE_NOT_FOUND,std::system_category(),"independent CRT probe");}
  catch(...) {state->error=std::current_exception();}
  return 0;
}
int native_error_resources() {
  const auto* actions=std::getenv("GITHUB_ACTIONS");
  const auto* environment=std::getenv("RUNNER_ENVIRONMENT");
  check(actions && std::string_view(actions)=="true" && environment &&
      std::string_view(environment)=="github-hosted","error resource probe is hosted-only");
  auto count=[](const char* phase) {
    DWORD value=0;check(GetProcessHandleCount(GetCurrentProcess(),&value),"native error handle count");
    std::cout<<"native_error_handles "<<phase<<"="<<value<<'\n';
    return value;
  };
  auto message=[] {
    char* text=nullptr;
    const auto size=FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|
        FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,ERROR_FILE_NOT_FOUND,0x0409,
        reinterpret_cast<char*>(&text),0,nullptr);
    if(text)LocalFree(text);
    check(size>0,"independent Windows error message unavailable");
  };
  count("initial");
  message();const auto first=count("format-first");
  for(unsigned i=0;i<8;++i)message();
  check(count("format-repeat")<=first,"native error formatting accumulates handles");
  {std::system_error error(ERROR_FILE_NOT_FOUND,std::system_category(),"independent constructor");
    check(error.code().value()==ERROR_FILE_NOT_FOUND,"native error code changed");}
  count("system-error");
  {std::exception_ptr error;
    try {throw std::runtime_error("independent exception capture");}
    catch(...) {error=std::current_exception();}
    check(static_cast<bool>(error),"exception capture unavailable");}
  count("exception-released");
  DWORD threaded=0;
  for(unsigned i=0;i<9;++i) {
    auto state=std::make_shared<ErrorProbeState>();
    auto* context=new std::shared_ptr<ErrorProbeState>(state);
    const auto thread=_beginthreadex(nullptr,0,error_probe_thread,context,0,nullptr);
    if(!thread){delete context;throw std::runtime_error("independent probe thread failed");}
    HANDLE handle=reinterpret_cast<HANDLE>(thread);
    const auto wait=WaitForSingleObject(handle,2000);
    CloseHandle(handle);
    check(wait==WAIT_OBJECT_0 && state->error,"independent error thread did not finish");
    state.reset(); // Release the captured exception before measuring.
    const auto measured=count(i?"thread-repeat":"thread-first");
    if(!i)threaded=measured;
    else check(measured<=threaded,"independent error threads accumulate handles");
  }
  return 0;
}

template<class Predicate> void eventually(Predicate&& predicate,DWORD milliseconds,const char* why) {
  const auto start=GetTickCount64();
  while(!predicate()) {
    check(GetTickCount64()-start<milliseconds,why);
    Sleep(5); // Bounded fixture synchronization, never a production retry policy.
  }
}
GpuMemorySnapshot synthetic_memory(std::array<std::byte,8> luid) {
  GpuMemorySnapshot memory;
  memory.luid=luid;memory.node_mask=1;memory.captured_ms=100;
  memory.dedicated_bytes=memory.local_budget=1024ULL*1024*1024;
  memory.local_usage=16ULL*1024*1024;memory.valid=memory.amd_hardware=true;
  return memory; // Explicit test evidence. No real adapter telemetry is queried.
}
void asynchronous_fallback(const std::filesystem::path& executable) {
  std::array<std::byte,8> luid{};luid[0]=std::byte{1};
  GpuResultGate gate(mixed(),luid,1);
  const auto attempt=gate.begin(true,synthetic_memory(luid),100);
  check(attempt.code==GpuGateCode::admitted,"synthetic gate admission");
  const auto request=encode_worker_request(gate.batch(),attempt.generation);
  ProcessBudget budget;budget.timeout_ms=5000;
  auto session=WorkerExchangeSession::start(executable,{L"publish-hang"},request,budget);
  check(!WorkerExchangeSession::available(),"active asynchronous worker lost admission lease");
  eventually([&]{return session.process_id()!=0 || session.try_result();},2000,"asynchronous child did not start");
  check(session.process_id()!=0 && !session.try_result(),"hanging helper returned before fallback fixture");
  HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,session.process_id());
  check(child!=nullptr,"fixture exact live child observation");
  struct CloseChild {HANDLE value;~CloseChild(){CloseHandle(value);}} close{child};
  check(WaitForSingleObject(child,0)==WAIT_TIMEOUT,"helper already exited before CPU selection");
  const auto cpu=gate.use_cpu_fallback();
  check(gate.state()==GpuGateState::poisoned &&
      std::equal(cpu.begin(),cpu.end(),gate.batch().cpu_reference().begin()),
      "asynchronous CPU fallback changed verified bytes");
  // This proves selection did not wait for teardown, without a noisy latency
  // threshold: the exact helper is STILL alive after CPU bytes are available.
  check(WaitForSingleObject(child,0)==WAIT_TIMEOUT && !session.try_result(),
      "CPU fallback waited for the helper to exit");
  for(unsigned i=0;i<16;++i)
    rejects([&]{(void)WorkerExchangeSession::start(executable,{L"ok"},request);});
  rejects([&]{(void)run_worker_exchange(executable,{L"ok"},request);});
  check(session.request_cancel(),"asynchronous cancellation signal");
  eventually([&]{return session.try_result()!=nullptr;},4000,"cancelled helper did not return");
  const auto* result=session.try_result();
  check(result && result->code==WorkerExchangeCode::cancelled &&
      result->process.cancelled && !result->process.timed_out &&
      result->process.exit_confirmed && result->response.empty() && !session.failure(),
      "cancelled candidate escaped or exit was unconfirmed");
  eventually([&]{return session.retirement_confirmed() && WorkerExchangeSession::available();},
      2000,"confirmed asynchronous retirement did not release admission");
  check_closed(*result);
  GpuBatchReply late{attempt.generation,Bytes(cpu.begin(),cpu.end()),
      std::vector<std::uint32_t>(gate.batch().tasks().size(),gpu_detail::success)};
  check(gate.complete(std::move(late),synthetic_memory(luid),101)==GpuGateCode::wrong_state &&
      gate.state()==GpuGateState::poisoned,"late result reopened CPU-selected gate");
  std::cout<<"PASS: verified CPU bytes became available while the exact helper remained live; cancellation ran separately.\n";
}
void abandoned_session(const std::filesystem::path& executable,const std::vector<std::byte>& request) {
  HANDLE child=nullptr;
  {
    ProcessBudget budget;budget.timeout_ms=2000;
    auto session=WorkerExchangeSession::start(executable,{L"publish-hang"},request,budget);
    eventually([&]{return session.process_id()!=0 || session.try_result();},2000,"abandonment helper did not start");
    check(session.process_id()!=0 && !session.try_result(),"abandonment fixture failed");
    child=OpenProcess(SYNCHRONIZE,FALSE,session.process_id());
    check(child!=nullptr && WaitForSingleObject(child,0)==WAIT_TIMEOUT,"abandonment child not live");
  }
  struct CloseChild {HANDLE value;~CloseChild(){CloseHandle(value);}} close{child};
  check(WaitForSingleObject(child,0)==WAIT_TIMEOUT && !WorkerExchangeSession::available(),
      "destroyed session waited or prematurely released worker ownership");
  rejects([&]{(void)WorkerExchangeSession::start(executable,{L"ok"},request);});
  eventually([&]{return WaitForSingleObject(child,0)==WAIT_OBJECT_0 && WorkerExchangeSession::available();},
      5000,"abandoned helper was not supervised/retired");
  std::cout<<"PASS: abandoned session retained supervision and blocked replacement until exact-child exit.\n";
}

}
int wmain(int argc,wchar_t** argv){try{
  if(argc==2 && std::wstring_view(argv[1])==L"--native-error-resource-probe")return native_error_resources();
  if(argc==4 && std::wstring_view(argv[2])==L"--worker-channel")return child(argv[1],argv[3]);
  check(argc==1,"unexpected worker test arguments");
  const auto executable=self();const auto batch=mixed();const auto request=encode_worker_request(batch,221);
  // Warm the full success path, including validation/copying and object-lifetime
  // queries, before measuring process-wide handles. No growth allowance follows.
  for(unsigned i=0;i<2;++i) {
    const auto warmup=run_worker_exchange(executable,{L"ok"},request);
    check(warmup.code==WorkerExchangeCode::completed,"worker warmup exchange");
    (void)accept_worker_reply(warmup.response,batch,221,WorkerExecution::cpu_semantics);
    check_closed(warmup);
    // Native error-message initialization has an independently measured first-
    // use handle cost. Warm the REAL failure route before the strict baseline;
    // repeated failures below must still have ZERO growth allowance.
    auto failed=WorkerExchangeSession::start(executable.parent_path()/L"missing-flitzfs-worker.exe",{L"ok"},request);
    eventually([&]{return failed.try_result()!=nullptr;},2000,"failure warmup did not finish");
    eventually([&]{return failed.retirement_confirmed() && WorkerExchangeSession::available();},
        2000,"failure warmup did not retire");
    check(failed.try_result()->code==WorkerExchangeCode::supervisor_failed &&
        failed.failure() && failed.try_result()->response.empty(),"native failure warmup not observed");
  }
  struct HandleObservation { const wchar_t* step=nullptr; DWORD count=0; };
  std::array<HandleObservation,48> observations{};std::size_t observed=0;
  auto observe=[&](const wchar_t* step) {
    DWORD count=0;check(GetProcessHandleCount(GetCurrentProcess(),&count),"observe handle count");
    check(observed<observations.size(),"observation bound");observations[observed++]={step,count};
  };
  observe(L"warmup");
  DWORD initial_handles=0;check(GetProcessHandleCount(GetCurrentProcess(),&initial_handles),"initial handle count");
  for(const auto* mode:{L"ok",L"security",L"double"}) {
    auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed && !result.process.timed_out,"valid exchange failed");
    auto reply=accept_worker_reply(result.response,batch,221,WorkerExecution::cpu_semantics);
    check(reply.bytes==Bytes(batch.cpu_reference().begin(),batch.cpu_reference().end()),"cross-process byte parity");
    check(result.process.peak_committed_bytes<=128ULL*1024*1024,"worker memory cap exceeded");
    check_closed(result);
    observe(mode);
  }
  for(const auto* mode:{L"bad-wire",L"wrong-id",L"wrong-backend",L"bad-status",L"bad-bytes",L"trailing"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::completed,"malformed reply test did not reach validation");
    rejects([&]{accept_worker_reply(result.response,batch,221,WorkerExecution::cpu_semantics);});
    check_closed(result);
    observe(mode);
  }
  for(const auto* mode:{L"exit-empty",L"oversize",L"tamper-input",L"tamper-guard",L"tamper-length",L"tamper-header"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::invalid_response && result.response.empty(),"invalid transport output escaped");
    check_closed(result);
    observe(mode);
  }
  for(const auto* mode:{L"exit-bad",L"publish-fail"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::worker_failed && result.response.empty(),"failed helper output escaped");
    check(result.publication_observed==(std::wstring_view(mode)==L"publish-fail"),"failure publication evidence");
    check_closed(result);
    observe(mode);
  }
  ProcessBudget quick;quick.timeout_ms=2000;
  const auto timed=run_worker_exchange(executable,{L"publish-hang"},request,quick);
  check(timed.code==WorkerExchangeCode::timed_out && timed.process.exit_confirmed && timed.publication_observed && timed.response.empty(),"timed-out response escaped");
  check_closed(timed);
  observe(L"timeout");
  const auto recovered=run_worker_exchange(executable,{L"ok"},request);
  check(recovered.code==WorkerExchangeCode::completed,"fresh isolated exchange after timeout failed");
  (void)accept_worker_reply(recovered.response,batch,221,WorkerExecution::cpu_semantics);
  check_closed(recovered);
  observe(L"recovery");
  for(const auto* token:{L"",L"..",L"Global\\name",L"0123456789abcdef0123456789abcdefff!",L"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"})
    rejects([&]{(void)WorkerChannel::open(token);});
  rejects([&]{run_worker_exchange(executable,{},{});});
  auto oversized=request;oversized.resize(kWorkerRequestLimit+1);
  rejects([&]{run_worker_exchange(executable,{},oversized);});
  rejects([&]{run_worker_exchange(executable,std::vector<std::wstring>(15,L"arg"),request);});
  observe(L"argument-rejections");
  {
    auto original=WorkerExchangeSession::start(executable,{L"ok"},request);
    auto moved=std::move(original);
    check(!original.try_result() && !original.process_id() && !original.request_cancel(),
        "moved-from session retained transport ownership");
    eventually([&]{return moved.try_result()!=nullptr;},3000,"asynchronous success did not publish");
    const auto* result=moved.try_result();
    check(result && result->code==WorkerExchangeCode::completed && !moved.failure(),
        "asynchronous successful child failed");
    const auto reply=accept_worker_reply(result->response,batch,221,WorkerExecution::cpu_semantics);
    check(reply.bytes==Bytes(batch.cpu_reference().begin(),batch.cpu_reference().end()),
        "asynchronous actual byte parity");
    eventually([&]{return moved.retirement_confirmed() && WorkerExchangeSession::available();},
        2000,"successful asynchronous supervisor did not retire");
    check_closed(*result);
  }
  observe(L"async-success");
  asynchronous_fallback(executable);
  observe(L"async-fallback");
  abandoned_session(executable,request);
  observe(L"async-abandoned");
  for(unsigned i=0;i<4;++i) {
    auto failed=WorkerExchangeSession::start(executable.parent_path()/L"missing-flitzfs-worker.exe",{L"ok"},request);
    eventually([&]{return failed.try_result()!=nullptr;},2000,"missing helper supervisor did not finish");
    eventually([&]{return WorkerExchangeSession::available();},2000,"failed supervisor thread did not retire");
    check(failed.try_result()->code==WorkerExchangeCode::supervisor_failed &&
        failed.try_result()->response.empty() && failed.failure() &&
        failed.retirement_confirmed() && WorkerExchangeSession::available(),
        "supervisor failure was accepted or leaked admission");
  }
  observe(L"async-launch-failure");
  // Repeated complete transactions must not accumulate process handles or keep
  // either mapping alive. This is not a stress test: four tiny serial requests.
  for(unsigned i=0;i<4;++i) {
    const auto result=run_worker_exchange(executable,{L"ok"},request);
    check(result.code==WorkerExchangeCode::completed,"repeat exchange failed");
    (void)accept_worker_reply(result.response,batch,221,WorkerExecution::cpu_semantics);
    check_closed(result);observe(L"repeat");
  }
  DWORD final_handles=0;check(GetProcessHandleCount(GetCurrentProcess(),&final_handles),"final handle count");
  for(std::size_t i=0;i<observed;++i)
    std::wcerr<<L"handle_observation "<<observations[i].step<<L"="<<observations[i].count<<L'\n';
  std::cerr<<"handle_totals initial="<<initial_handles<<" final="<<final_handles<<'\n';
  check(final_handles<=initial_handles,"worker channel handles accumulated");
  std::cout<<"PASS: "<<checks<<" real Windows worker exchange checks; CPU semantics only, no GPU or data files.\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"worker exchange failure after "<<checks<<": "<<e.what()<<'\n';return 1;}}
