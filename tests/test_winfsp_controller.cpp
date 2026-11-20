#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "winfsp_memory_volume.hpp"
#include "anchor_batch_profile.hpp"
#include "anchor_search_fixture.hpp"
#include "flitzfs/paged_store.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "native_fs_fixture.hpp"
#include "../src/native_group_codec.hpp"
#include "../src/native_maintenance_source.hpp"
#include "../src/record_digest.hpp"
#include "volume_range_fixture.hpp"
#include "windows_app_budget.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <thread>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
inline constexpr auto queued_anchor_members=1+(kAnchorProfileMaxQueuedDispatches-1)*
    anchor_match::dispatch_member_limit(anchor_match::BatchOutput::anchors);
#endif
DWORD handle_count() {DWORD count=0;if(!GetProcessHandleCount(GetCurrentProcess(),&count))throw std::runtime_error("handle telemetry");return count;}
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejected(F&& run,const char* why) {
  bool refused=false;
  try{run();}catch(const std::exception&){refused=true;}
  require(refused,why);
}
void native_rng_initialization() {
  std::array<DWORD,3> observed{};const auto cold=handle_count();
  for(unsigned i=0;i<observed.size();++i) {
    std::array<UCHAR,16> bytes{};
    require(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,
        "native system RNG initialization failed");observed[i]=handle_count();
  }
  std::cout<<"native_rng_handles cold="<<cold<<" first="<<observed[0]<<" second="<<observed[1]<<" third="<<observed[2]<<'\n';
  require(observed[1]==observed[2],"native RNG handle initialization did not stabilize");
}
void initialize_long_pdh_window() {
  const auto initial=handle_count();
  std::thread([] {
    WindowsMaintenanceHost host;
    for(unsigned sample=0;sample<35;++sample) {
      {MaintenanceThreadScope scope;host.observe(false);}
      Sleep(1010);
    }
  }).join();
  std::cout<<"long_native_pdh_handles before="<<initial<<" after="<<handle_count()<<'\n';
}
template<class F>void await(F predicate,std::uint64_t milliseconds,const char* why) {
  const auto until=GetTickCount64()+milliseconds;
  while(!predicate()){if(GetTickCount64()>=until)throw std::runtime_error(why);Sleep(10);}
  ++checks;
}
StoreIdentity id(unsigned n){auto result=identity();result[0]=static_cast<std::byte>(n);return result;}
struct Fixture {
  std::shared_ptr<Ram> head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  std::unique_ptr<WinFspNativeWritable> adapter=WinFspNativeWritable::create_managed_paged(
      head,id(90),image,id(1),{kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false});
};
struct Provider {
  FSP_FILE_SYSTEM fs{};
  std::atomic<unsigned> calls{0};std::array<char,16> order{};
  std::atomic<bool> stopping{false},release_stop{true};
  bool fail_mount=false,callback_alive=false,read_only=false,report_stop=true;
  static Provider* active;
  void record(char c){const auto n=calls.load();if(n<order.size())order[n]=c;calls.store(n+1);}
  static NTSTATUS preflight(PWSTR,PWSTR){active->record('p');return STATUS_SUCCESS;}
  static NTSTATUS create(PWSTR,const FSP_FSCTL_VOLUME_PARAMS* params,const FSP_FILE_SYSTEM_INTERFACE* table,
      FSP_FILE_SYSTEM** out) {
    active->read_only=params->ReadOnlyVolume!=0;
    active->record('c');active->fs.Version=static_cast<UINT16>(sizeof(FSP_FILE_SYSTEM));
    active->fs.UmFileContextIsUserContext2=TRUE;active->fs.Interface=table;*out=&active->fs;return STATUS_SUCCESS;
  }
  static NTSTATUS start(FSP_FILE_SYSTEM*,ULONG){active->record('s');return STATUS_SUCCESS;}
  static NTSTATUS mount(FSP_FILE_SYSTEM*,PWSTR){active->record('m');return active->fail_mount?STATUS_OBJECT_NAME_COLLISION:STATUS_SUCCESS;}
  static VOID stop(FSP_FILE_SYSTEM* fs) {
    active->record('t');active->stopping=true;
    const auto until=GetTickCount64()+5000;
    while(!active->release_stop.load() && GetTickCount64()<until)Sleep(1);
    FSP_FSCTL_VOLUME_INFO info{};active->callback_alive=fs->Interface->GetVolumeInfo(fs,&info)==STATUS_SUCCESS;
    if(active->report_stop)fs->Interface->DispatcherStopped(fs,TRUE);
  }
  static VOID unmount(FSP_FILE_SYSTEM*){active->record('r');}
  static VOID destroy(FSP_FILE_SYSTEM* fs){active->record('d');fs->UserContext=nullptr;}
  static UINT32 origin(){return 0;}
  WinFspSessionApi api(){return {preflight,create,start,mount,stop,unmount,destroy,origin};}
};
Provider* Provider::active=nullptr;
struct StopRelease {Provider& provider;~StopRelease(){provider.release_stop=true;}};
NativeWriteMountPlan plan(){return {L"R:\\FlitzFS-model\\unused",true,true};}
void callback_cleanup_close(Provider& p,PVOID context) {
  FSP_FSCTL_TRANSACT_REQ request;
  FSP_FSCTL_TRANSACT_RSP response;
  std::memset(&request,0,sizeof(request));
  std::memset(&response,0,sizeof(response));
  request.Kind=FspFsctlTransactCleanupKind;
  request.Req.Cleanup.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpCleanup(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Cleanup");
  request.Kind=FspFsctlTransactCloseKind;
  request.Req.Close.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpClose(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Close");
}
void callback_file(Provider& p,unsigned n,const Bytes& bytes) {
  const auto* table=p.fs.Interface;SIZE_T security_size=0;UINT32 attributes=0;
  require(table->GetSecurityByName(&p.fs,const_cast<PWSTR>(L"\\"),&attributes,nullptr,&security_size)==STATUS_BUFFER_OVERFLOW,
      "controller root security size");Bytes security(security_size);
  require(table->GetSecurityByName(&p.fs,const_cast<PWSTR>(L"\\"),&attributes,security.data(),&security_size)==STATUS_SUCCESS,
      "controller root security");
  auto name=L"\\f"+std::to_wstring(n);PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
  require(table->Create(&p.fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&context,&info)==STATUS_SUCCESS,"controller SDK Create");
  ULONG count=0;require(table->Write(&p.fs,context,const_cast<std::byte*>(bytes.data()),0,static_cast<ULONG>(bytes.size()),
      FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==bytes.size(),"controller SDK Write");
  Bytes out(bytes.size());count=0;require(table->Read(&p.fs,context,out.data(),0,static_cast<ULONG>(out.size()),&count)==STATUS_SUCCESS &&
      count==out.size() && out==bytes,"controller SDK byte readback");
  callback_cleanup_close(p,context);
}
void volume_file_callbacks() {
  constexpr PagedStoreLimits limits{8ULL*1024*1024};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto first=std::make_shared<Ram>();first->failure_unit=4096;
  const auto payload=data(16712,431),another=data(3948,393);
  {
    Provider initial;Provider::active=&initial;
    WinFspNativeSession mounted(WinFspNativeWritable::create_file_volume(first,id(90),id(1),limits,raw),initial.api());
    require(mounted.start(plan())==STATUS_SUCCESS,"volume-file callback provider start");
    callback_file(initial,0,payload);
    mounted.stop();
    require(mounted.write_restart_ready() && initial.fs.UserContext==nullptr,
        "volume-file provider failed to retire before reopening");
  }
  auto recovered=image(first->stable,4096);
  {
    const auto cpu=NativeVolumeFile::inspect(recovered,id(90));
    require(cpu.filesystem().read("f0",0,static_cast<std::uint32_t>(payload.size()))==payload,
        "volume-file CPU reopen lost acknowledged callback data");
  }
  {
    Provider replacement;Provider::active=&replacement;
    WinFspNativeSession mounted(WinFspNativeWritable::resume_file_volume(recovered,id(90),raw),replacement.api());
    require(mounted.start(plan())==STATUS_SUCCESS,"volume-file callback provider restart");
    auto* fs=&replacement.fs;
    wchar_t name[]=L"\\f0";
    PVOID handle=nullptr;FSP_FSCTL_FILE_INFO found{};
    constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
    require(fs->Interface->Open(fs,name,open_file,FILE_READ_DATA,&handle,&found)==STATUS_SUCCESS &&
        handle && found.FileSize==payload.size(),"reopened callback metadata differs");
    Bytes output(payload.size());ULONG written=0;
    require(fs->Interface->Read(fs,handle,output.data(),0,static_cast<ULONG>(output.size()),&written)==STATUS_SUCCESS &&
        written==output.size() && output==payload,"reopened file callback readback differs");
    callback_cleanup_close(replacement,handle);
    callback_file(replacement,1,another);
    mounted.stop();
    require(mounted.write_restart_ready() && replacement.fs.UserContext==nullptr,"reopened volume-file owner not retired");
  }
  const auto cpu=NativeVolumeFile::inspect(image(recovered->stable,4096),id(90));
  require(cpu.filesystem().read("f1",0,10000)==another &&
      cpu.filesystem().read("f0",0,static_cast<std::uint32_t>(payload.size()))==payload,
      "fresh CPU model reopen lost original or subsequent callback bytes");
}
void volume_file_scrub_checkpoint() {
  using Scrub=PagedPayloadScrubState;
  constexpr PagedStoreLimits limits{24ULL*1024*1024};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto volume=id(90);
  auto backing=std::make_shared<SparseVolumeRam>(4096);
  {
    Provider provider;Provider::active=&provider;
    WinFspNativeSession writer(WinFspNativeWritable::create_file_volume(backing,volume,id(1),limits,raw),provider.api());
    require(writer.start(plan())==STATUS_SUCCESS,"scrub fixture did not start its SDK dispatcher");
    for(unsigned i=0;i<5;++i)callback_file(provider,i,data(1024*1024,1020+i));
    writer.stop();
    require(writer.write_restart_ready(),"scrub fixture producer did not retire");
  }
  const auto advisory=[&] {
    const auto& bytes=backing->io.stable;
    auto area=native_fixture::image(Bytes(bytes.begin()+kVolumeHeadBytes,
        bytes.begin()+kNativeVolumeFileImageOffset));
    return read_optimization_state(*area,volume);
  };
  OptimizationPayloadScrub saved;
  const auto phase=[&](bool reopen) {
    Provider provider;Provider::active=&provider;
    const auto bytes_before=backing->io.read_bytes,write_before=backing->io.write_bytes;
    auto adapter=WinFspNativeWritable::resume_file_volume(backing,volume,raw);
    if(!reopen)std::cout<<"paired_indexed_reopen logical_bytes="<<5ULL*1024*1024
        <<" backing_read_bytes="<<backing->io.read_bytes-bytes_before
        <<" backing_write_bytes="<<backing->io.write_bytes-write_before<<'\n';
    WinFspNativeSession session(std::move(adapter),provider.api());
    require(session.start(plan())==STATUS_SUCCESS,"scrub fixture reopen did not start its SDK dispatcher");
    NativeMaintenanceObservation observed{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
    const auto advance=[&] {
      ++observed.sequence;observed.captured_ms+=1000;
      return session.advance_background_maintenance(observed,observed.captured_ms,{},WinFspMaintenanceContinuation::none,true);
    };
    const auto reads=backing->io.read_calls,writes=backing->io.write_bytes;
    require(advance().progress.deferral==NativeMaintenanceDeferral::foreground &&
        backing->io.read_calls==reads && backing->io.write_bytes==writes,"busy scrub reached storage");
    observed.foreground_pending=false;
    bool verified=false;
    for(unsigned i=0;i<256 && !verified;++i) {
      const auto step=advance().payload_scrub;
      if(!step.object_id)continue;
      require(step.state==Scrub::checked,"scrub fixture found corruption in a clean volume");
      if(reopen) {
        require(step.object_id>saved.position.after && step.object_id<=saved.position.through,
            "SDK restart discarded its verified checkpoint");
        verified=true;
      }else if(const auto state=advisory();state.state.payload_scrub) {
        saved=*state.state.payload_scrub;
        require(!state.degraded && saved.position.after==step.object_id,
            "SDK scheduler saved a different scrub cursor");
        verified=true;
      }
    }
    require(verified,"SDK scrub did not publish or resume a checkpoint");
    session.stop();
    require(session.write_restart_ready(),"scrub fixture did not retire before next ownership");
  };
  phase(false);
  const auto cut=backing->io.write_bytes;
  phase(true);
  require(backing->io.write_bytes==cut,"SDK reopen rewrote the same checkpoint");
  auto read=NativeVolumeFile::inspect(backing,volume);
  read.filesystem().verify_all();
  for(unsigned i=0;i<5;++i)
    require(read.filesystem().read("f"+std::to_string(i),0,1024*1024)==data(1024*1024,1020+i),
        "CPU reader could not recover acknowledged SDK writes");
  std::cout<<"paired_scrub_checkpoint retired_and_resumed=1 foreground_yield=1 independent_bytes=1\n";
}

void indexed_reopen_platform_admission() {
  constexpr PagedStoreLimits limits{8ULL*1024*1024};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto backing=std::make_shared<Ram>();
  backing->failure_unit=4096;
  const auto bytes=data(3137,6287);
  const auto volume=id(90);
  {
    // The portable filesystem can have files without Windows security state.
    auto session=NativeVolumeFile::create(backing,volume,id(1),limits,raw);
    session.create_entry("portable");
    const auto file=session.open("portable",{true,true,true});
    session.write(file,0,bytes);
    session.cleanup(file,false);session.close(file);
  }
  const auto body=backing->live.begin()+kNativeVolumeFileImageOffset;
  const auto primary=std::search(body,backing->live.end(),bytes.begin(),bytes.begin()+64);
  require(primary!=backing->live.end(),"platform admission test data record missing");
  require(std::size_t(backing->live.end()-primary)>=backing->failure_unit+64,"platform admission peer bounds");
  const auto peer=primary+backing->failure_unit;
  require(std::equal(bytes.begin(),bytes.begin()+64,peer),"platform admission test peer missing");
  *peer^=std::byte{1};
  backing->stable=backing->live;
  const auto writes=backing->writes,barriers=backing->flushes;
  bool refused=false;
  try{(void)WinFspNativeWritable::resume_file_volume(backing,volume,raw);}
  catch(const std::runtime_error&){refused=true;}
  require(refused && backing->writes==writes && backing->flushes==barriers,
      "invalid Windows metadata allowed repair before refusal");
  const auto verified=NativeVolumeFile::inspect(backing,volume);
  require(verified.filesystem().storage_info().degraded_checkpoint &&
      verified.filesystem().read("portable",0,8192)==bytes,"platform refusal changed the independently readable source");
  std::cout<<"paired_reopen_windows_admission before_repair=1 surviving_bytes=1\n";
}

enum class RepairFixture {foreground,rotation,scrub,startup_rotation};
void volume_file_payload_repair(bool live,RepairFixture fixture=RepairFixture::foreground) {
  const bool startup=fixture==RepairFixture::startup_rotation;
  const bool rotate=startup || fixture==RepairFixture::rotation,scrub=fixture==RepairFixture::scrub;
  constexpr PagedStoreLimits limits{8ULL*1024*1024};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto original=data(rotate?12289U:2049U,814);
  auto file=std::make_shared<SparseVolumeRam>(4096);
  {
    Provider source;Provider::active=&source;
    WinFspNativeSession writer(WinFspNativeWritable::create_file_volume(file,id(90),id(1),limits,raw),source.api());
    require(writer.start(plan())==STATUS_SUCCESS,"repair fixture original writer startup");
    callback_file(source,0,original);
    writer.stop();
    require(writer.write_restart_ready(),"repair fixture original writer retirement");
  }
  if(rotate) {
    auto writer=NativeVolumeFile::resume(file,id(90),raw);
    const auto handle=writer.open("f0",{true,true,true});
    for(unsigned i=0;i<28 && !writer.persistent_reclamation_pending();++i)
      writer.write(handle,0,data(original.size(),916+i));
    writer.write(handle,0,original);
    writer.cleanup(handle,false);
    writer.close(handle);
    require(writer.persistent_reclamation_pending(),"repair rotation fixture did not admit source compaction");
    using Reclaim=NativeVolumeReclaimProgress;
    auto stage=Reclaim::idle;
    for(unsigned i=0;i<350 && stage!=Reclaim::ready;++i) {
      stage=writer.advance_persistent_reclamation([]{return id(94);});
      require(stage==Reclaim::copying || stage==Reclaim::ready,"repair rotation fixture could not pack its source");
    }
    require(stage==Reclaim::ready && writer.advance_persistent_reclamation({})==Reclaim::published,
        "repair rotation fixture could not select its packed source");
    for(unsigned i=0;writer.persistent_reclamation_pending();++i)
      require(i<8 && writer.advance_persistent_reclamation({})==Reclaim::retiring,
          "repair rotation fixture retained the first range");
  }
  std::unique_ptr<WinFspNativeWritable> adapter;
  if(!startup)adapter=WinFspNativeWritable::resume_file_volume(file,id(90),raw);
  if(rotate)file->admitted_end=file->extent();
  constexpr auto prefix_size=64;
  auto& body=file->io.live;
  const auto image_start=kNativeVolumeFileImageOffset+(rotate?limits.maximum_physical_bytes:0);
  const auto primary=std::search(body.begin()+image_start,body.end(),original.begin(),original.begin()+prefix_size);
  require(primary!=body.end(),"repair fixture did not find its selected payload");
  const auto at=static_cast<std::size_t>(primary-body.begin());
  const auto peer=at+file->io.failure_unit;
  require(peer+prefix_size<=body.size() &&
      std::equal(original.begin(),original.begin()+prefix_size,body.begin()+peer),"repair fixture did not identify the peer");
  const auto damaged_offset=scrub?peer:at;
  body[damaged_offset]^=std::byte{1};
  file->io.stable[damaged_offset]=body[damaged_offset];
  const auto damaged=body;
  const auto calls=file->io.calls;
  if(startup)adapter=WinFspNativeWritable::resume_file_volume(file,id(90),raw);

  Provider provider;Provider::active=&provider;
  const auto open=[&] {
    wchar_t name[]=L"\\f0";
    constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
    PVOID handle=nullptr;FSP_FSCTL_FILE_INFO info{};
    require(provider.fs.Interface->Open(&provider.fs,name,open_file,FILE_READ_DATA|FILE_WRITE_DATA,&handle,&info)==
        STATUS_SUCCESS && handle,"repair fixture open");
    return handle;
  };
  const auto read=[&](PVOID handle) {
    Bytes bytes(original.size());ULONG count=0;
    require(provider.fs.Interface->Read(&provider.fs,handle,bytes.data(),0,static_cast<ULONG>(bytes.size()),&count)==
        STATUS_SUCCESS && count==bytes.size() && bytes==original,"repair callback read differs");
  };
  if(startup) {
    const auto checked=adapter->snapshot();
    checked.verify_all();
    require(!checked.storage_info().degraded_checkpoint,"paired startup adapter remained degraded");
    WinFspNativeSession session(std::move(adapter),provider.api());
    require(session.start(plan())==STATUS_SUCCESS,"paired repair could not start writable callbacks");
    const auto handle=open();
    read(handle);
    callback_cleanup_close(provider,handle);
    session.stop();
    require(session.write_restart_ready(),"paired startup repair did not retire");
    std::cout<<"paired_startup_rotation peer_restored_before_dispatch=1 cpu_verified=1\n";
  }else if(live) {
    WinFspVolumeController controller(std::move(adapter),provider.api(),plan());
    await([&]{return controller.view().state==SessionState::running || controller.view().retired;},5000,
        "repair controller startup deadline");
    require(controller.view().state==SessionState::running,"repair controller startup");
    const auto handle=open();
    if(!scrub)read(handle);
    const auto start=GetTickCount64();
    try {
      await([&]{const auto view=controller.view();
        const bool repaired=rotate?view.compactions!=0:view.payload_repairs!=0;
        return (repaired && (!scrub || view.payload_scrub_passes)) || view.retired || view.maintenance_disabled;
      },35000,"live payload repair deadline");
    }catch(...) {
      const auto view=controller.view();
      std::cerr<<"repair owner quanta="<<view.maintenance_quanta<<" host="<<static_cast<unsigned>(view.host)
          <<" phase="<<static_cast<unsigned>(view.maintenance.phase)
          <<" deferral="<<static_cast<unsigned>(view.maintenance.deferral)
          <<" checks="<<view.payload_scrub_checks<<" passes="<<view.payload_scrub_passes
          <<" faults="<<view.payload_scrub_faults<<" repairs="<<view.payload_repairs
          <<" rotations="<<view.compactions<<'\n';
      throw;
    }
    read(handle);
    callback_cleanup_close(provider,handle);
    controller.stop_and_join();
    const auto status=controller.view();
    require(status.payload_repairs==(rotate?0U:1U) && status.compactions==(rotate?1U:0U) &&
        status.retired && !status.offline && !status.maintenance_disabled,
        "live repair did not publish its verified replacement and retire");
    if(scrub)require(status.payload_scrub_passes==1 && status.payload_scrub_checks && !status.payload_scrub_faults,
        "latent repair did not finish its background scan");
    std::cout<<"live_payload_repair elapsed_ms="<<GetTickCount64()-start<<" repairs="<<status.payload_repairs
        <<" rotations="<<status.compactions<<" max_quantum_ns="<<status.maximum_quantum_ns<<'\n';
    if(scrub)std::cout<<"live_payload_scrub peer_before_read=1 checks="<<status.payload_scrub_checks
        <<" passes="<<status.payload_scrub_passes<<" faults="<<status.payload_scrub_faults<<'\n';
  }else if(scrub) {
    WinFspNativeSession session(std::move(adapter),provider.api());
    require(session.start(plan())==STATUS_SUCCESS,"modeled scrub controller startup");
    const auto handle=open();
    NativeMaintenanceObservation sample{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
    const auto advance=[&] {
      ++sample.sequence;sample.captured_ms+=1000;
      return session.advance_background_maintenance(sample,sample.captured_ms,{},WinFspMaintenanceContinuation::none,true);
    };
    const auto reads=file->io.read_calls;
    using Deferral=NativeMaintenanceDeferral;
    require(advance().progress.deferral==Deferral::foreground,"scrub started during foreground access");
    sample.foreground_pending=false;
    sample.cpu_busy_ticks=sample.cpu_total_ticks;
    require(advance().progress.deferral==Deferral::cpu,"scrub started without idle CPU");
    sample.cpu_busy_ticks=0;
    const auto physical=sample.physical_available;
    sample.physical_available=1;
    require(advance().progress.deferral==Deferral::memory && file->io.read_calls==reads && file->io.calls==calls,
        "denied scrub opportunities reached the backing");
    sample.physical_available=physical;
    bool discovered=false,repaired=false;
    for(unsigned i=0;i<40 && !repaired;++i) {
      const auto before=file->io.calls;
      const auto step=advance();
      if(step.payload_scrub.state==PagedPayloadScrubState::repair_queued) {
        require(file->io.calls==before && !step.payload_repaired,"scrub wrote during a verification opportunity");
        discovered=true;
      }
      repaired=step.payload_repaired;
    }
    require(discovered && repaired,"modeled scrub did not restore the latent peer before reading");
    read(handle);
    callback_cleanup_close(provider,handle);
    session.stop();
    require(session.write_restart_ready(),"modeled scrub controller did not retire");
    std::cout<<"modeled_payload_scrub foreground_cpu_memory_yield=1 peer_before_read=1\n";
  }else {
    WinFspNativeSession session(std::move(adapter),provider.api());
    require(session.start(plan())==STATUS_SUCCESS,"modeled repair controller startup");
    const auto handle=open();
    read(handle);
    require(session.background_maintenance_pending() && file->io.calls==calls,"read performed a repair write");
    auto changed=original;changed[63]^=std::byte{1};
    ULONG written=0;FSP_FSCTL_FILE_INFO info{};
    require(provider.fs.Interface->Write(&provider.fs,handle,changed.data(),0,static_cast<ULONG>(changed.size()),
        FALSE,FALSE,&written,&info)==STATUS_RETRY && !written && file->io.calls==calls &&
        session.background_maintenance_pending(),"write touching a damaged copy did not yield to repair");
    NativeMaintenanceObservation sample{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
    using Deferral=NativeMaintenanceDeferral;
    require(session.advance_background_maintenance(sample,sample.captured_ms).progress.deferral==Deferral::foreground &&
        file->io.calls==calls,"repair ran with foreground pending");
    sample.foreground_pending=false;
    sample.continued=true;++sample.sequence;
    require(session.advance_background_maintenance(sample,sample.captured_ms,{},WinFspMaintenanceContinuation::compaction)
        .progress.deferral==Deferral::observation && file->io.calls==calls,"repair ran on reused CPU evidence");
    sample.continued=false;++sample.sequence;sample.captured_ms+=1000;
    require(session.advance_background_maintenance(sample,sample.captured_ms).payload_repaired,
        "fresh owner opportunity did not repair the damaged record");
    read(handle);
    callback_cleanup_close(provider,handle);
    session.stop();
    require(session.write_restart_ready(),"repair controller did not retire");
  }
  if(!rotate)
    require(!file->range_reclaims && file->io.calls>calls &&
        std::equal(damaged.begin()+image_start+2*kStoreCommitUnit,damaged.end(),
            file->io.live.begin()+image_start+2*kStoreCommitUnit),"repair rewrote an existing body bank");
  const auto reopened=NativeVolumeFile::inspect(file->replay(),id(90));
  require(reopened.info().selection.epoch==(rotate?3U:1U) && !reopened.filesystem().storage_info().degraded_checkpoint &&
      reopened.filesystem().read("f0",0,static_cast<std::uint32_t>(original.size()))==original,
      "repair did not restore independently reopened copies");
}

void volume_file_idle_reclamation() {
  constexpr PagedStoreLimits limits{8ULL*1024*1024};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  Provider provider;Provider::active=&provider;
  auto file=std::make_shared<SparseVolumeRam>(4096);
  auto adapter=WinFspNativeWritable::create_file_volume(file,id(90),id(1),limits,raw);
  auto* source=adapter.get();
  WinFspNativeSession session(std::move(adapter),provider.api());
  require(session.start(plan())==STATUS_SUCCESS,"paired-file native SDK start");
  auto expected=data(15731,901);
  callback_file(provider,0,expected);
  for(unsigned i=0;i<7;++i) {
    wchar_t name[]=L"\\f0";
    constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO metadata{};
    require(provider.fs.Interface->Open(&provider.fs,name,open_file,FILE_WRITE_DATA,&context,&metadata)==STATUS_SUCCESS &&
        context,"paired-file native open before overwrite");
    expected=data(expected.size(),250+i);
    ULONG written=0;
    require(provider.fs.Interface->Write(&provider.fs,context,expected.data(),0,static_cast<ULONG>(expected.size()),
        FALSE,FALSE,&written,&metadata)==STATUS_SUCCESS && written==expected.size(),"paired-file native overwrite");
    callback_cleanup_close(provider,context);
  }
  NativeMaintenanceObservation observed{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
  using Continuation=WinFspMaintenanceContinuation;
  using Deferral=NativeMaintenanceDeferral;
  const auto step=[&](bool continued,std::uint64_t age_ms=0) {
    ++observed.sequence;
    observed.continued=continued;
    if(!continued)observed.captured_ms+=1000;
    return session.advance_background_maintenance(observed,observed.captured_ms+age_ms,{},
        continued?Continuation::compaction:Continuation::none);
  };
  const auto refuse=[&](Deferral reason,std::uint64_t age_ms=0) {
    const auto writes=file->io.calls;
    require(step(true,age_ms).progress.deferral==reason && file->io.calls==writes,
        "paired-file continuation performed deferred backing work");
  };
  const auto before=file->occupied();
  unsigned private_steps=0,publish_boundaries=0;
  {
    const auto retained=source->snapshot();
    const auto writes=file->io.calls;
    require(step(false).progress.deferral==Deferral::foreground && file->io.calls==writes,
        "paired-file compaction ran under foreground load");
    observed.foreground_pending=false;
    refuse(Deferral::observation); // A reused sample cannot start a new image.

    auto progress=step(false);
    require(progress.continuation==Continuation::compaction,"paired-file preparation was not admitted");
    refuse(Deferral::observation,NativeMaintenancePolicy{}.maximum_age_ms+1);
    const auto available=observed.physical_available;
    observed.physical_available=0;
    refuse(Deferral::memory);
    observed.physical_available=available;
    observed.foreground_pending=true;
    refuse(Deferral::foreground);
    observed.foreground_pending=false;

    for(unsigned tick=0;!progress.compacted && tick<350;++tick) {
      const bool continued=progress.continuation==Continuation::compaction;
      if(continued)++private_steps;
      else {
        refuse(Deferral::observation);
        ++publish_boundaries;
      }
      progress=step(continued);
      require(!progress.compaction_failed && progress.progress.deferral==Deferral::none,
          "paired-file SDK idle compaction failed");
    }
    require(progress.compacted && private_steps && publish_boundaries==2 && !file->range_reclaims &&
        retained.read("f0",0,50000)==expected,"paired-file publication boundaries or pinned source changed");
  }
  refuse(Deferral::observation); // Even an unpinned range needs a fresh capture.
  require(!file->range_reclaims,"continued step reclaimed the old range");
  for(unsigned tick=0;tick<8 && file->allocated_extent(kNativeVolumeFileImageOffset,limits.maximum_physical_bytes);++tick)
    (void)step(false);
  require(file->range_reclaims && file->occupied()<before,"the native idle owner did not reclaim the old range");
  session.stop();
  require(session.write_restart_ready(),"paired-file dispatcher did not retire");
  require(NativeVolumeFile::inspect(file->replay()).filesystem().read("f0",0,50000)==expected,
      "reclaimed SDK volume lost acknowledged bytes on independent CPU reopen");
  std::cout<<"paired_file_sdk_idle private_steps="<<private_steps<<" fresh_publish_boundaries="<<publish_boundaries
      <<" published=1 prior_readers_preserved=1 reclaimed=1 cpu_reopen=1\n";
}
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
std::array<std::byte,32> artifact_digest(std::string_view input) {
  require(input.size()==64,"Expected exact artifact digest");
  const auto digit=[](char c)->unsigned {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    throw std::invalid_argument("Expected lowercase digest");
  };
  std::array<std::byte,32> result{};
  for(std::size_t i=0;i<result.size();++i)result[i]=static_cast<std::byte>(16*digit(input[2*i])+digit(input[2*i+1]));
  return result;
}
void restore_private_anchor_package() {
  std::array<wchar_t,32768> executable{};
  const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
  require(length && length<executable.size(),"Restoration fixture executable path");
  const auto root=std::filesystem::canonical(std::filesystem::path(std::wstring_view(executable.data(),length)).parent_path());
  require(root.filename().native().starts_with(L"anchor-input-restore-"),"Restoration requires its private fixture directory");
  const auto staged=root/L"prepared"/L"amd";
  require(std::filesystem::canonical(staged)==staged,"Restoration inputs leave the fixture directory");
  const auto destination=root/L"amd";
  require(std::filesystem::create_directory(destination) &&
      std::filesystem::create_directory(destination/L"hip"),"Restoration destination already exists");
  for(const auto& relative:{L"flitzfs_hip_worker.exe",L"hip/flitzfs_rocm_kernels.dll"}) {
    const auto input=staged/relative;
    require(std::filesystem::canonical(input)==input &&
        CopyFileW(input.c_str(),(destination/relative).c_str(),TRUE),"Could not restore approved fixture input");
  }
}

constexpr auto open_existing_file=FILE_NON_DIRECTORY_FILE|(1U<<24);

void foreground_read_samples(Provider& provider,unsigned file,const Bytes& original,std::string_view phase) {
  auto name=L"\\f"+std::to_wstring(file);
  auto* fs=&provider.fs;
  PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
  require(fs->Interface->Open(fs,name.data(),open_existing_file,FILE_READ_DATA,&context,&info)==STATUS_SUCCESS && context,
      "Foreground sample Open");
  std::array<std::uint64_t,101> elapsed{};
  std::array<std::byte,4096> output{};
  require(original.size()>=output.size(),"Foreground sample input size");
  for(std::size_t i=0;i<elapsed.size();++i) {
    const auto offset=(i*997)%(original.size()-output.size()+1);
    ULONG transferred=0;
    const auto start=std::chrono::steady_clock::now();
    const auto status=fs->Interface->Read(fs,context,output.data(),offset,
        static_cast<ULONG>(output.size()),&transferred);
    elapsed[i]=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
    require(status==STATUS_SUCCESS && transferred==output.size() &&
        std::ranges::equal(output,std::span(original).subspan(offset,output.size())),"Foreground sample Read changed bytes");
  }
  callback_cleanup_close(provider,context);
  std::ranges::sort(elapsed);
  std::cout<<"foreground_read phase="<<phase<<" count="<<elapsed.size()<<" bytes_per_read="<<output.size()
      <<" p50_ns="<<elapsed[50]<<" p95_ns="<<elapsed[95]<<" p99_ns="<<elapsed[99]<<" max_ns="<<elapsed.back()<<'\n';
}

void replace_callback_file(Provider& provider,unsigned file,const Bytes& replacement) {
  auto name=L"\\f"+std::to_wstring(file);
  auto* fs=&provider.fs;
  PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
  require(fs->Interface->Open(fs,name.data(),open_existing_file,FILE_WRITE_DATA,&context,&info)==STATUS_SUCCESS && context,
      "Selected extent edit Open");
  ULONG transferred=0;
  require(fs->Interface->Write(fs,context,const_cast<std::byte*>(replacement.data()),0,
      static_cast<ULONG>(replacement.size()),FALSE,FALSE,&transferred,&info)==STATUS_SUCCESS &&
      transferred==replacement.size(),"Selected extent edit Write");
  callback_cleanup_close(provider,context);
}

int run_live_anchor_owner(std::string_view mode,std::optional<NativeAnchorSearchOptions> options) {
  const bool installed=!options;
  const bool read_after_verification=mode=="verified-read";
  const bool edit_after_verification=mode=="verified-edit";
  const bool queued=mode=="queued" || mode=="queued-yield" || read_after_verification || edit_after_verification;
  const bool restoring=mode=="restored";
  const bool device_not_ready=mode=="device-not-ready";
  const bool two_planes=mode=="xor-wide" || mode=="xor-narrow";
  const bool xor_encoding=two_planes || mode=="xor" || mode=="xor-cpu";
  const bool packed=mode=="batch" || mode=="batch-yield" || xor_encoding;
  const unsigned files_per_group=queued?queued_anchor_members+1:installed && (packed || restoring)?3:5;
  const unsigned groups=restoring?4:(packed || device_not_ready)?2:1;
  const unsigned initial_files=restoring?2*files_per_group:groups*files_per_group;
  const bool yield=mode=="yield" || mode=="batch-yield" || mode=="queued-yield";
  const auto count=groups*files_per_group;
  const auto before=handle_count();
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  Provider provider;Provider::active=&provider;
  NativeMaintenancePolicy qualification;
  qualification.maximum_cpu_busy_ppm=500000;
  auto adapter=WinFspNativeWritable::create_managed_indexed(
      head,id(90),image,id(1),{kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false},{},qualification);
  std::vector<Bytes> originals;
  originals.reserve(count);
  for(unsigned group=0;group<groups;++group) {
    const auto variant=restoring && group>=2?group-2:group;
    const auto seed=714+variant*5731+(restoring && group<2?99991:0);
    const auto base=data(kNativeExtentBytes,seed);
    for(unsigned i=0;i<files_per_group;++i) {
      auto derived=base;
      if(i && xor_encoding) {
        const auto variation=data(kNativeExtentBytes,1511+group*103+i);
        const auto mask=two_planes?std::byte{0x30}:std::byte{1};
        for(std::size_t at=0;at<base.size();++at)derived[at]^=variation[at]&mask;
      } else if(i && restoring && group<2)derived[kNativeExtentBytes/8+(i+1)*719]^=std::byte{0x67};
      else if(i)derived=anchor_search_fixture::fragmented(base,i);
      if(!two_planes)std::copy_n(base.begin(),kNativeExtentBytes/8,derived.begin());
      originals.push_back(std::move(derived));
    }
  }
  WinFspControllerView result;
  bool signalled=false,restored=false;
  std::array<unsigned,static_cast<std::size_t>(NativeMaintenanceDeferral::capacity)+1> deferrals{};
  std::uint64_t last_quantum=0,settled_ms=0;
  {
    WinFspVolumeController controller(std::move(adapter),provider.api(),plan(),std::move(options));
    await([&]{return controller.view().state==SessionState::running;},5000,"AMD controller startup deadline");
    for(unsigned i=0;i<initial_files;++i)callback_file(provider,i,originals[i]);
    if(read_after_verification) {
      require(!controller.view().anchor_search.submitted,"GPU launched before baseline reads");
      foreground_read_samples(provider,1,originals[1],"before_launch");
    }
    // Each group is staged and published across admitted idle observations.
    const auto started=GetTickCount64();
    const auto deadline=started+(queued?85000ULL:57000ULL)*groups;
    do {
      result=controller.view();
      if(result.maintenance_quanta!=last_quantum) {
        last_quantum=result.maintenance_quanta;
        ++deferrals.at(static_cast<std::size_t>(result.maintenance.deferral));
      }
      if(result.maintenance.published_groups>=groups || result.retired || result.maintenance_disabled ||
          (mode=="stop" && result.installed_anchor_attempts))break;
      if(restored && result.anchor_search.verified &&
          result.maintenance.phase==NativeMaintenancePhase::idle)break;
      if(restoring && !restored && result.maintenance.phase==NativeMaintenancePhase::idle &&
          result.maintenance.sampled_extents>=initial_files && result.maintenance.published_groups<2)break;
      if(restoring && !restored && result.maintenance.published_groups==2) {
        require(result.installed_anchor_attempts==1 && result.installed_anchor_queries==0 &&
            result.installed_anchor_status==HipDiscoveryStatus::package_unavailable && !result.anchor_search.submitted,
            "Unchanged missing inputs triggered a repeated attempt or compute");
        std::cout<<"Unchanged missing package: two CPU groups, one discovery attempt"<<std::endl;
        restore_private_anchor_package();
        restored=true;
        for(unsigned i=initial_files;i<count;++i)callback_file(provider,i,originals[i]);
      }
      if(yield && result.anchor_search.submitted && !signalled) {
        if(queued)foreground_read_samples(provider,1,originals[1],"outstanding_exchange");
        else {
          FSP_FSCTL_VOLUME_INFO info{};
          require(provider.fs.Interface->GetVolumeInfo(&provider.fs,&info)==STATUS_SUCCESS,
              "foreground callback could not run while AMD work was outstanding");
        }
        signalled=true;
      }
      if(!signalled && result.anchor_search.verified && (read_after_verification || edit_after_verification)) {
        if(read_after_verification)foreground_read_samples(provider,1,originals[1],"verified_reply");
        else {
          auto& changed=originals[initial_files-1];
          changed=data(changed.size(),970131);
          replace_callback_file(provider,initial_files-1,changed);
        }
        signalled=true;
      }
      Sleep(10);
    }while(GetTickCount64()<deadline);
    controller.stop_and_join();
    settled_ms=GetTickCount64()-started;
    result=controller.view();
    require(result.retired && result.anchor_search.retirement_confirmed && !result.controller_failed &&
        WorkerExchangeSession::available(),"AMD controller or worker did not retire");
  }
  const auto proved=NativeVolumeHeadView::open(head,id(90),[&](const auto& key)->std::shared_ptr<StoreByteIo>{
    if(key!=id(1))throw std::runtime_error("AMD controller image identity");
    return image;
  });
  const auto& snapshot=proved.filesystem();snapshot.verify_all();
  const auto accepted_files=restored?count:initial_files;
  for(unsigned i=0;i<accepted_files;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
      "AMD controller changed accepted file bytes");
  std::cout<<"anchor_owner mode="<<mode<<" host="<<static_cast<int>(result.host)
      <<" quanta="<<result.maintenance_quanta<<" groups="<<result.maintenance.published_groups
      <<" continued_steps="<<result.continued_preparation_steps<<" settled_ms="<<settled_ms
      <<" declined_groups="<<result.maintenance.declined_groups<<" phase="<<static_cast<int>(result.maintenance.phase)<<" sampled="<<result.maintenance.sampled_extents
      <<" attempted="<<result.maintenance.attempted_groups<<" cpu_limit_ppm="<<qualification.maximum_cpu_busy_ppm
      <<" submitted="<<result.anchor_search.submitted<<" verified="<<result.anchor_search.verified
      <<" submitted_queued_batches="<<result.anchor_search.submitted_queued_batches
      <<" verified_queued_batches="<<result.anchor_search.verified_queued_batches
      <<" dispatches_per_wait="<<result.anchor_search.queued_dispatches_per_wait
      <<" submitted_groups="<<result.anchor_search.submitted_groups<<" verified_groups="<<result.anchor_search.verified_groups
      <<" copy_suggested_ranges="<<result.anchor_search.copy_suggested_ranges
      <<" copy_compared_ranges="<<result.anchor_search.copy_compared_ranges
      <<" copy_suggested_bytes="<<result.anchor_search.copy_suggested_bytes
      <<" copy_compared_bytes="<<result.anchor_search.copy_compared_bytes
      <<" declined="<<result.anchor_search.declined<<" yielded="<<result.anchor_search.yielded
      <<" integrity_failures="<<result.anchor_search.integrity_failures
      <<" empty_xor_batches="<<result.anchor_search.empty_xor_batches
      <<" advised_members="<<result.maintenance.advised_members_published
      <<" installed_attempts="<<result.installed_anchor_attempts
      <<" metadata_queries="<<result.installed_anchor_queries
      <<" installed_status="<<(result.installed_anchor_status?static_cast<int>(*result.installed_anchor_status):-1)
      <<" handles_before="<<before<<" handles_after="<<handle_count()
      <<" retired=true cpu_readback_verified=true deferrals=";
  for(const auto occurrences:deferrals)std::cout<<occurrences<<',';
  std::cout<<'\n';
  if(installed) {
    require(result.installed_anchor_attempts==(restoring?2U:1U) && result.installed_anchor_status &&
        result.installed_anchor_queries<=64,"Installed route did not make the expected bounded attempt");
    if(mode=="stop") {
      require(result.anchor_search.submitted==0,"Stop allowed a post-acquisition compute launch");
      return result.installed_anchor_status==HipDiscoveryStatus::cancelled ||
          result.installed_anchor_status==HipDiscoveryStatus::ready?0:2;
    }
    if(mode=="missing" || mode=="changed" || device_not_ready) {
      const auto status=device_not_ready?HipDiscoveryStatus::device_not_ready:
          mode=="missing"?HipDiscoveryStatus::package_unavailable:HipDiscoveryStatus::worker_untrusted;
      return result.installed_anchor_status==status && !result.installed_anchor_queries && !result.anchor_search.submitted &&
          result.maintenance.published_groups==groups?0:2;
    }
    require(result.installed_anchor_status==HipDiscoveryStatus::ready && result.installed_anchor_queries!=0,
        "Installed package did not acquire a verified route");
  }
  if(xor_encoding) {
    const auto root=PagedObjectStoreSnapshot::open(image,{kStoreMaxPhysicalBytes}).read(1);
    require(root[8]==std::byte{5} && snapshot.shared_base_info().members==count &&
        result.maintenance.published_groups==groups && !result.anchor_search.integrity_failures,
        "packed XOR callback groups or verified worker results differed");
    auto packed_image=std::make_shared<Ram>();packed_image->failure_unit=image->failure_unit;
    (void)NativeFilesystemWriter::compact(snapshot,packed_image,id(3),{StoreCompressionMode::raw,1,false});
    const auto reopened=NativeFilesystemSnapshot::open_image(packed_image,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    for(unsigned i=0;i<count;++i)
      require(reopened.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"compacted XOR group changed data");
    std::cout<<"xor_owner mode="<<mode<<" compacted_bytes="<<packed_image->extent()
        <<" settled_ms="<<settled_ms<<" logical_bytes="<<count*kNativeExtentBytes
        <<" exact_compacted_cpu_readback=true\n";
    if(mode=="xor-cpu")
      return !result.anchor_search.submitted && !result.maintenance.advised_members_published?0:2;
    if(mode=="xor-narrow")
      return result.anchor_search.submitted==1 && result.anchor_search.empty_xor_batches==1 &&
          !result.anchor_search.verified && !result.maintenance.advised_members_published?0:2;
  }
  if(restoring)return restored && result.anchor_search.submitted==1 && result.anchor_search.verified==1 &&
      result.anchor_search.verified_groups==2 && result.maintenance.published_groups>2 &&
      result.maintenance.advised_members_published>0?0:2;
  if(queued)require(result.anchor_search.submitted_queued_batches==1 &&
      (yield?result.anchor_search.verified_queued_batches==0:result.anchor_search.verified_queued_batches==1 &&
          result.anchor_search.queued_dispatches_per_wait==kAnchorProfileMaxQueuedDispatches),
      "Queued dispatch did not match the controller retirement and acceptance outcome");
  if(read_after_verification || edit_after_verification)
    require(signalled,"Foreground activity missed the verified reply before publication");
  if(yield)return signalled && result.anchor_search.yielded==1 && result.anchor_search.submitted_groups==groups &&
      result.maintenance.published_groups==groups && result.maintenance.advised_members_published==0?0:2;
  if(edit_after_verification)
    return result.maintenance.published_groups==groups && result.maintenance.attempted_groups>=2 &&
        result.maintenance.advised_members_published==0 && !result.anchor_search.integrity_failures?0:2;
  return result.anchor_search.submitted==1 && result.anchor_search.verified==1 && result.anchor_search.verified_groups==groups &&
      result.maintenance.published_groups==groups && result.maintenance.advised_members_published==count-groups?0:2;
}

std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point started) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-started).count());
}

bool sampled_bias_reply(std::span<const std::byte> body,std::span<const std::byte> base,
    std::span<const std::byte> member) {
  require(body.size()>=anchor_match::delta_parameters_bytes,"Truncated modular reply");
  const auto word=[](std::span<const std::byte> bytes) {
    std::uint64_t value=0;
    for(std::size_t i=0;i<bytes.size();++i)value|=std::to_integer<std::uint64_t>(bytes[i])<<(8*i);
    return value;
  };
  const auto width=std::to_integer<std::uint32_t>(body.front());
  require(width==1 || width==2 || width==4 || width==8,"Invalid width in modular reply");
  auto first=word(member.first(width))-word(base.first(width));
  if(width<8)first&=(std::uint64_t{1}<<(8*width))-1;
  const auto selected=word(body.subspan(4,8));
  return selected && selected!=first;
}

using anchor_search_fixture::IndexedBatchPattern;
std::optional<IndexedBatchPattern> multiwindow_pattern(std::string_view mode) {
  if(mode=="multiwindow" || mode=="xor-multiwindow")return IndexedBatchPattern::packed_xor;
  if(mode=="modular-multiwindow")return IndexedBatchPattern::modular_delta;
  if(mode=="modular-shifted-bias")return IndexedBatchPattern::shifted_bias;
  if(mode=="xor-reference")return IndexedBatchPattern::xor_reference;
  if(mode=="xor-tile-reference")return IndexedBatchPattern::xor_tile_reference;
  if(mode=="xor-sparse-reference")return IndexedBatchPattern::xor_sparse_reference;
  if(mode=="xor-full-reference" || mode=="xor-full-reference-sparse")return IndexedBatchPattern::xor_full_reference;
  if(mode=="sparse-xor" || mode=="sparse-xor-device")return IndexedBatchPattern::sparse_xor;
  if(mode=="sparse-xor-all-planes" || mode=="sparse-xor-all-planes-device")return IndexedBatchPattern::sparse_xor_all_planes;
  if(mode=="collective" || mode=="collective-sparse" || mode=="collective-delta")return IndexedBatchPattern::collective;
  if(mode=="revision-corpus")return IndexedBatchPattern::revision_corpus;
  if(mode=="raw-revision-corpus")return IndexedBatchPattern::raw_revision_corpus;
  return {};
}

bool revision_corpus(IndexedBatchPattern pattern) noexcept {
  return pattern==IndexedBatchPattern::revision_corpus || pattern==IndexedBatchPattern::raw_revision_corpus;
}

std::vector<Bytes> read_revision_corpus(const std::filesystem::path& path) {
  require(path.is_absolute(),"Expected the absolute revision corpus path");
  const auto file=ReadOnlyFile::open(path);
  require(file.size()>=4 && file.size()<=4*1024*1024,"Revision corpus exceeds fixture memory budget");
  Bytes input(static_cast<std::size_t>(file.size()));
  file.read_exact_at(0,input.data(),input.size());
  std::span<const std::byte> remaining(input);
  const auto read_size=[&] {
    require(remaining.size()>=sizeof(std::uint32_t),"Truncated revision corpus header");
    std::uint32_t value=0;
    for(unsigned i=0;i<sizeof(value);++i)value|=std::to_integer<std::uint32_t>(remaining[i])<<(8*i);
    remaining=remaining.subspan(sizeof(value));
    return value;
  };
  const auto count=read_size();
  require(count>0 && count<=64,"Revision corpus has an invalid file count");
  std::vector<Bytes> files;
  files.reserve(count);
  for(std::uint32_t i=0;i<count;++i) {
    const auto bytes=read_size();
    require(bytes>0 && bytes<=256*1024 && bytes<=remaining.size(),"Invalid revision corpus file extent");
    const auto data=remaining.first(bytes);
    files.emplace_back(data.begin(),data.end());
    remaining=remaining.subspan(bytes);
  }
  require(remaining.empty(),"Revision corpus contains trailing bytes");
  return files;
}

struct RevisionReadProfile {
  std::uint64_t full_logical=0,full_backing=0,sampled_logical=0,sampled_backing=0;
};

RevisionReadProfile profile_revision_reads(const NativeFilesystemSnapshot& snapshot,
    const Ram& backing,std::span<const Bytes> files) {
  RevisionReadProfile result;
  auto read_start=backing.read_bytes;
  for(std::size_t i=0;i<files.size();++i) {
    const auto path="f"+std::to_string(i);
    require(snapshot.read(path,0,files[i].size())==files[i],"Whole revision read differs");
    result.full_logical+=files[i].size();
  }
  result.full_backing=backing.read_bytes-read_start;
  read_start=backing.read_bytes;
  for(std::size_t i=0;i<files.size();++i) {
    const auto path="f"+std::to_string(i);
    const auto count=(std::min)(files[i].size(),std::size_t{4096});
    const auto last=files[i].size()-count;
    for(const auto offset:{std::size_t{0},last/2,last}) {
      const auto bytes=snapshot.read(path,offset,count);
      require(std::ranges::equal(bytes,std::span(files[i]).subspan(offset,count)),
          "Sampled revision read differs");
      result.sampled_logical+=count;
    }
  }
  result.sampled_backing=backing.read_bytes-read_start;
  return result;
}

int live_multiwindow_anchor(std::optional<NativeAnchorSearchOptions> options,
    const std::filesystem::path& bank_path,std::uint64_t discovery_ns,IndexedBatchPattern pattern) {
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  const bool revision=revision_corpus(pattern);
  const bool shifted_bias=pattern==IndexedBatchPattern::shifted_bias;
  const bool tile_reference=pattern==IndexedBatchPattern::xor_tile_reference;
  const bool sparse_reference=pattern==IndexedBatchPattern::xor_sparse_reference;
  const bool full_reference=pattern==IndexedBatchPattern::xor_full_reference;
  const bool all_planes=pattern==IndexedBatchPattern::sparse_xor_all_planes;
  const bool sparse_xor=pattern==IndexedBatchPattern::sparse_xor || all_planes || sparse_reference || full_reference;
  const bool collective=pattern==IndexedBatchPattern::collective;
  const bool sparse_device=options && options->output==anchor_match::BatchOutput::sparse_xor;
  const bool collective_device=options && options->output==anchor_match::BatchOutput::collective;
  const bool reference_device=options && options->output==anchor_match::BatchOutput::reference_xor;
  const bool dense_overflow=options && all_planes && !sparse_device;
  const bool alternate_reference=pattern==IndexedBatchPattern::xor_reference || tile_reference;
  const bool modular=shifted_bias || pattern==IndexedBatchPattern::modular_delta;
  const auto members=anchor_search_fixture::small_indexed_batch(pattern)?families*(anchor_search_fixture::indexed_small_extents_per_family-1):
      anchor_search_fixture::indexed_batch_members;
  const auto compression=pattern==IndexedBatchPattern::revision_corpus?
      StoreCompressionOptions{StoreCompressionMode::automatic,6,true}:
      StoreCompressionOptions{StoreCompressionMode::raw,1,false};
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  // Match the multiwindow CPU fixture's modeled device geometry. Leave room
  // in the 64 MiB image for both live records and independently committed COW.
  head->failure_unit=image->failure_unit=4096;
  std::optional<NativeFileSession> session(NativeFileSession::create_indexed_volume(
      head,id(90),image,id(1),{kStoreMaxPhysicalBytes},compression));
  Bytes bank;
  if(!revision && !bank_path.empty()) {
    require(bank_path.is_absolute() && std::filesystem::file_size(bank_path)==anchor_search_fixture::indexed_sample_bank_bytes,
        "Expected the bounded public sample bank");
    std::ifstream input(bank_path,std::ios::binary);
    bank.resize(anchor_search_fixture::indexed_sample_bank_bytes);
    require(bool(input.read(reinterpret_cast<char*>(bank.data()),static_cast<std::streamsize>(bank.size()))) &&
        input.peek()==std::char_traits<char>::eof(),"Public sample bank changed");
  }
  const auto originals=revision?read_revision_corpus(bank_path):anchor_search_fixture::indexed_batch_files(bank,pattern);
  std::vector<std::byte> file_digests;
  file_digests.reserve(32*originals.size());
  for(std::size_t i=0;i<originals.size();++i) {
    const auto digest=detail::record_digest(originals[i]);
    file_digests.insert(file_digests.end(),digest.begin(),digest.end());
    const auto path="f"+std::to_string(i);
    session->create_entry(path);
    const auto handle=session->open(path,{true,true,false});
    session->write(handle,0,originals[i]);
    session->cleanup(handle,false);session->close(handle);
  }
  const auto resolve=[&](const auto& key)->std::shared_ptr<StoreByteIo> {return key==id(1)?image:nullptr;};
  std::uint64_t baseline_bytes=0;
  RevisionReadProfile baseline_reads;
  session.reset();
  {
    const auto closed=NativeVolumeHeadView::open(head,id(90),resolve);
    auto copy=std::make_shared<Ram>();copy->failure_unit=image->failure_unit;
    (void)NativeFilesystemWriter::compact(closed.filesystem(),copy,id(2),compression);
    baseline_bytes=copy->extent();
    if(revision) {
      const auto reopened=NativeFilesystemSnapshot::open_image(copy,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
      reopened.verify_all();
      baseline_reads=profile_revision_reads(reopened,*copy,originals);
    }
  }
  session.emplace(NativeFileSession::resume_closed_indexed_volume(head,id(90),1,resolve,compression));
  const auto maintenance_start=std::chrono::steady_clock::now();
  const NativeMaintenancePolicy policy;
  NativeGroupMaintenance maintenance(*session,policy);
  std::unique_ptr<NativeAnchorSearchController> anchor;
  if(options)anchor=std::make_unique<NativeAnchorSearchController>(std::move(*options));
  if(anchor && !revision && bank.empty() && !shifted_bias && !alternate_reference && !sparse_xor && !collective) {
    const auto now=GetTickCount64();
    require(!anchor->stage_grant(now,members) && anchor->stage_grant(now,members+families+1)==members+families,
        "Wide route used a legacy cap or ignored its funded range");
  }
  NativeMaintenanceProgress progress;
  std::uint64_t sequence=0,worker_ns=0;
  std::uint64_t packed_payload_bytes=0,sampled_bias_members=0;
  std::size_t sparse_bodies=0,delta_bodies=0;
  unsigned packed_widths=0;
  for(unsigned i=0;i<5200 && maintenance.pending();++i) {
    MEMORYSTATUSEX host{};host.dwLength=sizeof(host);
    require(GlobalMemoryStatusEx(&host)!=0,"Cannot confirm current memory headroom");
    // Model idle time to exercise the larger batch quickly. HIP admission and
    // the physical and commit observations still use the real host.
    NativeMaintenanceObservation observation{++sequence,1000*sequence,host.ullTotalPhys,host.ullAvailPhys,
        host.ullTotalPageFile,host.ullAvailPageFile,1000,0,1000,true,false};
    const auto resources=detail::maintenance_resources(observation,observation.captured_ms,policy);
    require(resources.has_value(),"Cannot establish a modeled opportunity with live memory");
    const auto funded=detail::maintenance_group_member_limit(*resources,true);
    const auto grant=anchor?anchor->stage_grant(GetTickCount64(),funded):0;
    const auto replies=anchor?anchor->replies():std::span<const NativeSharedBaseSearchReply>{};
    progress=maintenance.advance(observation,observation.captured_ms,{grant,nullptr,anchor!=nullptr,replies});
    if(progress.phase==NativeMaintenancePhase::failed || progress.deferral==NativeMaintenanceDeferral::memory ||
        progress.deferral==NativeMaintenanceDeferral::capacity)
      std::cerr<<"multiwindow refusal phase="<<static_cast<unsigned>(progress.phase)
          <<" deferral="<<static_cast<unsigned>(progress.deferral)<<" sampled="<<progress.sampled_extents
          <<" published="<<progress.published_groups<<" declined="<<progress.declined_groups
          <<" image_bytes="<<image->extent()<<" physical_available="<<host.ullAvailPhys
          <<" commit_available="<<host.ullAvailPageFile<<'\n';
    require(progress.phase!=NativeMaintenancePhase::failed && progress.deferral!=NativeMaintenanceDeferral::memory &&
        progress.deferral!=NativeMaintenanceDeferral::capacity,"Large batch preparation or publication failed");
    if(!anchor)continue;
    anchor->settle_reply_checks();
    if(!maintenance.has_retained_search())anchor->discard_reply();
    if(!progress.search_batch_ready)continue;
    const auto windows=maintenance.search_batch();
    const auto started=std::chrono::steady_clock::now();
    anchor->start(windows,1);
    await([&] {
      anchor->observe(1,GetTickCount64(),true);
      return !anchor->waiting();
    },8000,"Multiwindow AMD worker did not retire");
    worker_ns+=elapsed_ns(started);
    if(modular || alternate_reference || sparse_xor || collective)for(const auto& reply:anchor->replies()) {
      require(reply.packed_bodies.size()==reply.window.members().size(),"Unbound packed reply");
      for(std::size_t member=0;member<reply.packed_bodies.size();++member) {
        auto body=std::span<const std::byte>(reply.packed_bodies[member]);
        if(body.empty())continue;
        packed_payload_bytes+=body.size();
        auto method=reply.body_method;
        if(collective_device) {
          const auto selected=detail::parse_collective_group_body(body);
          require(selected.has_value(),"Expected typed collective device advice");
          method=selected->method;
          body=selected->bytes;
        }
        if(method==NativePackedGroupMethod::sparse_xor)++sparse_bodies;
        if(method==NativePackedGroupMethod::modular_delta) {
          ++delta_bodies;
          packed_widths|=std::to_integer<unsigned>(body.front());
          if(sampled_bias_reply(body,reply.window.base(),reply.window.members()[member]))++sampled_bias_members;
        }
      }
    }
  }
  const auto stats=anchor?anchor->stats():NativeAnchorSearchStats{};
  const bool retired=(!anchor || anchor->stop()) && WorkerExchangeSession::available();
  require(!maintenance.pending() && (revision || progress.published_groups) && retired && !stats.integrity_failures,
      "Large batch lost protected publication or ownership");
  if(dense_overflow) {
    require(stats.submitted==1 && stats.empty_xor_batches==1 && !stats.verified &&
        !progress.advised_members_published,"Dense slots unexpectedly handled all active planes");
  }else if(anchor && shifted_bias) {
    std::cout<<"shifted_bias_reply submitted="<<stats.submitted<<" verified="<<stats.verified
        <<" empty="<<stats.empty_xor_batches<<" published="<<progress.advised_members_published<<'\n';
    require(stats.submitted && (stats.verified || stats.empty_xor_batches || packed_payload_bytes),
        "Shifted-bias comparison lacked a completed AMD exchange");
  } else if(anchor && !revision) {
    require(stats.verified && progress.advised_members_published,"Large batch lost verified advice");
  }
  if(anchor && !revision && bank.empty() && !modular && !dense_overflow && !collective)
    require(stats.submitted==1 && stats.verified_groups==families &&
        (tile_reference || sparse_xor || progress.advised_members_published==members),
        "Related AMD work did not fill one verified multiwindow batch");
  if(tile_reference || sparse_xor)
    require(progress.published_groups==families,"Packed XOR did not group the complete cohort");
  if(anchor && modular && !shifted_bias)
    require(packed_widths==15 && packed_payload_bytes,"AMD modular batch did not cover all word widths");
  if(collective_device)
    require(stats.submitted==1 && sparse_bodies && delta_bodies && stats.verified_groups==families,
        "The shared AMD exchange did not verify both codec choices");
  if(reference_device)
    require(stats.reference_batches==1 && stats.reselected_groups==families && stats.verified_groups==families,
        "AMD full reference search did not verify each improved group");
  if(full_reference && !anchor)
    require(progress.cpu_reference_pairs && progress.reselected_reference_groups==families,
        "CPU full reference search did not improve each sampled reference");
  const auto maintenance_ns=elapsed_ns(maintenance_start);

  const auto verify_start=std::chrono::steady_clock::now();
  maintenance.cancel();
  session.reset();
  const auto closed=NativeVolumeHeadView::open(head,id(90),resolve);
  auto packed=std::make_shared<Ram>();packed->failure_unit=image->failure_unit;
  (void)NativeFilesystemWriter::compact(closed.filesystem(),packed,id(3),compression);
  const auto reopened=NativeFilesystemSnapshot::open_image(packed,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
  reopened.verify_all();
  RevisionReadProfile grouped_reads;
  if(revision)grouped_reads=profile_revision_reads(reopened,*packed,originals);
  else for(std::size_t i=0;i<originals.size();++i)
    require(reopened.read("f"+std::to_string(i),0,originals[i].size())==originals[i],
        "Large AMD batch changed independently reopened file bytes");
  const auto compact_verify_ns=elapsed_ns(verify_start);
  require(revision || packed->extent()<baseline_bytes,"Multiwindow maintenance did not reduce complete compacted size");
  const auto root=PagedObjectStoreSnapshot::open(packed,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  if(sparse_xor || collective)require(capability==7,"Sparse XOR did not survive compaction");
  if(revision) {
    std::cout<<"revision_reads zstd="<<(compression.mode==StoreCompressionMode::automatic?store_zstd_version():0)
        <<" full_logical="<<grouped_reads.full_logical<<" full_input_backing="<<baseline_reads.full_backing
        <<" full_grouped_backing="<<grouped_reads.full_backing<<" sampled_logical="<<grouped_reads.sampled_logical
        <<" sampled_input_backing="<<baseline_reads.sampled_backing<<" sampled_grouped_backing="<<grouped_reads.sampled_backing<<'\n';
  }
  const auto cohort=revision?"revision-corpus":collective?(bank.empty()?"collective-synthetic":"collective-public"):
      full_reference?(bank.empty()?"xor-full-reference-synthetic":"xor-full-reference-public"):
      sparse_reference?(bank.empty()?"xor-sparse-reference-synthetic":"xor-sparse-reference-public"):
      all_planes?(bank.empty()?"sparse-xor-all-planes-synthetic":"sparse-xor-all-planes-public"):
      sparse_xor?(bank.empty()?"sparse-xor-synthetic":"sparse-xor-public"):
      tile_reference?(bank.empty()?"xor-tile-reference-synthetic":"xor-tile-reference-public"):
      alternate_reference?(bank.empty()?"xor-reference-synthetic":"xor-reference-public"):
      shifted_bias?(bank.empty()?"shifted-bias-synthetic":"shifted-bias-public"):
      modular?(bank.empty()?"modular-synthetic":"modular-public"):(bank.empty()?"related":"mixed");
  std::cout<<"multiwindow_trial mode="<<(anchor?"amd":"cpu")<<" cohort="<<cohort
      <<" sparse_device="<<sparse_device<<" reply_bytes="<<stats.reply_bytes
      <<" copy_suggested_ranges="<<stats.copy_suggested_ranges<<" copy_compared_ranges="<<stats.copy_compared_ranges
      <<" copy_suggested_bytes="<<stats.copy_suggested_bytes<<" copy_compared_bytes="<<stats.copy_compared_bytes
      <<" files="<<originals.size()<<" groups="<<progress.published_groups<<" advised_members="<<progress.advised_members_published
      <<" capability="<<capability
      <<" submitted="<<stats.submitted<<" verified_batches="<<stats.verified<<" worker_ns="<<worker_ns
      <<" submitted_queued_batches="<<stats.submitted_queued_batches
      <<" verified_queued_batches="<<stats.verified_queued_batches<<" dispatches_per_wait="<<stats.queued_dispatches_per_wait
      <<" queued_worker_wall_ns="<<stats.queued_worker_wall_ns<<" queued_kernel_wait_ns="<<stats.queued_kernel_wait_ns
      <<" empty_batches="<<stats.empty_xor_batches
      <<" discovery_ns="<<discovery_ns<<" maintenance_ns="<<maintenance_ns<<" compact_verify_ns="<<compact_verify_ns
      <<" input_compacted_bytes="<<baseline_bytes<<" grouped_compacted_bytes="<<packed->extent()
      <<" packed_widths="<<packed_widths<<" packed_payload_bytes="<<packed_payload_bytes
      <<" sampled_bias_members="<<sampled_bias_members
      <<" sparse_bodies="<<sparse_bodies<<" delta_bodies="<<delta_bodies
      <<" reference_batches="<<stats.reference_batches<<" reselected_groups="<<stats.reselected_groups
      <<" cpu_reference_pairs="<<progress.cpu_reference_pairs<<" published_references_changed="<<progress.reselected_reference_groups
      <<" retired=true independent_cpu_readback=true modeled_idle_time=true input_blake3=";
  constexpr char digits[]="0123456789abcdef";
  for(const auto byte:detail::record_digest(file_digests)) {
    const auto value=std::to_integer<unsigned>(byte);
    std::cout<<digits[value>>4]<<digits[value&15];
  }
  std::cout<<'\n';
  return 0;
}

int live_anchor_owner(int argc,char** argv) {
  const std::string_view mode(argv[6]);
  const auto multiwindow=multiwindow_pattern(mode);
  const bool queued=mode=="queued" || mode=="queued-yield" || mode=="verified-read" || mode=="verified-edit" || argc==9;
  const bool xor_mode=mode=="xor" || mode=="xor-wide" || mode=="xor-narrow";
  require(mode=="accept" || mode=="yield" || mode=="both" || mode=="batch" || mode=="batch-yield" || queued ||
      xor_mode || multiwindow,
      "Expected an AMD ownership trial");
  require(argc!=9 || (multiwindow && revision_corpus(*multiwindow) && std::string_view(argv[8])=="queued"),
      "A queued comparison needs a revision corpus");
  const ProcessBudget profile{128,2000,5000,16};
  HipRouteDiscoveryRequest request;
  request.helper=argv[2];request.helper_sha256=artifact_digest(argv[3]);
  request.anchor_module_sha256=artifact_digest(argv[4]);request.runtime_basename=std::filesystem::path(argv[5]).wstring();
  request.worker=profile;request.maximum_identity_queries=4;
  const auto discovery_start=std::chrono::steady_clock::now();
  const auto binding=discover_system_hip_route(request);
  const auto discovery_ns=elapsed_ns(discovery_start);
  require(binding.status==HipDiscoveryStatus::ready && binding.route && binding.route->pins->anchor_module_pinned() &&
      !binding.route->pins->uniform_module_pinned(),"Shared-base worker route was not authenticated");
  if(multiwindow) {
    constexpr auto families=anchor_search_fixture::indexed_batch_families;
    const bool small_batch=anchor_search_fixture::small_indexed_batch(*multiwindow);
    const bool modular=*multiwindow==IndexedBatchPattern::modular_delta || *multiwindow==IndexedBatchPattern::shifted_bias ||
        mode=="collective-delta";
    const bool sparse=mode=="sparse-xor-device" || mode=="sparse-xor-all-planes-device" || mode=="collective-sparse" ||
        mode=="xor-sparse-reference" || mode=="xor-full-reference-sparse";
    const auto members=small_batch?families*(anchor_search_fixture::indexed_small_extents_per_family-1):
        anchor_search_fixture::indexed_batch_members;
    const auto minimum=argc>=8 || small_batch?2U:static_cast<std::uint32_t>(members);
    const auto output=revision_corpus(*multiwindow)?anchor_match::BatchOutput::anchors:
        mode=="xor-full-reference"?anchor_match::BatchOutput::reference_xor:
        mode=="collective"?anchor_match::BatchOutput::collective:sparse?anchor_match::BatchOutput::sparse_xor:
        modular?anchor_match::BatchOutput::modular_delta:anchor_match::BatchOutput::wide_packed_xor;
    return live_multiwindow_anchor(NativeAnchorSearchOptions{*binding.route,profile,minimum,
        static_cast<std::uint32_t>(members+families),10000,
        output,small_batch?4U:modular?16U:0U,queued?kAnchorProfileMaxQueuedDispatches:1U},
        argc>=8?std::filesystem::path(argv[7]):std::filesystem::path{},
        discovery_ns,*multiwindow);
  }
  const std::uint32_t members=queued?queued_anchor_members:(mode=="batch" || mode=="batch-yield" || xor_mode)?8:4;
  NativeAnchorSearchOptions execution{*binding.route,profile,members,members};
  if(queued)execution.dispatches_per_wait=kAnchorProfileMaxQueuedDispatches;
  if(xor_mode)execution.output=mode=="xor-wide"?anchor_match::BatchOutput::wide_packed_xor:
      anchor_match::BatchOutput::packed_xor;
  if(mode!="both")return run_live_anchor_owner(mode,execution);
  const auto accepted=run_live_anchor_owner("accept",execution);
  if(accepted)return accepted;
  const auto warmed=handle_count();
  const auto yielded=run_live_anchor_owner("yield",execution);
  const auto retired=handle_count();
  std::cout<<"anchor_owner_repeated handles_between="<<warmed<<" handles_after="<<retired<<'\n';
  require(warmed==retired,"Repeated AMD controllers retained handles");
  return yielded;
}
#endif
void event_ownership() {
  Fixture f;Provider p;Provider::active=&p;
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"test event");
  f.adapter->set_maintenance_event(event);
  bool rejected=false;try{f.adapter->set_maintenance_event(event);}catch(const std::invalid_argument&){rejected=true;}
  require(rejected,"maintenance event replacement accepted");
  require(WaitForSingleObject(event,0)==WAIT_TIMEOUT,"event signaled before callback");
  f.adapter->bind_callbacks(p.fs,Provider::origin);p.fs.Interface=&WinFspNativeWritable::callbacks();
  FSP_FSCTL_VOLUME_INFO info{};
  require(p.fs.Interface->GetVolumeInfo(&p.fs,&info)==STATUS_SUCCESS && WaitForSingleObject(event,0)==WAIT_OBJECT_0,
      "callback activity did not signal");
  CloseHandle(event); // Adapter owns a separate noninheritable signal handle.
  require(p.fs.Interface->GetVolumeInfo(&p.fs,&info)==STATUS_SUCCESS && !f.adapter->maintenance_event_failed(),
      "adapter used borrowed closed wait handle");
  HANDLE late=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(late!=nullptr,"late test event");
  Fixture other;other.adapter->bind_callbacks(p.fs,Provider::origin);rejected=false;
  try{other.adapter->set_maintenance_event(late);}catch(const std::invalid_argument&){rejected=true;}
  CloseHandle(late);require(rejected,"bound adapter accepted a new signal handle");
}
void lifecycle() {
  for(bool failure:{false,true}) {
    Provider p;Provider::active=&p;p.fail_mount=failure;Fixture f;
    WinFspVolumeController controller(std::move(f.adapter),p.api(),plan());
    await([&]{const auto v=controller.view();return v.state==SessionState::running||v.retired;},5000,"controller start deadline");
    if(!failure) {
      p.release_stop=false;StopRelease release{p};controller.request_stop();
      await([&]{return p.stopping.load();},5000,"controller did not enter stop");
      require(!controller.view().retired,"controller exposed retirement before dispatcher joined");
      p.release_stop=true;controller.stop_and_join();
    } else controller.stop_and_join();
    const auto v=controller.view();require(v.retired && p.callback_alive && p.fs.UserContext==nullptr,"controller failed to retire ownership");
    require(std::string(p.order.data(),p.calls.load())==(failure?"pcsmtd":"pcsmtrd"),"controller release order");
    require(v.start_status==(failure?STATUS_OBJECT_NAME_COLLISION:STATUS_SUCCESS),"controller start result");
    controller.stop_and_join(); // Idempotent serialized owner call.
  }
  Provider p;Provider::active=&p;Fixture f;
  {
    WinFspVolumeController controller(std::move(f.adapter),p.api(),plan());
    await([&]{return controller.view().state==SessionState::running;},5000,"destructor test start deadline");
  }
  require(p.callback_alive && p.fs.UserContext==nullptr && p.order[p.calls.load()-1]=='d',"destructor left an owner worker");
}
void idle_until_owner_stop(DWORD idle_ms) {
  Provider p;Provider::active=&p;
  auto head=std::make_shared<Ram>();
  auto image=std::make_shared<Ram>();
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),
      {kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false});
  WinFspVolumeController controller(std::move(adapter),p.api(),plan());
  await([&]{return controller.view().state==SessionState::running;},5000,"idle controller start deadline");
  const auto start=GetTickCount64();
  Sleep(idle_ms);
  const auto elapsed=GetTickCount64()-start;
  const auto live=controller.view();
  require(live.state==SessionState::running && !live.retired && !p.stopping.load() && p.calls.load()==4,
      "idle controller stopped without its owner");
  require(live.maintenance_quanta==0 && live.gpu_budget_queries==0,"idle controller consumed resource work");
  controller.stop_and_join();
  const auto retired=controller.view();
  require(retired.state==SessionState::stopped && retired.retired && !retired.offline &&
      !retired.controller_failed && p.calls.load()==7 && p.fs.UserContext==nullptr,
      "idle controller failed to retire on the owner signal");
  std::cout<<"idle_owner_hold_ms="<<elapsed<<" maintenance_quanta="<<live.maintenance_quanta
      <<" gpu_budget_queries="<<live.gpu_budget_queries<<" stopped_and_joined=true\n";
}
void invalid_and_unexpected() {
  Provider p;Provider::active=&p;Fixture invalid;bool rejected=false;auto bad=plan();bad.allow_native_writes=false;
  try{WinFspVolumeController controller(std::move(invalid.adapter),p.api(),bad);}
  catch(const std::invalid_argument&){rejected=true;}
  require(rejected && p.calls.load()==0,"invalid controller plan reached a provider");
  Fixture f;WinFspVolumeController controller(std::move(f.adapter),p.api(),plan());
  await([&]{return controller.view().state==SessionState::running;},5000,"unexpected-stop start deadline");
  p.fs.Interface->DispatcherStopped(&p.fs,FALSE);
  await([&]{return controller.view().retired;},5000,"unexpected dispatcher stop did not wake controller");
  const auto v=controller.view();controller.stop_and_join();
  require(v.state==SessionState::failed && !v.controller_failed && p.fs.UserContext==nullptr,
      "unexpected stop was concealed or did not release ownership");
}
void live_opportunities() {
  Provider p;Provider::active=&p;Fixture f;const auto head=f.head,image=f.image;
  // Complete only the empty model before exposure. The actual populated-volume
  // opportunities below use the real Windows collector, never this clock.
  NativeMaintenanceObservation model;model.valid=true;model.physical_total=16ULL<<30;
  model.physical_available=12ULL<<30;model.commit_limit=32ULL<<30;model.commit_available=24ULL<<30;
  model.cpu_total_ticks=1000;model.cpu_window_ms=1000;
  for(unsigned i=0;i<1000 && f.adapter->background_maintenance_pending();++i) {
    model.sequence=i+1;model.captured_ms=(i+1)*1000;
    f.adapter->advance_background_maintenance(model,model.captured_ms);
  }
  require(!f.adapter->background_maintenance_pending(),"empty modeled setup did not settle");
  WinFspVolumeController controller(std::move(f.adapter),p.api(),plan());
  await([&]{return controller.view().state==SessionState::running;},5000,"live controller start deadline");
  auto before=controller.view();Sleep(1150);auto after=controller.view();
  require(before.maintenance_quanta==0 && after.maintenance_quanta==0,"unchanged volume kept sampling");
  auto first=data(kNativeExtentBytes,971),second=first;second[731]^=std::byte{1};
  callback_file(p,0,first);callback_file(p,1,second);
  await([&]{const auto v=controller.view();return v.maintenance_quanta>after.maintenance_quanta || v.maintenance_disabled;},5000,
      "callback content did not wake controller");
  // Live host load is authoritative. Busy/low-memory hosts must defer rather
  // than manufacture idle evidence; deterministic grouping is separately tested.
  const auto until=GetTickCount64()+18000;
  while(GetTickCount64()<until) {
    const auto v=controller.view();
    if(v.maintenance.published_groups || v.maintenance_disabled || v.retired)break;
    Sleep(25);
  }
  const auto observed=controller.view();controller.stop_and_join();
  require(controller.view().retired,"live controller not joined");
  auto cpu=NativeVolumeHeadView::open(head,id(90),[&](const auto& key)->std::shared_ptr<StoreByteIo>{
    if(key!=id(1))throw std::runtime_error("controller image identity");return image;});
  const auto& snapshot=cpu.filesystem();snapshot.verify_all();
  require(snapshot.read("f0",0,first.size())==first && snapshot.read("f1",0,second.size())==second,"live controller lost accepted bytes");
  require(snapshot.workload_info().committed_input_bytes==2*kNativeExtentBytes,"controller minted accepted input");
  if(observed.maintenance.published_groups)require(snapshot.shared_base_info().members==2,"reported live publication not retained");
  std::cout<<"live_host host="<<static_cast<int>(observed.host)<<" disabled="<<observed.maintenance_disabled
      <<" quanta="<<observed.maintenance_quanta<<" continued_steps="<<observed.continued_preparation_steps
      <<" groups="<<observed.maintenance.published_groups
      <<" maximum_quantum_ns="<<observed.maximum_quantum_ns<<'\n';
}
void memory_planning() {
  constexpr std::uint64_t mib=1024ULL*1024;
  for(const auto& host:std::array<MemoryVolumeObservation,4>{{
      {4*1024*mib,3072*mib,8*1024*mib,6*1024*mib},
      {16*1024*mib,12*1024*mib,32*1024*mib,24*1024*mib},
      {64*1024*mib,56*1024*mib,128*1024*mib,11*1024*mib},
      {UINT64_MAX,UINT64_MAX,UINT64_MAX,UINT64_MAX}}}) {
    const auto layout=plan_memory_volume(host);
    const auto addressable=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    require(layout.domain_capacity==(std::min)(usable_memory_volume_headroom(host),addressable)/65536*65536 &&
        layout.image_capacity>=3*kStoreCommitUnit && layout.image_capacity%65536==0,"adaptive factory plan bounds");
    const auto indexed=plan_memory_volume(host,true);
    const auto areas=(NativeFileSession::max_optimization_bindings+1)*layout.area_capacity;
    require(2*layout.image_capacity+areas<=layout.domain_capacity && !layout.coherence_capacity &&
        indexed.domain_capacity==layout.domain_capacity && indexed.coherence_capacity>=3*kStoreCommitUnit &&
        indexed.coherence_capacity%kStoreCommitUnit==0 && indexed.image_capacity>=3*kStoreCommitUnit &&
        2*indexed.image_capacity+areas+2*indexed.coherence_capacity<=indexed.domain_capacity,
        "factory omitted candidate or optional indexed advisory reservations");
  }
  const auto roomy=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,24*1024*mib});
  const auto tight=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,5*1024*mib});
  const auto larger=plan_memory_volume({64*1024*mib,56*1024*mib,128*1024*mib,96*1024*mib});
  const auto minimal=plan_memory_volume({4*1024*mib,3072*mib,8*1024*mib,769*mib});
  require(tight.domain_capacity<roomy.domain_capacity && larger.domain_capacity>40*mib &&
      larger.image_capacity>roomy.image_capacity && minimal.image_capacity==3*kStoreCommitUnit &&
      !plan_memory_volume({4*1024*mib,3072*mib,8*1024*mib,769*mib},true).coherence_capacity,
      "factory sizing did not use available address space and format minima");
  for(const auto& host:std::array<MemoryVolumeObservation,5>{{{}, {1024*mib,512*mib,2048*mib,1024*mib},
      {1024,1025,4096,4096},{4096,4096,1024,1025},{4*1024*mib,3072*mib,8*1024*mib,768*mib}}}) {
    bool refused=false;try{(void)plan_memory_volume(host);}catch(const std::exception&){refused=true;}
    require(refused,"invalid/low headroom admitted a factory plan");
  }
}
void memory_paged_footprint() {
  auto backing=MemoryVolumeBacking::create();
  const auto run=[&](const std::shared_ptr<StoreByteIo>& head,const std::shared_ptr<StoreByteIo>& image) {
    auto session=NativeFileSession::create_indexed_volume(head,backing.volume_identity,image,
        backing.image_identity,{backing.layout.image_capacity},{StoreCompressionMode::raw,1,false});
    session.create_entry("tiny");
    const auto file=session.open("tiny",{true,true,true});
    Bytes expected;
    for(unsigned i=0;i<12;++i) {
      expected=data(37,i);
      session.write(file,0,expected);
    }
    session.cleanup(file,false);
    session.close(file);
    const auto checked=NativeVolumeHeadView::open(head,backing.volume_identity,
        [&](const auto& id) {
          require(id==backing.image_identity,"indexed head resolved a different native memory store");
          return image;
        });
    require(checked.filesystem().read("tiny",0,100)==expected,
        "compared memory layout changed acknowledged bytes");
    return checked.filesystem().storage_info().physical_end;
  };
  const auto old_layout=run(std::make_shared<Ram>(),std::make_shared<Ram>());
  const auto selected=run(backing.head,backing.image);
  const auto page=backing.image->contract().write_failure_unit_bytes;
  require(page>=kStoreCommitUnit?selected==old_layout:selected<old_layout,
      "live page isolation did not reduce the selected physical extent");
  backing.domain->verify_resident();
  std::cout<<"memory_metadata_fixture page_bytes="<<page<<" prior_extent="<<old_layout
           <<" selected_extent="<<selected<<" resident="<<backing.domain->usage().resident<<'\n';
}

void memory_coherence_lifecycle() {
  auto backing=MemoryVolumeBacking::create(true);
  const auto volume=backing.volume_identity,image=backing.image_identity;
  const auto fixture_catalog_limit=2*kStoreCommitUnit+7ULL*backing.image->contract().write_failure_unit_bytes;
  require(backing.layout.coherence_capacity>=fixture_catalog_limit,"insufficient catalog fixture capacity");
  backing.layout.coherence_capacity=fixture_catalog_limit;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  std::optional<NativeFileSession> session(NativeFileSession::create_indexed_volume(backing.head,volume,
      backing.image,image,{backing.layout.image_capacity},raw));
  require(session->create_budgeted_optimization_area(backing.advice).phase==NativeOptimizationPhase::ready,
      "indexed memory catalog needs the funded journal");
  const auto initial=backing.domain->usage();
  backing.configure_coherence(*session);
  require(backing.domain->usage().stores==initial.stores && backing.domain->usage().resident==initial.resident,
      "mount-time configuration committed optional catalog space");
  const auto put=[&](std::string_view name,const Bytes& bytes) {
    session->create_entry(name);
    const auto file=session->open(name,{true,true,true});
    session->write(file,0,bytes);
    session->cleanup(file,false);session->close(file);
  };
  const auto first=data(kNativeExtentBytes,119),second=data(kNativeExtentBytes,771);
  put("first",first);put("second",second);
  put("input-a",Bytes(3*kOptimizationGroupScanInputBytes/4));
  put("input-b",Bytes(3*kOptimizationGroupScanInputBytes/4));
  put("input-c",Bytes(3*kOptimizationGroupScanInputBytes/4));
  const auto input_resident=backing.domain->usage().resident;
  std::uint64_t sequence=0;
  const auto scan=[&] {
    NativeGroupMaintenance controller(*session);
    for(unsigned i=0;i<2000 && controller.pending();++i) {
      NativeMaintenanceObservation o;
      o.sequence=++sequence;o.captured_ms=1000*sequence;
      o.physical_total=16ULL<<30;o.physical_available=12ULL<<30;
      o.commit_limit=32ULL<<30;o.commit_available=24ULL<<30;
      o.cpu_total_ticks=1000;o.cpu_busy_ticks=0;o.cpu_window_ms=1000;o.valid=true;
      require(controller.advance(o,o.captured_ms).phase!=NativeMaintenancePhase::failed,"memory catalog scan failed");
    }
    require(!controller.pending(),"memory catalog scan failed to quiesce");
    return controller.progress();
  };
  require(scan().checkpoints_published==1,"memory scanner did not fund the catalog");
  const auto catalog=backing.domain->usage();
  require(catalog.stores==initial.stores+1 && catalog.reserved==initial.reserved+backing.layout.coherence_capacity &&
      catalog.resident>input_resident && !backing.coherence_retirement_pending(),
      "funded scan did not reserve and lock the owned catalog pages");
  backing.domain->verify_resident();

  const auto saved=session->optimization_snapshot();
  session.reset();
  backing.release_unselected_after_join();
  require(backing.domain->usage().resident==catalog.resident && backing.domain->usage().stores==catalog.stores,
      "healthy catalog did not survive controller retirement");
  auto resolve=[&](const auto& id)->std::shared_ptr<StoreByteIo> {return id==backing.image_identity?backing.image:nullptr;};
  session.emplace(NativeFileSession::resume_closed_indexed_volume(backing.head,volume,1,resolve,raw));
  require(session->resume_budgeted_optimization_area(backing.advice,saved.sequence).phase==NativeOptimizationPhase::ready,
      "retained memory catalog lost its funded journal");
  backing.configure_coherence(*session);
  require(!scan().checkpoints_published && !backing.coherence_retirement_pending() &&
      backing.domain->usage().stores==catalog.stores && backing.domain->usage().resident==catalog.resident,
      "remount did not preserve the healthy catalog on unchanged input");

  put("edited",data(kNativeExtentBytes,1729));
  for(unsigned i=0;i<2;++i)put("append-input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  require(scan().coherence_catalog_bytes==fixture_catalog_limit,"fixture did not fill the catalog");
  const auto edited=data(kNativeExtentBytes,17523);
  const auto edited_file=session->open("edited",{true,true,true});
  session->write(edited_file,0,edited);session->cleanup(edited_file,false);session->close(edited_file);
  for(unsigned i=0;i<3;++i)put("rotation-input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  const auto compacted=scan();
  const auto both=backing.domain->usage();
  require(compacted.coherence_compactions==1 && compacted.coherence_compacted_bytes_saved>0 &&
      both.stores==initial.stores+2 && backing.coherence_retirement_pending() &&
      session->volume_info().selection.image_identity==image,"Windows did not select the smaller catalog on the same image");
  while(backing.coherence_retirement_pending())backing.advance_coherence_retirement();
  const auto rotated=backing.domain->usage();
  require(rotated.stores==initial.stores+1 && rotated.reserved+fixture_catalog_limit==both.reserved &&
      both.resident-rotated.resident==fixture_catalog_limit &&
      session->snapshot().read("edited",0,kNativeExtentBytes)==edited,
      "old catalog pages or reservation survived rotation");
  std::cout<<"memory_catalog_rotation old_bytes="<<fixture_catalog_limit<<" new_bytes="<<compacted.coherence_catalog_bytes
      <<" retired_bytes="<<(both.resident-rotated.resident)<<'\n';

  session->create_entry("overwritten");
  const auto file=session->open("overwritten",{true,true,true});
  auto revision=data(4*kNativeExtentBytes,298);
  for(unsigned i=0;i<16 && !backing.compaction_pending(*session);++i) {
    revision[i]^=std::byte{1};
    session->write(file,0,revision);
  }
  require(backing.compaction_pending(*session),"catalog handoff fixture lacks obsolete history");
  bool published=false;
  for(unsigned i=0;i<2000 && !published;++i) {
    const auto step=backing.advance_compaction(*session);
    published=step==MemoryCompactionStep::published;
    require(published || step==MemoryCompactionStep::copying || step==MemoryCompactionStep::ready,
        "catalog handoff compaction failed");
  }
  require(published && backing.image_identity!=image && backing.coherence_retirement_pending(),
      "selected image did not retire the old catalog");
  const auto obsolete=backing.domain->usage();
  unsigned quanta=0;
  while(backing.coherence_retirement_pending()) {
    const auto before=backing.domain->usage().resident;
    backing.advance_coherence_retirement();
    const auto after=backing.domain->usage().resident;
    require(++quanta<32 && before>after && before-after<=kStoreMaxObjectBytes,
        "catalog retirement exceeded its page quantum or failed to progress");
  }
  const auto reclaimed=backing.domain->usage();
  require(reclaimed.stores+1==obsolete.stores &&
      reclaimed.reserved+backing.layout.coherence_capacity==obsolete.reserved,
      "catalog retirement retained its address reservation");
  require(session->read(file,0,static_cast<std::uint32_t>(revision.size()))==revision,
      "catalog and image retirement changed the open file");
  session->cleanup(file,false);session->close(file);

  for(unsigned i=0;i<3;++i)put("new-input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  const auto replacement=scan();
  require(replacement.checkpoints_published && replacement.coherence_catalog_bytes &&
      backing.domain->usage().stores==reclaimed.stores+1,"the successor image did not acquire a new funded catalog");
  session.reset();
  backing.release_unselected_after_join();
  const auto retained=backing.domain->usage();
  require(retained.stores==initial.stores+1,"joined successor did not retain the catalog lease");
  auto view=backing.open_read_only();
  view.verify_all();
  require(view.read(L"\\first",0,kNativeExtentBytes)==first &&
      view.read(L"\\second",0,kNativeExtentBytes)==second,"catalog retirement changed original files");
  const auto sealed=backing.domain->usage();
  require(sealed.stores+1==retained.stores && sealed.reserved+backing.layout.coherence_capacity==retained.reserved,
      "read-only seal retained the writable catalog lease");
  backing.domain->verify_resident();
  std::cout<<"memory_coherence_catalog locked_bytes="<<(catalog.resident-input_resident)
      <<" reclaimed_bytes="<<(obsolete.resident-reclaimed.resident)<<" reclamation_quanta="<<quanta<<'\n';
}
void memory_compaction_retirement() {
  auto backing=MemoryVolumeBacking::create();
  auto session=NativeFileSession::create_indexed_volume(backing.head,backing.volume_identity,
      backing.image,backing.image_identity,{backing.layout.image_capacity},{StoreCompressionMode::raw,1,false});
  require(!backing.compaction_pending(session),"fresh volume queued compaction");
  session.create_entry("visible");
  const auto file=session.open("visible",{true,true,true});
  auto bytes=data(4*kNativeExtentBytes,298);
  session.write(file,0,bytes);
  std::optional<NativeFilesystemSnapshot> earlier(session.snapshot());
  const auto first=bytes;
  for(unsigned i=0;i<7;++i) {
    bytes[i]^=std::byte{1};
    session.write(file,0,bytes);
  }
  const auto original=backing.image_identity;
  const auto before=backing.domain->usage();
  require(backing.compaction_pending(session),"committed obsolete history did not fund compaction");
  bool published=false;
  for(unsigned i=0;i<2000 && !published;++i) {
    const auto step=backing.advance_compaction(session);
    published=step==MemoryCompactionStep::published;
    require(published || step==MemoryCompactionStep::copying || step==MemoryCompactionStep::ready,
        "healthy memory compaction did not make progress");
  }
  require(published && backing.image_identity!=original &&
      session.volume_info().selection.image_identity==backing.image_identity &&
      session.read(file,0,static_cast<std::uint32_t>(bytes.size()))==bytes,
      "selected memory rewrite did not preserve the live open handle");
  const auto pinned=backing.domain->usage();
  require(pinned.stores==before.stores+1 && pinned.resident>before.resident &&
      backing.advance_compaction(session)==MemoryCompactionStep::pinned &&
      backing.domain->usage().resident==pinned.resident &&
      earlier->read("visible",0,static_cast<std::uint32_t>(first.size()))==first,
      "memory compaction released pages pinned by an older snapshot");
  earlier.reset();
  unsigned retirement_steps=0;
  while(backing.compaction_pending(session)) {
    const auto resident=backing.domain->usage().resident;
    require(backing.advance_compaction(session)==MemoryCompactionStep::retiring &&
        resident-backing.domain->usage().resident<=kStoreMaxObjectBytes && ++retirement_steps<100,
        "memory retirement exceeded one bounded opportunity");
  }
  const auto after=backing.domain->usage();
  require(retirement_steps>1 && after.stores==before.stores && after.resident<before.resident &&
      session.snapshot().read("visible",0,static_cast<std::uint32_t>(bytes.size()))==bytes,
      "retiring an unpinned image did not recover resident space");
  backing.domain->verify_resident();
  session.cleanup(file,false);
  session.close(file);
  std::cout<<"memory_compaction resident_before="<<before.resident<<" resident_after="<<after.resident
           <<" retirement_steps="<<retirement_steps<<'\n';
}

void memory_capacity_repair(bool startup=false) {
  auto backing=MemoryVolumeBacking::create();
  const auto bytes=data(2049,637);
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  {
    const auto unpacked=[&] {
      auto head=std::make_shared<Ram>(),source=std::make_shared<Ram>();
      head->failure_unit=source->failure_unit=4096;
      auto adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),source,id(1),{8ULL<<20},raw);
      auto* engine=adapter.get();
      Provider provider;Provider::active=&provider;
      WinFspNativeSession model(std::move(adapter),provider.api());
      require(model.start(plan())==STATUS_SUCCESS,"memory repair fixture seed startup");
      callback_file(provider,0,bytes);
      auto snapshot=engine->snapshot();
      model.stop();
      require(model.write_restart_ready(),"memory repair fixture seed retirement");
      return snapshot;
    }();
    (void)NativeFilesystemWriter::compact(unpacked,backing.image,backing.image_identity,raw);
  }
  const auto full=backing.image->extent();
  {
    const auto sealed=NativeFilesystemSnapshot::open_image(backing.image,{NativeImageKind::indexed_paged,full});
    (void)NativeVolumeHeadWriter::create(backing.head,backing.volume_identity,sealed);
  }
  const auto resolve=[&](const auto& identity)->std::shared_ptr<StoreByteIo> {
    return identity==backing.image_identity?backing.image:nullptr;
  };
  const auto damage_replica=[&] {
    Bytes image(static_cast<std::size_t>(full));
    backing.image->read_exact(0,image);
    constexpr std::size_t marker=40;
    const auto primary=std::search(image.begin()+2*kStoreCommitUnit,image.end(),bytes.begin(),bytes.begin()+marker);
    require(primary!=image.end(),"memory repair fixture payload missing");
    const auto offset=static_cast<std::size_t>(primary-image.begin());
    const auto peer=offset+backing.image->contract().write_failure_unit_bytes;
    require(peer+marker<=image.size() && std::equal(bytes.begin(),bytes.begin()+marker,image.begin()+peer),
        "memory repair fixture has no disjoint peer");
    const auto at=startup?peer:offset;
    const auto damage=image[at]^std::byte{1};
    backing.image->write_exact(at,std::span(&damage,1));
    backing.image->flush();
  };
  if(startup) {
    const auto prior=backing.image_identity;
    const auto stores=backing.domain->usage().stores;
    damage_replica();
    auto adapter=WinFspNativeWritable::resume_memory_volume(backing,NativeImageKind::indexed_paged,raw);
    const auto selected=NativeVolumeHeadView::inspect_selector(*backing.head,backing.volume_identity).selection;
    require(selected.epoch==2 && selected.parent_identity==prior && selected.image_identity==backing.image_identity &&
        backing.domain->usage().stores==stores+1,"memory startup repair lost retained backing ownership");
    {
      const auto checked=adapter->snapshot();checked.verify_all();
      require(checked.storage_info().physical_end==full && !checked.storage_info().degraded_checkpoint &&
          checked.read("f0",0,4096)==bytes,"memory startup repair did not verify a same-size replacement");
    }
    Provider provider;Provider::active=&provider;
    WinFspNativeSession reopened(std::move(adapter),provider.api());
    require(reopened.start(plan())==STATUS_SUCCESS,"memory startup repair could not start writable callbacks");
    reopened.stop();
    require(reopened.write_restart_ready(),"memory startup repair did not retire");
    backing.release_unselected_after_join();
    require(backing.domain->usage().stores==stores,"memory startup repair retained a retired alternate");
    backing.domain->verify_resident();
    std::cout<<"memory_startup_rotation equal_size=1 retained_old_owner=1 cpu_verified=1\n";
    return;
  }
  auto session=NativeFileSession::resume_closed_indexed_volume(backing.head,backing.volume_identity,1,resolve,raw);
  const auto handle=session.open("f0",{true,true,true});
  require(!backing.compaction_pending(session) && !session.indexed_volume_space().free_bytes,
      "memory repair fixture was not full and packed");
  {
    const auto pinned=session.snapshot();
    damage_replica();
    require(session.read(handle,0,4096)==bytes &&
        session.repair_indexed_payload()==PagedPayloadRepairProgress::capacity &&
        backing.compaction_pending(session),"full memory volume did not request replica rotation");
    auto step=MemoryCompactionStep::idle;
    for(unsigned i=0;i<400 && step!=MemoryCompactionStep::published;++i) {
      step=backing.advance_compaction(session);
      require(step==MemoryCompactionStep::copying || step==MemoryCompactionStep::ready ||
          step==MemoryCompactionStep::published,"full memory repair failed to progress");
    }
    const auto healed=session.snapshot();
    healed.verify_all();
    require(step==MemoryCompactionStep::published && session.volume_info().selection.epoch==2 &&
        healed.storage_info().physical_end==full && !healed.storage_info().degraded_checkpoint &&
        session.read(handle,0,4096)==bytes && pinned.read("f0",0,4096)==bytes &&
        backing.advance_compaction(session)==MemoryCompactionStep::pinned,"full memory repair lost a selected or pinned file");
  }
  for(unsigned i=0;backing.compaction_pending(session);++i)
    require(i<20 && backing.advance_compaction(session)==MemoryCompactionStep::retiring,
        "repaired memory store did not retire the unpinned source");
  session.cleanup(handle,false);
  session.close(handle);
  backing.domain->verify_resident();
  std::cout<<"memory_capacity_repair equal_size=1 pinned_reader=1 cpu_verified=1\n";
}

void live_indexed_compaction(bool reconcile=false) {
  Provider p;Provider::active=&p;
  WinFspMemoryVolume volume(p.api(),plan(),NativeImageKind::indexed_paged);
  await([&]{return volume.view().state==SessionState::running || volume.view().retired;},5000,
      "live compaction controller start deadline");
  require(volume.view().state==SessionState::running,"live compaction controller start");
  const auto* table=p.fs.Interface;
  SIZE_T security_size=0;UINT32 attributes=0;wchar_t root[]=L"\\";
  require(table->GetSecurityByName(&p.fs,root,&attributes,nullptr,&security_size)==STATUS_BUFFER_OVERFLOW,
      "live compaction root security size");
  Bytes security(security_size);
  require(table->GetSecurityByName(&p.fs,root,&attributes,security.data(),&security_size)==STATUS_SUCCESS,
      "live compaction root security");
  wchar_t path[]=L"\\compacted";
  PVOID file=nullptr;FSP_FSCTL_FILE_INFO file_info{};
  require(table->Create(&p.fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&file,&file_info)==STATUS_SUCCESS && file,"live compaction SDK Create");
  auto expected=data(kNativeExtentBytes,519);
  const auto overwrite=[&](unsigned i) {
    expected[i]^=std::byte{1};ULONG count=0;
    require(table->Write(&p.fs,file,expected.data(),0,static_cast<ULONG>(expected.size()),
        FALSE,FALSE,&count,&file_info)==STATUS_SUCCESS && count==expected.size(),"live compaction SDK Write");
  };
  for(unsigned i=0;i<7;++i)overwrite(i);
  const auto begin=GetTickCount64(),deadline=begin+82000;
  unsigned foreground_edits=0;
  while(GetTickCount64()<deadline) {
    const auto status=volume.view();
    if(status.compactions || status.compaction_failures || status.maintenance_disabled || status.retired)break;
    if(reconcile && foreground_edits==0 && status.continued_compaction_steps &&
        status.maintenance.phase==NativeMaintenancePhase::preparing) {
      overwrite(7);
      ++foreground_edits;
    }
    if(reconcile && foreground_edits==1 && status.maintenance.phase==NativeMaintenancePhase::publishing) {
      overwrite(8);
      ++foreground_edits;
    }
    Sleep(20);
  }
  const auto status=volume.view();
  require(status.compactions==1 && status.compaction_failures==0 && !status.maintenance_disabled,
      "live host observations did not complete staged compaction");
  require(!reconcile || foreground_edits==2,"did not observe both foreground reconciliation windows");
  Bytes readback(expected.size());ULONG count=0;
  require(table->Read(&p.fs,file,readback.data(),0,static_cast<ULONG>(readback.size()),&count)==STATUS_SUCCESS &&
      count==readback.size() && readback==expected,"SDK open handle read changed after automatic selection");
  table->Cleanup(&p.fs,file,nullptr,0);
  table->Close(&p.fs,file);
  volume.stop_and_join();
  require(volume.view().retired && !volume.view().offline && volume.view().state==SessionState::stopped,
      "live compaction controller did not retire cleanly");
  const auto recovered=volume.inspect_retired();
  require(recovered.info().selection.epoch==2 &&
      recovered.filesystem().read("compacted",0,static_cast<std::uint32_t>(expected.size()))==expected,
      "CPU inspection did not resolve automatic selection");
  std::cout<<"live_indexed_compaction elapsed_ms="<<GetTickCount64()-begin
           <<" quanta="<<status.maintenance_quanta<<" continued_compaction_steps="<<status.continued_compaction_steps
           <<" max_quantum_ns="<<status.maximum_quantum_ns
           <<" foreground_edits="<<foreground_edits<<" selected_epoch="<<recovered.info().selection.epoch<<'\n';
}

void memory_backing_ownership() {
  SIZE_T old_min=0,old_max=0;DWORD old_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&old_min,&old_max,&old_flags)!=FALSE,"factory old working set");
  std::shared_ptr<StoreByteIo> retained;
  const std::array<std::byte,4> marker{std::byte{1},std::byte{3},std::byte{7},std::byte{9}};
  {
    auto backing=MemoryVolumeBacking::create();const auto usage=backing.domain->usage();
    require(usage.capacity==backing.layout.domain_capacity && usage.stores==3 && usage.resident==0 &&
        usage.reserved==backing.layout.image_capacity+2*backing.layout.area_capacity,"factory reservation accounting");
    require(backing.head!=backing.image && backing.head!=backing.advice && backing.image!=backing.advice &&
        backing.volume_identity!=backing.image_identity,"factory aliases new identities/backends");
    require(backing.head->extent()==0 && backing.image->extent()==0 && backing.advice->extent()==0,"factory did not create empty backing");
    SIZE_T empty_min=0,empty_max=0;DWORD empty_flags=0;
    require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&empty_min,&empty_max,&empty_flags)!=FALSE &&
        empty_min==old_min && empty_max==old_max && empty_flags==old_flags,
        "empty backing raised the working-set quota");
    SYSTEM_INFO system{};GetSystemInfo(&system);
    require(backing.image->contract().write_failure_unit_bytes==system.dwPageSize,
        "memory provider did not use the live Windows page size");
    const auto page=std::uint64_t(system.dwPageSize);
    backing.image->admit_write(3*page+1);
    require(backing.image->extent()==0 && backing.domain->usage().resident==4*page,
        "preflight did not lock just the required pages or changed the stream extent");
    backing.image->write_exact(4*page,marker);
    backing.image->flush();backing.domain->verify_resident();
    std::array<std::byte,4> zero{};
    std::array<std::byte,4> untouched{std::byte{9},std::byte{9},std::byte{9},std::byte{9}};
    backing.image->read_exact(2*page,untouched);
    require(untouched==zero && backing.domain->usage().resident==5*page &&
        backing.domain->usage().reserved==usage.reserved,"write growth or zero-filled gap changed");
    std::cout<<"memory_demand_lock domain="<<usage.capacity<<" image="<<backing.layout.image_capacity
        <<" initially_reserved="<<usage.reserved<<" initially_locked="<<usage.resident
        <<" after_write_locked="<<backing.domain->usage().resident<<" page_bytes="<<page<<'\n';
    retained=backing.image;
  }
  bool refused=false;try{(void)MemoryVolumeBacking::create();}catch(const std::exception&){refused=true;}
  require(refused,"factory lost the surviving store's domain lease");
  SYSTEM_INFO system{};GetSystemInfo(&system);
  std::array<std::byte,4> read{};
  retained->read_exact(4ULL*system.dwPageSize,read);
  require(read==marker,"factory wrapper retirement lost retained bytes");
  retained.reset();
  SIZE_T restored_min=0,restored_max=0;DWORD restored_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&restored_min,&restored_max,&restored_flags)!=FALSE &&
      restored_min==old_min && restored_max==old_max && restored_flags==old_flags && MemoryVolumeDomain::cleanup_healthy(),
      "factory did not restore working set after last backing reference");
  {auto next=MemoryVolumeBacking::create();next.domain->verify_resident();}
}
void memory_composition(NativeImageKind kind=NativeImageKind::paged) {
  Provider p;Provider::active=&p;
  // Startup rollback retires the dispatcher, but retains the newly initialized
  // volume for independent inspection until the owner discards its wrapper.
  {
    p.fail_mount=true;WinFspMemoryVolume failed(p.api(),plan(),kind);
    await([&]{return failed.view().retired;},5000,"memory mount refusal retirement");
    bool refused=false;try{(void)failed.inspect_retired();}catch(const std::exception&){refused=true;}
    require(refused,"memory inspection accepted status without explicit join");
    refused=false;try{(void)failed.open_read_only();}catch(const std::exception&){refused=true;}
    require(refused,"read-only projection accepted status without explicit join");
    failed.stop_and_join();auto cpu=failed.inspect_retired();cpu.filesystem().verify_all();
    require(failed.view().start_status==STATUS_OBJECT_NAME_COLLISION && cpu.filesystem().list().empty() &&
        cpu.filesystem().image_storage().kind==kind,
        "memory mount rollback discarded the new valid volume");
    auto projected=failed.open_read_only();
    require(projected.list(L"\\").empty(),"read-only conversion lost the retired namespace");
  }
}
void read_only_memory() {
  const auto expected=data(kNativeExtentBytes+29,919);
  std::optional<ReadOnlyProjection> surviving;
  {
    auto backing=MemoryVolumeBacking::create();
    auto aliased=backing.image;
    auto unrelated=std::make_shared<Ram>();
    backing.image=unrelated;
    rejected([&]{(void)backing.open_read_only();},"unowned backing was admitted");
    backing.image=aliased;
    {
      auto writer=NativeFileSession::create_paged_volume(backing.head,backing.volume_identity,backing.image,
          backing.image_identity,{backing.layout.image_capacity},{StoreCompressionMode::automatic,3,false});
      writer.create_entry("Data",true);writer.create_entry("Data/sample.bin");
      const auto file=writer.open("Data/sample.bin",{true,true,false},NativeOpenKind::file);
      writer.write(file,0,expected);writer.flush(file);writer.cleanup(file,false);writer.close(file);
      rejected([&]{(void)backing.open_read_only();},"projection accepted an active writer");
      writer.create_entry("empty.bin");
    }
    auto projection=backing.open_read_only();
    const auto info=projection.native_info();
    require(info && info->identity==backing.volume_identity && info->image_bytes==backing.image->extent() &&
        projection.list(L"\\").size()==2,"selected read-only managed volume identity");
    require(projection.read(L"\\data\\SAMPLE.bin",0,static_cast<std::uint32_t>(expected.size()))==expected,
        "sealed CPU projection byte parity");
    const auto original_extent=aliased->extent();
    rejected([&]{aliased->write_exact(0,expected);},"raw alias wrote a sealed image");
    rejected([&]{aliased->admit_write(original_extent);},"sealed image admitted a write");
    rejected([&]{aliased->flush();},"sealed image accepted a mutable barrier");
    rejected([&]{(void)backing.domain->create_store(65536);},"sealed domain created another store");
    rejected([&]{(void)PagedObjectStoreWriter::resume(aliased,backing.image_identity,
        info->storage.generation,{backing.layout.image_capacity});},"sealed image acquired a writer");
    const auto api=linked_winfsp_session_api();
    require(api.create==FspFileSystemCreate && api.preflight==FspFileSystemPreflight &&
        api.stop_dispatcher==FspFileSystemStopDispatcher && api.remove_mount_point==FspFileSystemRemoveMountPoint,
        "incomplete SDK-linked native provider");
    const ReadOnlyMountPlan fixture{plan().directory,30,true};
    {
      Provider denied;Provider::active=&denied;denied.fail_mount=true;
      WinFspReadOnlySession session(backing.open_read_only(),denied.api());
      require(session.start(fixture)==STATUS_OBJECT_NAME_COLLISION && denied.read_only &&
          denied.callback_alive && denied.calls==6 && std::string_view(denied.order.data(),6)=="pcsmtd",
          "failed mount did not retire its owned callback state");
    }
    {
      Provider p;Provider::active=&p;
      WinFspReadOnlySession session(backing.open_read_only(),p.api());
      require(session.start(fixture)==STATUS_SUCCESS && p.read_only,"read-only lifecycle parameters");
      auto* fs=&p.fs;const auto* callbacks=fs->Interface;
      auto name=std::wstring(L"\\data\\SAMPLE.bin");
      auto other=std::wstring(L"\\mutated.bin");
      PVOID handle=nullptr;FSP_FSCTL_FILE_INFO metadata{};
      require(callbacks->Open(fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_WRITE_DATA,&handle,&metadata)==
          STATUS_MEDIA_WRITE_PROTECTED && !handle,"read-only SDK authorized write access");
      require(callbacks->Open(fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_READ_DATA,&handle,&metadata)==STATUS_SUCCESS &&
          metadata.FileSize==expected.size(),"read-only SDK Open");
      ULONG wrote=99;
      require(callbacks->Write(fs,handle,const_cast<std::byte*>(expected.data()),0,1,FALSE,FALSE,&wrote,&metadata)==
          STATUS_MEDIA_WRITE_PROTECTED && wrote==0,"read-only SDK Write");
      require(callbacks->Rename(fs,handle,name.data(),other.data(),TRUE)==STATUS_MEDIA_WRITE_PROTECTED &&
          callbacks->SetDelete(fs,handle,name.data(),TRUE)==STATUS_MEDIA_WRITE_PROTECTED,"read-only SDK mutation");
      callbacks->Cleanup(fs,handle,name.data(),FspCleanupDelete);
      std::array<std::byte,64> out{};ULONG count=0;
      constexpr auto offset=kNativeExtentBytes-35;
      require(callbacks->Read(fs,handle,out.data(),offset,static_cast<ULONG>(out.size()),&count)==STATUS_SUCCESS &&
          count==out.size() && std::equal(out.begin(),out.end(),expected.begin()+offset),"post-Cleanup extent-crossing SDK read");
      callbacks->Close(fs,handle);session.stop();session.stop();
      require(p.callback_alive && session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly() &&
          p.calls==7 && std::string_view(p.order.data(),7)=="pcsmtrd","read-only joined retirement");
    }
    require(aliased->extent()==original_extent,"read-only lifecycle changed image length");
    surviving.emplace(std::move(projection));
  }
  require(surviving->read(L"\\Data\\sample.bin",0,static_cast<std::uint32_t>(expected.size()))==expected,
      "projection lost the sealed backing after owner destruction");
  rejected([&]{(void)MemoryVolumeBacking::create();},"sealed reader lost the provider lease");
  surviving.reset();
  {auto backing=MemoryVolumeBacking::create();backing.domain->verify_resident();}
  require(MemoryVolumeDomain::cleanup_healthy(),"sealed reader did not retire the provider");
}
void live_memory_composition(NativeImageKind kind=NativeImageKind::paged,bool shifted=false,bool checkpoint=false) {
  Provider p;Provider::active=&p;std::optional<NativeVolumeHeadView> surviving;
  const auto first=data(kNativeExtentBytes,889);auto second=first;second[999]^=std::byte{7};
  if(shifted)std::rotate(second.begin(),second.begin()+201,second.end());
  {
    WinFspMemoryVolume volume(p.api(),plan(),kind);
    await([&]{const auto v=volume.view();return v.state==SessionState::running || v.retired;},5000,"memory controller start deadline");
    require(volume.view().state==SessionState::running,"memory provider startup failed");
    const auto layout=volume.layout();require(layout.domain_capacity>=2*layout.image_capacity &&
        layout.image_capacity>=3*kStoreCommitUnit,"memory controller used wrong backing envelope");
    callback_file(p,0,first);callback_file(p,1,second);
    const auto unrelated=data(kNativeExtentBytes,57);
    const auto sparse_input=3*kOptimizationGroupScanInputBytes/4;
    if(checkpoint) {
      callback_file(p,2,unrelated);
      const Bytes sparse(sparse_input);
      for(unsigned i=3;i<6;++i)callback_file(p,i,sparse);
    }
    const auto until=GetTickCount64()+(kind==NativeImageKind::indexed_paged?28000:18000);
    while(GetTickCount64()<until) {
      const auto v=volume.view();
      if((v.maintenance.published_groups && (!checkpoint || v.maintenance.coherence_catalog_bytes)) ||
          v.maintenance_disabled || v.retired)break;
      Sleep(25);
    }
    const auto observed=volume.view();volume.stop_and_join();
    require(volume.view().retired && !volume.view().offline,"memory controller did not retire cleanly");
    auto cpu=volume.inspect_retired();cpu.filesystem().verify_all();
    require(cpu.filesystem().image_storage().kind==kind,
        "memory controller did not create managed paged storage");
    require(cpu.filesystem().read("f0",0,first.size())==first && cpu.filesystem().read("f1",0,second.size())==second,
        "memory controller lost accepted data after stop");

    std::cout<<"live_memory indexed="<<(kind==NativeImageKind::indexed_paged)<<" shifted="<<shifted<<" domain="<<layout.domain_capacity
        <<" image="<<layout.image_capacity<<" groups="<<observed.maintenance.published_groups
        <<" sampled="<<observed.maintenance.sampled_extents<<" attempts="<<observed.maintenance.attempted_groups
        <<" checkpoints="<<observed.maintenance.checkpoints_published
        <<" catalog_bytes="<<observed.maintenance.coherence_catalog_bytes
        <<" phase="<<static_cast<unsigned>(observed.maintenance.phase)<<" deferral="<<static_cast<unsigned>(observed.maintenance.deferral)
        <<" compactions="<<observed.compactions<<" quanta="<<observed.maintenance_quanta
        <<" maximum_quantum_ns="<<observed.maximum_quantum_ns<<" disabled="<<observed.maintenance_disabled
        <<" host="<<static_cast<unsigned>(observed.host)
        <<" input="<<cpu.filesystem().workload_info().committed_input_bytes<<'\n';
    if(checkpoint) {
      require(observed.maintenance.checkpoints_published>=1 && observed.maintenance.coherence_catalog_bytes>0 &&
          cpu.filesystem().workload_info()==NativeWorkloadInfo{true,3*kNativeExtentBytes+3*sparse_input} &&
          cpu.filesystem().read("f2",0,unrelated.size())==unrelated,
          "live controller did not fund and attach a catalog during normal maintenance");
      (void)volume.open_read_only(); // The retained catalog writer must retire before sealing.
    }
    if(kind==NativeImageKind::paged) {
      require(cpu.filesystem().shared_base_info().enabled &&
          cpu.filesystem().workload_info()==NativeWorkloadInfo{true,2*kNativeExtentBytes},"memory controller input provenance");
    } else require(observed.maintenance.published_groups==1 && !observed.maintenance_disabled,
        "live indexed group did not complete within admitted opportunities");
    if(observed.maintenance.published_groups)require(cpu.filesystem().shared_base_info().members==2,"memory group not retained");
    surviving.emplace(std::move(cpu));
  }
  surviving->filesystem().verify_all();require(surviving->filesystem().read("f0",0,first.size())==first,
      "retained CPU view lost backing after memory controller destruction");
  bool refused=false;try{(void)MemoryVolumeBacking::create();}catch(const std::exception&){refused=true;}
  require(refused,"retained CPU reader did not hold the provider lease");
  surviving.reset();{auto next=MemoryVolumeBacking::create();next.domain->verify_resident();}
  require(MemoryVolumeDomain::cleanup_healthy(),"memory composition cleanup failed");
}

void indexed_memory_composition() {
  Provider p;Provider::active=&p;
  std::optional<ReadOnlyProjection> sealed;
  const auto keep=data(128,719),orphan=data(19,827);
  constexpr unsigned blocks=65;
  {
    WinFspMemoryVolume volume(p.api(),plan(),NativeImageKind::indexed_paged);
    await([&]{const auto v=volume.view();return v.state==SessionState::running || v.retired;},5000,
        "indexed memory controller start deadline");
    require(volume.view().state==SessionState::running,"indexed memory controller start");
    callback_file(p,0,keep);
    const auto* table=p.fs.Interface;
    SIZE_T length=0;UINT32 attributes=0;wchar_t root[]=L"\\";
    require(table->GetSecurityByName(&p.fs,root,&attributes,nullptr,&length)==STATUS_BUFFER_OVERFLOW,
        "indexed memory security size");
    Bytes security(length);
    require(table->GetSecurityByName(&p.fs,root,&attributes,security.data(),&length)==STATUS_SUCCESS,
        "indexed memory security");
    FSP_FSCTL_FILE_INFO file{};
    const auto create=[&](PWSTR name) {
      PVOID handle=nullptr;
      require(table->Create(&p.fs,name,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
          security.data(),0,&handle,&file)==STATUS_SUCCESS && handle,"indexed memory Create");
      return handle;
    };
    const auto write=[&](PVOID handle,std::uint64_t offset,const Bytes& data) {
      ULONG count=0;
      require(table->Write(&p.fs,handle,const_cast<std::byte*>(data.data()),offset,
          static_cast<ULONG>(data.size()),FALSE,FALSE,&count,&file)==STATUS_SUCCESS && count==data.size(),
          "indexed memory Write");
    };
    const auto delete_intent=[&](PVOID handle,PWSTR name) {
      require(table->SetDelete(&p.fs,handle,name,TRUE)==STATUS_SUCCESS,"indexed memory delete intent");
      table->Cleanup(&p.fs,handle,nullptr,FspCleanupDelete);
    };
    wchar_t retired_name[]=L"\\retire";
    auto* retired=create(retired_name);
    for(unsigned first=0;first<blocks;first+=32) {
      const auto span=(std::min)(32U,blocks-first);
      Bytes sparse(span*kNativeExtentBytes);
      for(unsigned i=0;i<span;++i)sparse[i*kNativeExtentBytes]=std::byte{0xa5};
      write(retired,std::uint64_t(first)*kNativeExtentBytes,sparse);
    }
    delete_intent(retired,retired_name);
    FSP_FSCTL_TRANSACT_REQ request;
    FSP_FSCTL_TRANSACT_RSP response;
    std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));
    request.Kind=FspFsctlTransactCloseKind;
    request.Req.Close.UserContext2=reinterpret_cast<std::uintptr_t>(retired);
    require(FspFileSystemOpClose(&p.fs,&request,&response)==STATUS_SUCCESS,"indexed memory Close");
    const auto until=GetTickCount64()+9000;
    while(GetTickCount64()<until) {
      const auto view=volume.view();
      if(view.reclamation_quanta>=2 || view.retired || view.maintenance_disabled)break;
      Sleep(20);
    }
    const auto maintenance=volume.view();
    require(!maintenance.retired && !maintenance.maintenance_disabled && maintenance.maintenance_quanta &&
        (maintenance.reclamation_quanta==2 || maintenance.maintenance.phase==NativeMaintenancePhase::deferred ||
          (maintenance.reclamation_quanta==1 && maintenance.maintenance.phase==NativeMaintenancePhase::publishing)),
        "indexed memory reclamation neither advanced nor reported a live deferral");

    // Deliberately retire the synthetic dispatcher with one detached context.
    // Inspection still owns the bytes. Sealing finishes the old lifetime only
    // after the controller has joined and dropped its writer.
    wchar_t orphan_name[]=L"\\orphan";
    const auto detached=create(orphan_name);
    write(detached,0,orphan);
    const auto inode=file.IndexNumber;
    delete_intent(detached,orphan_name);
    volume.stop_and_join();
    auto retained=volume.inspect_retired();
    const auto& snapshot=retained.filesystem();
    require(volume.view().retired && snapshot.image_storage().kind==NativeImageKind::indexed_paged &&
        snapshot.needs_session_recovery() && snapshot.read_inode(inode,0,100)==orphan,
        "indexed retired inspection lost detached bytes");
    const auto extent_count=snapshot.allocation_usage().live_extents;
    const auto quanta=volume.view().reclamation_quanta;
    require((quanta<=2 && extent_count==(quanta==0?67:quanta==1?3:2)) &&
        snapshot.read("f0",0,256)==keep,"indexed reaper extent accounting or retained file bytes");
    sealed.emplace(volume.open_read_only());
    const auto recovered=volume.inspect_retired();
    require(!recovered.filesystem().needs_session_recovery() &&
        sealed->list(L"\\").size()==1 && sealed->read(L"\\f0",0,256)==keep &&
        snapshot.read_inode(inode,0,100)==orphan,"indexed sealing lost immutable or recovered files");
    std::cout<<"live_indexed_reclamation extents="<<blocks<<" quanta="<<quanta
        <<" deferral="<<static_cast<unsigned>(maintenance.maintenance.deferral)
        <<" max_quantum_ns="<<maintenance.maximum_quantum_ns<<" closed_session_recovered=1\n";
  }
  require(sealed->read(L"\\f0",0,256)==keep,"sealed indexed projection lost its retained backing");
  sealed.reset();
  require(MemoryVolumeDomain::cleanup_healthy(),"indexed memory lease did not retire");
}

void retained_memory_remount(NativeImageKind kind) {
  const auto original=data(4097,271),added=data(2049,379),deleted=data(127,283);
  const bool indexed=kind==NativeImageKind::indexed_paged;
  constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
  Provider initial,collision,reopened;
  collision.fail_mount=true;
  Provider::active=&initial;
  WinFspMemoryVolume volume(initial.api(),plan(),kind);
  await([&]{return volume.view().state==SessionState::running;},5000,"initial remount fixture startup");
  rejected([&]{volume.remount(initial.api(),plan());},"active volume acquired a second dispatcher");
  callback_file(initial,0,original);
  std::uint64_t detached_inode=0;
  if(indexed) {
    callback_file(initial,1,deleted);
    auto* fs=&initial.fs;
    wchar_t name[]=L"\\f1";
    PVOID handle=nullptr;
    FSP_FSCTL_FILE_INFO metadata{};
    require(fs->Interface->Open(fs,name,open_file,DELETE,&handle,&metadata)==STATUS_SUCCESS &&
        handle,"could not open indexed remount delete fixture");
    detached_inode=metadata.IndexNumber;
    require(fs->Interface->SetDelete(fs,handle,name,TRUE)==STATUS_SUCCESS,"indexed remount delete intent");
    fs->Interface->Cleanup(fs,handle,name,FspCleanupDelete);
  }
  volume.stop_and_join();
  require(volume.view().write_restart_ready && initial.fs.UserContext==nullptr,"initial provider did not retire");
  Provider::active=&collision;
  NativeVolumeSelection selected;
  {
    auto retained=volume.inspect_retired();
    selected=retained.info().selection;
    require(retained.filesystem().read("f0",0,original.size())==original &&
        retained.filesystem().needs_session_recovery()==indexed,"retained bytes or recovery state differ");
    if(indexed)require(retained.filesystem().read_inode(detached_inode,0,deleted.size())==deleted,
        "retired indexed fixture lost unresolved deletion bytes");
    rejected([&]{volume.remount(collision.api(),plan());},"pinned inspection allowed a concurrent writer");
  }
  auto incomplete=collision.api();incomplete.preflight=nullptr;
  rejected([&]{volume.remount(incomplete,plan());},"remount accepted incomplete provider");
  rejected([&]{volume.remount(collision.api(),{});},"remount accepted invalid plan");
  require(collision.calls==0,"remount refusal called the provider");
  volume.remount(collision.api(),plan());
  await([&]{return volume.view().retired;},5000,"mount collision did not release its provider");
  volume.stop_and_join();
  require(volume.view().start_status==STATUS_OBJECT_NAME_COLLISION && volume.view().write_restart_ready &&
      collision.calls==6 && std::string_view(collision.order.data(),6)=="pcsmtd",
      "remount collision concealed provider failure or retained a dispatcher");
  {
    auto retained=volume.inspect_retired();
    const auto selection=retained.info().selection;
    require(selection==selected && retained.filesystem().read("f0",0,original.size())==original &&
        !retained.filesystem().needs_session_recovery(),"failed remount changed retained selection or bytes");
  }
  Provider::active=&reopened;
  volume.remount(reopened.api(),{L"R:\\FlitzFS-model\\next",true,true});
  await([&]{return volume.view().state==SessionState::running;},5000,"retained volume did not remount");
  auto* fs=&reopened.fs;
  wchar_t name[]=L"\\f0";
  PVOID handle=nullptr;
  FSP_FSCTL_FILE_INFO metadata{};
  require(fs->Interface->Open(fs,name,open_file,FILE_READ_DATA,&handle,&metadata)==STATUS_SUCCESS &&
      handle && metadata.FileSize==original.size(),"remounted provider did not expose original file");
  Bytes out(original.size());
  ULONG count=0;
  require(fs->Interface->Read(fs,handle,out.data(),0,static_cast<ULONG>(out.size()),&count)==STATUS_SUCCESS &&
      count==out.size() && out==original,"remounted provider changed original bytes");
  callback_cleanup_close(reopened,handle);
  callback_file(reopened,2,added);
  volume.stop_and_join();
  {
    auto retained=volume.inspect_retired();
    retained.filesystem().verify_all();
    require(volume.view().write_restart_ready && !volume.view().offline && retained.info().selection==selected &&
        retained.filesystem().read("f0",0,original.size())==original &&
        retained.filesystem().read("f2",0,added.size())==added,"independent CPU reopen changed remounted data");
  }
  require(volume.open_read_only().read(L"\\f2",0,static_cast<std::uint32_t>(added.size()))==added,
      "remounted data could not be sealed");
  rejected([&]{volume.remount(reopened.api(),plan());},"sealed backing reacquired writable mount");
  require(reopened.calls==7 && reopened.fs.UserContext==nullptr,
      "remount or seal retained the second provider");
  std::cout<<"memory_remount indexed="<<indexed<<" epoch="<<selected.epoch
      <<" collision_retired=1 detached_recovery="<<indexed<<" cpu_readback=1 sealed=1\n";
}

void unconfirmed_remount_refusal() {
  Provider silent,other;
  silent.report_stop=false;
  silent.fail_mount=true;
  Provider::active=&silent;
  WinFspMemoryVolume volume(silent.api(),plan(),NativeImageKind::indexed_paged);
  await([&]{return volume.view().retired;},5000,"unconfirmed startup fixture did not retire its wrapper");
  volume.stop_and_join();
  const auto status=volume.view();
  require(status.start_status==STATUS_OBJECT_NAME_COLLISION && status.offline &&
      !status.write_restart_ready && silent.fs.UserContext==nullptr,"missing dispatcher evidence admitted restart");
  Provider::active=&other;
  rejected([&]{volume.remount(other.api(),plan());},"unconfirmed dispatcher retirement allowed remount");
  rejected([&]{(void)volume.open_read_only();},"unconfirmed dispatcher retirement allowed sealing");
  require(other.calls==0,"unconfirmed remount reached the provider");
}

void indexed_group_callback_contract() {
  Provider provider;Provider::active=&provider;
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  head->failure_unit=image->failure_unit=4096;
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),
      {kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false});
  WinFspNativeSession session(std::move(adapter),provider.api());
  require(session.start(plan())==STATUS_SUCCESS && !session.background_maintenance_pending(),
      "empty indexed adapter requested a group");
  const auto first=data(kNativeExtentBytes,739);auto second=first;second[101]^=std::byte{13};
  callback_file(provider,0,first);callback_file(provider,1,second);
  NativeMaintenanceObservation observed;
  observed.valid=true;observed.physical_total=16ULL<<30;observed.physical_available=12ULL<<30;
  observed.commit_limit=32ULL<<30;observed.commit_available=24ULL<<30;
  observed.cpu_total_ticks=1000;observed.cpu_busy_ticks=126;
  observed.cpu_window_ms=1000;observed.sequence=1;observed.captured_ms=1000;
  const auto reads=image->read_calls,calls=image->calls;
  const auto deferred=session.advance_background_maintenance(observed,observed.captured_ms);
  require(deferred.progress.deferral==NativeMaintenanceDeferral::cpu &&
      image->read_calls==reads && image->calls==calls,"busy indexed adapter did backing work");
  observed.cpu_busy_ticks=0;
  WinFspMaintenanceResult state;
  for(unsigned i=0;session.background_maintenance_pending() && i<1000;++i) {
    ++observed.sequence;observed.captured_ms+=observed.cpu_window_ms;
    state=session.advance_background_maintenance(observed,observed.captured_ms);
    require(state.progress.phase!=NativeMaintenancePhase::failed,"indexed adapter grouping failed");
  }
  require(state.progress.published_groups==1 && state.progress.minimum_compacted_bytes_saved>0 &&
      !session.background_maintenance_pending(),"indexed adapter did not settle one beneficial group");
  session.stop();
  const auto recovered=NativeVolumeHeadView::open(head,id(90),[&](const auto&){return image;});
  const auto& volume=recovered.filesystem();
  require(volume.shared_base_info().members==2 && volume.read("f0",0,kNativeExtentBytes)==first &&
      volume.read("f1",0,kNativeExtentBytes)==second,"CPU reopen changed indexed callback content");
  volume.verify_all();
  std::cout<<"indexed_callback_group published="<<state.progress.published_groups
      <<" lower_bound_savings="<<state.progress.minimum_compacted_bytes_saved<<'\n';
}
}
int main(int argc,char** argv){try {
  app_fixture::Budget budget;DWORD before=0,after=0;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if((argc==7 || ((argc==8 || argc==9) && multiwindow_pattern(argv[6]))) &&
      std::string_view(argv[1])=="--live-anchor-owner")return live_anchor_owner(argc,argv);
  if(argc==2 || argc==3) {
    const std::string_view command(argv[1]);
    if(command.starts_with("--cpu-")) {
      if(const auto pattern=multiwindow_pattern(command.substr(6)))
        return live_multiwindow_anchor(std::nullopt,argc==3?std::filesystem::path(argv[2]):std::filesystem::path{},0,*pattern);
    }
  }
  if(argc==2 && std::string_view(argv[1])=="--live-packed-xor-cpu")
    return run_live_anchor_owner("xor-cpu",NativeAnchorSearchOptions{});
  if(argc==3 && std::string_view(argv[1])=="--live-installed-anchor-owner") {
    const std::string_view mode(argv[2]);
    require(mode=="accept" || mode=="missing" || mode=="changed" || mode=="stop" || mode=="repeat" || mode=="batch" ||
        mode=="restored" || mode=="queued" || mode=="verified-read" || mode=="verified-edit" || mode=="device-not-ready",
        "Unknown installed route trial");
    if(mode=="repeat") {
      const auto accepted=run_live_anchor_owner("accept",std::nullopt);
      if(accepted)return accepted;
      const auto first=handle_count();
      const auto stopped=run_live_anchor_owner("stop",std::nullopt);
      const auto second=handle_count();
      std::cout<<"installed_owner_repeated handles_between="<<first<<" handles_after="<<second<<'\n';
      require(first==second,"Repeated installed-route acquisition retained handles");
      return stopped;
    }
    return run_live_anchor_owner(mode,std::nullopt);
  }
#endif
  // PDH may initialize process-wide native caches. Warm that same native path
  // before checking ownership balance; never count its caches as our handles.
  const bool lifetime_only=argc==2 && std::string_view(argv[1])=="--owned-lifetime";
  const bool shifted_grouping=argc==2 && std::string_view(argv[1])=="--live-indexed-shifted-grouping";
  const bool checkpoint=argc==2 && std::string_view(argv[1])=="--live-indexed-checkpoint";
  {WindowsMaintenanceHost warm;warm.observe(false);}
  // The system-preferred RNG initializes process-wide native handles on first
  // use. Prove native-only stabilization before measuring factory ownership.
  const bool remount=argc==2 && std::string_view(argv[1])=="--memory-remount";
  const bool volume_file=argc==2 && std::string_view(argv[1])=="--volume-file";
  const bool live_repair_rotation=argc==2 && std::string_view(argv[1])=="--live-indexed-repair-rotation";
  const bool live_payload_scrub=argc==2 && std::string_view(argv[1])=="--live-indexed-scrub";
  const bool live_payload_repair=live_repair_rotation || live_payload_scrub ||
      (argc==2 && std::string_view(argv[1])=="--live-indexed-repair");
  const bool reconciled_compaction=argc==2 && std::string_view(argv[1])=="--live-indexed-reconcile";
  const bool indexed_compaction=reconciled_compaction || (argc==2 && std::string_view(argv[1])=="--live-indexed-compaction");
  if(lifetime_only || shifted_grouping || checkpoint || remount || volume_file || indexed_compaction ||
      live_payload_repair ||
      (argc==2 && (std::string_view(argv[1])=="--memory" || std::string_view(argv[1])=="--memory-handles" ||
      std::string_view(argv[1])=="--read-only-memory" || std::string_view(argv[1])=="--indexed-memory" ||
      std::string_view(argv[1])=="--live-indexed-grouping")))
    native_rng_initialization();
  // A long native PDH-only control initializes process-wide OS handles
  // before the explicit long-volume mode measures controller ownership.
  if(lifetime_only || indexed_compaction)initialize_long_pdh_window();
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"initial handle count");
  if(argc==1){event_ownership();lifecycle();idle_until_owner_stop(100);invalid_and_unexpected();live_opportunities();}
  else if(lifetime_only)idle_until_owner_stop(31000);
  else if(argc==2 && std::string_view(argv[1])=="--memory") {
    memory_planning();memory_paged_footprint();memory_compaction_retirement();memory_capacity_repair();
    memory_capacity_repair(true);
    std::cout<<"memory_handles start="<<handle_count()<<'\n';
    memory_backing_ownership();std::cout<<"memory_handles backing="<<handle_count()<<'\n';
    memory_composition();std::cout<<"memory_handles rollback="<<handle_count()<<'\n';
    live_memory_composition();std::cout<<"memory_handles live="<<handle_count()<<'\n';
  }else if(argc==2 && std::string_view(argv[1])=="--indexed-memory") {
    memory_composition(NativeImageKind::indexed_paged);
    indexed_memory_composition();
    indexed_group_callback_contract();
    memory_coherence_lifecycle();
  }else if(volume_file) {
    volume_file_callbacks();
    volume_file_scrub_checkpoint();
    indexed_reopen_platform_admission();
    volume_file_idle_reclamation();
    volume_file_payload_repair(false);
    volume_file_payload_repair(false,RepairFixture::scrub);
    volume_file_payload_repair(false,RepairFixture::startup_rotation);
  }else if(live_payload_repair) {
    const auto fixture=live_repair_rotation?RepairFixture::rotation:
        live_payload_scrub?RepairFixture::scrub:RepairFixture::foreground;
    volume_file_payload_repair(true,fixture);
  }else if(argc==2 && std::string_view(argv[1])=="--read-only-memory") {
    read_only_memory();
  }else if(remount) {
    retained_memory_remount(NativeImageKind::paged);
    retained_memory_remount(NativeImageKind::indexed_paged);
    unconfirmed_remount_refusal();
    require(MemoryVolumeDomain::cleanup_healthy(),"remount fixture did not retire memory");
  }else if(indexed_compaction) {
    live_indexed_compaction(reconciled_compaction);
  }else if(argc==2 && std::string_view(argv[1])=="--live-indexed-grouping") {
    live_memory_composition(NativeImageKind::indexed_paged);
  }else if(shifted_grouping) {
    live_memory_composition(NativeImageKind::indexed_paged,true);
  }else if(checkpoint) {
    live_memory_composition(NativeImageKind::indexed_paged,true,true);
  }else if(argc==2 && std::string_view(argv[1])=="--memory-handles") {
    for(unsigned i=0;i<3;++i){memory_backing_ownership();std::cout<<"backing_handles cycle="<<i<<" count="<<handle_count()<<'\n';}
  }else throw std::invalid_argument("Unknown owned controller qualification mode");
  require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE,"final handle count");
  std::cout<<"controller_handles before="<<before<<" after="<<after<<'\n';
  require(before==after,"controller handle leak");
  std::cout<<"PASS: "<<checks<<" owned controller checks; real local telemetry and SDK callbacks; injected lifecycle; no mount/GPU\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
