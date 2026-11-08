#include "worker_exchange.hpp"
#include "flzip_native_fixture.hpp"
#include "gpu/decode_kernel.hpp"
#include <windows.h>
#include <aclapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace flitzfs;
using namespace flzip_fixture;
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
}
int wmain(int argc,wchar_t** argv){try{
  if(argc==4 && std::wstring_view(argv[2])==L"--worker-channel")return child(argv[1],argv[3]);
  check(argc==1,"unexpected worker test arguments");
  const auto executable=self();const auto batch=mixed();const auto request=encode_worker_request(batch,221);
  const auto warmup=run_worker_exchange(executable,{L"ok"},request);
  check(warmup.code==WorkerExchangeCode::completed,"worker warmup exchange");
  DWORD initial_handles=0;check(GetProcessHandleCount(GetCurrentProcess(),&initial_handles),"initial handle count");
  for(const auto* mode:{L"ok",L"security",L"double"}) {
    auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed && !result.process.timed_out,"valid exchange failed");
    auto reply=accept_worker_reply(result.response,batch,221,WorkerExecution::cpu_semantics);
    check(reply.bytes==Bytes(batch.cpu_reference().begin(),batch.cpu_reference().end()),"cross-process byte parity");
    check(result.process.peak_committed_bytes<=128ULL*1024*1024,"worker memory cap exceeded");
  }
  for(const auto* mode:{L"bad-wire",L"wrong-id",L"wrong-backend",L"bad-status",L"bad-bytes",L"trailing"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::completed,"malformed reply test did not reach validation");
    rejects([&]{accept_worker_reply(result.response,batch,221,WorkerExecution::cpu_semantics);});
  }
  for(const auto* mode:{L"exit-empty",L"oversize",L"tamper-input",L"tamper-guard",L"tamper-length",L"tamper-header"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::invalid_response && result.response.empty(),"invalid transport output escaped");
  }
  for(const auto* mode:{L"exit-bad",L"publish-fail"}) {
    const auto result=run_worker_exchange(executable,{mode},request);
    check(result.code==WorkerExchangeCode::worker_failed && result.response.empty(),"failed helper output escaped");
  }
  ProcessBudget quick;quick.timeout_ms=1000;
  const auto timed=run_worker_exchange(executable,{L"publish-hang"},request,quick);
  check(timed.code==WorkerExchangeCode::timed_out && timed.process.exit_confirmed && timed.response.empty(),"timed-out response escaped");
  const auto recovered=run_worker_exchange(executable,{L"ok"},request);
  check(recovered.code==WorkerExchangeCode::completed,"fresh isolated exchange after timeout failed");
  (void)accept_worker_reply(recovered.response,batch,221,WorkerExecution::cpu_semantics);
  for(const auto* token:{L"",L"..",L"Global\\name",L"0123456789abcdef0123456789abcdefff!",L"FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"})
    rejects([&]{(void)WorkerChannel::open(token);});
  rejects([&]{run_worker_exchange(executable,{},{});});
  auto oversized=request;oversized.resize(kWorkerRequestLimit+1);
  rejects([&]{run_worker_exchange(executable,{},oversized);});
  rejects([&]{run_worker_exchange(executable,std::vector<std::wstring>(15,L"arg"),request);});
  DWORD final_handles=0;check(GetProcessHandleCount(GetCurrentProcess(),&final_handles),"final handle count");
  check(final_handles<=initial_handles,"worker channel handles accumulated");
  std::cout<<"PASS: "<<checks<<" real Windows worker exchange checks; CPU semantics only, no GPU or data files.\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"worker exchange failure after "<<checks<<": "<<e.what()<<'\n';return 1;}}
