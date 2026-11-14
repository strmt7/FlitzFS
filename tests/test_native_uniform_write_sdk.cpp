#include "native_uniform_write.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0;
constexpr auto mode=NativeFileInfoAllocation::native_reserved_cache;
constexpr std::uint64_t stamp=133800000000000137ULL;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void eventually(F&& done,unsigned ms,const char* why){
  const auto start=GetTickCount64();while(!done()){if(GetTickCount64()-start>=ms)throw std::runtime_error(why);Sleep(1);}
}
NativeBasicMetadata basic(){return {11,22,33,44,2};}
GpuHealthEvidence health(std::uint64_t now){
  GpuHealthEvidence e;e.configuration[0]=std::byte{1};e.sequence=now;e.compatible=e.supervisor_quiescent=true;
  auto& m=e.memory;m.luid[0]=std::byte{1};m.node_mask=1;m.captured_ms=now;m.valid=m.amd_hardware=true;
  m.dedicated_bytes=m.local_budget=1ULL<<30;m.local_usage=16ULL<<20;m.nonlocal_budget=8ULL<<30;
  m.physical_total=16ULL<<30;m.physical_available=8ULL<<30;m.commit_limit=32ULL<<30;m.commit_available=16ULL<<30;
  return e; // Synthetic policy evidence; actual transport repeats OS admission.
}
GpuLz4ResidentCost cost(const GpuHealthEvidence& e){
  GpuLz4ResidentCost c;c.fixed_bytes=256ULL<<20;c.input_copies=6;c.slab_copies=2;c.output_copies=3;
  c.per_chunk_bytes=64;c.configuration=e.configuration;c.recovery_generation=e.recovery_generation;
  c.profiled_chunk_bytes=65536;c.maximum_grid_x=65535;c.kernel_ns_per_chunk=1000000;
  c.qualified_kernel_quantum_ns=200000000;c.qualified=true;return c;
}
void exact(const FSP_FSCTL_FILE_INFO& i,std::uint64_t bytes){
  check(i.FileAttributes==0x22 && !i.ReparseTag && i.AllocationSize==(bytes+4095)/4096*4096 && i.FileSize==bytes &&
      i.CreationTime==11 && i.LastAccessTime==stamp && i.LastWriteTime==stamp && i.ChangeTime==stamp &&
      i.IndexNumber==2 && !i.HardLinks && !i.EaSize,"controlled uniform write lost complete FileInfo");
}
void run(const std::filesystem::path& worker,StoreCompressionMode compression,bool compact,unsigned fault){
  auto ram=std::make_shared<Ram>();if(compact)ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{compression,6,false},{compact}));
  const std::array<NativeFileMutation,4> seed{{file_create("target"),file_write("target",0,data(7001,91)),
      file_create("keep"),file_write("keep",0,data(37,92))}};
  const NativeBasicMutation b{0,1,basic()};const NativeSecurityMutation security{0,1,native_security_fixture::descriptor()};
  writer.apply_with_metadata(seed,{&b,1},{&security,1});writer.reserve_inode_result(2,8192);
  auto session=NativeFileSession::create(std::move(writer));auto handle=session.open("target",{true,true,true});
  const auto prior=session.snapshot();
  const auto block=native_lz4_fixture::source();Bytes input=block;input.insert(input.end(),block.begin(),block.end());
  const auto payload=input;auto expected=input;
  if(fault==4){expected=data(7001,91);expected.insert(expected.end(),payload.begin(),payload.end());}
  if(fault==5)expected.resize(7001);
  if(fault==6){const auto old=data(7001,91);expected.assign(old.begin(),old.begin()+31);
    expected.insert(expected.end(),payload.begin(),payload.end());}
  const auto e=health(100);const auto profile=cost(e);
  NativeUniformWorkerBinding binding;binding.helper=worker;binding.configuration=e.configuration;
  binding.cpu_fixture_arguments={fault==1?L"uniform-native2-corrupt":fault==2?L"uniform-native2-hold":
      fault==4 || fault==6?L"uniform-native1":L"uniform-native2"};
  ControlledNativeUniformWrite operation(session,WorkerExecution::cpu_semantics);
  const auto started=operation.begin(handle,input,fault==4?UINT64_MAX:fault==6?31:0,fault==4,fault==5,{stamp,true,true,true,true},mode,binding,
      true,true,e,100,profile,{16,100000000},{},3000);
  check(started.admission.status==STATUS_SUCCESS && !started.admission.offline_required &&
      started.eligible_bytes==(fault==5?0U:fault==4 || fault==6?65536U:131072U),"controlled uniform begin rejected complete extents");
  input.assign(input.size(),std::byte{0xff}); // External caller storage no longer owns the request.
  if(started.worker.decision.submitted){
    check(!operation.finish(),"waiting controlled write acknowledged bytes");
    if(fault==2){
      eventually([&]{return operation.process_id()!=0;},2000,"controlled held worker PID");
      const auto cpu=operation.use_cpu_fallback();
      check(std::ranges::equal(cpu,payload),"controlled CPU selection changed full owned write");
      check(operation.request_cancel(),"controlled cancellation signal");
    }else{
      eventually([&]{const auto state=operation.poll(e.memory,101);
        return state!=GpuWorkerState::waiting || operation.awaiting_fresh_observation();},
        3000,"controlled uniform worker did not validate");
      if(operation.awaiting_fresh_observation())operation.poll(health(102).memory,102);
    }
  }else check(!gpu_lz4_cpu_verification_available() || fault==5,"qualified controlled worker did not start");
  if(fault==3)ram->fail_at=ram->calls;
  const auto result=operation.finish();
  if(fault==3){
    check(result && result->outcome.status==STATUS_IO_DEVICE_ERROR && result->outcome.offline_required &&
        !result->transferred && session.poisoned() && session.stats().contexts==1,
        "controlled storage failure acknowledged bytes or released context");
    const auto* info=reinterpret_cast<const std::byte*>(&result->info);
    check(std::all_of(info,info+sizeof(result->info),[](auto v){return v==std::byte{0};}),
        "controlled storage failure exposed FileInfo");
    const auto before=ram->calls;const auto again=operation.finish();
    check(again && !again->transferred && ram->calls==before,"controlled failure retried storage");
    auto recovered=image(ram->stable,ram->failure_unit);
    const auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    check(view.read("target",0,7001)==data(7001,91) && view.read("keep",0,37)==data(37,92),
        "controlled failed write lost acknowledged data");
    eventually([&]{return operation.try_retire();},4000,"failed controlled write retained worker");return;
  }
  check(result && result->outcome.status==STATUS_SUCCESS && !result->outcome.offline_required &&
      result->transferred==(fault==5?7001U:payload.size()),"controlled write lost committed count");
  exact(result->info,expected.size());
  check(result->candidate_count==(!gpu_lz4_cpu_verification_available() || fault==1 || fault==2 || fault==5?0U:
      fault==4 || fault==6?1U:2U),
      "controlled write supplied wrong advisory extent count");
  const auto calls=ram->calls,reads=ram->read_calls;
  const auto again=operation.finish();
  check(again && again->transferred==result->transferred && again->outcome.status==result->outcome.status &&
      std::memcmp(&again->info,&result->info,sizeof(result->info))==0 && ram->calls==calls && ram->read_calls==reads,
      "controlled completion repeated I/O or changed committed facts");
  check(session.read(handle,0,expected.size())==expected &&
      prior.read("target",0,7001)==data(7001,91) && prior.read("keep",0,37)==data(37,92),
      "controlled write changed current/prior/unrelated CPU bytes");
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
  check(reopened.read("target",0,expected.size())==expected && reopened.read("keep",0,37)==data(37,92),
      "controlled write failed independent CPU recovery");
  if(fault==0 && compression==StoreCompressionMode::fast && store_lz4_available()){
    auto objects=ObjectStoreSnapshot::open(ram);bool found=false;
    for(const auto& object:objects.list())if(object.logical_bytes==block.size() && objects.read(object.id)==block){
      found=true;check(object.stored_payload_bytes==native_lz4_fixture::alternative_block(block).size(),
          "verified uniform bytes did not reach stored native record");
    }
    check(found,"controlled compressed native extent absent");
  }
  eventually([&]{return operation.try_retire();},4000,"controlled worker/session did not retire");
  const auto before_reads=ram->read_calls,before_calls=ram->calls;
  const auto rejected=operation.begin(handle,expected,0,false,false,{},NativeFileInfoAllocation::unavailable,
      binding,true,true,e,100,profile,{16,100000000},{},3000);
  check(rejected.admission.status==STATUS_INVALID_PARAMETER && !operation.finish() &&
      ram->calls==before_calls && ram->read_calls==before_reads,"failed begin exposed old completion or touched backing");
  session.cleanup(handle,false);session.close(handle);
  check(session.stats().contexts==0,"controlled write retained native context");
}
}
int wmain(int argc,wchar_t** argv){try{
  check(argc==2,"trusted worker path required");const std::filesystem::path worker(argv[1]);
  check(worker.is_absolute(),"absolute trusted worker required");
  for(const auto compression:{StoreCompressionMode::raw,StoreCompressionMode::fast})
    for(bool compact:{false,true})run(worker,compression,compact,0);
  run(worker,StoreCompressionMode::fast,true,1);run(worker,StoreCompressionMode::fast,true,2);
  run(worker,StoreCompressionMode::fast,true,3);
  run(worker,StoreCompressionMode::fast,true,4);run(worker,StoreCompressionMode::fast,true,5);
  run(worker,StoreCompressionMode::fast,true,6);
  std::cout<<"Controlled native uniform writes: checks="<<checks
      <<" exact_advice_storage=true CPU_fallback=true prior_and_unrelated_preserved=true"
      <<" gpu_executed=false mounted=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
