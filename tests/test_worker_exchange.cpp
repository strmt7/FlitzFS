#include "worker_exchange.hpp"
#include "gpu_worker_common.hpp"
#include "hip_worker_exit.hpp"
#include "hip_identity_wire.hpp"
#include "hip_memory_wire.hpp"
#include "hip_phase_profile.hpp"
#include "gpu_worker_controller.hpp"
#include "gpu_match_worker_controller.hpp"
#include "gpu_lz4_worker_controller.hpp"
#include "uniform_lz4_worker_controller.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include "flitzfs/lz4_worker_wire.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "flitzfs/native_session.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "store_codec.hpp"
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
Bytes lz4_literal_fixture(Lz4WorkerBytes input) {
  Bytes result{static_cast<std::byte>(std::min<std::size_t>(input.size(),15U)<<4)};
  if(input.size()>=15) {
    auto left=input.size()-15;
    while(left>=255){result.push_back(std::byte{255});left-=255;}
    result.push_back(static_cast<std::byte>(left));
  }
  result.insert(result.end(),input.begin(),input.end());return result;
}
UniformLz4Limits uniform_fixture_limits(){
  uniform_lz4::Layout p{};check(uniform_lz4::make_layout(16U*65536+37,65536,p),"uniform channel fixture layout");
  return uniform_lz4_limits(p);
}
constexpr WorkerChannelCapacity sized_capacity{3U*1024*1024+37,2U*1024*1024+71};
int sized_child(std::wstring_view mode,std::wstring_view token) {
  check_job();
  if(mode==L"sized-wrong-capacity") {
    auto wrong=sized_capacity;wrong.request_bytes--;
    rejects([&]{(void)WorkerChannel::open_sized(token,wrong);});return 0;
  }
  if(mode==L"sized-legacy") {
    rejects([&]{(void)WorkerChannel::open(token);});return 0;
  }
  auto channel=WorkerChannel::open_sized(token,sized_capacity);
  const auto input=channel.request();
  check(input.size()==sized_capacity.request_bytes,"sized source extent lost");
  bool exact=true;
  for(std::size_t i=0;i<input.size();++i)
    exact=exact && input[i]==static_cast<std::byte>((i*13+i/251)%256);
  check(exact,"sized private input mismatch");
  Bytes output(sized_capacity.reply_bytes);
  for(std::size_t i=0;i<output.size();++i)output[i]=input[input.size()-1-i];
  if(mode==L"sized-hold"){Sleep(5000);return 0;}
  if(mode==L"sized-guard") {
    const auto name=L"Local\\FlitzFS.worker."+std::wstring(token)+L".reply";
    HANDLE object=OpenFileMappingW(FILE_MAP_WRITE,FALSE,name.c_str());
    check(object!=nullptr,"sized guard mapping");
    auto* view=static_cast<std::byte*>(MapViewOfFile(object,FILE_MAP_WRITE,0,0,16+output.size()+16));
    if(!view){CloseHandle(object);throw std::runtime_error("sized guard view");}
    view[16+output.size()]=std::byte{0};UnmapViewOfFile(view);CloseHandle(object);
    rejects([&]{channel.publish(output);});return 0;
  }
  channel.publish(output);
  rejects([&]{channel.publish(output);});
  return mode==L"sized-fail-after"?17:0;
}
int child(std::wstring_view mode,std::wstring_view token) {
  if(mode.starts_with(L"uniform-")){
    check_job();auto limits=uniform_fixture_limits();
    if(mode.starts_with(L"uniform-native")){
      const auto bytes=mode.starts_with(L"uniform-native1")?65536U:2U*65536;
      uniform_lz4::Layout layout{};check(uniform_lz4::make_layout(bytes,65536,layout),"native fixture uniform layout");
      limits=uniform_lz4_limits(layout);
    }
    auto channel=WorkerChannel::open_sized(token,{static_cast<std::size_t>(limits.request_bytes),
        static_cast<std::size_t>(limits.reply_bytes)});
    auto request=decode_uniform_lz4_request(channel.request(),limits);
    if(!gpu_lz4_cpu_verification_available())return 17;
    if(mode==L"uniform-hold" || mode==L"uniform-native2-hold"){Sleep(5000);return 0;}
    UniformLz4Candidates c;
    for(std::uint32_t i=0;i<request.batch.layout().chunks;++i){
      const auto source=request.batch.input(i);
      const auto fixture=native_lz4_fixture::source();
      const auto payload=mode.starts_with(L"uniform-native") && std::ranges::equal(source,fixture)?
          native_lz4_fixture::alternative_block(source):lz4_literal_fixture(source);
      c.sizes.push_back(static_cast<std::uint32_t>(payload.size()));
      c.payload.insert(c.payload.end(),payload.begin(),payload.end());
    }
    auto frame=encode_uniform_lz4_reply(request.batch,request.request_id,c,WorkerExecution::cpu_semantics);
    if(mode==L"uniform-corrupt" || mode==L"uniform-delay-invalid" || mode==L"uniform-native2-corrupt")frame.back()^=std::byte{1};
    if(mode==L"uniform-delay-invalid")Sleep(150);
    channel.publish(frame);return mode==L"uniform-fail-after"?17:0;
  }
  if(mode.starts_with(L"sized-"))return sized_child(mode,token);
  check_job();
  if(mode==L"exit-empty")return 0;
  if(mode==L"exit-bad")return 17;
  // These declared HIP failures execute no HIP; only exit transport is tested.
  if(mode.size()==13 && mode.starts_with(L"staged-exit-") && mode.back()>=L'1' && mode.back()<=L'6')
    return static_cast<int>(hip_worker_diagnostic_exit(
        static_cast<HipWorkerExit>(0x46530000U+static_cast<unsigned>(mode.back()-L'0')),
        HipWorkerStage::device_properties));
  auto channel=WorkerChannel::open(token);
  if(mode==L"lz4-ok" || mode==L"lz4-corrupt" || mode==L"lz4-fail-after" ||
      mode==L"lz4-hold" || mode==L"lz4-delay-invalid" || mode==L"lz4-hip-declaration") {
    const auto input=decode_lz4_worker_request(channel.request());
    if(mode==L"lz4-hold"){Sleep(5000);return 0;}
    if(!gpu_lz4_cpu_verification_available())return 17;
    std::vector<Bytes> payloads;
    for(std::size_t i=0;i<input.batch.count();++i)payloads.push_back(lz4_literal_fixture(input.batch.input(i)));
    std::vector<Lz4WorkerBytes> views;for(const auto& payload:payloads)views.emplace_back(payload);
    auto frame=encode_lz4_worker_reply(input.batch,input.request_id,views,
        mode==L"lz4-hip-declaration"?WorkerExecution::hip:WorkerExecution::cpu_semantics);
    if(mode==L"lz4-corrupt" || mode==L"lz4-delay-invalid")frame.back()^=std::byte{1};
    if(mode==L"lz4-delay-invalid")Sleep(150);
    channel.publish(frame);
    return mode==L"lz4-fail-after"?17:0;
  }
  if(mode==L"memory-metadata" || mode==L"memory-corrupt" || mode==L"memory-fail-after"){
    const auto input=decode_worker_request(channel.request());
    HipMemoryObservation value;
    value.binding={input.request_id,3,1,{std::byte{5}},input.batch.device_buffer_bytes(),input.batch.cpu_reference().size()};
    value.cpu_verified_bytes=value.binding.decoded_bytes;
    value.failure_category=0x46530005;value.failure_stage=31;value.captured=7;value.flags=59;
    for(unsigned i=0;i<3;++i){
      auto& snapshot=value.snapshots[i];snapshot.luid=value.binding.luid;snapshot.node_mask=1;
      snapshot.captured_ms=1000+i;snapshot.dedicated_bytes=1ULL<<30;snapshot.local_budget=1ULL<<29;
      snapshot.nonlocal_budget=1ULL<<28;snapshot.nonlocal_usage=i==2?4096:0;snapshot.valid=true;snapshot.amd_hardware=true;
    }
    auto frame=encode_hip_memory_observation(value);
    if(mode==L"memory-corrupt")frame[60]|=std::byte{128};
    channel.publish(frame);
    // Declared values test framing only; this fixture never loads HIP or
    // executes a GPU, regardless of the execution-step metadata it serializes.
    return mode==L"memory-fail-after"?17:0;
  }
  if(mode==L"identity-metadata") {
    const auto request=decode_hip_identity_request(channel.request());
    channel.publish(encode_hip_identity_reply({request.request_id,request.ordinal,0,request.luid}));
    return 0; // CPU-only transport fixture, never a HIP runtime query.
  }
  if(mode==L"match" || mode==L"match-invalid" || mode==L"match-hold" ||
      mode==L"match-delay-invalid" || mode==L"match-hip-declaration" ||
      mode==L"match-profile" || mode==L"match-profile-corrupt" || mode==L"match-profile-fail") {
    const auto input=decode_match_worker_request(channel.request());
    if(mode==L"match-hold"){Sleep(5000);return 0;}
    const auto& search=input.search;
    std::vector<match_detail::Match> matches(search.lanes());
    for(unsigned lane=0;lane<search.lanes();++lane)
      matches[lane]=match_detail::search(reinterpret_cast<const unsigned char*>(search.input().data()),
          static_cast<unsigned>(search.input().size()),search.predecessors().data(),
          static_cast<unsigned>(search.predecessors().size()),lane);
    if(mode==L"match-profile" || mode==L"match-profile-corrupt" || mode==L"match-profile-fail") {
      // CPU fixture measurements test ownership/framing; no HIP call is made.
      HipPhaseProfile value;
      value.binding={input.request_id,3,1,static_cast<std::uint32_t>(search.input().size()),
          search.lanes(),{std::byte{5}},{}};
      std::copy_n(channel.request().begin()+32,32,value.binding.source_digest.begin());
      HipPhaseRecorder recorder(&value.trace);
      for(unsigned n=0;n<hip_phase_count;++n){recorder.begin(static_cast<HipPhase>(n));recorder.end(static_cast<HipPhase>(n));}
      recorder.finish();
      auto metadata=encode_hip_phase_profile(value);
      if(mode==L"match-profile-corrupt")metadata.back()=std::byte{1};
      channel.publish(metadata);
      return mode==L"match-profile-fail"?7:0;
    }
    // The HIP declaration below exercises policy only. This child always runs CPU code.
    auto frame=encode_match_worker_reply(search,input.request_id,matches,
        mode==L"match-hip-declaration"?WorkerExecution::hip:WorkerExecution::cpu_semantics);
    if(mode==L"match-invalid" || mode==L"match-delay-invalid")frame[kMatchWorkerHeaderBytes]=std::byte{1};
    if(mode==L"match-delay-invalid")Sleep(150);
    channel.publish(frame);
    return 0;
  }
  const auto input=decode_worker_request(channel.request());
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
  if(mode==L"bad-bytes" || mode==L"delay-bad-bytes")reply.bytes.back()^=std::byte{1};
  auto frame=encode_worker_reply(reply,mode==L"wrong-backend"?WorkerExecution::hip:WorkerExecution::cpu_semantics);
  if(mode==L"bad-wire")frame[0]=std::byte{0};
  if(mode==L"trailing")frame.push_back(std::byte{0});
  if(mode==L"oversize") {
    frame.resize(kWorkerReplyLimit+1);
    rejects([&]{channel.publish(frame);});return 0;
  }
  if(mode==L"security")check_security(token);
  if(mode==L"delay-bad-bytes")Sleep(150); // Bounded fixture ordering only.
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
void lz4_worker_transactions(const std::filesystem::path& executable) {
  const std::vector<Bytes> source{Bytes(17,std::byte{11}),Bytes(33,std::byte{22}),Bytes(1024,std::byte{33})};
  std::vector<Lz4WorkerBytes> inputs;for(const auto& bytes:source)inputs.emplace_back(bytes);
  const auto batch=PreparedLz4WorkerBatch::prepare(inputs);
  const auto request=encode_lz4_worker_request(batch,991);
  const auto result=run_worker_exchange(executable,{L"lz4-ok"},request);
  check(result.process.exit_confirmed && !result.process.timed_out &&
      result.process.peak_committed_bytes<=128ULL*1024*1024,"LZ4 CPU helper retirement/budget");
  if(gpu_lz4_cpu_verification_available()) {
    check(result.code==WorkerExchangeCode::completed,"LZ4 CPU helper did not complete");
    const auto accepted=accept_lz4_worker_reply(result.response,batch,991,WorkerExecution::cpu_semantics);
    check(accepted.size()==source.size(),"LZ4 cross-process candidate count");
    for(std::size_t i=0;i<source.size();++i)
      check(accepted[i]==lz4_literal_fixture(source[i]) && std::ranges::equal(batch.input(i),source[i]),
          "LZ4 cross-process candidate/source bytes");
  }else {
    check(result.code==WorkerExchangeCode::worker_failed && result.process.exit_code==17 &&
        !result.publication_observed && result.response.empty(),"Unavailable CPU LZ4 exposed a candidate");
  }
  check_closed(result);
  auto malformed=request;malformed[0]^=std::byte{1};
  const auto refused=run_worker_exchange(executable,{L"lz4-ok"},malformed);
  check(refused.code==WorkerExchangeCode::worker_failed && refused.process.exit_confirmed &&
      refused.response.empty() && !refused.publication_observed,"Malformed LZ4 source exposed output");
  check_closed(refused);
  if(gpu_lz4_cpu_verification_available()) {
    const auto corrupt=run_worker_exchange(executable,{L"lz4-corrupt"},request);
    check(corrupt.code==WorkerExchangeCode::completed && corrupt.process.exit_confirmed,
        "Corrupt LZ4 fixture transport failed before validation");
    rejects([&]{(void)accept_lz4_worker_reply(corrupt.response,batch,991,WorkerExecution::cpu_semantics);});
    check_closed(corrupt);
    const auto failed=run_worker_exchange(executable,{L"lz4-fail-after"},request);
    check(failed.code==WorkerExchangeCode::worker_failed && failed.process.exit_confirmed &&
        failed.process.exit_code==17 && failed.publication_observed && failed.response.empty(),
        "Failed LZ4 helper exposed a published candidate");
    check_closed(failed);
  }
  std::cout<<"LZ4 worker process framing: decoder_available="<<gpu_lz4_cpu_verification_available()
      <<" chunks=3 source_bytes=1074 gpu_executed=false\n";
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
  memory.nonlocal_budget=8ULL<<30;
  memory.physical_total=16ULL<<30;memory.physical_available=8ULL<<30;
  memory.commit_limit=32ULL<<30;memory.commit_available=16ULL<<30;
  return memory; // Explicit test evidence. No real adapter telemetry is queried.
}

GpuHealthEvidence coordinated_health(std::uint64_t sequence,std::uint64_t time,unsigned config=1) {
  GpuHealthEvidence e;e.configuration[0]=static_cast<std::byte>(config);
  e.sequence=sequence;e.compatible=e.supervisor_quiescent=true;
  std::array<std::byte,8> luid{};luid[0]=std::byte{1};
  e.memory=synthetic_memory(luid);e.memory.captured_ms=time;return e;
}
PreparedGpuBatch coordinated_fill(unsigned fill) {
  const GpuBlockView view{1,static_cast<std::uint8_t>(fill),4097,{}};
  return PreparedGpuBatch::prepare({&view,1});
}
void coordinated_requests(const std::filesystem::path& executable) {
  const auto missing=executable.parent_path()/L"missing-flitzfs-worker.exe";
  const auto health=coordinated_health(1,100);
  const auto after=coordinated_health(2,101).memory;
  const auto expected_batch=mixed();
  const Bytes expected(expected_batch.cpu_reference().begin(),expected_batch.cpu_reference().end());
  auto selected=[&](GpuWorkerController& controller) {
    check(Bytes(controller.selected_bytes().begin(),controller.selected_bytes().end())==expected,
        "coordinated selected bytes differ from independent CPU reference");
  };
  for(const auto* mode:{L"missing",L"bad-bytes",L"delay-bad-bytes"}) {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    const auto path=std::wstring_view(mode)==L"missing"?missing:executable;
    check(controller.begin(std::move(batch),path,{mode},true,true,health,100).submitted,
        "late-failure fixture not submitted");
    (void)controller.use_cpu_fallback();selected(controller);
    if(std::wstring_view(mode)==L"delay-bad-bytes") {
      auto next=coordinated_fill(83);const Bytes cpu(next.cpu_reference().begin(),next.cpu_reference().end());
      const auto e=coordinated_health(2,102);
      const auto decision=controller.begin(std::move(next),executable,{L"ok"},true,true,e,102);
      check(!decision.submitted && (decision.route.reason==GpuRouteReason::worker_busy ||
          decision.route.reason==GpuRouteReason::configuration_blocked) &&
          Bytes(controller.selected_bytes().begin(),controller.selected_bytes().end())==cpu,
          "different CPU request lost pending late-failure reference or waited");
    }
    eventually([&]{return controller.try_retire();},3000,"late failed helper did not retire");
    check(controller.state()==GpuWorkerState::cpu_selected && controller.consecutive_failures()==1,
        "late failure changed terminal CPU selection or was counted twice");
    auto retry=mixed();const auto fresh=coordinated_health(3,103);
    const auto blocked=controller.begin(std::move(retry),executable,{L"ok"},true,true,fresh,103);
    check(!blocked.submitted && blocked.route.reason==GpuRouteReason::configuration_blocked,
        "CPU-selected late unavailable/corrupt worker evidence was forgotten");
    selected(controller);
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);
    auto batch=mixed();
    const auto disabled=controller.begin(std::move(batch),missing,{L"ok"},false,true,health,100);
    check(!disabled.submitted && disabled.route.reason==GpuRouteReason::disabled &&
        controller.state()==GpuWorkerState::cpu_selected && !controller.process_id(),"disabled coordinator launched a helper");
    selected(controller);
    auto no_work=mixed();
    const auto unqualified=controller.begin(std::move(no_work),missing,{L"ok"},true,false,health,100);
    check(!unqualified.submitted && unqualified.route.reason==GpuRouteReason::unqualified &&
        controller.consecutive_failures()==0,"unqualified workload altered worker history");
    selected(controller);
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    check(controller.begin(std::move(batch),executable,{L"ok"},true,true,health,100).submitted,
        "coordinated success was not submitted");
    auto overlap=coordinated_fill(33);
    const auto size=overlap.cpu_reference().size();
    check(!controller.begin(std::move(overlap),executable,{L"ok"},true,true,health,100).submitted &&
        overlap.cpu_reference().size()==size,"overlap consumed input or started another request");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"coordinated success did not complete");
    check(controller.state()==GpuWorkerState::worker_selected &&
        controller.last_gate_code()==GpuGateCode::accepted,"coordinator accepted no valid worker bytes");
    selected(controller);
    check(controller.use_cpu_fallback().size()==expected.size() && controller.state()==GpuWorkerState::worker_selected,
        "late CPU selection rewrote a terminal worker outcome");
    eventually([&]{return controller.try_retire();},2000,"coordinated successful supervisor did not retire");
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    check(controller.begin(std::move(batch),missing,{L"ok"},true,true,health,100).submitted,
        "missing-helper failure fixture did not start supervision");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"missing coordinated helper did not fail");
    check(controller.state()==GpuWorkerState::cpu_selected && controller.consecutive_failures()==1,
        "missing helper failed to select CPU/report once");
    selected(controller);
    eventually([&]{return controller.try_retire();},2000,"missing coordinated helper retained retirement");
    for(unsigned i=0;i<4;++i) {
      auto fresh=mixed();const auto e=coordinated_health(i+2,i+102);
      const auto blocked=controller.begin(std::move(fresh),executable,{L"ok"},true,true,e,i+102);
      check(!blocked.submitted && blocked.route.reason==GpuRouteReason::configuration_blocked &&
          !controller.process_id() && controller.consecutive_failures()==1,
          "unchanged unavailable configuration was retried or recounted");
      selected(controller);
    }
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    check(controller.begin(std::move(batch),executable,{L"bad-bytes"},true,true,health,100).submitted,
        "integrity fixture not submitted");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"coordinated integrity failure not observed");
    check(controller.state()==GpuWorkerState::cpu_selected &&
        controller.last_gate_code()==GpuGateCode::invalid_output,"corrupt worker output escaped coordinator");
    selected(controller);eventually([&]{return controller.try_retire();},2000,"corrupt worker did not retire");
    auto changed=mixed();const auto other=coordinated_health(2,102,2);
    check(controller.begin(std::move(changed),executable,{L"ok"},true,true,other,102).submitted,
        "independently qualified changed configuration remained blocked");
    eventually([&]{return controller.poll(coordinated_health(3,103,2).memory,103)!=GpuWorkerState::waiting;},
        3000,"changed configuration did not complete");
    check(controller.state()==GpuWorkerState::worker_selected,"changed configuration not independently accepted");
    eventually([&]{return controller.try_retire();},2000,"changed configuration did not retire");
    auto original=mixed();const auto old=coordinated_health(4,104);
    const auto blocked=controller.begin(std::move(original),executable,{L"ok"},true,true,old,104);
    check(!blocked.submitted && blocked.route.reason==GpuRouteReason::configuration_blocked,
        "A-to-B-to-A forgot corrupt configuration history");
    selected(controller);
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    check(controller.begin(std::move(batch),executable,{L"publish-hang"},true,true,health,100).submitted,
        "coordinated fallback fixture not submitted");
    eventually([&]{return controller.process_id()!=0;},2000,"coordinated live helper did not start");
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,controller.process_id());
    check(child!=nullptr,"coordinated fixture child observation");
    struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{child};
    check(WaitForSingleObject(child,0)==WAIT_TIMEOUT,"coordinated helper exited before CPU selection");
    (void)controller.use_cpu_fallback();selected(controller);
    check(WaitForSingleObject(child,0)==WAIT_TIMEOUT && controller.consecutive_failures()==0 &&
        !controller.try_retire(),"coordinated CPU selection waited for teardown or fabricated retirement");
    auto next=coordinated_fill(91);const Bytes next_cpu(next.cpu_reference().begin(),next.cpu_reference().end());
    const auto fresh=coordinated_health(2,102);
    const auto busy=controller.begin(std::move(next),executable,{L"ok"},true,true,fresh,102);
    check(!busy.submitted && busy.route.reason==GpuRouteReason::worker_busy &&
        Bytes(controller.selected_bytes().begin(),controller.selected_bytes().end())==next_cpu,
        "new CPU request waited for retiring worker or used old request bytes");
    check(controller.consecutive_failures()==0 && controller.request_cancel(),"unobserved fallback outcome was fabricated/cancellation lost");
    eventually([&]{return controller.try_retire();},3000,"coordinated cancelled helper did not retire");
    check(controller.consecutive_failures()==1,"cancelled fallback outcome not reported exactly once");
    for(unsigned i=0;i<2;++i) {
      auto retry=mixed();const auto e=coordinated_health(3+i,103+i);
      check(controller.begin(std::move(retry),executable,{L"ok"},true,true,e,103+i).submitted,
          "fresh healthy observation after retirement did not allow retry");
      eventually([&]{return controller.poll(coordinated_health(4+i,104+i).memory,104+i)!=GpuWorkerState::waiting;},
          3000,"coordinated recovery did not complete");
      check(controller.state()==GpuWorkerState::worker_selected &&
          controller.consecutive_failures()==(i?0U:1U),"one success erased flapping history or two failed to clear it");
      eventually([&]{return controller.try_retire();},2000,"coordinated recovered helper did not retire");
    }
  }
  {
    GpuWorkerController controller(WorkerExecution::cpu_semantics);auto batch=mixed();
    check(controller.begin(std::move(batch),executable,{L"publish-hang"},true,true,health,100).submitted,
        "coordinated expiry fixture not submitted");
    check(controller.poll(coordinated_health(3,200).memory,200)==GpuWorkerState::cpu_selected &&
        controller.last_gate_code()==GpuGateCode::expired && controller.consecutive_failures()==0,
        "coordinated deadline did not select terminal CPU");
    selected(controller);
    for(unsigned i=0;i<4;++i)(void)controller.poll(after,101);
    check(controller.consecutive_failures()==0,"late polling fabricated an unobserved worker outcome");
    eventually([&]{return controller.try_retire();},3000,"expired coordinated helper did not retire");
    check(controller.consecutive_failures()==1,"expired request outcome not reported once after observation");
  }
  // Expected-backend policy tests still execute ONLY this CPU fixture. A HIP
  // declaration permits controller-path testing; it is NOT hardware evidence.
  // External cancellation, a supervisor timeout, and a foreground gate expiry
  // must all retain a driver recovery requirement after exact process exit.
  for(const auto* trigger:{L"cancel",L"timeout",L"expire"}) {
    GpuWorkerController controller(WorkerExecution::hip);auto batch=mixed();
    ProcessBudget budget;budget.timeout_ms=std::wstring_view(trigger)==L"timeout"?250:5000;
    check(controller.begin(std::move(batch),executable,{L"publish-hang"},true,true,health,100,budget).submitted,
        "HIP-policy CPU fixture was not submitted");
    eventually([&]{return controller.process_id()!=0;},2000,"HIP-policy fixture did not start");
    if(std::wstring_view(trigger)==L"cancel") {
      check(controller.request_cancel(),"HIP-policy cancellation signal failed");
      eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,
          "HIP-policy cancelled fixture did not complete");
    }else if(std::wstring_view(trigger)==L"timeout") {
      // Frozen synthetic gate time keeps the supervisor timeout independent.
      eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,
          "HIP-policy timed fixture did not complete");
    }else {
      check(controller.poll(coordinated_health(2,200).memory,200)==GpuWorkerState::cpu_selected &&
          controller.last_gate_code()==GpuGateCode::expired,"HIP-policy expiry did not select CPU");
    }
    selected(controller);
    eventually([&]{return controller.try_retire();},3000,"HIP-policy exact worker did not retire");
    check(controller.consecutive_failures()==1,"HIP-policy failure was forgotten or counted twice");
    // Time, fresh telemetry and repeated healthy samples do not establish
    // driver recovery, even when no child/thread remains.
    for(unsigned i=0;i<3;++i) {
      auto retry=mixed();const auto fresh=coordinated_health(3+i,300+i);
      const auto denied=controller.begin(std::move(retry),executable,{L"wrong-backend"},true,true,fresh,300+i);
      check(!denied.submitted && denied.route.reason==GpuRouteReason::device_quarantined &&
          controller.consecutive_failures()==1 && !controller.process_id(),
          "HIP-policy retirement/telemetry incorrectly cleared driver quarantine");
      selected(controller);
    }
    // Separately qualified recovery, rather than elapsed time, admits one new
    // bounded attempt. The helper explicitly declares HIP but computes on CPU.
    auto retry=mixed();auto recovered=coordinated_health(6,304);recovered.recovery_generation=1;
    check(controller.begin(std::move(retry),executable,{L"wrong-backend"},true,true,recovered,304).submitted,
        "separate recovery evidence failed to admit HIP-policy fixture");
    eventually([&]{return controller.poll(coordinated_health(7,305).memory,305)!=GpuWorkerState::waiting;},
        3000,"HIP-policy recovered fixture did not complete");
    check(controller.state()==GpuWorkerState::worker_selected && controller.consecutive_failures()==1,
        "one synthetic accepted attempt erased flapping history");
    selected(controller);eventually([&]{return controller.try_retire();},2000,
        "HIP-policy recovered fixture did not retire");
  }
  check(WorkerExchangeSession::available(),"coordinated requests leaked process/thread admission");
}

PreparedGpuMatchSearch coordinated_matches(unsigned seed=0,bool full=false) {
  Bytes bytes(full?65536:128);
  for(unsigned i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i%31)+seed);
  return PreparedGpuMatchSearch::prepare(bytes);
}
void coordinated_match_requests(const std::filesystem::path& executable) {
  const auto health=coordinated_health(1,100);
  const auto after=coordinated_health(2,101).memory;
  const auto missing=executable.parent_path()/L"missing-flitzfs-worker.exe";
  const auto same_source=[&](GpuMatchWorkerController& controller,const Bytes& source) {
    check(std::ranges::equal(controller.source(),source),"match controller lost current CPU source");
    if(controller.state()==GpuWorkerState::cpu_selected)
      check(controller.selected_proposals().empty(),"CPU match selection retained worker proposals");
  };
  {
    GpuMatchWorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_matches();
    const Bytes original(input.input().begin(),input.input().end());
    const auto disabled=controller.begin(std::move(input),missing,{L"match"},false,true,health,100);
    check(!disabled.submitted&&disabled.route.reason==GpuRouteReason::disabled&&!controller.process_id(),
        "disabled match request launched a process");
    same_source(controller,original);
    auto next=coordinated_matches(1);
    const auto denied=controller.begin(std::move(next),missing,{L"match"},true,false,health,100);
    check(!denied.submitted&&denied.route.reason==GpuRouteReason::unqualified&&controller.consecutive_failures()==0,
        "unqualified match work changed recovery history");
  }
  {
    GpuMatchWorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_matches(0,true);
    const Bytes original(input.input().begin(),input.input().end());
    check(controller.begin(std::move(input),executable,{L"match"},true,true,health,100).submitted,
        "qualified match request did not start");
    auto overlapping=coordinated_matches(1);
    const auto size=overlapping.input().size();
    check(!controller.begin(std::move(overlapping),executable,{L"match"},true,true,health,100).submitted&&
        overlapping.input().size()==size,"overlapping match attempt consumed caller input");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"match controller success did not finish");
    check(controller.state()==GpuWorkerState::worker_selected&&controller.last_gate_code()==GpuGateCode::accepted&&
        !controller.selected_proposals().empty(),"match controller accepted no proposals");
    const auto selected=controller.selected_proposals().size();
    (void)controller.use_cpu_fallback();
    check(controller.state()==GpuWorkerState::worker_selected&&controller.selected_proposals().size()==selected,
        "CPU selection overwrote a terminal compression result");
    same_source(controller,original);
    eventually([&]{return controller.try_retire();},2000,"successful match controller did not retire");
    // Feed the controller-selected hints through the real native writer.
    auto head=std::make_shared<native_fixture::Ram>(),source=std::make_shared<native_fixture::Ram>();
    auto target=std::make_shared<native_fixture::Ram>();
    auto volume=native_fixture::identity(),image=volume,next=volume;image[0]=std::byte{61};next[0]=std::byte{62};
    auto native=NativeFileSession::create_volume(head,volume,source,image,{StoreCompressionMode::raw,6,false});
    native.create_entry("data");auto handle=native.open("data",{true,true,false});native.write(handle,0,original);
    native.begin_recompression(target,next);
    check(native.advance_recompression()==NativeFileSession::RecompressionProgress::copying,"controller source layout");
    auto work=native.next_recompression_input();
    const auto progress=work?native.advance_recompression(work->object_id,controller.selected_proposals()):
        native.advance_recompression();
    check(progress==NativeFileSession::RecompressionProgress::ready,"controller proposal failed staged encoding");
    native.publish_recompression();
    check(native.read(handle,0,65536)==original,"controller-selected native bytes changed");
    native.cleanup(handle,false);native.close(handle);
  }
  for(const auto* mode:{L"missing",L"match-invalid",L"match-delay-invalid"}) {
    GpuMatchWorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_matches();
    const auto helper=std::wstring_view(mode)==L"missing"?missing:executable;
    check(controller.begin(std::move(input),helper,{mode},true,true,health,100).submitted,"match fault request not submitted");
    (void)controller.use_cpu_fallback();
    auto later=coordinated_matches(1);
    const Bytes later_source(later.input().begin(),later.input().end());
    const auto fresh=coordinated_health(2,102);
    const auto busy=controller.begin(std::move(later),executable,{L"match"},true,true,fresh,102);
    check(!busy.submitted&&(busy.route.reason==GpuRouteReason::worker_busy||
        busy.route.reason==GpuRouteReason::configuration_blocked),"match retry bypassed pending fault/retirement");
    same_source(controller,later_source);
    eventually([&]{return controller.try_retire();},3000,"late match fault did not retire");
    check(controller.consecutive_failures()==1&&controller.state()==GpuWorkerState::cpu_selected,
        "late match fault was forgotten or counted twice");
    auto retry=coordinated_matches(2);
    const auto denied=controller.begin(std::move(retry),executable,{L"match"},true,true,coordinated_health(3,103),103);
    check(!denied.submitted&&denied.route.reason==GpuRouteReason::configuration_blocked,
        "unchanged corrupt/unavailable match configuration retried");
  }
  {
    GpuMatchWorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_matches();
    check(controller.begin(std::move(input),executable,{L"match-hold"},true,true,health,100).submitted,"held match start");
    eventually([&]{return controller.process_id()!=0;},2000,"held match child did not start");
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,controller.process_id());
    check(child!=nullptr,"held match exact child observation");
    struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{child};
    const auto cpu=controller.use_cpu_fallback();
    check(!cpu.empty()&&WaitForSingleObject(child,0)==WAIT_TIMEOUT&&controller.consecutive_failures()==0,
        "match CPU selection waited for retirement or invented a failure");
    auto other=coordinated_matches(3);const Bytes expected(other.input().begin(),other.input().end());
    const auto busy=controller.begin(std::move(other),executable,{L"match"},true,true,coordinated_health(2,102),102);
    check(!busy.submitted&&busy.route.reason==GpuRouteReason::worker_busy,"new match CPU request launched during retirement");
    same_source(controller,expected);
    check(controller.request_cancel(),"held match cancellation failed");
    eventually([&]{return controller.try_retire();},3000,"held match controller did not retire");
    check(controller.consecutive_failures()==1,"match cancellation outcome missing");
    for(unsigned i=0;i<2;++i) {
      auto recovered=coordinated_matches();
      const auto e=coordinated_health(3+i,103+i);
      check(controller.begin(std::move(recovered),executable,{L"match"},true,true,e,103+i).submitted,
          "fresh healthy evidence after match pressure did not allow one attempt");
      eventually([&]{return controller.poll(coordinated_health(4+i,104+i).memory,104+i)!=GpuWorkerState::waiting;},
          3000,"recovered match did not finish");
      check(controller.state()==GpuWorkerState::worker_selected&&
          controller.consecutive_failures()==(i?0U:1U),"match flapping history cleared prematurely");
      eventually([&]{return controller.try_retire();},2000,"recovered match controller did not retire");
    }
  }
  // These use a CPU helper and synthetic telemetry while exercising HIP policy.
  for(const auto* trigger:{L"cancel",L"timeout",L"expire"}) {
    GpuMatchWorkerController controller(WorkerExecution::hip);
    auto input=coordinated_matches();
    ProcessBudget budget;budget.timeout_ms=std::wstring_view(trigger)==L"timeout"?250:5000;
    check(controller.begin(std::move(input),executable,{L"match-hold"},true,true,health,100,budget).submitted,
        "HIP-policy match fixture not submitted");
    eventually([&]{return controller.process_id()!=0;},2000,"HIP-policy match child did not start");
    if(std::wstring_view(trigger)==L"cancel")check(controller.request_cancel(),"HIP-policy match cancel");
    if(std::wstring_view(trigger)==L"expire")
      check(controller.poll(coordinated_health(2,200).memory,200)==GpuWorkerState::cpu_selected&&
          controller.last_gate_code()==GpuGateCode::expired,"match gate expiry did not select CPU");
    else eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"HIP-policy match result not observed");
    eventually([&]{return controller.try_retire();},3000,"HIP-policy match retirement missing");
    check(controller.consecutive_failures()==1&&controller.selected_proposals().empty(),"HIP match failure outcome");
    for(unsigned i=0;i<3;++i) {
      auto retry=coordinated_matches();
      const auto denied=controller.begin(std::move(retry),executable,{L"match-hip-declaration"},true,true,
          coordinated_health(3+i,300+i),300+i);
      check(!denied.submitted&&denied.route.reason==GpuRouteReason::device_quarantined,
          "match telemetry/retirement cleared a device fault");
    }
    auto recovered=coordinated_matches();
    auto qualified=coordinated_health(6,400);qualified.recovery_generation=1;
    check(controller.begin(std::move(recovered),executable,{L"match-hip-declaration"},true,true,qualified,400).submitted,
        "separately qualified match recovery did not admit work");
    auto valid_after=coordinated_health(7,401).memory;
    eventually([&]{return controller.poll(valid_after,401)!=GpuWorkerState::waiting;},3000,"qualified match recovery did not finish");
    check(controller.state()==GpuWorkerState::worker_selected,"qualified match recovery failed verification");
    eventually([&]{return controller.try_retire();},2000,"qualified recovered match did not retire");
  }
  for(unsigned fault=0;fault<4;++fault) {
    GpuMatchWorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_matches();
    check(controller.begin(std::move(input),executable,{L"match"},true,true,health,100).submitted,"match telemetry fixture");
    auto bad=after;
    if(fault==0)bad.captured_ms=99;
    if(fault==1)bad.luid[0]=std::byte{2};
    if(fault==2)bad.nonlocal_usage+=29ULL<<20;
    if(fault==3)bad.local_budget=bad.local_usage;
    eventually([&]{return controller.poll(bad,101)!=GpuWorkerState::waiting;},3000,"invalid match telemetry not observed");
    check(controller.state()==GpuWorkerState::cpu_selected&&controller.selected_proposals().empty(),
        "match telemetry failure exposed proposals");
    eventually([&]{return controller.try_retire();},2000,"invalid-telemetry match did not retire");
  }
}
PreparedLz4WorkerBatch coordinated_lz4(unsigned seed=0,bool full=false) {
  Bytes bytes(full?65536:128);
  for(unsigned i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i%31)+seed);
  const std::array<Lz4WorkerBytes,1> input{bytes};
  return PreparedLz4WorkerBatch::prepare(input);
}
void coordinated_lz4_requests(const std::filesystem::path& executable) {
  if(!gpu_lz4_cpu_verification_available()) {
    GpuLz4WorkerController controller;
    auto input=coordinated_lz4();
    const Bytes plain(input.plaintext().begin(),input.plaintext().end());
    const auto refused=controller.begin(std::move(input),executable,{L"lz4-ok"},true,true,
        coordinated_health(1,100),100);
    check(!refused.submitted && refused.route.reason==GpuRouteReason::unqualified &&
        !controller.process_id() && std::ranges::equal(controller.source(),plain),
        "missing independent compression decoder launched work or lost CPU bytes");
    std::cout<<"LZ4 controller: missing CPU decoder refused before launch; gpu_executed=false\n";
    return;
  }
  const auto health=coordinated_health(1,100);
  const auto after=coordinated_health(2,101).memory;
  const auto missing=executable.parent_path()/L"missing-flitzfs-worker.exe";
  const auto same_source=[&](GpuLz4WorkerController& controller,const Bytes& source) {
    check(std::ranges::equal(controller.source(),source),"compression controller lost current CPU source");
    if(controller.state()==GpuWorkerState::cpu_selected)
      check(controller.candidate(0).empty(),"CPU compression selection retained worker blocks");
  };
  {
    Bytes first(128,std::byte{0x34}),second(65,std::byte{0x76});
    const auto expected_first=first,expected_second=second;
    const std::array<Lz4WorkerBytes,2> views{first,second};
    auto input=PreparedLz4WorkerBatch::prepare(views);
    first.assign(first.size(),std::byte{0xff});
    second.assign(second.size(),std::byte{0xee});
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    check(controller.begin(std::move(input),executable,{L"lz4-ok"},true,true,health,100).submitted,
        "two-chunk compression request did not start");
    check(controller.count()==2 && std::ranges::equal(controller.source(0),expected_first) &&
        std::ranges::equal(controller.source(1),expected_second) && controller.source(2).empty() &&
        controller.candidate(0).empty(),"compression source ownership or waiting candidate exposure");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},
        3000,"two-chunk compression did not finish");
    check(controller.state()==GpuWorkerState::worker_selected &&
        std::ranges::equal(controller.candidate(0),lz4_literal_fixture(expected_first)) &&
        std::ranges::equal(controller.candidate(1),lz4_literal_fixture(expected_second)) &&
        controller.candidate(2).empty(),"compression block order or independent owned source changed");
    eventually([&]{return controller.try_retire();},2000,"two-chunk compression did not retire");
  }
  {
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_lz4();
    const Bytes original(input.plaintext().begin(),input.plaintext().end());
    const auto disabled=controller.begin(std::move(input),missing,{L"lz4-ok"},false,true,health,100);
    check(!disabled.submitted&&disabled.route.reason==GpuRouteReason::disabled&&!controller.process_id(),
        "disabled compression request launched a process");
    same_source(controller,original);
    auto next=coordinated_lz4(1);
    const auto denied=controller.begin(std::move(next),missing,{L"lz4-ok"},true,false,health,100);
    check(!denied.submitted&&denied.route.reason==GpuRouteReason::unqualified&&controller.consecutive_failures()==0,
        "unqualified compression work changed recovery history");
  }
  {
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_lz4(0,true);
    const Bytes original(input.plaintext().begin(),input.plaintext().end());
    check(controller.begin(std::move(input),executable,{L"lz4-ok"},true,true,health,100).submitted,
        "qualified compression request did not start");
    auto overlapping=coordinated_lz4(1);
    const auto size=overlapping.plaintext().size();
    check(!controller.begin(std::move(overlapping),executable,{L"lz4-ok"},true,true,health,100).submitted&&
        overlapping.plaintext().size()==size,"overlapping compression attempt consumed caller input");
    eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"compression controller success did not finish");
    check(controller.state()==GpuWorkerState::worker_selected&&controller.last_gate_code()==GpuGateCode::accepted&&
        !controller.candidate(0).empty(),"compression controller accepted no blocks");
    const auto selected=controller.candidate(0).size();
    (void)controller.use_cpu_fallback();
    check(controller.state()==GpuWorkerState::worker_selected&&controller.candidate(0).size()==selected,
        "CPU selection overwrote a terminal compression result");
    same_source(controller,original);
    eventually([&]{return controller.try_retire();},2000,"successful compression controller did not retire");

  }
  for(const auto* mode:{L"missing",L"lz4-corrupt",L"lz4-delay-invalid"}) {
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_lz4();
    const auto helper=std::wstring_view(mode)==L"missing"?missing:executable;
    check(controller.begin(std::move(input),helper,{mode},true,true,health,100).submitted,"compression fault request not submitted");
    (void)controller.use_cpu_fallback();
    auto later=coordinated_lz4(1);
    const Bytes later_source(later.plaintext().begin(),later.plaintext().end());
    const auto fresh=coordinated_health(2,102);
    const auto busy=controller.begin(std::move(later),executable,{L"lz4-ok"},true,true,fresh,102);
    check(!busy.submitted&&(busy.route.reason==GpuRouteReason::worker_busy||
        busy.route.reason==GpuRouteReason::configuration_blocked),"compression retry bypassed pending fault/retirement");
    same_source(controller,later_source);
    eventually([&]{return controller.try_retire();},3000,"late compression fault did not retire");
    check(controller.consecutive_failures()==1&&controller.state()==GpuWorkerState::cpu_selected,
        "late compression fault was forgotten or counted twice");
    auto retry=coordinated_lz4(2);
    const auto denied=controller.begin(std::move(retry),executable,{L"lz4-ok"},true,true,coordinated_health(3,103),103);
    check(!denied.submitted&&denied.route.reason==GpuRouteReason::configuration_blocked,
        "unchanged corrupt/unavailable compression configuration retried");
  }
  {
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_lz4();
    check(controller.begin(std::move(input),executable,{L"lz4-hold"},true,true,health,100).submitted,"held compression start");
    eventually([&]{return controller.process_id()!=0;},2000,"held compression child did not start");
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,controller.process_id());
    check(child!=nullptr,"held compression exact child observation");
    struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{child};
    const auto cpu=controller.use_cpu_fallback();
    check(!cpu.empty()&&WaitForSingleObject(child,0)==WAIT_TIMEOUT&&controller.consecutive_failures()==0,
        "compression CPU selection waited for retirement or invented a failure");
    auto other=coordinated_lz4(3);const Bytes expected(other.plaintext().begin(),other.plaintext().end());
    const auto busy=controller.begin(std::move(other),executable,{L"lz4-ok"},true,true,coordinated_health(2,102),102);
    check(!busy.submitted&&busy.route.reason==GpuRouteReason::worker_busy,"new compression CPU request launched during retirement");
    same_source(controller,expected);
    check(controller.request_cancel(),"held compression cancellation failed");
    eventually([&]{return controller.try_retire();},3000,"held compression controller did not retire");
    check(controller.consecutive_failures()==1,"compression cancellation outcome missing");
    for(unsigned i=0;i<2;++i) {
      auto recovered=coordinated_lz4();
      const auto e=coordinated_health(3+i,103+i);
      check(controller.begin(std::move(recovered),executable,{L"lz4-ok"},true,true,e,103+i).submitted,
          "fresh healthy evidence after compression pressure did not allow one attempt");
      eventually([&]{return controller.poll(coordinated_health(4+i,104+i).memory,104+i)!=GpuWorkerState::waiting;},
          3000,"recovered compression did not finish");
      check(controller.state()==GpuWorkerState::worker_selected&&
          controller.consecutive_failures()==(i?0U:1U),"compression flapping history cleared prematurely");
      eventually([&]{return controller.try_retire();},2000,"recovered compression controller did not retire");
    }
  }
  // These use a CPU helper and synthetic telemetry while exercising HIP policy.
  for(const auto* trigger:{L"cancel",L"timeout",L"expire"}) {
    GpuLz4WorkerController controller(WorkerExecution::hip);
    auto input=coordinated_lz4();
    ProcessBudget budget;budget.timeout_ms=std::wstring_view(trigger)==L"timeout"?250:5000;
    check(controller.begin(std::move(input),executable,{L"lz4-hold"},true,true,health,100,budget).submitted,
        "HIP-policy compression fixture not submitted");
    eventually([&]{return controller.process_id()!=0;},2000,"HIP-policy compression child did not start");
    if(std::wstring_view(trigger)==L"cancel")check(controller.request_cancel(),"HIP-policy compression cancel");
    if(std::wstring_view(trigger)==L"expire")
      check(controller.poll(coordinated_health(2,200).memory,200)==GpuWorkerState::cpu_selected&&
          controller.last_gate_code()==GpuGateCode::expired,"compression gate expiry did not select CPU");
    else eventually([&]{return controller.poll(after,101)!=GpuWorkerState::waiting;},3000,"HIP-policy compression result not observed");
    eventually([&]{return controller.try_retire();},3000,"HIP-policy compression retirement missing");
    check(controller.consecutive_failures()==1&&controller.candidate(0).empty(),"HIP compression failure outcome");
    for(unsigned i=0;i<3;++i) {
      auto retry=coordinated_lz4();
      const auto denied=controller.begin(std::move(retry),executable,{L"lz4-hip-declaration"},true,true,
          coordinated_health(3+i,300+i),300+i);
      check(!denied.submitted&&denied.route.reason==GpuRouteReason::device_quarantined,
          "compression telemetry/retirement cleared a device fault");
    }
    auto recovered=coordinated_lz4();
    auto qualified=coordinated_health(6,400);qualified.recovery_generation=1;
    check(controller.begin(std::move(recovered),executable,{L"lz4-hip-declaration"},true,true,qualified,400).submitted,
        "separately qualified compression recovery did not admit work");
    auto valid_after=coordinated_health(7,401).memory;
    eventually([&]{return controller.poll(valid_after,401)!=GpuWorkerState::waiting;},3000,"qualified compression recovery did not finish");
    check(controller.state()==GpuWorkerState::worker_selected,"qualified compression recovery failed verification");
    eventually([&]{return controller.try_retire();},2000,"qualified recovered compression did not retire");
  }
  for(unsigned fault=0;fault<4;++fault) {
    GpuLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=coordinated_lz4();
    check(controller.begin(std::move(input),executable,{L"lz4-ok"},true,true,health,100).submitted,"compression telemetry fixture");
    auto bad=after;
    if(fault==0)bad.captured_ms=99;
    if(fault==1)bad.luid[0]=std::byte{2};
    if(fault==2)bad.nonlocal_usage+=29ULL<<20;
    if(fault==3)bad.local_budget=bad.local_usage;
    eventually([&]{return controller.poll(bad,101)!=GpuWorkerState::waiting;},3000,"invalid compression telemetry not observed");
    check(controller.state()==GpuWorkerState::cpu_selected&&controller.candidate(0).empty(),
        "compression telemetry failure exposed blocks");
    eventually([&]{return controller.try_retire();},2000,"invalid-telemetry compression did not retire");
  }
  check(WorkerExchangeSession::available(),"compression controller leaked worker admission");
  std::cout<<"LZ4 controller: terminal CPU selection, late reply quarantine and exact retirement passed; gpu_executed=false\n";
}
void match_transactions(const std::filesystem::path& executable) {
  using Progress=NativeFileSession::RecompressionProgress;
  for(const auto* mode:{L"match",L"match-invalid",L"match-hold"}) {
    auto head=std::make_shared<native_fixture::Ram>();
    auto source=std::make_shared<native_fixture::Ram>();
    auto target=std::make_shared<native_fixture::Ram>();
    auto volume=native_fixture::identity(),original=volume,next=volume;
    original[0]=std::byte{31};next[0]=std::byte{32};
    auto native=NativeFileSession::create_volume(head,volume,source,original,{StoreCompressionMode::raw,6,false});
    native.create_entry("data");
    auto handle=native.open("data",{true,true,false});
    auto bytes=native_fixture::data(8193,43);
    const auto prefix=bytes;bytes.insert(bytes.end(),prefix.begin(),prefix.end());
    native.write(handle,0,bytes);
    native.begin_recompression(target,next);
    check(native.snapshot().storage_info().objects==2&&native.advance_recompression()==Progress::copying,
        "match channel fixture object layout");
    auto work=native.next_recompression_input();
    auto extent=bytes;extent.resize(kNativeExtentBytes);
    const auto prepared=PreparedGpuMatchSearch::prepare(extent);
    check(bool(work)==flitzfs::detail::store_match_sequences_available()&&(!work||work->bytes==extent),
        "match channel staged source differs");
    const auto request=encode_match_worker_request(prepared,813);
    if(std::wstring_view(mode)==L"match-hold") {
      auto pending=WorkerExchangeSession::start(executable,{mode},request);
      eventually([&]{return pending.process_id()!=0||pending.try_result();},2000,"match worker did not start");
      HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,pending.process_id());
      check(child!=nullptr,"match fixture live worker handle");
      struct Close {HANDLE value;~Close(){CloseHandle(value);}} close{child};
      check(WaitForSingleObject(child,0)==WAIT_TIMEOUT&&!pending.try_result(),"match worker exited before CPU selection");
      check(native.advance_recompression()==Progress::ready,"CPU fallback did not finish its object");
      native.publish_recompression();
      check(native.read(handle,0,65536)==bytes&&WaitForSingleObject(child,0)==WAIT_TIMEOUT,
          "CPU fallback waited on match worker retirement");
      check(pending.request_cancel(),"match worker cancellation signal");
      eventually([&]{return pending.try_result()!=nullptr;},2500,"match worker cancellation did not complete");
      eventually([&]{return pending.retirement_confirmed()&&WorkerExchangeSession::available();},
          2500,"match worker supervision did not retire");
      check(pending.try_result()->process.exit_confirmed&&pending.try_result()->response.empty(),
          "cancelled match worker exposed a response");
      check_closed(*pending.try_result());
    } else {
      const auto exchanged=run_worker_exchange(executable,{mode},request);
      check(exchanged.code==WorkerExchangeCode::completed,"match CPU channel exchange failed");
      check_closed(exchanged);
      if(std::wstring_view(mode)==L"match-invalid") {
        rejects([&]{(void)accept_match_worker_reply(exchanged.response,prepared,813,WorkerExecution::cpu_semantics);});
        check(native.advance_recompression()==Progress::ready,"invalid match result blocked CPU fallback");
      } else {
        const auto proposals=accept_match_worker_reply(exchanged.response,prepared,813,WorkerExecution::cpu_semantics);
        check(!proposals.empty(),"worker match proposals empty");
        const auto progress=work?native.advance_recompression(work->object_id,proposals):native.advance_recompression();
        check(progress==Progress::ready,"verified worker result did not reach staged encoding");
      }
      native.publish_recompression();
      check(native.read(handle,0,65536)==bytes,"match channel changed selected native bytes");
    }
    native.cleanup(handle,false);native.close(handle);
    auto selected=NativeVolumeHeadView::open(head,volume,[&](const StoreIdentity& id)->std::shared_ptr<StoreByteIo>{
      check(id==next,"match channel selected wrong image");return target;
    });
    check(selected.filesystem().read("data",0,65536)==bytes&&native.stats().contexts==0,
        "match channel CPU reopen or context retirement failed");
  }
}
void uniform_transactions(const std::filesystem::path& executable){
  const auto limits=uniform_fixture_limits();Bytes source(static_cast<std::size_t>(limits.input_bytes));
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::byte>((i*17+i/101)%256);
  const auto prepared=PreparedUniformLz4::prepare(source,limits);
  const auto request=encode_uniform_lz4_request(prepared,941);
  WorkerChannelCapacity capacity{static_cast<std::size_t>(limits.request_bytes),static_cast<std::size_t>(limits.reply_bytes)};
  for(const auto* mode:{L"uniform-ok",L"uniform-corrupt",L"uniform-fail-after"}){
    auto session=WorkerExchangeSession::start_sized(executable,{mode},request,capacity,{});
    eventually([&]{return session.try_result()!=nullptr;},5000,"uniform channel did not finish");
    const auto* result=session.try_result();
    check(result && !session.failure() && result->process.exit_confirmed,"uniform channel child retirement");
    if(!gpu_lz4_cpu_verification_available() || std::wstring_view(mode)==L"uniform-fail-after")
      check(result->code==WorkerExchangeCode::worker_failed && result->response.empty(),"failed uniform child exposed reply");
    else if(std::wstring_view(mode)==L"uniform-corrupt"){
      check(result->code==WorkerExchangeCode::completed,"corrupt uniform child transport");
      rejects([&]{(void)accept_uniform_lz4_reply(result->response,prepared,941,WorkerExecution::cpu_semantics);});
    }else{
      check(result->code==WorkerExchangeCode::completed,"uniform channel transport failed");
      const auto accepted=accept_uniform_lz4_reply(result->response,prepared,941,WorkerExecution::cpu_semantics);
      verify_uniform_lz4_candidates(prepared,accepted);
      check(accepted.sizes.size()==17 && std::equal(source.begin(),source.end(),prepared.plaintext().begin()),
          "uniform real channel changed source or dropped chunks");
    }
    eventually([&]{return session.retirement_confirmed() && WorkerExchangeSession::available();},
        2000,"uniform supervisor failed retirement");check_closed(*result);
  }
  std::cout<<"Uniform channel: input="<<source.size()<<" chunks="<<prepared.layout().chunks
      <<" CPU_decoder="<<gpu_lz4_cpu_verification_available()<<" gpu_executed=false\n";
}
PreparedUniformLz4 uniform_owned(unsigned fill=0){
  const auto limits=uniform_fixture_limits();Bytes source(static_cast<std::size_t>(limits.input_bytes),static_cast<std::byte>(fill));
  return PreparedUniformLz4::prepare(source,limits);
}
GpuLz4ResidentCost uniform_cost(const GpuHealthEvidence& e){
  GpuLz4ResidentCost c;c.fixed_bytes=256ULL<<20;c.input_copies=6;c.slab_copies=2;
  c.output_copies=3;c.per_chunk_bytes=64;c.configuration=e.configuration;c.recovery_generation=e.recovery_generation;
  c.profiled_chunk_bytes=65536;c.maximum_grid_x=65535;c.kernel_ns_per_chunk=1000000;
  c.qualified_kernel_quantum_ns=200000000;c.qualified=true;return c;
}
void coordinated_uniform_requests(const std::filesystem::path& executable){
  const auto e=coordinated_health(1,100);const auto cost=uniform_cost(e);
  const GpuLz4WindowPolicy window{16,100000000};ProcessBudget budget;
  const auto begin=[&](UniformLz4WorkerController& controller,PreparedUniformLz4&& input,
      const wchar_t* mode,const GpuHealthEvidence& health,const GpuLz4ResidentCost& profile){
    return controller.begin(std::move(input),executable,{mode},true,true,health,health.memory.captured_ms,
        profile,window,budget,3000);
  };
  if(!gpu_lz4_cpu_verification_available()){
    UniformLz4WorkerController controller;auto input=uniform_owned(7);
    const auto size=input.plaintext().size();const auto r=begin(controller,std::move(input),L"uniform-ok",e,cost);
    check(r.source_adopted && !r.decision.submitted && r.decision.route.reason==GpuRouteReason::unqualified &&
        !controller.process_id() && controller.source().size()==size,"raw uniform controller touched worker/lost source");
    return;
  }
  {
    UniformLz4WorkerController controller(WorkerExecution::cpu_semantics);
    auto input=uniform_owned(7);const auto r=begin(controller,std::move(input),L"uniform-ok",e,cost);
    check(r.source_adopted && r.decision.submitted && !input.ready(),"uniform controller source ownership");
    auto overlap=uniform_owned(8);
    const auto busy=begin(controller,std::move(overlap),L"uniform-ok",e,cost);
    check(!busy.source_adopted && !busy.decision.submitted && overlap.ready(),"uniform overlap consumed caller source");
    eventually([&]{controller.poll(e.memory,101);return controller.awaiting_fresh_observation();},
        3000,"uniform controller did not verify candidate");
    check(controller.state()==GpuWorkerState::waiting && controller.candidate(0).empty(),
        "uniform controller accepted before fresh final observation");
    controller.poll(e.memory,101);
    check(controller.awaiting_fresh_observation() && controller.candidate(0).empty(),
        "identical observation authorized uniform output");
    const auto after=coordinated_health(2,102).memory;
    controller.poll(after,102);
    check(controller.state()==GpuWorkerState::worker_selected && controller.count()==17 &&
        !controller.candidate(0).empty() && !controller.candidate(16).empty() && controller.candidate(17).empty(),
        "uniform controller lost verified block order");
    (void)controller.use_cpu_fallback();
    check(controller.state()==GpuWorkerState::worker_selected,"CPU overwrote terminal uniform result");
    eventually([&]{return controller.try_retire();},2000,"uniform success did not retire");
  }
  for(const bool expired:{false,true}){
    UniformLz4WorkerController controller(WorkerExecution::cpu_semantics);auto input=uniform_owned(8);
    check(begin(controller,std::move(input),L"uniform-ok",e,cost).decision.submitted,"uniform final-stage fixture");
    eventually([&]{controller.poll(e.memory,101);return controller.awaiting_fresh_observation();},
        3000,"uniform final-stage verification did not complete");
    const auto* retained=controller.source().data();
    if(expired)controller.poll(coordinated_health(2,3100).memory,3100);
    else (void)controller.use_cpu_fallback();
    check(controller.state()==GpuWorkerState::cpu_selected && controller.source().data()==retained &&
        controller.candidate(0).empty(),"uniform verified candidate escaped terminal CPU/deadline selection");
    controller.poll(coordinated_health(3,3101).memory,3101);
    check(controller.state()==GpuWorkerState::cpu_selected,"fresh data reopened terminal uniform CPU result");
    eventually([&]{return controller.try_retire();},2000,"uniform final-stage fixture retirement");
  }
  for(unsigned fault=0;fault<3;++fault){
    UniformLz4WorkerController controller(WorkerExecution::cpu_semantics);auto input=uniform_owned(9);
    auto changed=cost;
    if(fault==0)changed.configuration[1]=std::byte{1};
    if(fault==1){changed.fixed_bytes=8ULL<<20;changed.input_copies=1;changed.slab_copies=0;changed.output_copies=0;changed.per_chunk_bytes=0;}
    if(fault==2)changed.profiled_chunk_bytes=32768;
    const auto r=begin(controller,std::move(input),L"uniform-ok",e,changed);
    check(r.source_adopted && !r.decision.submitted && !controller.process_id() &&
        !controller.source().empty(),"unqualified/underaccounted uniform profile launched worker");
  }
  {
    UniformLz4WorkerController controller(WorkerExecution::cpu_semantics);auto input=uniform_owned(11);
    check(begin(controller,std::move(input),L"uniform-hold",e,cost).decision.submitted,"uniform held request not started");
    eventually([&]{return controller.process_id()!=0;},2000,"uniform held PID missing");
    HANDLE child=OpenProcess(SYNCHRONIZE,FALSE,controller.process_id());
    check(child!=nullptr,"uniform held exact child");struct Close{HANDLE h;~Close(){CloseHandle(h);}} close{child};
    const auto original=controller.source().data();const auto cpu=controller.use_cpu_fallback();
    check(cpu.data()==original && cpu.size()==16U*65536+37 &&
        WaitForSingleObject(child,0)==WAIT_TIMEOUT && controller.state()==GpuWorkerState::cpu_selected &&
        controller.candidate(0).empty(),"uniform CPU selection waited for cleanup or exposed a candidate");
    auto next=uniform_owned(12);const auto replacement=begin(controller,std::move(next),L"uniform-ok",e,cost);
    check(replacement.source_adopted && !replacement.decision.submitted &&
        controller.source()[0]==std::byte{12},"uniform retiring worker blocked/lost replacement CPU source");
    check(controller.request_cancel(),"uniform cancellation signal");
    eventually([&]{return controller.try_retire();},4000,"uniform held supervisor did not retire");
    check(controller.source()[0]==std::byte{12},"retirement changed replacement CPU bytes");
  }
  {
    UniformLz4WorkerController controller(WorkerExecution::cpu_semantics);auto input=uniform_owned(13);
    check(begin(controller,std::move(input),L"uniform-delay-invalid",e,cost).decision.submitted,"uniform late-invalid start");
    (void)controller.use_cpu_fallback();auto next=uniform_owned(14);
    check(begin(controller,std::move(next),L"uniform-ok",e,cost).source_adopted,"uniform late CPU replacement");
    eventually([&]{return controller.try_retire();},4000,"uniform late invalid worker did not retire");
    auto fresh=coordinated_health(2,110);auto fresh_cost=uniform_cost(fresh);auto final=uniform_owned(15);
    const auto denied=begin(controller,std::move(final),L"uniform-ok",fresh,fresh_cost);
    check(!denied.decision.submitted && denied.decision.route.reason==GpuRouteReason::configuration_blocked &&
        controller.source()[0]==std::byte{15},"late corruption was forgotten or changed CPU bytes");
  }
  std::cout<<"Uniform controller: configuration/peak/shape admission, fresh final observation, live-helper CPU fallback, "
      "late quarantine and exact retirement passed; gpu_executed=false\n";
}
void sized_transactions(const std::filesystem::path& executable) {
  ProcessBudget budget;
  check(valid_worker_channel_capacity(sized_capacity,budget),"valid sized capacity rejected");
  for(const auto capacity:{WorkerChannelCapacity{},WorkerChannelCapacity{31,32},
      WorkerChannelCapacity{32,31},WorkerChannelCapacity{~std::size_t{0},32},
      WorkerChannelCapacity{32,~std::size_t{0}},WorkerChannelCapacity{128U*1024*1024,32}})
    check(!valid_worker_channel_capacity(capacity,budget),"invalid sized capacity admitted");
  Bytes source(sized_capacity.request_bytes);
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::byte>((i*13+i/251)%256);
  for(const auto* mode:{L"sized-ok",L"sized-fail-after",L"sized-guard",L"sized-wrong-capacity",L"sized-legacy"}) {
    auto session=WorkerExchangeSession::start_sized(executable,{mode},source,sized_capacity,budget);
    eventually([&]{return session.try_result()!=nullptr;},5000,"sized exchange did not finish");
    const auto* result=session.try_result();
    check(result && !session.failure() && result->process.exit_confirmed &&
        !result->process.timed_out,"sized exchange supervisor failed");
    if(std::wstring_view(mode)==L"sized-ok") {
      check(result->code==WorkerExchangeCode::completed &&
          result->response.size()==sized_capacity.reply_bytes,"large sized response rejected");
      check(std::equal(result->response.begin(),result->response.end(),source.rbegin()),
          "large sized byte mismatch");
    }else check(result->code!=WorkerExchangeCode::completed && result->response.empty(),
        "failed/incompatible sized child exposed bytes");
    eventually([&]{return session.retirement_confirmed() && WorkerExchangeSession::available();},
        2000,"sized worker failed exact retirement");
    check_closed(*result);
  }
  {
    auto session=WorkerExchangeSession::start_sized(executable,{L"sized-hold"},source,sized_capacity,budget);
    eventually([&]{return session.process_id()!=0 || session.try_result();},2000,"sized child did not start");
    check(session.process_id()!=0 && !session.try_result(),"sized hold did not stay active");
    rejects([&]{(void)WorkerExchangeSession::start(executable,{L"ok"},source);});
    rejects([&]{(void)WorkerExchangeSession::start_sized(executable,{L"sized-ok"},source,sized_capacity,budget);});
    check(session.request_cancel(),"sized cancel signal failed");
    eventually([&]{return session.try_result()!=nullptr;},4000,"sized cancel failed");
    check(session.try_result()->code==WorkerExchangeCode::cancelled &&
        session.try_result()->response.empty(),"cancelled sized output escaped");
    eventually([&]{return session.retirement_confirmed() && WorkerExchangeSession::available();},
        2000,"cancelled sized supervisor retained admission");
    check_closed(*session.try_result());
  }
  rejects([&]{(void)WorkerExchangeSession::start_sized(executable,{L"sized-ok"},source,{32,32},budget);});
  check(WorkerExchangeSession::available(),"invalid sized admission held supervisor");
  std::cout<<"Sized channel: request="<<sized_capacity.request_bytes<<" reply="
      <<sized_capacity.reply_bytes<<" cpu_semantics_only=true gpu_executed=false\n";
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
  uniform_transactions(executable);observe(L"uniform-transport");
  coordinated_uniform_requests(executable);observe(L"uniform-controller");
  sized_transactions(executable);observe(L"sized-transport");
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
  {
    const std::array expected{GpuRecoveryOutcome::integrity_failure,
        GpuRecoveryOutcome::runtime_unavailable,GpuRecoveryOutcome::unsupported_configuration,
        GpuRecoveryOutcome::temporary_pressure,GpuRecoveryOutcome::device_fault,
        GpuRecoveryOutcome::integrity_failure};
    for(unsigned category=1;category<=expected.size();++category) {
      const auto mode=L"staged-exit-"+std::to_wstring(category);
      auto session=WorkerExchangeSession::start(executable,{mode},request);
      eventually([&]{return session.try_result()!=nullptr;},3000,"staged failure did not complete");
      const auto* result=session.try_result();
      check(result && result->code==WorkerExchangeCode::worker_failed &&
          result->process.exit_confirmed && result->response.empty() && !result->publication_observed &&
          result->process.exit_code==hip_worker_diagnostic_exit(
              static_cast<HipWorkerExit>(0x46530000U+category),HipWorkerStage::device_properties),
          "staged exit lost transport identity or exposed candidate bytes");
      check(worker_transport_failure(session,*result,WorkerExecution::hip)==expected[category-1],
          "staged exit changed trusted HIP recovery classification");
      check(worker_transport_failure(session,*result,WorkerExecution::cpu_semantics)==GpuRecoveryOutcome::device_fault,
          "CPU fixture gained trusted HIP exit interpretation");
      auto unconfirmed=*result;unconfirmed.process.exit_confirmed=false;
      check(worker_transport_failure(session,unconfirmed,WorkerExecution::hip)==GpuRecoveryOutcome::device_fault,
          "unconfirmed exit supplied recovery evidence");
      for(unsigned stage=16;stage<=31;++stage) {
        auto named=*result;
        named.process.exit_code=hip_worker_diagnostic_exit(
            static_cast<HipWorkerExit>(0x46530000U+category),static_cast<HipWorkerStage>(stage));
        check(worker_transport_failure(session,named,WorkerExecution::hip)==expected[category-1],
            "new named stage changed recovery category");
        check(worker_transport_failure(session,named,WorkerExecution::cpu_semantics)==GpuRecoveryOutcome::device_fault,
            "CPU fixture gained new-stage HIP trust");
        named.process.exit_confirmed=false;
        check(worker_transport_failure(session,named,WorkerExecution::hip)==GpuRecoveryOutcome::device_fault,
            "unconfirmed named exit supplied recovery evidence");
      }
      auto unknown=*result;unknown.process.exit_code=0x46532002U;
      check(worker_transport_failure(session,unknown,WorkerExecution::hip)==GpuRecoveryOutcome::device_fault,
          "unknown stage downgraded recovery requirements");
      eventually([&]{return session.retirement_confirmed() && WorkerExchangeSession::available();},
          2000,"staged failure supervisor did not retire");
      check_closed(*result);
    }
  }
  observe(L"staged-failures");
  {
    const HipIdentityRequest expected{71,3,1,{std::byte{5}}};
    const auto request_frame=encode_hip_identity_request(expected);
    const auto result=run_worker_exchange(executable,{L"identity-metadata"},request_frame);
    check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed &&
        !result.process.timed_out,"identity metadata child did not complete");
    const auto observed=decode_hip_identity_reply(result.response,expected);
    check(observed.node_mask==0 && observed.luid==expected.luid,
        "identity metadata transport lost an unreported node mask");
    rejects([&]{(void)accept_worker_reply(result.response,batch,221,WorkerExecution::hip);});
    check_closed(result);
    auto malformed=request_frame;malformed[12]=std::byte{1};
    const auto refused=run_worker_exchange(executable,{L"identity-metadata"},malformed);
    check(refused.code==WorkerExchangeCode::worker_failed && refused.process.exit_confirmed &&
        refused.response.empty(),"failed identity metadata child exposed bytes");
    check_closed(refused);
  }
  observe(L"identity-metadata");

  {
    const HipMemoryBinding expected{221,3,1,{std::byte{5}},batch.device_buffer_bytes(),batch.cpu_reference().size()};
    const auto result=run_worker_exchange(executable,{L"memory-metadata"},request);
    check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed &&
        !result.process.timed_out,"memory metadata fixture did not complete");
    const auto value=decode_hip_memory_observation(result.response,expected);
    check(value.failure_stage==31 && value.snapshots[2].nonlocal_usage==4096 &&
        value.snapshots[2].nonlocal_budget==(1ULL<<28),"memory metadata fixture lost counters");
    rejects([&]{(void)accept_worker_reply(result.response,batch,221,WorkerExecution::hip);});
    check_closed(result);
    const auto corrupt=run_worker_exchange(executable,{L"memory-corrupt"},request);
    check(corrupt.code==WorkerExchangeCode::completed && corrupt.process.exit_confirmed,
        "malformed memory fixture transport did not complete");
    rejects([&]{(void)decode_hip_memory_observation(corrupt.response,expected);});
    check_closed(corrupt);
    const auto failed=run_worker_exchange(executable,{L"memory-fail-after"},request);
    check(failed.code==WorkerExchangeCode::worker_failed && failed.process.exit_confirmed &&
        failed.response.empty(),"failed memory fixture exposed a published observation");
    check_closed(failed);
    rejects([&]{(void)run_worker_exchange(executable,{L"memory-metadata"},std::span(request).first(12));});
    check(WorkerExchangeSession::available(),"invalid memory fixture held supervisor admission");
    auto malformed=request;malformed[0]^=std::byte{1}; // Full-size transport, invalid child protocol.
    const auto refused=run_worker_exchange(executable,{L"memory-metadata"},malformed);
    check(refused.code==WorkerExchangeCode::worker_failed && refused.process.exit_confirmed &&
        refused.response.empty(),"malformed memory fixture request exposed metadata");
    check_closed(refused);
  }
  observe(L"memory-metadata");

  {
    const auto search=PreparedGpuMatchSearch::prepare(Bytes(4096,std::byte{23}));
    const auto frame=encode_match_worker_request(search,73);
    HipProfileBinding expected{73,3,1,4096,search.lanes(),{std::byte{5}},{}};
    std::copy_n(frame.begin()+32,32,expected.source_digest.begin());
    for(const auto* mode:{L"match-profile",L"match-profile-corrupt",L"match-profile-fail"}) {
      const auto result=run_worker_exchange(executable,{mode},frame);
      check(result.process.exit_confirmed&&!result.process.timed_out,"Profile fixture retirement");
      if(std::wstring_view(mode)==L"match-profile-fail") {
        check(result.code==WorkerExchangeCode::worker_failed && result.response.empty(),
            "Failed profile child exposed metadata");
      } else {
        check(result.code==WorkerExchangeCode::completed,"Profile fixture transport failed");
        if(std::wstring_view(mode)==L"match-profile-corrupt")
          rejects([&]{(void)decode_hip_phase_profile(result.response,expected,WorkerExecution::cpu_semantics);});
        else {
          const auto observed=decode_hip_phase_profile(result.response,expected,WorkerExecution::cpu_semantics);
          check(observed.trace.finished && observed.trace.completed==hip_phase_complete,"Incomplete CPU phase trace");
          rejects([&]{(void)decode_hip_phase_profile(result.response,expected,WorkerExecution::hip);});
          rejects([&]{(void)accept_match_worker_reply(result.response,search,73,WorkerExecution::cpu_semantics);});
        }
      }
      check_closed(result);
    }
  }
  lz4_worker_transactions(executable);
  coordinated_lz4_requests(executable);
  observe(L"match-profile-metadata");
  coordinated_requests(executable);observe(L"coordinated-recovery");
  match_transactions(executable);observe(L"match-transactions");
  coordinated_match_requests(executable);observe(L"match-recovery");
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
