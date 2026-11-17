#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "winfsp_memory_volume.hpp"
#include "native_fs_fixture.hpp"
#include "windows_app_budget.hpp"
#include <bcrypt.h>
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
  bool fail_mount=false,callback_alive=false;
  static Provider* active;
  void record(char c){const auto n=calls.load();if(n<order.size())order[n]=c;calls.store(n+1);}
  static NTSTATUS preflight(PWSTR,PWSTR){active->record('p');return STATUS_SUCCESS;}
  static NTSTATUS create(PWSTR,const FSP_FSCTL_VOLUME_PARAMS*,const FSP_FILE_SYSTEM_INTERFACE* table,FSP_FILE_SYSTEM** out) {
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
    fs->Interface->DispatcherStopped(fs,TRUE);
  }
  static VOID unmount(FSP_FILE_SYSTEM*){active->record('r');}
  static VOID destroy(FSP_FILE_SYSTEM* fs){active->record('d');fs->UserContext=nullptr;}
  static UINT32 origin(){return 0;}
  WinFspSessionApi api(){return {preflight,create,start,mount,stop,unmount,destroy,origin};}
};
Provider* Provider::active=nullptr;
struct StopRelease {Provider& provider;~StopRelease(){provider.release_stop=true;}};
NativeWriteMountPlan plan(unsigned seconds=30){return {L"R:\\FlitzFS-model\\unused",seconds,true,true};}
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
  FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
  std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));request.Kind=FspFsctlTransactCleanupKind;
  request.Req.Cleanup.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpCleanup(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Cleanup");
  request.Kind=FspFsctlTransactCloseKind;request.Req.Close.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpClose(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Close");
}
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
void expiration() {
  Provider p;Provider::active=&p;Fixture f;WinFspVolumeController controller(std::move(f.adapter),p.api(),plan(1));
  await([&]{return controller.view().retired;},5000,"lifetime did not retire controller");
  const auto v=controller.view();controller.stop_and_join();
  require(v.expired && v.state==SessionState::stopped && !v.offline,"lifetime retirement result");
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
  require(v.state==SessionState::failed && !v.expired && !v.controller_failed && p.fs.UserContext==nullptr,
      "unexpected stop was concealed or did not release ownership");
}
void live_opportunities() {
  Provider p;Provider::active=&p;Fixture f;const auto head=f.head,image=f.image;
  // Complete only the empty model before exposure. The actual populated-volume
  // opportunities below use the real Windows collector, never this clock.
  NativeMaintenanceObservation model;model.valid=true;model.physical_total=16ULL<<30;
  model.physical_available=12ULL<<30;model.commit_limit=32ULL<<30;model.commit_available=24ULL<<30;
  model.cpu_total_ticks=1000;model.cpu_window_ms=1000;
  for(unsigned i=0;i<1000 && f.adapter->group_maintenance_pending();++i) {
    model.sequence=i+1;model.captured_ms=(i+1)*1000;
    f.adapter->advance_group_maintenance(model,model.captured_ms);
  }
  require(!f.adapter->group_maintenance_pending(),"empty modeled setup did not settle");
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
    require(layout.domain_capacity>=9*mib && layout.domain_capacity<=MemoryVolumeDomain::maximum_capacity &&
        layout.domain_capacity%65536==0 && layout.image_capacity%65536==0,"adaptive factory plan bounds");
    require(2*layout.image_capacity+(NativeFileSession::max_optimization_bindings+1)*layout.area_capacity<=layout.domain_capacity,
        "factory omitted candidate/advisory reservations");
  }
  const auto roomy=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,24*1024*mib});
  const auto tight=plan_memory_volume({16*1024*mib,12*1024*mib,32*1024*mib,5*1024*mib});
  require(tight.domain_capacity<roomy.domain_capacity,"commit headroom did not reduce factory sizing");
  for(const auto& host:std::array<MemoryVolumeObservation,5>{{{}, {1024*mib,512*mib,2048*mib,1024*mib},
      {1024,1025,4096,4096},{4096,4096,1024,1025},{4*1024*mib,3072*mib,8*1024*mib,768*mib}}}) {
    bool refused=false;try{(void)plan_memory_volume(host);}catch(const std::exception&){refused=true;}
    require(refused,"invalid/low headroom admitted a factory plan");
  }
}
void memory_backing_ownership() {
  SIZE_T old_min=0,old_max=0;DWORD old_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&old_min,&old_max,&old_flags)!=FALSE,"factory old working set");
  std::shared_ptr<StoreByteIo> retained;
  const std::array<std::byte,4> marker{std::byte{1},std::byte{3},std::byte{7},std::byte{9}};
  {
    auto backing=MemoryVolumeBacking::create();const auto usage=backing.domain->usage();
    require(usage.capacity==backing.layout.domain_capacity && usage.stores==3 &&
        usage.allocated==backing.layout.image_capacity+2*backing.layout.area_capacity,"factory backing accounting");
    require(backing.head!=backing.image && backing.head!=backing.advice && backing.image!=backing.advice &&
        backing.volume_identity!=backing.image_identity,"factory aliases new identities/backends");
    require(backing.head->extent()==0 && backing.image->extent()==0 && backing.advice->extent()==0,"factory did not create empty backing");
    backing.image->write_exact(0,marker);backing.image->flush();backing.domain->verify_resident();retained=backing.image;
  }
  bool refused=false;try{(void)MemoryVolumeBacking::create();}catch(const std::exception&){refused=true;}
  require(refused,"factory lost the surviving store's domain lease");
  std::array<std::byte,4> read{};retained->read_exact(0,read);require(read==marker,"factory wrapper retirement lost retained bytes");
  retained.reset();
  SIZE_T restored_min=0,restored_max=0;DWORD restored_flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&restored_min,&restored_max,&restored_flags)!=FALSE &&
      restored_min==old_min && restored_max==old_max && restored_flags==old_flags && MemoryVolumeDomain::cleanup_healthy(),
      "factory did not restore working set after last backing reference");
  {auto next=MemoryVolumeBacking::create();next.domain->verify_resident();}
}
void memory_composition() {
  Provider p;Provider::active=&p;
  // Startup rollback retires the dispatcher, but retains the newly initialized
  // volume for independent inspection until the owner discards its wrapper.
  {
    p.fail_mount=true;WinFspMemoryVolume failed(p.api(),plan());
    await([&]{return failed.view().retired;},5000,"memory mount refusal retirement");
    bool refused=false;try{(void)failed.inspect_retired();}catch(const std::exception&){refused=true;}
    require(refused,"memory inspection accepted status without explicit join");
    failed.stop_and_join();auto cpu=failed.inspect_retired();cpu.filesystem().verify_all();
    require(failed.view().start_status==STATUS_OBJECT_NAME_COLLISION && cpu.filesystem().list().empty(),
        "memory mount rollback discarded the new valid volume");
  }
}
void live_memory_composition() {
  Provider p;Provider::active=&p;std::optional<NativeVolumeHeadView> surviving;
  const auto first=data(kNativeExtentBytes,889);auto second=first;second[999]^=std::byte{7};
  {
    WinFspMemoryVolume volume(p.api(),plan());
    await([&]{const auto v=volume.view();return v.state==SessionState::running || v.retired;},5000,"memory controller start deadline");
    require(volume.view().state==SessionState::running,"memory provider startup failed");
    const auto layout=volume.layout();require(layout.domain_capacity<=MemoryVolumeDomain::maximum_capacity &&
        layout.image_capacity>=4*1024*1024,"memory controller used wrong backing envelope");
    callback_file(p,0,first);callback_file(p,1,second);
    const auto until=GetTickCount64()+18000;
    while(GetTickCount64()<until) {
      const auto v=volume.view();if(v.maintenance.published_groups || v.maintenance_disabled || v.retired)break;Sleep(25);
    }
    const auto observed=volume.view();volume.stop_and_join();
    require(volume.view().retired && !volume.view().offline,"memory controller did not retire cleanly");
    auto cpu=volume.inspect_retired();cpu.filesystem().verify_all();
    require(cpu.filesystem().image_storage().kind==NativeImageKind::paged && cpu.filesystem().shared_base_info().enabled,
        "memory controller did not create managed paged storage");
    require(cpu.filesystem().read("f0",0,first.size())==first && cpu.filesystem().read("f1",0,second.size())==second,
        "memory controller lost accepted data after stop");
    require(cpu.filesystem().workload_info()==NativeWorkloadInfo{true,2*kNativeExtentBytes},"memory controller input provenance");
    if(observed.maintenance.published_groups)require(cpu.filesystem().shared_base_info().members==2,"memory group not retained");
    std::cout<<"live_memory domain="<<layout.domain_capacity<<" image="<<layout.image_capacity<<" groups="
        <<observed.maintenance.published_groups<<" quanta="<<observed.maintenance_quanta<<" maximum_quantum_ns="
        <<observed.maximum_quantum_ns<<" disabled="<<observed.maintenance_disabled<<'\n';
    surviving.emplace(std::move(cpu));
  }
  surviving->filesystem().verify_all();require(surviving->filesystem().read("f0",0,first.size())==first,
      "retained CPU view lost backing after memory controller destruction");
  bool refused=false;try{(void)MemoryVolumeBacking::create();}catch(const std::exception&){refused=true;}
  require(refused,"retained CPU reader did not hold the provider lease");
  surviving.reset();{auto next=MemoryVolumeBacking::create();next.domain->verify_resident();}
  require(MemoryVolumeDomain::cleanup_healthy(),"memory composition cleanup failed");
}
}
int main(int argc,char** argv){try {
  app_fixture::Budget budget;DWORD before=0,after=0;
  // PDH may initialize process-wide native caches. Warm that same native path
  // before checking ownership balance; never count its caches as our handles.
  {WindowsMaintenanceHost warm;warm.observe(false);}
  // The system-preferred RNG initializes process-wide native handles on first
  // use. Prove native-only stabilization before measuring factory ownership.
  if(argc==2 && (std::string_view(argv[1])=="--memory" || std::string_view(argv[1])=="--memory-handles"))
    native_rng_initialization();
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"initial handle count");
  if(argc==1){event_ownership();lifecycle();expiration();invalid_and_unexpected();live_opportunities();}
  else if(argc==2 && std::string_view(argv[1])=="--memory") {
    memory_planning();std::cout<<"memory_handles start="<<handle_count()<<'\n';
    memory_backing_ownership();std::cout<<"memory_handles backing="<<handle_count()<<'\n';
    memory_composition();std::cout<<"memory_handles rollback="<<handle_count()<<'\n';
    live_memory_composition();std::cout<<"memory_handles live="<<handle_count()<<'\n';
  }else if(argc==2 && std::string_view(argv[1])=="--memory-handles") {
    for(unsigned i=0;i<3;++i){memory_backing_ownership();std::cout<<"backing_handles cycle="<<i<<" count="<<handle_count()<<'\n';}
  }else throw std::invalid_argument("Unknown owned controller qualification mode");
  require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE && before==after,"controller handle leak");
  std::cout<<"PASS: "<<checks<<" owned controller checks; real local telemetry and SDK callbacks; injected lifecycle; no mount/GPU\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
