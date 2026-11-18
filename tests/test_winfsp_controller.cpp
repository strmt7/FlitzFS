#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "winfsp_memory_volume.hpp"
#include "anchor_search_fixture.hpp"
#include "flitzfs/paged_store.hpp"
#include "native_fs_fixture.hpp"
#include "windows_app_budget.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0;
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
int run_live_anchor_owner(std::string_view mode,std::optional<NativeAnchorSearchOptions> options) {
  const bool installed=!options;
  constexpr unsigned count=5;
  const auto before=handle_count();
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  Provider provider;Provider::active=&provider;
  NativeMaintenancePolicy qualification;
  qualification.maximum_cpu_busy_ppm=500000;
  auto adapter=WinFspNativeWritable::create_managed_indexed(
      head,id(90),image,id(1),{kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false},{},qualification);
  std::vector<Bytes> originals{data(kNativeExtentBytes,714)};
  for(unsigned i=1;i<count;++i) {
    auto derived=anchor_search_fixture::fragmented(originals.front(),i);
    std::copy_n(originals.front().begin(),kNativeExtentBytes/8,derived.begin());
    originals.push_back(std::move(derived));
  }
  WinFspControllerView result;
  bool signalled=false;
  std::array<unsigned,7> deferrals{};
  std::uint64_t last_quantum=0;
  {
    WinFspVolumeController controller(std::move(adapter),provider.api(),plan(),std::move(options));
    await([&]{return controller.view().state==SessionState::running;},5000,"AMD controller startup deadline");
    for(unsigned i=0;i<count;++i)callback_file(provider,i,originals[i]);
    const auto deadline=GetTickCount64()+57000;
    do {
      result=controller.view();
      if(result.maintenance_quanta!=last_quantum) {
        last_quantum=result.maintenance_quanta;
        ++deferrals.at(static_cast<std::size_t>(result.maintenance.deferral));
      }
      if(result.maintenance.published_groups || result.retired || result.maintenance_disabled ||
          (mode=="stop" && result.installed_anchor_attempts))break;
      if(mode=="yield" && result.anchor_search.submitted && !signalled) {
        FSP_FSCTL_VOLUME_INFO info{};
        require(provider.fs.Interface->GetVolumeInfo(&provider.fs,&info)==STATUS_SUCCESS,
            "foreground callback could not run while AMD work was outstanding");
        signalled=true;
      }
      Sleep(10);
    }while(GetTickCount64()<deadline);
    controller.stop_and_join();
    result=controller.view();
    require(result.retired && result.anchor_search.retirement_confirmed && !result.controller_failed &&
        WorkerExchangeSession::available(),"AMD controller or worker did not retire");
  }
  const auto proved=NativeVolumeHeadView::open(head,id(90),[&](const auto& key)->std::shared_ptr<StoreByteIo>{
    if(key!=id(1))throw std::runtime_error("AMD controller image identity");
    return image;
  });
  const auto& snapshot=proved.filesystem();snapshot.verify_all();
  for(unsigned i=0;i<count;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
      "AMD controller changed accepted file bytes");
  std::cout<<"anchor_owner mode="<<mode<<" host="<<static_cast<int>(result.host)
      <<" quanta="<<result.maintenance_quanta<<" groups="<<result.maintenance.published_groups
      <<" phase="<<static_cast<int>(result.maintenance.phase)<<" sampled="<<result.maintenance.sampled_extents
      <<" attempted="<<result.maintenance.attempted_groups<<" cpu_limit_ppm="<<qualification.maximum_cpu_busy_ppm
      <<" submitted="<<result.anchor_search.submitted<<" verified="<<result.anchor_search.verified
      <<" declined="<<result.anchor_search.declined<<" yielded="<<result.anchor_search.yielded
      <<" advised_members="<<result.maintenance.advised_members_published
      <<" installed_attempts="<<result.installed_anchor_attempts
      <<" metadata_queries="<<result.installed_anchor_queries
      <<" installed_status="<<(result.installed_anchor_status?static_cast<int>(*result.installed_anchor_status):-1)
      <<" handles_before="<<before<<" handles_after="<<handle_count()
      <<" retired=true cpu_readback_verified=true deferrals=";
  for(const auto occurrences:deferrals)std::cout<<occurrences<<',';
  std::cout<<'\n';
  if(installed) {
    require(result.installed_anchor_attempts==1 && result.installed_anchor_status &&
        result.installed_anchor_queries<=64,"Installed route did not make the expected bounded attempt");
    if(mode=="stop") {
      require(result.anchor_search.submitted==0,"Stop allowed a post-acquisition compute launch");
      return result.installed_anchor_status==HipDiscoveryStatus::cancelled ||
          result.installed_anchor_status==HipDiscoveryStatus::ready?0:2;
    }
    if(mode=="missing" || mode=="changed") {
      const auto status=mode=="missing"?HipDiscoveryStatus::package_unavailable:HipDiscoveryStatus::worker_untrusted;
      return result.installed_anchor_status==status && !result.installed_anchor_queries && !result.anchor_search.submitted &&
          result.maintenance.published_groups==1?0:2;
    }
    require(result.installed_anchor_status==HipDiscoveryStatus::ready && result.installed_anchor_queries!=0,
        "Installed package did not acquire a verified route");
  }
  if(mode=="yield")return signalled && result.anchor_search.yielded==1 &&
      result.maintenance.published_groups==1 && result.maintenance.advised_members_published==0?0:2;
  return result.anchor_search.verified==1 && result.maintenance.advised_members_published==count-1?0:2;
}
int live_anchor_owner(char** argv) {
  const std::string_view mode(argv[6]);
  require(mode=="accept" || mode=="yield" || mode=="both","Expected an AMD ownership trial");
  const ProcessBudget profile{128,2000,5000,16};
  HipRouteDiscoveryRequest request;
  request.helper=argv[2];request.helper_sha256=artifact_digest(argv[3]);
  request.anchor_module_sha256=artifact_digest(argv[4]);request.runtime_basename=std::filesystem::path(argv[5]).wstring();
  request.worker=profile;request.maximum_identity_queries=4;
  const auto binding=discover_system_hip_route(request);
  require(binding.status==HipDiscoveryStatus::ready && binding.route && binding.route->pins->anchor_module_pinned() &&
      !binding.route->pins->uniform_module_pinned(),"Shared-base worker route was not authenticated");
  const NativeAnchorSearchOptions execution{*binding.route,profile,4,4};
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
      <<" quanta="<<observed.maintenance_quanta<<" groups="<<observed.maintenance.published_groups
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
    require(2*layout.image_capacity+(NativeFileSession::max_optimization_bindings+1)*layout.area_capacity<=layout.domain_capacity,
        "factory omitted candidate/advisory reservations");
  }
  const auto roomy=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,24*1024*mib});
  const auto tight=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,5*1024*mib});
  const auto larger=plan_memory_volume({64*1024*mib,56*1024*mib,128*1024*mib,96*1024*mib});
  const auto minimal=plan_memory_volume({4*1024*mib,3072*mib,8*1024*mib,769*mib});
  require(tight.domain_capacity<roomy.domain_capacity && larger.domain_capacity>40*mib &&
      larger.image_capacity>roomy.image_capacity && minimal.image_capacity==3*kStoreCommitUnit,
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

void live_indexed_compaction() {
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
  for(unsigned i=0;i<7;++i) {
    expected[i]^=std::byte{1};ULONG count=0;
    require(table->Write(&p.fs,file,expected.data(),0,static_cast<ULONG>(expected.size()),
        FALSE,FALSE,&count,&file_info)==STATUS_SUCCESS && count==expected.size(),"live compaction SDK Write");
  }
  const auto begin=GetTickCount64(),deadline=begin+82000;
  while(GetTickCount64()<deadline) {
    const auto status=volume.view();
    if(status.compactions || status.compaction_failures || status.maintenance_disabled || status.retired)break;
    Sleep(50);
  }
  const auto status=volume.view();
  require(status.compactions==1 && status.compaction_failures==0 && !status.maintenance_disabled,
      "live host observations did not complete staged compaction");
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
           <<" quanta="<<status.maintenance_quanta<<" max_quantum_ns="<<status.maximum_quantum_ns
           <<" selected_epoch="<<recovered.info().selection.epoch<<'\n';
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
    if(checkpoint) {
      const Bytes sparse((kOptimizationGroupScanInputBytes-2*kNativeExtentBytes)/2);
      callback_file(p,2,sparse);
      callback_file(p,3,sparse);
    }
    const auto until=GetTickCount64()+(kind==NativeImageKind::indexed_paged?28000:18000);
    while(GetTickCount64()<until) {
      const auto v=volume.view();
      if((v.maintenance.published_groups && (!checkpoint || v.maintenance.checkpoints_published)) ||
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
    if(checkpoint)require(observed.maintenance.checkpoints_published==1 &&
        cpu.filesystem().workload_info()==NativeWorkloadInfo{true,kOptimizationGroupScanInputBytes},
        "live controller did not publish funded scan progress");
    std::cout<<"live_memory indexed="<<(kind==NativeImageKind::indexed_paged)<<" shifted="<<shifted<<" domain="<<layout.domain_capacity
        <<" image="<<layout.image_capacity<<" groups="<<observed.maintenance.published_groups
        <<" sampled="<<observed.maintenance.sampled_extents<<" attempts="<<observed.maintenance.attempted_groups
        <<" checkpoints="<<observed.maintenance.checkpoints_published
        <<" phase="<<static_cast<unsigned>(observed.maintenance.phase)<<" deferral="<<static_cast<unsigned>(observed.maintenance.deferral)
        <<" compactions="<<observed.compactions<<" quanta="<<observed.maintenance_quanta
        <<" maximum_quantum_ns="<<observed.maximum_quantum_ns<<" disabled="<<observed.maintenance_disabled<<'\n';
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
  if(argc==7 && std::string_view(argv[1])=="--live-anchor-owner")return live_anchor_owner(argv);
  if(argc==3 && std::string_view(argv[1])=="--live-installed-anchor-owner") {
    const std::string_view mode(argv[2]);
    require(mode=="accept" || mode=="missing" || mode=="changed" || mode=="stop" || mode=="repeat","Unknown installed route trial");
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
  if(lifetime_only || shifted_grouping || checkpoint || remount || (argc==2 && (std::string_view(argv[1])=="--memory" || std::string_view(argv[1])=="--memory-handles" ||
      std::string_view(argv[1])=="--read-only-memory" || std::string_view(argv[1])=="--indexed-memory" ||
      std::string_view(argv[1])=="--live-indexed-compaction" || std::string_view(argv[1])=="--live-indexed-grouping")))
    native_rng_initialization();
  // A long native PDH-only control initializes process-wide OS handles
  // before the explicit long-volume mode measures controller ownership.
  if(lifetime_only || (argc==2 && std::string_view(argv[1])=="--live-indexed-compaction"))
    initialize_long_pdh_window();
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"initial handle count");
  if(argc==1){event_ownership();lifecycle();idle_until_owner_stop(100);invalid_and_unexpected();live_opportunities();}
  else if(lifetime_only)idle_until_owner_stop(31000);
  else if(argc==2 && std::string_view(argv[1])=="--memory") {
    memory_planning();memory_paged_footprint();memory_compaction_retirement();
    std::cout<<"memory_handles start="<<handle_count()<<'\n';
    memory_backing_ownership();std::cout<<"memory_handles backing="<<handle_count()<<'\n';
    memory_composition();std::cout<<"memory_handles rollback="<<handle_count()<<'\n';
    live_memory_composition();std::cout<<"memory_handles live="<<handle_count()<<'\n';
  }else if(argc==2 && std::string_view(argv[1])=="--indexed-memory") {
    memory_composition(NativeImageKind::indexed_paged);
    indexed_memory_composition();
    indexed_group_callback_contract();
  }else if(argc==2 && std::string_view(argv[1])=="--read-only-memory") {
    read_only_memory();
  }else if(remount) {
    retained_memory_remount(NativeImageKind::paged);
    retained_memory_remount(NativeImageKind::indexed_paged);
    unconfirmed_remount_refusal();
    require(MemoryVolumeDomain::cleanup_healthy(),"remount fixture did not retire memory");
  }else if(argc==2 && std::string_view(argv[1])=="--live-indexed-compaction") {
    live_indexed_compaction();
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
