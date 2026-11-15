// Disposable hosted CPU callback qualification. No filesystem/dispatcher creation.
#include "winfsp_native.hpp"
#include "locked_ram_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
namespace {
using namespace flitzfs;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void eventually(F&& done,const char* why){
  const auto start=GetTickCount64();while(!done()){if(GetTickCount64()-start>6000)throw std::runtime_error(why);Sleep(1);}
}
struct Collector {
  std::mutex mutex;std::array<std::array<std::byte,sizeof(FSP_FSCTL_TRANSACT_RSP)>,16> replies{};unsigned count=0;bool overflow=false;
};
Collector* receiver=nullptr;std::atomic<UINT64> hints{71};
bool hint(UINT64& value) noexcept{value=hints.fetch_add(1);return true;}
void send(FSP_FILE_SYSTEM*,FSP_FSCTL_TRANSACT_RSP& response) noexcept{
  std::lock_guard guard(receiver->mutex);
  if(receiver->count==receiver->replies.size()){receiver->overflow=true;return;}
  std::memcpy(receiver->replies[receiver->count++].data(),&response,sizeof(response));
}
GpuHealthEvidence health(){
  GpuHealthEvidence e;const auto now=GetTickCount64();
  e.configuration[0]=std::byte{1};e.sequence=now+1;e.compatible=e.supervisor_quiescent=true;
  auto& m=e.memory;m.luid[0]=std::byte{1};m.node_mask=1;m.captured_ms=now;m.valid=m.amd_hardware=true;
  m.dedicated_bytes=m.local_budget=1ULL<<30;m.local_usage=16ULL<<20;m.nonlocal_budget=8ULL<<30;
  m.physical_total=16ULL<<30;m.physical_available=8ULL<<30;m.commit_limit=32ULL<<30;m.commit_available=16ULL<<30;
  return e; // Synthetic policy evidence; real transport repeats actual OS admission.
}
NativeDeferredWriteOptions options(const std::filesystem::path& worker,unsigned kind){
  NativeDeferredWriteOptions o;o.expected=WorkerExecution::cpu_semantics;
  o.hint_query=hint;o.response_sink=send;o.budget={128,2000,3000};o.deadline_ms=2500;
  o.allow_gpu=kind!=0;o.workload_qualified=true;o.helper_independent=true;
  const auto e=health();o.binding.configuration=e.configuration;o.binding.helper=worker;
  o.binding.cpu_fixture_arguments={kind==2?L"uniform-native16-sweep-hold":L"uniform-native16-sweep"};
  uniform_lz4::Layout quantum{};check(uniform_lz4::make_layout(2U*65536,65536,quantum),"callback quantum");
  o.binding.sweep=NativeUniformSweepGrant{16,quantum.total_bytes};
  auto& c=o.cost;c.fixed_bytes=256ULL<<20;c.input_copies=6;c.slab_copies=2;c.output_copies=3;c.per_chunk_bytes=64;
  c.configuration=e.configuration;c.profiled_chunk_bytes=65536;c.maximum_grid_x=65535;
  c.kernel_ns_per_chunk=1000000;c.qualified_kernel_quantum_ns=200000000;c.qualified=true;
  o.window={16,100000000};return o;
}
void transport_required(){
  auto ram=std::make_shared<native_fixture::Ram>();
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(
      ram,native_fixture::identity(),{StoreCompressionMode::raw,6,false}));
  const auto seed=native_fixture::file_create("target");
  const NativeBasicMutation basic{0,1,{11,22,33,44,2}};
  const NativeSecurityMutation security{0,1,native_security_fixture::descriptor()};
  writer.apply_with_metadata({&seed,1},{&basic,1},{&security,1});
  auto session=NativeFileSession::create(std::move(writer));
  std::mutex control;std::atomic<bool> offline{false};
  const auto reads=ram->read_calls,writes=ram->calls;
  for(unsigned mask=0;mask<3;++mask){
    NativeDeferredWriteOptions options;options.expected=WorkerExecution::cpu_semantics;
    if(mask&1)options.hint_query=hint;
    if(mask&2)options.response_sink=send;
    bool rejected=false;
    try{NativeDeferredWriteService forbidden(session,control,offline,std::move(options));}
    catch(const std::invalid_argument&){rejected=true;}
    check(rejected&&!offline.load()&&ram->read_calls==reads&&ram->calls==writes,
        "missing deferred transport was accepted or touched storage");
  }
}
struct Harness {
  locked_ram_fixture::WorkingSet working;
  std::shared_ptr<StoreByteIo> head=std::make_shared<locked_ram_fixture::Store>(kVolumeHeadBytes);
  std::shared_ptr<StoreByteIo> image=std::make_shared<locked_ram_fixture::Store>(4U*1024*1024);
  Collector collector;FSP_FILE_SYSTEM fs{};std::unique_ptr<WinFspNativeWritable> adapter;
  std::vector<std::byte> security;
  explicit Harness(const NativeDeferredWriteOptions& options){
    StoreIdentity volume{},id{};volume[0]=std::byte{5};id[0]=std::byte{6};
    adapter=WinFspNativeWritable::create_managed_hosted(head,volume,image,id,{StoreCompressionMode::fast,6,false});
    receiver=&collector;adapter->enable_deferred_writes_hosted(options);
    adapter->publish_deferred_health_hosted(health());
    fs.Version=static_cast<UINT16>(sizeof(fs));fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();adapter->bind_callbacks(fs,[]()->UINT32{return 0;});
    SIZE_T bytes=0;UINT32 attrs=0;
    check(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attrs,nullptr,&bytes)==STATUS_BUFFER_OVERFLOW,
        "deferred root descriptor size");
    security.resize(bytes);
    check(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attrs,security.data(),&bytes)==STATUS_SUCCESS,
        "deferred root descriptor");
  }
  ~Harness(){
    if(adapter){
      while(adapter->stop_deferred_writes_hosted()==NativeDeferredStop::running)Sleep(1);
      adapter.reset();
    }
    receiver=nullptr;
  }
  PVOID create(PCWSTR path){
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
    check(fs.Interface->Create(&fs,const_cast<PWSTR>(path),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,
        FILE_ATTRIBUTE_NORMAL,security.data(),0,&context,&info)==STATUS_SUCCESS&&context,
        "deferred callback create");return context;
  }
  FSP_FSCTL_TRANSACT_RSP reply(unsigned index){
    FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
    eventually([&]{
      adapter->publish_deferred_health_hosted(health());
      std::lock_guard guard(collector.mutex);
      if(collector.count<=index)return false;std::memcpy(&response,collector.replies[index].data(),sizeof(response));return true;
    },"deferred callback completion absent");
    return response;
  }
  void close(PVOID context){
    FSP_FSCTL_TRANSACT_REQ request;std::memset(&request,0,sizeof(request));FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
    request.Kind=FspFsctlTransactCleanupKind;
    request.Req.Cleanup.UserContext2=reinterpret_cast<std::uintptr_t>(context);
    check(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS,"deferred SDK cleanup");
    std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));request.Kind=FspFsctlTransactCloseKind;
    request.Req.Close.UserContext2=reinterpret_cast<std::uintptr_t>(context);
    check(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS,"deferred SDK close");
  }
};
void run(const std::filesystem::path& worker,unsigned kind){
  Harness h(options(worker,kind));const auto target=h.create(L"\\target");
  const auto prior=h.adapter->snapshot();
  const auto block=native_lz4_fixture::source();std::vector<std::byte> bytes;
  for(unsigned i=0;i<16;++i)bytes.insert(bytes.end(),block.begin(),block.end());
  const auto expected=bytes;ULONG count=999;FSP_FSCTL_FILE_INFO info{};info.FileSize=999;
  h.adapter->publish_deferred_health_hosted(health());
  const auto expected_hint=hints.load();
  check(h.fs.Interface->Write(&h.fs,target,bytes.data(),0,static_cast<ULONG>(bytes.size()),FALSE,FALSE,
      &count,&info)==STATUS_PENDING && count==0 && info.FileSize==0,"callback did not own/defer write");
  std::fill(bytes.begin(),bytes.end(),std::byte{0xff});
  if(kind==2 && gpu_lz4_cpu_verification_available()){
    eventually([&]{h.adapter->publish_deferred_health_hosted(health());
      return h.adapter->deferred_write_stats().worker_submitted==1;},"held callback child not submitted");
    // A subsequent read forces CPU publication without waiting for the held child.
    std::vector<std::byte> actual(expected.size());ULONG got=0;
    check(h.fs.Interface->Read(&h.fs,target,actual.data(),0,static_cast<ULONG>(actual.size()),&got)==STATUS_SUCCESS &&
        got==expected.size()&&actual==expected,"read did not resolve pending CPU bytes");
  }
  if(kind==3){
    std::array<NativeDeferredStop,2> states{NativeDeferredStop::running,NativeDeferredStop::running};
    std::atomic<bool> ok{true};
    auto stop=[&](unsigned index) noexcept{
      try{eventually([&]{states[index]=h.adapter->stop_deferred_writes_hosted();
        return states[index]!=NativeDeferredStop::running;},"queued shutdown did not retire");}
      catch(...){ok.store(false);}
    };
    // Both callers may observe the signaled OS thread. Joining must stay
    // serialized even after native pending publication has completed.
    std::jthread first(stop,0),second(stop,1);first.join();second.join();
    check(ok.load()&&states[0]==NativeDeferredStop::quiescent&&states[1]==NativeDeferredStop::quiescent,
        "concurrent queued shutdown retirement unconfirmed");
  }
  const auto response=h.reply(0);
  check(response.Kind==FspFsctlTransactWriteKind && response.Hint==expected_hint &&
      response.IoStatus.Status==STATUS_SUCCESS && response.IoStatus.Information==expected.size() &&
      response.Rsp.Write.FileInfo.FileSize==expected.size(),"deferred response lost committed facts");
  check(h.adapter->snapshot().read("target",0,static_cast<std::uint32_t>(expected.size()))==expected &&
      prior.read("target",0,1).empty(),"deferred CPU current/prior readback");
  h.close(target);
  NativeDeferredStop stopped;
  eventually([&]{stopped=h.adapter->stop_deferred_writes_hosted();return stopped!=NativeDeferredStop::running;},
      "deferred controller did not stop");
  const auto stats=h.adapter->deferred_write_stats();
  check(stopped==NativeDeferredStop::quiescent && stats.thread_exited && stats.retirement_confirmed &&
      !stats.native_pending && !stats.worker_busy && stats.queued==1 && stats.completed==1 &&
      stats.response_attempts==1 && !h.collector.overflow && h.adapter->stats().contexts==0,
      "deferred shutdown/one-response ownership");
  if(kind==1 && gpu_lz4_cpu_verification_available())check(stats.advised==1&&stats.worker_submitted==1,
      "deferred controller omitted verified sweep advice");
  if(kind==2 && gpu_lz4_cpu_verification_available())check(stats.cpu_forced==1,"held write did not select CPU");
  check(!h.adapter->requires_offline()&&WorkerExchangeSession::available(),"deferred controller left failed state");
}
}
int wmain(int argc,wchar_t** argv){try{
  check(argc==2,"trusted CPU helper required");const std::filesystem::path worker(argv[1]);
  check(worker.is_absolute(),"absolute CPU helper required");
  transport_required();
  for(unsigned kind=0;kind<4;++kind){
    run(worker,kind);
    check(locked_ram_fixture::locked_bytes.load()==0 && locked_ram_fixture::cleanup_ok.load(),
        "deferred fixture locked/working-set leak");
  }
  std::cout<<"Deferred native callbacks: checks="<<checks
      <<" owned_input=true exact_response=true CPU_fallback=true controller_joined=true"
      <<" gpu_executed=false mounted=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
