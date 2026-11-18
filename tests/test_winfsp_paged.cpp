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
  PVOID create(std::wstring name,bool directory) {
    PVOID context=nullptr;
    FSP_FSCTL_FILE_INFO info{};
    const auto options=directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE;
    const auto attributes=directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL;
    const auto status=fs.Interface->Create(&fs,name.data(),options,FILE_ALL_ACCESS,attributes,
        security.data(),0,&context,&info);
    require(status==STATUS_SUCCESS && context,
        ("paged SDK Create status "+std::to_string(static_cast<std::uint32_t>(status))).c_str());
    return context;
  }
  PVOID create(unsigned n) {return create(L"\\f"+std::to_wstring(n),false);}
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
void directory_pages() {
  Harness h(4096);
  const auto folder=h.create(L"\\Folder",true);
  const auto early=h.create(L"\\Folder\\(first)",false);
  const auto payload=data(23,719);
  h.write(early,payload);
  h.close(early);
  for(unsigned i=100;i<138;++i)
    h.close(h.create(L"\\Folder\\Item-"+std::to_wstring(i),false));
  const auto reference=h.adapter->snapshot().list("Folder");
  require(reference.size()==39,"owned directory test fixture");
  const auto writes=h.image->writes;

  struct Row {std::wstring name;std::uint64_t inode,size;};
  struct Page {std::vector<Row> rows;std::uint64_t backing_bytes=0;bool eof=false;};
  const auto page=[&](PWSTR marker,ULONG capacity) {
    alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,8192> output{};
    require(capacity<=output.size(),"owned directory output capacity");
    ULONG count=0;
    const auto before=h.image->read_bytes;
    require(h.fs.Interface->ReadDirectory(&h.fs,folder,nullptr,marker,output.data(),capacity,&count)==STATUS_SUCCESS &&
        count<=capacity,"paged SDK directory read");
    Page result;
    result.backing_bytes=h.image->read_bytes-before;
    for(std::size_t offset=0;offset+sizeof(UINT16)<=count;) {
      UINT16 size=0;
      std::memcpy(&size,output.data()+offset,sizeof(size));
      if(!size){result.eof=true;break;}
      require(size>=sizeof(FSP_FSCTL_DIR_INFO) && offset+size<=count &&
          (size-sizeof(FSP_FSCTL_DIR_INFO))%sizeof(wchar_t)==0,"paged SDK directory record bounds");
      const auto* row=reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(output.data()+offset);
      result.rows.push_back({{row->FileNameBuf,(size-sizeof(FSP_FSCTL_DIR_INFO))/sizeof(wchar_t)},
          row->FileInfo.IndexNumber,row->FileInfo.FileSize});
      offset+=(size+7U)&~std::size_t(7);
    }
    return result;
  };
  const auto all=page(nullptr,8192);
  require(all.eof && all.rows.size()==reference.size()+2 && all.rows[0].name==L"." && all.rows[1].name==L"..",
      "multi-page enumeration lost dot entries or completion");
  for(std::size_t i=0;i<reference.size();++i) {
    const auto& expected=reference[i];
    const auto name=expected.path.substr(std::string_view("Folder/").size());
    const auto& row=all.rows[i+2];
    require(row.name==std::wstring(name.begin(),name.end()) &&
        row.inode==expected.inode && row.size==expected.logical_bytes,
        "checked directory pages changed spelling, inode or size");
  }
  std::wstring dotdot=L"..";
  const auto one=page(dotdot.data(),static_cast<ULONG>(sizeof(FSP_FSCTL_DIR_INFO)+24));
  require(one.rows.size()==1 && one.rows[0].name==L"(first)" &&
      one.rows[0].size==payload.size() && !one.eof,"exclusive dot-dot continuation");
  auto marker=one.rows[0].name;
  const auto remainder=page(marker.data(),8192);
  require(remainder.eof && remainder.rows.size()==38 && remainder.rows.front().name==L"Item-100",
      "short output continuation repeated or skipped a child");
  std::wstring folded=L"iTeM-115";
  const auto after=page(folded.data(),8192);
  require(after.eof && after.rows.size()==22 && after.rows.front().name==L"Item-116",
      "case-insensitive marker is not exclusive");
  require(one.backing_bytes<all.backing_bytes && h.image->writes==writes,
      "short enumeration scanned all entries or changed backing");
  h.close(folder);
  std::cout<<"directory_read entries="<<reference.size()<<" short_result_bytes="<<one.backing_bytes
      <<" complete_result_bytes="<<all.backing_bytes<<'\n';
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
void indexed_callbacks(unsigned unit) {
  auto head=ram(unit),backing=ram(unit);
  constexpr PagedStoreLimits capacity{kStoreMaxPhysicalBytes};
  const auto volume=id(70),image_id=id(71);
  const auto resolver=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo> {
    require(key==image_id,"indexed SDK restart resolved another image");
    return backing;
  };
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,volume,backing,image_id,capacity,raw);
  FSP_FILE_SYSTEM fs{};
  const auto bind=[&] {
    fs={};
    fs.Version=static_cast<UINT16>(sizeof(fs));
    fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();
    adapter->bind_callbacks(fs,[]()->UINT32{return 0;});
  };
  bind();
  SIZE_T security_bytes=0;
  UINT32 attributes=0;
  wchar_t root[]=L"\\";
  require(fs.Interface->GetSecurityByName(&fs,root,&attributes,nullptr,&security_bytes)==STATUS_BUFFER_OVERFLOW,
      "indexed SDK root descriptor length");
  Bytes security(security_bytes);
  require(fs.Interface->GetSecurityByName(&fs,root,&attributes,security.data(),&security_bytes)==STATUS_SUCCESS,
      "indexed SDK root descriptor");
  const auto create=[&](PWSTR path) {
    void* context=nullptr;
    FSP_FSCTL_FILE_INFO output{};
    require(fs.Interface->Create(&fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
        security.data(),0,&context,&output)==STATUS_SUCCESS && context && output.IndexNumber,
        "indexed SDK creation");
    return context;
  };
  const auto cleanup=[&](void* context,ULONG flags) {
    fs.Interface->Cleanup(&fs,context,nullptr,flags);
    require(!adapter->requires_offline(),"indexed SDK Cleanup fenced ownership");
  };
  const auto close=[&](void* context) {
    FSP_FSCTL_TRANSACT_REQ request;
    FSP_FSCTL_TRANSACT_RSP response;
    std::memset(&request,0,sizeof(request));std::memset(&response,0,sizeof(response));
    request.Kind=FspFsctlTransactCloseKind;
    request.Req.Close.UserContext2=reinterpret_cast<std::uintptr_t>(context);
    require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS && !adapter->requires_offline(),
        "indexed SDK Close");
  };
  wchar_t keep[]=L"\\keep",gone[]=L"\\gone";
  auto handle=create(keep);
  auto initial=data(2U*1024*1024+17,917);
  ULONG bytes=0;
  FSP_FSCTL_FILE_INFO info{};
  require(fs.Interface->Write(&fs,handle,initial.data(),0,static_cast<ULONG>(initial.size()),
      FALSE,FALSE,&bytes,&info)==STATUS_SUCCESS && bytes==initial.size() &&
      info.FileSize==initial.size(),"indexed SDK multi-megabyte write");
  constexpr std::uint64_t eof=96ULL*1024*1024+19;
  require(fs.Interface->SetFileSize(&fs,handle,eof,FALSE,&info)==STATUS_SUCCESS &&
      info.FileSize==eof && info.AllocationSize==eof-19+kNativeAllocationUnit,
      "indexed SDK sparse EOF inherited the legacy quota");
  auto tail=data(8,991);
  require(fs.Interface->Write(&fs,handle,tail.data(),UINT64_MAX,static_cast<ULONG>(tail.size()),
      TRUE,FALSE,&bytes,&info)==STATUS_SUCCESS && bytes==tail.size() && info.FileSize==eof+tail.size(),
      "indexed SDK append chose the wrong EOF");
  const auto calls=backing->calls;
  require(fs.Interface->SetFileSize(&fs,handle,UINT64_MAX,FALSE,&info)==STATUS_INVALID_PARAMETER &&
      !info.FileSize && backing->calls==calls,"indexed SDK accepted an unrepresentable EOF");
  FSP_FSCTL_VOLUME_INFO volume_info{};
  const auto occupied=adapter->snapshot().storage_info().physical_end;
  require(fs.Interface->GetVolumeInfo(&fs,&volume_info)==STATUS_SUCCESS &&
      volume_info.TotalSize==capacity.maximum_physical_bytes &&
      volume_info.FreeSize==capacity.maximum_physical_bytes-occupied,
      "indexed SDK free-space observation mixed logical reservations with physical occupancy");
  cleanup(handle,0);close(handle);

  const auto detached=create(gone);
  auto doomed=data(7,932);
  require(fs.Interface->Write(&fs,detached,doomed.data(),0,static_cast<ULONG>(doomed.size()),
      FALSE,FALSE,&bytes,&info)==STATUS_SUCCESS,"indexed SDK detached fixture write");
  const auto gone_id=info.IndexNumber;
  require(fs.Interface->SetDelete(&fs,detached,gone,TRUE)==STATUS_SUCCESS,"indexed SDK SetDelete");
  cleanup(detached,FspCleanupDelete);
  const auto pinned=adapter->snapshot();
  require(pinned.needs_session_recovery() && pinned.inode_state(gone_id).unlinked &&
      pinned.read_inode(gone_id,0,100)==doomed,"indexed SDK Cleanup lost detached bytes");
  fs.UserContext=nullptr;
  adapter.reset(); // The old synthetic owner and all its contexts have ended.
  adapter=WinFspNativeWritable::resume_indexed_retired(head,volume,1,resolver,raw);
  bind();
  require(!adapter->snapshot().needs_session_recovery() && pinned.read_inode(gone_id,0,100)==doomed,
      "indexed SDK restart lost immutable snapshot data or retained a dead context");
  void* removed=nullptr;
  constexpr UINT32 open_file=(1U<<24)|FILE_NON_DIRECTORY_FILE;
  require(fs.Interface->Open(&fs,gone,open_file,FILE_ALL_ACCESS,&removed,&info)==STATUS_OBJECT_NAME_NOT_FOUND && !removed,
      "indexed SDK recovery exposed the detached name");
  handle=nullptr;
  require(fs.Interface->Open(&fs,keep,open_file,FILE_ALL_ACCESS,&handle,&info)==STATUS_SUCCESS &&
      handle && info.FileSize==eof+tail.size(),"indexed SDK reopen lost sparse EOF");
  Bytes actual(initial.size());
  require(fs.Interface->Read(&fs,handle,actual.data(),0,static_cast<ULONG>(actual.size()),&bytes)==STATUS_SUCCESS &&
      bytes==actual.size() && actual==initial,"indexed SDK reopened data was not byte exact");
  actual.resize(tail.size());
  require(fs.Interface->Read(&fs,handle,actual.data(),eof,static_cast<ULONG>(actual.size()),&bytes)==STATUS_SUCCESS &&
      bytes==actual.size() && actual==tail,"indexed SDK reopened sparse tail was not byte exact");
  cleanup(handle,0);close(handle);
  require(adapter->stats().contexts==0,"indexed SDK left open contexts");
  adapter->snapshot().verify_all();
  const auto selected_end=adapter->snapshot().storage_info().physical_end;
  fs.UserContext=nullptr;
  adapter.reset();
  const std::array unselected{std::byte{0x5b}};
  Ram::copy(backing->live,selected_end,unselected);
  backing->stable=backing->live;
  adapter=WinFspNativeWritable::resume_indexed_retired(head,volume,1,resolver,raw);
  bind();
  const auto append_floor=(selected_end/unit+1)*unit;
  require(adapter->snapshot().storage_info().physical_end==selected_end &&
      fs.Interface->GetVolumeInfo(&fs,&volume_info)==STATUS_SUCCESS &&
      volume_info.FreeSize==capacity.maximum_physical_bytes-append_floor,
      "indexed SDK free-space recovery ignored an unselected append tail");
  fs.UserContext=nullptr;
  std::cout<<"indexed_winfsp_callbacks unit="<<unit<<" large_write_bytes="<<initial.size()
      <<" reopened_eof="<<eof+tail.size()<<" occupied_bytes="<<occupied<<'\n';
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
  if(mode=="--callbacks"){
    callbacks(4096);callbacks(65536);directory_pages();selected_change();foreground_busy();
    deferred_response_busy();resume_formats();lifecycle();indexed_callbacks(4096);indexed_callbacks(65536);
  }
  else if(mode=="--faults-4096")publication_faults(4096);
  else if(mode=="--faults-65536")publication_faults(65536);
  else throw std::runtime_error("Unknown callback qualification mode");
  std::cout<<"PASS: "<<checks<<" paged WinFsp SDK checks; "<<faults<<" injected group failures; modeled CPU observations; memory only, no mount/driver/GPU\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
