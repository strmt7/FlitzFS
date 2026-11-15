#include "winfsp_native.hpp"
#include "winfsp_session.hpp"
#include "native_fs_fixture.hpp"
#include "windows_app_budget.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <thread>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0,faults=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
StoreIdentity id(unsigned n){auto value=identity();value[0]=static_cast<std::byte>(n);return value;}
auto ram(unsigned unit){auto p=std::make_shared<Ram>();p->failure_unit=unit;return p;}
Bytes family(unsigned n){auto result=data(kNativeExtentBytes,917);if(n)result[n*719%result.size()]^=std::byte{17};return result;}
struct Clock {
  std::uint64_t now=1000,sequence=0;
  NativeMaintenanceObservation next(){NativeMaintenanceObservation o;o.sequence=++sequence;o.captured_ms=now+=1000;
    o.valid=true;o.physical_total=16ULL<<30;o.physical_available=12ULL<<30;o.commit_limit=32ULL<<30;
    o.commit_available=24ULL<<30;o.cpu_total_ticks=1000;o.cpu_window_ms=1000;return o;}
};
class BlockingIo final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram;mutable std::mutex mutex;mutable std::condition_variable changed;
  mutable std::atomic<bool> armed{false};mutable bool entered=false,released=false;
  explicit BlockingIo(std::shared_ptr<Ram> r):ram(std::move(r)){}
  std::uint64_t extent() const override{return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t n) override{ram->admit_write(n);}
  void write_exact(std::uint64_t n,std::span<const std::byte> bytes) override{ram->write_exact(n,bytes);}
  void flush() override{ram->flush();}
  void read_exact(std::uint64_t n,std::span<std::byte> bytes) const override {
    if(armed.exchange(false)){std::unique_lock lock(mutex);entered=true;changed.notify_all();
      if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;}))throw std::runtime_error("controlled callback deadline");}
    ram->read_exact(n,bytes);
  }
  void release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
struct Harness {
  std::shared_ptr<Ram> head,image,advice;std::unique_ptr<WinFspNativeWritable> adapter;FSP_FILE_SYSTEM fs{};
  std::shared_ptr<BlockingIo> image_io;
  Bytes security;Clock clock;
  explicit Harness(unsigned unit,const Bytes* previous=nullptr,const Bytes* old_head=nullptr,const Bytes* old_advice=nullptr,
      const NativeDeferredWriteOptions* deferred=nullptr):
      head(ram(unit)),image(ram(unit)),advice(ram(unit)) {
    image_io=std::make_shared<BlockingIo>(image);
    if(previous) {
      image->live=image->stable=*previous;head->live=head->stable=*old_head;advice->live=advice->stable=*old_advice;
      adapter=WinFspNativeWritable::resume_managed_retired(head,id(90),1,[&](const auto& key)->std::shared_ptr<StoreByteIo>{
          if(key!=id(1))throw std::runtime_error("fixture image identity");return image_io;},
          {StoreCompressionMode::raw,1,false},advice,1);
    } else adapter=WinFspNativeWritable::create_managed_paged(head,id(90),image_io,id(1),
        {kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false},advice);
    if(deferred)adapter->enable_deferred_writes_hosted(*deferred);
    fs.Version=static_cast<UINT16>(sizeof(fs));fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();adapter->bind_callbacks(fs,[]()->UINT32{return 0;});
    SIZE_T bytes=0;UINT32 attributes=0;
    require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,nullptr,&bytes)==STATUS_BUFFER_OVERFLOW,
        "paged root ACL size");security.resize(bytes);
    require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,security.data(),&bytes)==STATUS_SUCCESS,
        "paged root ACL bytes");
  }
  ~Harness(){fs.UserContext=nullptr;adapter.reset();}
  PVOID create(unsigned n) {
    auto name=L"\\f"+std::to_wstring(n);PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
    require(fs.Interface->Create(&fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
        security.data(),0,&context,&info)==STATUS_SUCCESS && context,"paged SDK Create");return context;
  }
  void write(PVOID context,const Bytes& bytes){ULONG count=0;FSP_FSCTL_FILE_INFO info{};
    require(fs.Interface->Write(&fs,context,const_cast<std::byte*>(bytes.data()),0,static_cast<ULONG>(bytes.size()),
        FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==bytes.size(),"paged SDK Write");}
  void read(PVOID context,const Bytes& expected) {
    Bytes actual(expected.size());ULONG count=0;
    require(fs.Interface->Read(&fs,context,actual.data(),0,static_cast<ULONG>(actual.size()),&count)==STATUS_SUCCESS &&
        count==actual.size() && actual==expected,"paged SDK Read");
  }
  void close(PVOID context) {
    // Actual SDK operation wrappers, with owned request/response storage. No
    // filesystem creation, dispatcher start, mount or driver API is invoked.
    FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
    std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));request.Kind=FspFsctlTransactCleanupKind;
    request.Req.Cleanup.UserContext2=reinterpret_cast<std::uintptr_t>(context);
    require(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS,"paged SDK Cleanup");
    std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));request.Kind=FspFsctlTransactCloseKind;
    request.Req.Close.UserContext2=reinterpret_cast<std::uintptr_t>(context);
    require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS,"paged SDK Close");
    require(!adapter->requires_offline(),"paged callback retirement fenced source");
  }
  auto step(){const auto observed=clock.next();return adapter->advance_group_maintenance(observed,clock.now);}
  auto finish(){for(unsigned i=0;i<1000;++i){const auto r=step();require(r.state!=WinFspGroupMaintenanceState::unavailable,
      "paged opportunity unavailable");if(r.progress.phase==NativeMaintenancePhase::idle && !adapter->group_maintenance_pending())return r;}
    throw std::runtime_error("paged group did not quiesce");}
  void populate(){for(unsigned i=0;i<4;++i){const auto context=create(i);write(context,family(i));read(context,family(i));close(context);}}
};
void callbacks(unsigned unit) {
  Harness h(unit);h.populate();const auto workload=h.adapter->snapshot().workload_info();
  auto context=h.create(4);h.write(context,family(4));
  auto result=h.finish();
  require(result.progress.published_groups==1 && h.adapter->snapshot().shared_base_info().members==5,
      "SDK-created data not automatically grouped");h.read(context,family(4));h.close(context);
  // Closing a context can update namespace metadata; reuse content hashes.
  result=h.finish();const auto reads=h.image->read_calls,calls=h.image->calls;
  for(unsigned i=0;i<20;++i)require(h.step().state==WinFspGroupMaintenanceState::unchanged,"unchanged callback volume not idle");
  require(h.image->read_calls==reads && h.image->calls==calls,"unchanged callback opportunity accessed backing");
  require(h.adapter->snapshot().workload_info()==NativeWorkloadInfo{true,workload.committed_input_bytes+kNativeExtentBytes},
      "callback groups minted accepted input");
  auto snapshot=h.adapter->snapshot();snapshot.verify_all();
  require(snapshot.image_storage().kind==NativeImageKind::paged && snapshot.shared_base_info().enabled,"wrong callback storage format");
  h.fs.Interface->DispatcherStopped(&h.fs,TRUE);const auto before=h.image->read_calls;
  require(h.step().state==WinFspGroupMaintenanceState::unavailable && !h.adapter->group_maintenance_pending() &&
      h.image->read_calls==before,"stopped callback owner accessed storage");
  std::cout<<"paged_callbacks unit="<<unit<<" members=5 groups="<<result.progress.published_groups<<" input="
      <<snapshot.workload_info().committed_input_bytes<<"\n";
}
void selected_change() {
  Harness h(4096);h.populate();
  for(unsigned i=0;i<1000 && h.step().progress.owned_payload_bytes==0;++i){}
  auto name=std::wstring(L"\\f0");PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
  require(h.fs.Interface->Open(&h.fs,name.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_ALL_ACCESS,&context,&info)==STATUS_SUCCESS,
      "paged SDK Open");const auto replacement=data(kNativeExtentBytes,17);h.write(context,replacement);h.close(context);
  h.finish();const auto snapshot=h.adapter->snapshot();
  require(snapshot.read("f0",0,kNativeExtentBytes)==replacement,"stale grouped work overwrote callback write");
  for(unsigned i=1;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"callback reconciliation lost another file");
  h.adapter->cancel_group_maintenance();const auto before=h.image->calls;
  require(!h.adapter->group_maintenance_pending() && h.step().state==WinFspGroupMaintenanceState::unavailable &&
      h.image->calls==before,"cancelled adapter restarted private work");
}
void foreground_busy() {
  Harness h(4096);auto context=h.create(0);h.write(context,family(0));
  h.image_io->armed=true;Bytes buffer(kNativeExtentBytes);ULONG count=0;NTSTATUS status=STATUS_UNSUCCESSFUL;
  struct Join {
    BlockingIo& io;std::thread thread;
    ~Join(){io.release();if(thread.joinable())thread.join();}
  } running{*h.image_io,std::thread([&]{status=h.fs.Interface->Read(&h.fs,context,buffer.data(),0,
      static_cast<ULONG>(buffer.size()),&count);})};
  {std::unique_lock lock(h.image_io->mutex);
    require(h.image_io->changed.wait_for(lock,std::chrono::seconds(5),[&]{return h.image_io->entered;}),"foreground callback did not enter backing");}
  const auto reads=h.image->read_calls,calls=h.image->calls;
  require(h.step().state==WinFspGroupMaintenanceState::busy && h.image->read_calls==reads && h.image->calls==calls,
      "background bypassed foreground control or accessed backing");
  h.image_io->release();running.thread.join();
  require(status==STATUS_SUCCESS && count==buffer.size() && buffer==family(0),"foreground callback lost bytes");h.close(context);
}
struct ResponseGate {
  std::mutex mutex;std::condition_variable changed;bool entered=false,released=false,expired=false;
  UINT32 status=0;UINT64 transferred=0;
  void release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
ResponseGate* response_gate=nullptr;
bool hint(UINT64& value) noexcept{value=17;return true;}
void response(FSP_FILE_SYSTEM*,FSP_FSCTL_TRANSACT_RSP& value) noexcept {
  auto& gate=*response_gate;std::unique_lock lock(gate.mutex);gate.status=value.IoStatus.Status;
  gate.transferred=value.IoStatus.Information;gate.entered=true;gate.changed.notify_all();
  if(!gate.changed.wait_for(lock,std::chrono::seconds(5),[&]{return gate.released;}))gate.expired=true;
}
void deferred_response_busy() {
  ResponseGate gate;response_gate=&gate;
  {
    NativeDeferredWriteOptions options;options.expected=WorkerExecution::cpu_semantics;
    options.hint_query=hint;options.response_sink=response;
    Harness h(4096,nullptr,nullptr,nullptr,&options);
    struct Release {ResponseGate& gate;~Release(){gate.release();}} release{gate};
    const auto context=h.create(0);const auto bytes=family(0);ULONG count=99;FSP_FSCTL_FILE_INFO info{};
    require(h.fs.Interface->Write(&h.fs,context,const_cast<std::byte*>(bytes.data()),0,static_cast<ULONG>(bytes.size()),FALSE,FALSE,
        &count,&info)==STATUS_PENDING && !count,"paged deferred write not owned");
    {std::unique_lock lock(gate.mutex);
      require(gate.changed.wait_for(lock,std::chrono::seconds(5),[&]{return gate.entered;}),"paged CPU response absent");}
    const auto stats=h.adapter->deferred_write_stats();const auto reads=h.image->read_calls,calls=h.image->calls;
    require(!stats.native_pending && stats.worker_busy && h.step().state==WinFspGroupMaintenanceState::busy &&
        h.image->read_calls==reads && h.image->calls==calls,"background bypassed outstanding response/retirement");
    gate.release();const auto deadline=GetTickCount64()+5000;
    while(h.adapter->stop_deferred_writes_hosted()==NativeDeferredStop::running){if(GetTickCount64()>deadline)
      throw std::runtime_error("paged CPU service retirement deadline");Sleep(1);}
    require(!gate.expired && gate.status==STATUS_SUCCESS && gate.transferred==bytes.size() &&
        h.adapter->deferred_write_stats().retirement_confirmed,"paged CPU response changed or failed retirement");
    h.read(context,bytes);h.close(context);h.finish();
  }
  response_gate=nullptr;
}
void resume_formats() {
  for(bool paged:{false,true}) {
    auto head=ram(4096),image=ram(4096);
    auto adapter=paged?WinFspNativeWritable::create_managed_paged(head,id(90),image,id(1),{kStoreMaxPhysicalBytes}):
        WinFspNativeWritable::create_managed_hosted(head,id(90),image,id(1));
    const auto before=adapter->snapshot();const auto root=before.basic_metadata(1);const auto security=before.security_descriptor(1);
    const auto parameters=adapter->parameters();adapter.reset();
    unsigned resolutions=0;
    adapter=WinFspNativeWritable::resume_managed_retired(head,id(90),1,[&](const auto& key)->std::shared_ptr<StoreByteIo>{
      ++resolutions;if(key!=id(1))throw std::runtime_error("wrong resumed image");return image;});
    const auto after=adapter->snapshot();
    require(after.basic_metadata(1)==root && after.security_descriptor(1)==security &&
        after.image_storage().kind==(paged?NativeImageKind::paged:NativeImageKind::flat) &&
        adapter->parameters().VolumeCreationTime==parameters.VolumeCreationTime &&
        adapter->parameters().VolumeSerialNumber==parameters.VolumeSerialNumber,"resume replaced Windows root identity/ACL/time or format");
    require(adapter->group_maintenance_pending()==paged,"resume silently promoted legacy format or lost grouping");
    require(resolutions==1,"resume resolved mutable image mapping more than once");
  }
  auto head=ram(4096),image=ram(4096);
  {auto core=NativeFileSession::create_paged_volume(head,id(90),image,id(1),{kStoreMaxPhysicalBytes});}
  const auto head_bytes=head->live,image_bytes=image->live;const auto writes=head->calls+image->calls;
  bool rejected=false;
  try{auto invalid=WinFspNativeWritable::resume_managed_retired(head,id(90),1,[&](const auto&)->std::shared_ptr<StoreByteIo>{return image;});}
  catch(const std::exception&){rejected=true;}
  require(rejected && head->calls+image->calls==writes && head->live==head_bytes && image->live==image_bytes,
      "Windows-incompatible root was exposed or repaired before profile admission");
}
struct Lifecycle {
  FSP_FILE_SYSTEM fs{};bool fail_mount=false,callback_alive=false;std::array<char,16> calls{};unsigned count=0;
  void record(char c){if(count<calls.size())calls[count++]=c;}
  static Lifecycle* active;
  static NTSTATUS preflight(PWSTR,PWSTR){active->record('p');return STATUS_SUCCESS;}
  static NTSTATUS create(PWSTR,const FSP_FSCTL_VOLUME_PARAMS*,const FSP_FILE_SYSTEM_INTERFACE* table,FSP_FILE_SYSTEM** out){
    active->record('c');active->fs.Interface=table;*out=&active->fs;return STATUS_SUCCESS;}
  static NTSTATUS start(FSP_FILE_SYSTEM*,ULONG){active->record('s');return STATUS_SUCCESS;}
  static NTSTATUS mount(FSP_FILE_SYSTEM*,PWSTR){active->record('m');return active->fail_mount?STATUS_OBJECT_NAME_COLLISION:STATUS_SUCCESS;}
  static VOID stop(FSP_FILE_SYSTEM* fs){active->record('t');FSP_FSCTL_VOLUME_INFO info{};
    active->callback_alive=fs->Interface->GetVolumeInfo(fs,&info)==STATUS_SUCCESS;fs->Interface->DispatcherStopped(fs,TRUE);}
  static VOID unmount(FSP_FILE_SYSTEM*){active->record('r');}
  static VOID destroy(FSP_FILE_SYSTEM* fs){active->record('d');fs->UserContext=nullptr;}
  static UINT32 origin(){return 0;}
};
Lifecycle* Lifecycle::active=nullptr;
void lifecycle() {
  for(bool fail:{false,true}) {
    Lifecycle provider;Lifecycle::active=&provider;provider.fail_mount=fail;
    auto head=ram(4096),image=ram(4096);auto adapter=WinFspNativeWritable::create_managed_paged(head,id(90),image,id(1),{kStoreMaxPhysicalBytes});
    WinFspNativeSession session(std::move(adapter),{Lifecycle::preflight,Lifecycle::create,Lifecycle::start,Lifecycle::mount,
        Lifecycle::stop,Lifecycle::unmount,Lifecycle::destroy,Lifecycle::origin});
    require(!session.group_maintenance_pending(),"ready injected session admitted group work");
    // The path is inert data passed ONLY to the injected functions above.
    // No native preflight/create/dispatcher/mount function is in this provider.
    const auto started=session.start({L"R:\\FlitzFS-model\\unused",15,true,true});
    if(!fail){require(started==STATUS_SUCCESS && session.group_maintenance_pending(),"paged injected lifecycle start");
      Clock clock;const auto o=clock.next();require(session.advance_group_maintenance(o,clock.now).state==WinFspGroupMaintenanceState::advanced,
          "session did not forward group opportunity");session.stop();}
    else require(started==STATUS_OBJECT_NAME_COLLISION && session.state()==SessionState::failed,"injected mount refusal lost ownership");
    require(!session.group_maintenance_pending() && provider.callback_alive && !provider.fs.UserContext &&
        std::string(provider.calls.data(),provider.count)==(fail?"pcsmtd":"pcsmtrd"),"paged lifecycle did not retire in order");
    session.cancel_group_maintenance();
  }
  Lifecycle::active=nullptr;
}
void publication_faults(unsigned unit) {
  Harness original(unit);original.populate();const auto bytes=original.image->stable,head=original.head->stable,
      advice=original.advice->stable;original.fs.UserContext=nullptr;original.adapter.reset();
  // Exact same retired starting bytes for every cut, including real UTC/ACL
  // metadata. Only fresh owned memory copies are opened by each new adapter.
  auto ready=[](Harness& h){for(unsigned i=0;i<1000;++i){const auto r=h.step();if(r.progress.phase==NativeMaintenancePhase::publishing)return;}
    throw std::runtime_error("SDK publication phase absent");};
  std::size_t cuts=0;
  {Harness probe(unit,&bytes,&head,&advice);ready(probe);const auto start=probe.image->calls;
    for(unsigned i=0;i<1000 && probe.adapter->group_maintenance_pending();++i)probe.step();cuts=probe.image->calls-start;}
  require(cuts>3 && cuts<256,"callback publication fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    Harness h(unit,&bytes,&head,&advice);ready(h);h.image->failure=mode;h.image->fail_at=h.image->calls+cut;
    WinFspGroupMaintenanceResult result;
    for(unsigned i=0;i<1000;++i){result=h.step();if(h.adapter->requires_offline())break;}
    require(h.adapter->requires_offline() && !h.adapter->group_maintenance_pending(),"failed callback group did not fence owner");
    const auto reads=h.image->read_calls,calls=h.image->calls;
    require(h.step().state==WinFspGroupMaintenanceState::unavailable && h.image->read_calls==reads && h.image->calls==calls,
        "offline group retried backing");
    auto stable=image(h.image->stable,unit);auto snapshot=NativeFilesystemSnapshot::open_image(stable,{NativeImageKind::paged,kStoreMaxPhysicalBytes});
    for(unsigned i=0;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"callback interruption lost acknowledged bytes");
    snapshot.verify_all();++faults;
  }
  std::cout<<"paged_callback_faults unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
}
int main(int argc,char** argv){try {
  app_fixture::Budget budget;
  if(argc!=2)throw std::runtime_error("Explicit callback/fault mode required");
  const std::string_view mode=argv[1];
  if(mode=="--callbacks"){callbacks(4096);callbacks(65536);selected_change();foreground_busy();deferred_response_busy();resume_formats();lifecycle();}
  else if(mode=="--faults-4096")publication_faults(4096);
  else if(mode=="--faults-65536")publication_faults(65536);
  else throw std::runtime_error("Unknown callback qualification mode");
  std::cout<<"PASS: "<<checks<<" paged WinFsp SDK checks; "<<faults<<" injected group failures; modeled CPU observations; memory only, no mount/driver/GPU\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
