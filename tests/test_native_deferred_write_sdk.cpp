// Disposable hosted CPU callback qualification. No filesystem/dispatcher creation.
#include "winfsp_native.hpp"
#include "winfsp_controller.hpp"
#include "locked_ram_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
// Type-check the driver-independent composition fixture in the ordinary SDK
// build too. Its execution remains in the separate runtime qualification gate.
#include "rocm_compile/deferred_native_fixture.hpp"
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
#include "hip_route_discovery.hpp"
#include "native_name_index.hpp"
#endif
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
  std::uint64_t capacity;
  std::shared_ptr<StoreByteIo> image;
  Collector collector;
  FSP_FILE_SYSTEM fs{};
  std::unique_ptr<WinFspNativeWritable> standalone;
  WinFspNativeWritable* adapter=nullptr;
  std::unique_ptr<WinFspVolumeController> controller;
  std::vector<std::byte> security;
  static Harness* provider;
  static NTSTATUS preflight(PWSTR,PWSTR){return STATUS_SUCCESS;}
  static NTSTATUS create_filesystem(PWSTR,const FSP_FSCTL_VOLUME_PARAMS*,const FSP_FILE_SYSTEM_INTERFACE* table,
      FSP_FILE_SYSTEM** out){
    provider->fs.Interface=table;
    *out=&provider->fs;
    return STATUS_SUCCESS;
  }
  static NTSTATUS start(FSP_FILE_SYSTEM*,ULONG){return STATUS_SUCCESS;}
  static NTSTATUS mount(FSP_FILE_SYSTEM*,PWSTR){return STATUS_SUCCESS;}
  static VOID stop(FSP_FILE_SYSTEM* fs){fs->Interface->DispatcherStopped(fs,TRUE);}
  static VOID unmount(FSP_FILE_SYSTEM*){}
  static VOID destroy(FSP_FILE_SYSTEM* fs){fs->UserContext=nullptr;}
  static UINT32 origin(){return 0;}
  explicit Harness(const NativeDeferredWriteOptions& options,bool indexed=false,bool observe_budget=false)
      :capacity(indexed?8U*1024*1024:4U*1024*1024),
       image(std::make_shared<locked_ram_fixture::Store>(static_cast<std::size_t>(capacity))){
    StoreIdentity volume{},id{};volume[0]=std::byte{5};id[0]=std::byte{6};
    constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,6,false};
    auto source=indexed?WinFspNativeWritable::create_managed_indexed(head,volume,image,id,{capacity},fast):
        WinFspNativeWritable::create_managed_hosted(head,volume,image,id,fast);
    adapter=source.get();
    adapter->enable_deferred_writes_hosted(options);
    fs.Version=static_cast<UINT16>(sizeof(fs));fs.UmFileContextIsUserContext2=TRUE;
    if(observe_budget) {
      provider=this;
      const WinFspSessionApi api{preflight,create_filesystem,start,mount,stop,unmount,destroy,origin};
      const NativeWriteMountPlan plan{L"R:\\FlitzFS-model\\unused",30,true,true};
      controller=std::make_unique<WinFspVolumeController>(std::move(source),api,plan);
      eventually([&]{return controller->view().state==SessionState::running || controller->view().retired;},
          "owned deferred controller start");
      check(controller->view().state==SessionState::running,"owned deferred controller failed startup");
    } else {
      standalone=std::move(source);
      if(options.controller_health) {
        const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        check(event!=nullptr,"owned GPU budget event fixture");
        try{adapter->set_maintenance_event(event);}
        catch(...){CloseHandle(event);throw;}
        CloseHandle(event);
      }
      fs.Interface=&WinFspNativeWritable::callbacks();
      adapter->bind_callbacks(fs,origin);
    }
    SIZE_T bytes=0;UINT32 attrs=0;
    check(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attrs,nullptr,&bytes)==STATUS_BUFFER_OVERFLOW,
        "deferred root descriptor size");
    security.resize(bytes);
    check(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attrs,security.data(),&bytes)==STATUS_SUCCESS,
        "deferred root descriptor");
    receiver=&collector;
  }
  ~Harness(){
    if(controller)controller->stop_and_join();
    if(standalone){
      while(adapter->stop_deferred_writes_hosted()==NativeDeferredStop::running)Sleep(1);
      standalone.reset();
    }
    provider=nullptr;
    receiver=nullptr;
  }
  PVOID create(PCWSTR path){
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
    check(fs.Interface->Create(&fs,const_cast<PWSTR>(path),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,
        FILE_ATTRIBUTE_NORMAL,security.data(),0,&context,&info)==STATUS_SUCCESS&&context,
        "deferred callback create");return context;
  }
  template<class Refresh>
  FSP_FSCTL_TRANSACT_RSP reply(unsigned index,Refresh refresh){
    FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
    eventually([&]{
      refresh();
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
Harness* Harness::provider=nullptr;
void budget_demand(const std::filesystem::path& worker) {
  if(!gpu_lz4_cpu_verification_available())return;
  auto settings=options(worker,1);
  settings.controller_health=health();
  auto& initial=*settings.controller_health;
  initial.memory.captured_ms-=kGpuMaxBudgetAgeMs+1;
  Harness h(settings,true);
  const auto target=h.create(L"\\target");
  auto bytes=native_lz4_fixture::source();
  const auto write=[&](ULONG size) {
    ULONG count=0;
    FSP_FSCTL_FILE_INFO info{};
    check(h.fs.Interface->Write(&h.fs,target,bytes.data(),0,size,FALSE,FALSE,&count,&info)==STATUS_SUCCESS &&
        count==size,"unready budget did not select synchronous CPU");
  };
  write(4096);
  check(!h.adapter->gpu_budget_task(),"small write triggered GPU telemetry");
  write(static_cast<ULONG>(bytes.size()));
  const auto demand=h.adapter->gpu_budget_task();
  check(demand && demand->luid==initial.memory.luid && demand->after_ms==initial.memory.captured_ms,
      "eligible stale write missed its GPU budget request");
  for(unsigned i=0;i<3;++i)write(static_cast<ULONG>(bytes.size()));
  check(h.adapter->gpu_budget_task()->sequence==demand->sequence,"stale writes did not coalesce budget work");
  auto different=*demand;
  ++different.sequence;
  auto observed=health().memory;
  check(!h.adapter->complete_gpu_budget_task(different,observed,GetTickCount64()) &&
      h.adapter->gpu_budget_task()->sequence==demand->sequence,"unowned budget result moved the demand");
  observed.luid[0]=std::byte{2};
  check(!h.adapter->complete_gpu_budget_task(*demand,observed,GetTickCount64()) &&
      !h.adapter->gpu_budget_task(),"wrong adapter retained budget authority");
  h.adapter->publish_deferred_health_hosted(health());
  write(static_cast<ULONG>(bytes.size()));
  check(!h.adapter->gpu_budget_task() && !h.adapter->deferred_write_stats().queued &&
      h.adapter->snapshot().read("target",0,static_cast<ULONG>(bytes.size()))==bytes,
      "unavailable adapter retried or did not preserve CPU bytes");
  h.close(target);
}
void completion_budget_refusal(const std::filesystem::path& worker) {
  if(!gpu_lz4_cpu_verification_available())return;
  auto settings=options(worker,1);
  settings.controller_health=health();
  settings.controller_health->memory.captured_ms-=kGpuMaxBudgetAgeMs+1;
  Harness h(settings,true);
  const auto target=h.create(L"\\target");
  const auto block=native_lz4_fixture::source();
  std::vector<std::byte> bytes;
  for(unsigned i=0;i<16;++i)bytes.insert(bytes.end(),block.begin(),block.end());
  const auto write=[&](UINT64 offset) {
    ULONG count=0;
    FSP_FSCTL_FILE_INFO info{};
    const auto status=h.fs.Interface->Write(&h.fs,target,bytes.data(),offset,static_cast<ULONG>(bytes.size()),
        FALSE,FALSE,&count,&info);
    check((status==STATUS_PENDING && !count) || (status==STATUS_SUCCESS && count==bytes.size()),
        "budget completion fixture write");
    return status;
  };
  check(write(0)==STATUS_SUCCESS,"stale completion fixture did not start on CPU");
  const auto demand=h.adapter->gpu_budget_task();
  check(demand.has_value(),"CPU write did not request a fresh budget");
  auto current=health().memory;
  check(h.adapter->complete_gpu_budget_task(*demand,current,GetTickCount64()),"valid initial budget refused");
  const auto expected_hint=hints.load();
  const auto offset=static_cast<UINT64>(bytes.size());
  check(write(offset)==STATUS_PENDING,"fresh evidence did not admit owned worker");
  std::optional<NativeGpuBudgetTask> completion;
  eventually([&]{completion=h.adapter->gpu_budget_task();return completion.has_value();},
      "independent reply validation did not request a newer budget");
  check(completion->sequence>demand->sequence && completion->after_ms>=current.captured_ms,
      "worker completion did not bind its newer budget");
  check(!h.adapter->complete_gpu_budget_task(*completion,{},GetTickCount64()),
      "unavailable budget accepted a worker result");
  const auto response=h.reply(0,[]{});
  const auto stats=h.adapter->deferred_write_stats();
  check(response.Hint==expected_hint && response.IoStatus.Status==STATUS_SUCCESS &&
      response.IoStatus.Information==bytes.size() && stats.cpu_forced==1 &&
      stats.worker_submitted==1 && !stats.advised,"completion resource loss failed to select CPU exactly once");
  check(h.adapter->snapshot().read("target",offset,static_cast<ULONG>(bytes.size()))==bytes,
      "CPU fallback after completion resource loss changed bytes");
  h.close(target);
  NativeDeferredStop stopped=NativeDeferredStop::running;
  eventually([&]{stopped=h.adapter->stop_deferred_writes_hosted();return stopped!=NativeDeferredStop::running;},
      "budget completion fixture did not retire");
  check(stopped==NativeDeferredStop::quiescent && h.adapter->deferred_write_stats().response_attempts==1 &&
      !h.adapter->gpu_budget_task() && !h.adapter->requires_offline(),
      "unavailable completion budget retained ownership");
}
void run(const std::filesystem::path& worker,unsigned kind,bool indexed){
  Harness h(options(worker,kind),indexed);const auto target=h.create(L"\\target");
  const auto prior=h.adapter->snapshot();
  const auto block=native_lz4_fixture::source();std::vector<std::byte> bytes;
  const unsigned blocks=indexed && kind==0?17:16;
  for(unsigned i=0;i<blocks;++i)bytes.insert(bytes.end(),block.begin(),block.end());
  const auto offset=indexed?32ULL*1024*1024:0;
  const auto expected=bytes;ULONG count=999;FSP_FSCTL_FILE_INFO info{};info.FileSize=999;
  h.adapter->publish_deferred_health_hosted(health());
  const auto expected_hint=hints.load();
  check(h.fs.Interface->Write(&h.fs,target,bytes.data(),offset,static_cast<ULONG>(bytes.size()),FALSE,FALSE,
      &count,&info)==STATUS_PENDING && count==0 && info.FileSize==0,"callback did not own/defer write");
  std::fill(bytes.begin(),bytes.end(),std::byte{0xff});
  if(kind==2 && gpu_lz4_cpu_verification_available()){
    eventually([&]{h.adapter->publish_deferred_health_hosted(health());
      return h.adapter->deferred_write_stats().worker_submitted==1;},"held callback child not submitted");
    UniformLz4WorkerController unrelated(WorkerExecution::cpu_semantics);
    check(!WorkerExchangeSession::available() && unrelated.try_retire(),
        "CPU-only ownership depended on unrelated live worker retirement");
    // A subsequent read forces CPU publication without waiting for the held child.
    std::vector<std::byte> actual(expected.size());ULONG got=0;
    check(h.fs.Interface->Read(&h.fs,target,actual.data(),offset,static_cast<ULONG>(actual.size()),&got)==STATUS_SUCCESS &&
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
  const auto response=h.reply(0,[&]{h.adapter->publish_deferred_health_hosted(health());});
  check(response.Kind==FspFsctlTransactWriteKind && response.Hint==expected_hint &&
      response.IoStatus.Status==STATUS_SUCCESS && response.IoStatus.Information==expected.size() &&
      response.Rsp.Write.FileInfo.FileSize==offset+expected.size(),"deferred response lost committed facts");
  check(h.adapter->snapshot().read("target",offset,static_cast<std::uint32_t>(expected.size()))==expected &&
      prior.read("target",offset,1).empty(),"deferred CPU current/prior readback");
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
  if(indexed) {
    const auto reopened=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
    reopened.verify_all();
    check(reopened.read("target",offset,static_cast<std::uint32_t>(expected.size()))==expected,
        "indexed deferred data did not reopen independently");
    std::cout<<"indexed_deferred mode="<<kind<<" offset="<<offset<<" write_bytes="<<expected.size()
        <<" submitted="<<stats.worker_submitted<<" advised="<<stats.advised<<" forced="<<stats.cpu_forced<<'\n';
  }
}
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
// The diagnostic independently removes the first literal prefix from the on-image
// stream to compare its body with the verified worker receipt.
std::vector<std::byte> extent_lz4_body(std::span<const std::byte> payload) {
  check(!payload.empty(),"indexed device record payload");
  const auto token=std::to_integer<unsigned>(payload.front());
  std::size_t literals=token>>4,at=1;
  if(literals==15) {
    unsigned extra=255;
    while(extra==255) {
      check(at<payload.size(),"indexed device literal extension absent");
      extra=std::to_integer<unsigned>(payload[at++]);literals+=extra;
    }
  }
  const std::string_view magic="FLTXPE01";
  constexpr std::size_t prefix=24;
  check(literals>=prefix && literals<=payload.size()-at &&
      std::memcmp(payload.data()+at,magic.data(),magic.size())==0,"indexed device literal prefix");
  literals-=prefix;
  std::vector<std::byte> result{static_cast<std::byte>(
      (((std::min)(literals,std::size_t{15}))<<4)|(token&15U))};
  if(literals>=15) {
    literals-=15;
    while(literals>=255){result.push_back(std::byte{255});literals-=255;}
    result.push_back(static_cast<std::byte>(literals));
  }
  result.insert(result.end(),payload.begin()+static_cast<std::ptrdiff_t>(at+prefix),payload.end());
  return result;
}
unsigned verified_image_records(const Harness& h,const NativeUniformWriteResult& receipt,std::uint64_t offset) {
  const auto paged=PagedObjectStoreSnapshot::open(h.image,{h.capacity});
  const detail::NativeNameIndex index(paged);
  constexpr std::size_t header_size=128;
  const auto first_block=offset/kNativeExtentBytes;
  const std::string_view magic="FLTXDAT1";
  const auto before=paged.info();
  std::vector<std::byte> image(static_cast<std::size_t>(before.physical_end));
  h.image->read_exact(0,image);
  unsigned matched=0;
  for(std::uint64_t i=0;i<receipt.candidate_count;++i) {
    const auto object_id=index.find(detail::native_extent_key(receipt.info.IndexNumber,first_block+i));
    check(object_id.has_value(),"indexed device extent mapping missing");
    const auto object=paged.stat(*object_id);
    check(object && object->logical_bytes==kNativeExtentBytes+24,"indexed device extent missing");
    unsigned copies=0;
    auto cursor=image.begin();
    while(cursor!=image.end()) {
      cursor=std::search(cursor,image.end(),magic.begin(),magic.end(),
          [](std::byte a,char b){return a==static_cast<std::byte>(b);});
      if(cursor==image.end())break;
      const auto offset=static_cast<std::size_t>(cursor-image.begin());
      ++cursor;
      if(image.size()-offset<header_size)break;
      const auto header=std::span<const std::byte>(image).subspan(offset,header_size);
      using deferred_native_fixture::integer;
      if(integer(header,16,8)!=object->id || integer(header,24,8)!=object->generation)continue;
      check(integer(header,12,4)==detail::kStoreLz4 && integer(header,32,4)==object->logical_bytes &&
          integer(header,36,4)==object->stored_payload_bytes,"indexed device record codec or framing");
      const auto unit=paged.isolation_unit();
      std::vector<std::byte> payload(object->stored_payload_bytes);
      for(std::size_t done=0;done<payload.size();) {
        const auto relative=offset%unit+header_size+done;
        const auto start=offset-offset%unit+2*(relative/unit)*unit+relative%unit;
        const auto count=(std::min)(payload.size()-done,static_cast<std::size_t>(unit-relative%unit));
        check(start<=image.size() && count<=image.size()-start,"indexed device payload bounds");
        std::copy_n(image.begin()+static_cast<std::ptrdiff_t>(start),count,
            payload.begin()+static_cast<std::ptrdiff_t>(done));
        done+=count;
      }
      const auto body=extent_lz4_body(payload);
      check(body.size()==receipt.advice.payload_bytes && detail::record_digest(body)==receipt.advice.payload_digest,
          "indexed stored body differs from verified HIP output");
      ++copies;
    }
    check(copies==2,"indexed device extent must have two matching copies");
    matched+=copies;
  }
  return matched;
}
unsigned hex_digest_digit(wchar_t c) {
  if(c>=L'0' && c<=L'9')return static_cast<unsigned>(c-L'0');
  if(c>=L'a' && c<=L'f')return 10U+static_cast<unsigned>(c-L'a');
  throw std::invalid_argument("expected lowercase SHA256");
}
int gpu_indexed(const std::filesystem::path& worker,std::wstring_view digest,const wchar_t* runtime,bool sweep) {
  check(store_lz4_available()&&gpu_lz4_cpu_verification_available(),"independent indexed CPU decoder unavailable");
  check(digest.size()==64,"verified worker SHA256 required");
  gpu_probe_fixture::Budget controller_budget;
  HipRouteDiscoveryRequest request;
  request.helper=worker;request.runtime_basename=runtime;request.worker={128,2000,5000};
  request.maximum_identity_queries=4;
  for(std::size_t i=0;i<request.helper_sha256.size();++i)
    request.helper_sha256[i]=static_cast<std::byte>(16*hex_digest_digit(digest[2*i])+hex_digest_digit(digest[2*i+1]));
  const auto discovery=discover_system_hip_route(request);
  std::cout<<"indexed_gpu_discovery status="<<static_cast<unsigned>(discovery.status)
      <<" queries="<<discovery.identity_queries<<" worker_exit="<<discovery.worker_exit<<'\n';
  check(discovery.status==HipDiscoveryStatus::ready && discovery.route && discovery.route->pins,
      "indexed AMD route not admitted");
  const auto& route=*discovery.route;
  NativeDeferredWriteOptions options;
  options.hint_query=hint;options.response_sink=send;
  options.allow_gpu=true;options.workload_qualified=true;options.helper_independent=true;
  options.binding.helper=worker;options.binding.configuration=route.identity;
  options.binding.ordinal=static_cast<int>(route.ordinal);
  options.binding.runtime_sha256=route.runtime_sha256;
  options.binding.capture_advice_receipt=true;
  options.budget=request.worker;options.deadline_ms=options.budget.timeout_ms;
  auto& cost=options.cost;
  cost.configuration=route.identity;cost.fixed_bytes=256ULL<<20;
  cost.input_copies=6;cost.slab_copies=2;cost.output_copies=3;cost.per_chunk_bytes=64;
  cost.profiled_chunk_bytes=kNativeExtentBytes;cost.kernel_ns_per_chunk=10000000;
  constexpr std::uint32_t quantum_chunks=2;
  const auto chunks=sweep?8U:quantum_chunks;
  cost.maximum_grid_x=chunks;cost.qualified_kernel_quantum_ns=200000000;cost.qualified=true;
  options.window={16,100000000};
  if(sweep) {
    uniform_lz4::Layout quantum{};
    check(uniform_lz4::make_layout(quantum_chunks*kNativeExtentBytes,kNativeExtentBytes,quantum),
        "AMD sweep quantum layout");
    options.binding.sweep=NativeUniformSweepGrant{quantum_chunks,quantum.total_bytes};
  }
  GpuHealthEvidence observed;
  observed.configuration=route.identity;
  observed.sequence=1;
  observed.compatible=true;
  observed.supervisor_quiescent=true;
  observed.memory=route.memory;
  options.controller_health=observed;
  options.controller_route_pins=route.pins;

  {
    Harness h(options,true,true);
    const auto target=h.create(L"\\target");const auto prior=h.adapter->snapshot();
    eventually([&]{return GetTickCount64()-route.memory.captured_ms>kGpuMaxBudgetAgeMs;},
        "initial probe should expire before testing on-demand collection");
    const auto block=native_lz4_fixture::source();
    std::vector<std::byte> expected;
    expected.reserve(chunks*block.size());
    for(unsigned i=0;i<chunks;++i)expected.insert(expected.end(),block.begin(),block.end());
    const auto planned=plan_gpu_lz4_window(expected.size(),kNativeExtentBytes,observed,observed.memory.captured_ms,
        cost,options.window);
    std::cout<<"indexed_gpu_plan gate="<<static_cast<unsigned>(planned.code)
        <<" input_bytes="<<planned.layout.input_bytes<<" host_peak="<<planned.host_peak_bytes<<'\n';
    check(planned.code==GpuGateCode::admitted && planned.layout.input_bytes==expected.size(),
        "indexed HIP work window not admitted");
    auto source=expected;
    constexpr std::uint64_t offset=32ULL*1024*1024;
    ULONG count=0;FSP_FSCTL_FILE_INFO info{};
    check(h.fs.Interface->Write(&h.fs,target,source.data(),0,static_cast<ULONG>(source.size()),FALSE,FALSE,
        &count,&info)==STATUS_SUCCESS && count==expected.size(),"absent fresh GPU budget delayed CPU write");
    eventually([&]{return h.controller->view().gpu_budget_queries==1;},
        "ordinary write did not trigger GPU budget observation");
    const auto expected_hint=hints.load();
    const auto started=std::chrono::steady_clock::now();
    check(h.fs.Interface->Write(&h.fs,target,source.data(),offset,static_cast<ULONG>(source.size()),FALSE,FALSE,
        &count,&info)==STATUS_PENDING && count==0 && info.FileSize==0,"indexed HIP callback did not defer");
    std::fill(source.begin(),source.end(),std::byte{0xff});
    const auto response=h.reply(0,[]{});
    eventually([&]{return h.controller->view().gpu_budget_queries==2;},
        "verified GPU completion did not request a fresh budget");
    const auto observations=h.controller->view();
    check(observations.gpu_budget_refusals==0,"AMD budget observer rejected its trusted adapter");
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-started).count();
    check(response.Kind==FspFsctlTransactWriteKind && response.Hint==expected_hint &&
        response.IoStatus.Status==STATUS_SUCCESS && response.IoStatus.Information==expected.size() &&
        response.Rsp.Write.FileInfo.FileSize==offset+expected.size(),"indexed HIP completion facts");
    auto stopped=NativeDeferredStop::running;
    eventually([&]{stopped=h.adapter->stop_deferred_writes_hosted();return stopped!=NativeDeferredStop::running;},
        "indexed HIP worker retirement deadline");
    const auto stats=h.adapter->deferred_write_stats();
    const auto receipt=h.adapter->deferred_write_receipt();
    std::cout<<"indexed_gpu_budget_queries="<<observations.gpu_budget_queries
        <<" refused="<<observations.gpu_budget_refusals<<'\n';
    std::cout<<"indexed_gpu_outcome submitted="<<stats.worker_submitted<<" advised="<<stats.advised
        <<" forced="<<stats.cpu_forced<<" retired="<<stats.retirement_confirmed
        <<" candidates="<<(receipt?receipt->candidate_count:0)<<'\n';
    check(stopped==NativeDeferredStop::quiescent && stats.retirement_confirmed && stats.thread_exited &&
        stats.worker_submitted==1 && stats.advised==1 && stats.response_attempts==1 && receipt &&
        receipt->candidate_count==chunks && receipt->advice.captured && receipt->advice.uniform_payloads,
        "indexed HIP verified candidacy or retirement");
    h.close(target);
    const auto reopened=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
    reopened.verify_all();
    check(reopened.read("target",offset,static_cast<std::uint32_t>(expected.size()))==expected &&
        reopened.read("target",0,static_cast<std::uint32_t>(expected.size()))==expected &&
        prior.read("target",offset,1).empty(),"indexed HIP image failed CPU readback");
    const auto copies=verified_image_records(h,*receipt,offset);
    std::cout<<"indexed_gpu_write sweep="<<sweep<<" quantum_chunks="<<quantum_chunks
        <<" source_bytes="<<expected.size()<<" offset="<<offset
        <<" worker_payload_bytes="<<receipt->candidate_bytes<<" matched_copies="<<copies
        <<" image_bytes="<<h.image->extent()<<" response_ns="<<ns<<" parent_peak_commit="
        <<controller_budget.peak_commit()<<'\n';
  }
  check(locked_ram_fixture::locked_bytes.load()==0 && locked_ram_fixture::cleanup_ok.load() &&
      WorkerExchangeSession::available() && !GetModuleHandleW(request.runtime_basename.c_str()),
      "indexed HIP backing/worker retirement or parent runtime ownership");
  return 0;
}
#endif
}
int wmain(int argc,wchar_t** argv){try{
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if(argc==5) {
    const std::wstring_view mode(argv[1]);
    if(mode==L"--gpu-indexed-once" || mode==L"--gpu-indexed-sweep")
      return gpu_indexed(argv[2],argv[3],argv[4],mode==L"--gpu-indexed-sweep");
  }
#endif
  check(argc==2,"trusted CPU helper required");const std::filesystem::path worker(argv[1]);
  check(worker.is_absolute(),"absolute CPU helper required");
  transport_required();
  budget_demand(worker);
  completion_budget_refusal(worker);
  for(const auto indexed : {false,true})for(unsigned kind=0;kind<4;++kind){
    run(worker,kind,indexed);
    check(locked_ram_fixture::locked_bytes.load()==0 && locked_ram_fixture::cleanup_ok.load(),
        "deferred fixture locked/working-set leak");
  }
  std::cout<<"Deferred native callbacks: checks="<<checks
      <<" owned_input=true exact_response=true CPU_fallback=true controller_joined=true"
      <<" gpu_executed=false mounted=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
