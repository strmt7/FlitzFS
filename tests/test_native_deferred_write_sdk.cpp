// User-mode callback qualification. The explicit AMD modes run separately from CTest.
#include "winfsp_native.hpp"
#include "winfsp_controller.hpp"
#include "locked_ram_fixture.hpp"
#include "memory_volume.hpp"
#include "native_lz4_fixture.hpp"
#include "native_amd_write_policy.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "flitzfs/volume_head.hpp"
// Type-check the driver-independent composition fixture in the ordinary SDK
// build too. Its execution remains in the separate runtime qualification gate.
#include "rocm_compile/deferred_native_fixture.hpp"
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
#include "gpu_budget.hpp"
#include "hip_route_discovery.hpp"
#include "native_lz4_capture.hpp"
#include "native_name_index.hpp"
#include "winfsp_volume_attachment.hpp"
#include "flitzfs/read_only_file.hpp"
#include "flitzfs/uniform_lz4_batch.hpp"
#include "uniform_lz4_sweep_profile.hpp"
#endif
#include <array>
#include <atomic>
#include <chrono>
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
  std::mutex mutex;
  std::array<std::array<std::byte,sizeof(FSP_FSCTL_TRANSACT_RSP)>,16> replies{};
  std::array<std::chrono::steady_clock::time_point,16> completed_at{};
  unsigned count=0;
  bool overflow=false;
};
Collector* receiver=nullptr;std::atomic<UINT64> hints{71};
bool hint(UINT64& value) noexcept{value=hints.fetch_add(1);return true;}
void send(FSP_FILE_SYSTEM*,FSP_FSCTL_TRANSACT_RSP& response) noexcept{
  std::lock_guard guard(receiver->mutex);
  if(receiver->count==receiver->replies.size()){receiver->overflow=true;return;}
  const auto slot=receiver->count++;
  receiver->completed_at[slot]=std::chrono::steady_clock::now();
  std::memcpy(receiver->replies[slot].data(),&response,sizeof(response));
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
void deferred_activity_notification(bool inject_failure) {
  auto ram=std::make_shared<native_fixture::Ram>();
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(
      ram,native_fixture::identity(),{StoreCompressionMode::raw,6,false}));
  const auto security=native_security_fixture::descriptor();
  writer.create_inode_storage_result("target",false,{11,22,33,44,FILE_ATTRIBUTE_ARCHIVE},security,0);
  const auto before=ram->stable;
  auto session=NativeFileSession::create(std::move(writer));
  const auto target=session.open("target",{true,true,false},NativeOpenKind::file);
  FSP_FILE_SYSTEM filesystem{};
  Collector received;
  receiver=&received;
  const std::unique_ptr<void,decltype(&CloseHandle)> event(CreateEventW(nullptr,FALSE,FALSE,nullptr),CloseHandle);
  check(event!=nullptr,"deferred owner event");
  std::mutex control;
  std::atomic<bool> offline=false;
  NativeDeferredWriteOptions options;
  options.expected=WorkerExecution::cpu_semantics;
  options.hint_query=hint;
  options.response_sink=send;
  const auto bytes=native_fixture::data(471,17);
  NativeDeferredWriteService service(session,control,offline,std::move(options));
  {
    std::lock_guard guard(control);
    service.bind_activity_event_locked(event.get());
    if(inject_failure)ram->fail_at=ram->calls;
    check(service.enqueue_locked(&filesystem,target,bytes,0,false,false),"deferred notification admission");
  }
  // There is no foreground callback and no GPU budget subscription to wake
  // this owner. Observe the signal before asking the worker to stop.
  check(WaitForSingleObject(event.get(),3000)==WAIT_OBJECT_0 &&
      offline.load(std::memory_order_acquire)==inject_failure,"deferred owner missed completion or fence");
  auto stop=NativeDeferredStop::running;
  eventually([&]{stop=service.stop();return stop!=NativeDeferredStop::running;},
      "deferred notification worker did not join");
  const auto stats=service.stats_locked();
  check(stop==NativeDeferredStop::quiescent && stats.response_attempts==1 && stats.worker_submitted==0,
      "deferred notification did not retire with one CPU response");
  receiver=nullptr;
  FSP_FSCTL_TRANSACT_RSP response;
  check(received.count==1,"deferred notification response count");
  std::memcpy(&response,received.replies[0].data(),sizeof(response));
  const auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(
      native_fixture::image(ram->stable)));
  recovered.verify_all();
  if(inject_failure) {
    check(!NT_SUCCESS(static_cast<NTSTATUS>(response.IoStatus.Status)) && !response.IoStatus.Information &&
        ram->stable==before && !recovered.stat("target").logical_bytes,
        "deferred fence altered the acknowledged checkpoint");
  } else {
    check(response.IoStatus.Status==STATUS_SUCCESS && response.IoStatus.Information==bytes.size() &&
        recovered.read("target",0,static_cast<std::uint32_t>(bytes.size()))==bytes,
        "deferred publication notification lost committed bytes");
    session.cleanup(target,false);
    session.close(target);
  }
}
constexpr StoreCompressionOptions fixture_compression{StoreCompressionMode::fast,6,false};
constexpr std::uint64_t measured_offset=32ULL*1024*1024;
constexpr unsigned grouped_files=4;
std::string grouped_file(unsigned member) {
  return member?"peer-"+std::to_string(member):"target";
}
std::vector<std::byte> grouped_bytes(unsigned member) {
  auto bytes=native_fixture::data(kNativeExtentBytes,997);
  bytes[1024+member*193]^=static_cast<std::byte>(member);
  bytes.back()=std::byte{0x7c};
  return bytes;
}
std::unique_ptr<WinFspNativeWritable> seeded_group_adapter(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::shared_ptr<StoreByteIo> image,StoreIdentity id,std::uint64_t capacity) {
  const auto snapshot=[&] {
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(image,id,{capacity},fixture_compression));
    const auto security=native_security_fixture::descriptor();
    writer.set_basic_metadata(1,{11,22,33,44,FILE_ATTRIBUTE_DIRECTORY});
    writer.set_security_descriptor(1,security);
    std::array<NativeExtentSelection,grouped_files> selections{};
    for(unsigned i=0;i<grouped_files;++i) {
      const auto path=grouped_file(i);
      const auto created=writer.create_inode_storage_result(path,false,{11,22,33,44,FILE_ATTRIBUTE_ARCHIVE},
          security,measured_offset+kNativeExtentBytes);
      native_fixture::file_apply(writer,native_fixture::file_write(path,measured_offset,grouped_bytes(i)));
      selections[i]={created.state.info.inode,measured_offset/kNativeExtentBytes};
    }
    check(writer.compress_shared_base(selections.front(),selections).applied,"indexed shared-base GPU fixture");
    auto grouped=writer.snapshot();
    grouped.verify_all();
    return grouped;
  }(); // Release the image writer before the head validates ownership.
  const auto epoch=[&] {
    auto owned_head=NativeVolumeHeadWriter::create(head,volume,snapshot);
    return owned_head.info().selection.epoch;
  }(); // Release the head writer before restarting the managed adapter.
  return WinFspNativeWritable::resume_indexed_retired(head,volume,epoch,
      [&](const StoreIdentity& requested){return requested==id?image:nullptr;},fixture_compression);
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
  explicit Harness(std::optional<NativeDeferredWriteOptions> options,bool indexed=false,bool observe_budget=false,
      bool with_grouped_target=false,std::uint64_t image_capacity=0,
      StoreCompressionOptions compression=fixture_compression,bool measure_write_demand=false)
      :capacity(image_capacity?image_capacity:indexed?8U*1024*1024:4U*1024*1024),
       image(std::make_shared<locked_ram_fixture::Store>(static_cast<std::size_t>(capacity))){
    StoreIdentity volume{},id{};volume[0]=std::byte{5};id[0]=std::byte{6};
    auto source=with_grouped_target?seeded_group_adapter(head,volume,image,id,capacity):
        indexed?WinFspNativeWritable::create_managed_indexed(head,volume,image,id,{capacity},compression):
        WinFspNativeWritable::create_managed_hosted(head,volume,image,id,compression);
    adapter=source.get();
    if(measure_write_demand)check(adapter->enable_amd_write_demand(),"CPU write demand could not be enabled");
    if(options)adapter->enable_deferred_writes_hosted(*options);
    fs.Version=static_cast<UINT16>(sizeof(fs));fs.UmFileContextIsUserContext2=TRUE;
    if(observe_budget) {
      provider=this;
      const WinFspSessionApi api{preflight,create_filesystem,start,mount,stop,unmount,destroy,origin};
      const NativeWriteMountPlan plan{L"R:\\FlitzFS-model\\unused",true,true};
      controller=std::make_unique<WinFspVolumeController>(std::move(source),api,plan);
      eventually([&]{return controller->view().state==SessionState::running || controller->view().retired;},
          "owned deferred controller start");
      check(controller->view().state==SessionState::running,"owned deferred controller failed startup");
    } else {
      standalone=std::move(source);
      if(options && options->controller_health) {
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
  PVOID open(PCWSTR path) {
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
    check(fs.Interface->Open(&fs,const_cast<PWSTR>(path),(FILE_OPEN<<24)|FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,&context,&info)==
        STATUS_SUCCESS && context,"deferred callback open");
    return context;
  }
  template<class Refresh>
  FSP_FSCTL_TRANSACT_RSP reply(unsigned index,Refresh refresh,
      std::chrono::steady_clock::time_point* completed_at=nullptr){
    FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
    eventually([&]{
      refresh();
      std::lock_guard guard(collector.mutex);
      if(collector.count<=index)return false;
      if(completed_at)*completed_at=collector.completed_at[index];
      std::memcpy(&response,collector.replies[index].data(),sizeof(response));
      return true;
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
void refused_foreground_offer() {
  Harness h(std::nullopt,true,true);
  const auto target=h.create(L"\\target");
  auto first=native_fixture::data(701,19),second=native_fixture::data(659,23);
  ULONG count=0;FSP_FSCTL_FILE_INFO info{};
  const auto write=[&](auto& data,UINT64 offset){
    check(h.fs.Interface->Write(&h.fs,target,data.data(),offset,static_cast<ULONG>(data.size()),FALSE,FALSE,
        &count,&info)==STATUS_SUCCESS && count==data.size(),"qualified-route refusal changed CPU write");
  };
  write(first,0);
  check(h.controller->offer_qualified_foreground_route({}),"running owner did not receive route offer");
  eventually([&]{return h.controller->view().foreground_activation==NativeDeferredActivation::refused;},
      "unqualified route was not refused");
  write(second,first.size());
  h.close(target);
  h.controller->stop_and_join();
  check(h.controller->view().write_restart_ready && !h.controller->offer_qualified_foreground_route({}),
      "retired controller accepted an offered route");
  first.insert(first.end(),second.begin(),second.end());
  const auto recovered=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
  recovered.verify_all();
  check(recovered.read("target",0,static_cast<std::uint32_t>(first.size()))==first && h.collector.count==0,
      "unqualified route changed acknowledged data or used a deferred response");
}
void detached_write_batch(const std::filesystem::path& helper) {
  constexpr auto extent=kNativeExtentBytes;
  constexpr auto allowance=48ULL*1024*1024;
  std::vector<std::byte> bytes(16*extent);
  for(std::size_t index=0;index<16;++index)
    std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(index*extent),extent,static_cast<std::byte>(index));
  const auto selected=native_lz4_fixture::source();
  for(const auto index:{0U,9U})std::ranges::copy(selected,bytes.begin()+index*extent);
  const auto source=std::span<const std::byte>(bytes);
  const std::array requests{UniformLz4BatchInput{9,source.first(4*extent)},
      UniformLz4BatchInput{11,source.subspan(4*extent,8*extent)},
      UniformLz4BatchInput{17,source.last(4*extent)}};
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const NativeAmdStoragePolicy storage{system.dwPageSize,{StoreCompressionMode::fast,1,false}};
  auto domain=MemoryVolumeDomain::create(allowance);
  const auto running=[] {return false;};
  static_assert(sized_worker_memory_share_divisor(ProcessBudget{128,2000,5000})==16);
  static_assert(sized_worker_memory_share_divisor(ProcessBudget{128,2000,5000,32})==32);
  auto duplicate=requests;
  duplicate[1].request_id=duplicate[0].request_id;
  auto partial=requests;
  partial[1].source=partial[1].source.first(8*extent-1);
  check(!record_native_amd_cpu_batch(storage,duplicate,allowance,&domain,running) &&
      !record_native_amd_cpu_batch(storage,partial,allowance,&domain,running) &&
      !record_native_amd_cpu_batch(storage,requests,1,&domain,running),
      "Grouped replay accepted a duplicate, partial or unfunded transaction");
  const auto cancelled=[&] {return domain.usage().stores!=0;};
  check(!record_native_amd_cpu_batch(storage,requests,allowance,&domain,cancelled) && domain.usage().stores==0,
      "Cancelled grouped transaction retained its private image");
  const auto cpu=record_native_amd_cpu_batch(storage,requests,allowance,&domain,running);
  check(cpu && cpu->transactions==3 && cpu->cpu_readback_verified && cpu->retirement_confirmed &&
      !cpu->device_executed && !cpu->stored_lz4_candidates && domain.usage().resident==0,
      "Grouped CPU transaction did not independently reopen");
  // Exercise a complete group beyond the single-transaction format limit.
  std::array<UniformLz4BatchInput,kStoreMaxTransactionBytes/(16*extent)+1> larger{};
  for(std::size_t index=0;index<larger.size();++index)larger[index]={index+1,source};
  const auto large=record_native_amd_cpu_batch(storage,larger,allowance,&domain,running);
  check(large && large->transactions==larger.size() && large->cpu_readback_verified &&
      large->retirement_confirmed && domain.usage().resident==0,"Group inherited the single-write limit");
  std::cout<<"grouped_cpu_limit writes="<<large->transactions<<" bytes="<<source.size()*larger.size()
      <<" occupied="<<large->occupied_bytes<<" retired=1\n";
  if(!gpu_lz4_cpu_verification_available())return;

  uniform_lz4::Layout layout{};
  check(uniform_lz4::make_layout(source.size(),extent,layout),"Grouped replay wire geometry");
  const auto prepared=PreparedUniformLz4Batch::prepare(requests,layout);
  const auto& limits=prepared.source().limits();
  constexpr auto exchange_id=4861ULL;
  const auto snapshot=std::make_shared<const WorkerRequestSnapshot>(encode_uniform_lz4_request(prepared.source(),exchange_id));
  const auto result=WorkerExchangeSession::run_sized(helper,{L"uniform-native16-sweep"},snapshot,
      {static_cast<std::size_t>(limits.request_bytes),static_cast<std::size_t>(limits.reply_bytes)},
      ProcessBudget{128,2000,5000},{});
  check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed && WorkerExchangeSession::available(),
      "Grouped CPU-semantic worker did not retire");
  const auto verified=VerifiedUniformLz4Batch::accept(result.response,prepared,exchange_id,WorkerExecution::cpu_semantics);
  auto other=requests;
  other[0].source=source.first(7*extent);
  other[1].source=source.subspan(7*extent,5*extent);
  const auto repartitioned=PreparedUniformLz4Batch::prepare(other,layout);
  check(!record_native_amd_verified_batch(storage,repartitioned,verified,allowance,&domain,running),
      "Grouped reply authorized different file boundaries");
  const auto advised=record_native_amd_verified_batch(storage,prepared,verified,allowance,&domain,running);
  check(advised && advised->transactions==3 && advised->input_digest==cpu->input_digest &&
      advised->stored_lz4_candidates==2 && advised->cpu_readback_verified && advised->retirement_confirmed &&
      !advised->device_executed && domain.usage().stores==0 && domain.usage().resident==0,
      "Grouped output crossed a file boundary or retained resources");
  std::vector<std::vector<std::byte>> frames;
  for(std::size_t i=0;i<prepared.members().size();++i) {
    const auto reply=verified.reply(i);
    UniformLz4Candidates part{{reply.payload.begin(),reply.payload.end()},{reply.sizes.begin(),reply.sizes.end()}};
    frames.push_back(encode_uniform_lz4_reply(prepared.prepare_member(i),reply.request_id,
        part,WorkerExecution::cpu_semantics));
  }
  std::vector<UniformLz4Bytes> views;
  for(const auto& frame:frames)views.push_back(frame);
  const auto separately=VerifiedUniformLz4Batch::accept_members(views,prepared,WorkerExecution::cpu_semantics);
  const auto stored=record_native_amd_verified_batch(storage,prepared,separately,allowance,&domain,running);
  check(stored && stored->transactions==advised->transactions && stored->input_digest==advised->input_digest &&
      stored->occupied_bytes==advised->occupied_bytes && stored->stored_lz4_candidates==advised->stored_lz4_candidates &&
      stored->cpu_readback_verified && stored->retirement_confirmed && domain.usage().stores==0 &&
      domain.usage().resident==0,"Separate replies changed native write grouping or recovery");
  std::cout<<"detached_repeated_reply_replay writes="<<stored->transactions
      <<" stored_advice="<<stored->stored_lz4_candidates<<" exact_reopen=1 retired=1 gpu_executed=0\n";
  std::cout<<"detached_write_batch writes="<<advised->transactions<<" bytes="<<bytes.size()
      <<" cpu_occupied="<<cpu->occupied_bytes<<" advised_occupied="<<advised->occupied_bytes
      <<" stored_advice="<<advised->stored_lz4_candidates<<" retired=1 gpu_executed=0\n";
}

void native_amd_worker_plan() {
  constexpr std::uint64_t mib=1024*1024;
  constexpr ProcessBudget budget{128,2000,5000,16};
  constexpr auto allowance=256*mib;
  const NativeAmdStoragePolicy storage{4096,{StoreCompressionMode::fast,1,false}};
  const std::vector<std::byte> plain(3*kNativeExtentBytes);
  const auto bytes=std::span<const std::byte>(plain);
  const std::array equal{UniformLz4BatchInput{1,bytes.first(kNativeExtentBytes)},
      UniformLz4BatchInput{2,bytes.subspan(kNativeExtentBytes,kNativeExtentBytes)}};
  const std::array unequal{equal.front(),UniformLz4BatchInput{2,bytes.subspan(kNativeExtentBytes)}};
  const auto grouped=plan_native_amd_write_worker(storage,equal,NativeAmdTrialLaunch::prepared,budget,allowance);
  const auto repeated=plan_native_amd_write_worker(storage,equal,NativeAmdTrialLaunch::prepared_requests,budget,allowance);
  check(grouped && repeated && grouped->group.input_bytes==repeated->group.input_bytes &&
      grouped->work.chunks==2 && repeated->work.chunks==1 &&
      repeated->channel.request_bytes<grouped->channel.request_bytes &&
      repeated->channel.reply_bytes>grouped->channel.reply_bytes &&
      repeated->parent_metadata_bytes==equal.size()*sizeof(UniformLz4Bytes),
      "Separate requests did not plan their own transport and shared source");
  check(plan_native_amd_write_worker(storage,unequal,NativeAmdTrialLaunch::prepared,budget,allowance).has_value() &&
      !plan_native_amd_write_worker(storage,unequal,NativeAmdTrialLaunch::prepared_requests,budget,allowance),
      "Unequal geometry changed grouped eligibility");
  const auto& limits=grouped->host_limits;
  const auto grouped_charge=budget.memory_mib*mib+6*limits.request_bytes+3*limits.reply_bytes+
      12*limits.chunks+4*mib+kNativeExtentBytes+sizeof(std::uint64_t);
  check(grouped->fits(budget,grouped_charge) && !repeated->fits(budget,grouped_charge) &&
      !plan_native_amd_write_worker(storage,equal,NativeAmdTrialLaunch::prepared_requests,budget,grouped_charge),
      "Separate requests could use a grant that covered only the grouped reply");
  const auto burst=plan_native_amd_write_burst(storage,kNativeExtentBytes,3,budget,allowance);
  check(burst && burst->maximum_requests==3 && burst->group.input_bytes==3*kNativeExtentBytes &&
      uniform_lz4_limits(burst->work)==uniform_lz4_limits(repeated->work) &&
      burst->channel.request_bytes==repeated->channel.request_bytes &&
      burst->channel.reply_bytes==3*(repeated->channel.reply_bytes/2) &&
      burst->parent_metadata_bytes>repeated->parent_metadata_bytes,
      "Burst did not fund later frames without an assumed source");
  check(!plan_native_amd_write_burst(storage,kNativeExtentBytes,0,budget,allowance) &&
      !plan_native_amd_write_burst(storage,kNativeExtentBytes,1,budget,allowance) &&
      !plan_native_amd_write_burst(storage,kNativeExtentBytes-1,3,budget,allowance) &&
      !plan_native_amd_write_burst(storage,kNativeExtentBytes,3,budget,grouped_charge),
      "An invalid or unfunded burst passed planning");
}

void native_amd_write_preference() {
  using Preference=NativeAmdWritePreference;
  GpuConfigurationId configuration{};configuration[0]=std::byte{12};
  const NativeAmdWriteShape shape{4*kNativeExtentBytes,65536,fixture_compression};
  std::array<NativeAmdWriteComparison,3> trials{};
  for(std::size_t i=0;i<trials.size();++i) {
    auto& trial=trials[i];
    trial.amd_first=i%2!=0;
    trial.cpu={100+i*10,256,{},true,true,false};
    trial.cpu.input_digest[0]=static_cast<std::byte>(i+1);
    trial.amd={90+i,256,trial.cpu.input_digest,true,true,true,2};
  }
  const NativeAmdWriteEvidence evidence{configuration,shape,trials};
  check(evaluate_native_amd_writes(evidence)==Preference::amd,"faster matched AMD writes refused");
  trials[1].amd.transactions=2;
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"Grouped time qualified a single write");
  trials[1].amd.transactions=1;
  trials[1].amd.stored_lz4_candidates=0;
  check(evaluate_native_amd_writes(evidence)==Preference::cpu,"discarded device output qualified a route");
  trials[1].amd.stored_lz4_candidates=5;
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"impossible device extent count accepted");
  trials[1].amd.stored_lz4_candidates=2;
  trials[0].cpu.stored_lz4_candidates=1;
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"CPU control claimed device extent storage");
  trials[0].cpu.stored_lz4_candidates=0;
  for(auto& trial:trials) {
    trial.amd.elapsed_ns=120;
    trial.amd.occupied_bytes=240;
  }
  check(evaluate_native_amd_writes(evidence)==Preference::amd,"smaller AMD writes inside observed latency refused");
  trials[0].amd.elapsed_ns=121;
  check(evaluate_native_amd_writes(evidence)==Preference::cpu,"unsupported AMD latency selected");
  trials[1].amd.input_digest[0]^=std::byte{1};
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"different source bytes compared");
  trials[1].amd.input_digest=trials[1].cpu.input_digest;
  trials[1].amd_first=false;
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"constant benchmark order accepted");
  trials[1].amd_first=true;
  trials[2].amd.retirement_confirmed=false;
  check(evaluate_native_amd_writes(evidence)==Preference::unqualified,"unretired trial counted");
}
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
void partial_measured_window_uses_cpu(const std::filesystem::path& worker) {
  if(!gpu_lz4_cpu_verification_available())return;
  auto settings=options(worker,1);
  settings.controller_health=health();
  settings.binding.sweep.reset();
  settings.cost.maximum_grid_x=1;
  auto bytes=native_lz4_fixture::source();
  const auto block=bytes;
  bytes.insert(bytes.end(),block.begin(),block.end());
  settings.measured_shape=NativeAmdWriteShape{static_cast<std::uint32_t>(bytes.size()),
      static_cast<std::uint32_t>(kStoreCommitUnit),fixture_compression};
  const auto& sample=*settings.controller_health;
  const auto partial=plan_gpu_lz4_window(bytes.size(),kNativeExtentBytes,sample,sample.memory.captured_ms,
      settings.cost,settings.window);
  check(partial.code==GpuGateCode::admitted && partial.layout.input_bytes==kNativeExtentBytes &&
      !NativeDeferredWriteService::measured_write_funded(settings,sample,sample.memory.captured_ms),
      "partial coverage was mistaken for the measured workload");
  Harness h(settings,true);
  const auto target=h.create(L"\\target");
  ULONG count=0;
  FSP_FSCTL_FILE_INFO info{};
  check(h.fs.Interface->Write(&h.fs,target,bytes.data(),0,static_cast<ULONG>(bytes.size()),
      FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==bytes.size(),
      "partially funded measured write was deferred");
  check(!h.adapter->deferred_write_stats().queued && !h.adapter->gpu_budget_task() &&
      h.adapter->snapshot().read("target",0,count)==bytes,"measured-write fallback lost source bytes");
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
void check_indexed_retirement() {
  check(locked_ram_fixture::locked_bytes.load()==0 && locked_ram_fixture::cleanup_ok.load() &&
      WorkerExchangeSession::available(),"indexed backing or worker did not retire");
}
void ordinary_amd_write_demand(bool owned_controller) {
  if(!gpu_lz4_cpu_verification_available())return;
  bool configured=false;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  configured=owned_controller && has_configured_uniform_write_package();
#endif
  {
    Harness h(std::nullopt,true,owned_controller,false,0,kWinFspFileForegroundCompression,!configured);
    check(!h.adapter->enable_amd_write_demand(),"AMD write demand enabled after callback binding");
    const auto file=h.create(L"\\observed");
    auto one=native_fixture::data(kNativeExtentBytes,109);
    auto two=one;
    two.insert(two.end(),one.begin(),one.end());
    const auto write=[&](std::vector<std::byte>& data,UINT64 offset,BOOLEAN append=FALSE,BOOLEAN constrained=FALSE) {
      ULONG written=0;FSP_FSCTL_FILE_INFO info{};
      check(h.fs.Interface->Write(&h.fs,file,data.data(),offset,static_cast<ULONG>(data.size()),
          append,constrained,&written,&info)==STATUS_SUCCESS && written==data.size(),
          "ordinary CPU write did not complete");
    };
    const auto observed=[&] {
      return owned_controller?h.controller->view().amd_write_demand:h.adapter->take_amd_write_demand();
    };
    const auto verify=[&](const NativeAmdWriteDemand& demand,const std::vector<std::byte>& data,std::uint64_t sequence) {
      check(demand.sequence==sequence && demand.shape.input_bytes==data.size() &&
          demand.shape.matches_storage(kWinFspFileForegroundCompression,h.image->contract().write_failure_unit_bytes),
          "ordinary write demand did not retain exact geometry and compression policy");
      check(std::all_of(demand.samples.begin(),demand.samples.end(),[](const auto& sample){return sample.elapsed_ns>0;}) &&
          std::is_sorted(demand.samples.begin(),demand.samples.end(),[](const auto& a,const auto& b) {
            return a.completed_ms<b.completed_ms;
          }),"ordinary CPU samples were empty or unordered");
    };
    for(std::size_t i=1;i<kNativeAmdWriteMinimumPairs;++i) {
      write(one,0);
      write(two,2*kNativeExtentBytes);
    }
    write(one,1);
    write(one,0,FALSE,TRUE);
    write(one,UINT64_MAX,TRUE);
    check(!observed(),"mixed geometry or ineligible writes funded a matched sample");
    write(one,0);
    std::optional<NativeAmdWriteDemand> first;
    eventually([&]{first=observed();return first.has_value();},"first aligned demand missing");
    verify(*first,one,1);
    write(two,2*kNativeExtentBytes);
    std::optional<NativeAmdWriteDemand> second;
    eventually([&]{second=observed();return second && second->sequence==2;},"interleaved write size lost its samples");
    verify(*second,two,2);
    write(one,0);
    std::optional<NativeAmdWriteDemand> third;
    eventually([&]{third=observed();return third && third->sequence==3;},"rolling CPU samples did not refresh");
    verify(*third,one,3);
    check(third->samples.front().elapsed_ns==first->samples[1].elapsed_ns &&
        third->samples[1].elapsed_ns==first->samples[2].elapsed_ns,
        "rolling observation did not retain only its own size samples");
    check(!h.adapter->take_amd_write_demand(),"consumed CPU demand was delivered twice");
    check(h.adapter->deferred_write_stats().worker_submitted==0 && WorkerExchangeSession::available(),
        "an ordinary CPU observation launched a device worker");
    h.close(file);
    const auto reopened=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
    reopened.verify_all();
    check(reopened.read("observed",0,static_cast<std::uint32_t>(one.size()))==one &&
        reopened.read("observed",2*kNativeExtentBytes,static_cast<std::uint32_t>(two.size()))==two &&
        reopened.read("observed",4*kNativeExtentBytes,static_cast<std::uint32_t>(one.size()))==one,
        "ordinary capture changed indexed contents");
    std::cout<<"ordinary_amd_write_demand owned="<<owned_controller<<" configured="<<configured
        <<" ready_shapes=2 one_bytes="<<first->shape.input_bytes<<" two_bytes="<<second->shape.input_bytes
        <<" one_cpu_ns="<<first->samples.front().elapsed_ns<<" two_cpu_ns="<<second->samples.front().elapsed_ns
        <<" cpu_readback=1 worker_submitted=0\n";
  }
  check_indexed_retirement();
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
constexpr ProcessBudget diagnostic_worker_budget{128,2000,5000};
std::vector<std::byte> indexed_source(unsigned chunks) {
  const auto block=native_lz4_fixture::source();
  std::vector<std::byte> source;
  source.reserve(chunks*block.size());
  for(unsigned i=0;i<chunks;++i)source.insert(source.end(),block.begin(),block.end());
  return source;
}
void indexed_oracle(const Harness& h,const NativeFilesystemSnapshot& prior,const std::vector<std::byte>& expected,
    bool grouped) {
  const auto reopened=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
  reopened.verify_all();
  check(reopened.read("target",measured_offset,static_cast<std::uint32_t>(expected.size()))==expected &&
      reopened.read("target",0,static_cast<std::uint32_t>(expected.size()))==expected,
      "indexed image failed independent CPU readback");
  if(!grouped) {
    check(prior.read("target",measured_offset,1).empty(),"prior image acquired future bytes");
    return;
  }
  check(prior.shared_base_info().members==grouped_files &&
      reopened.shared_base_info().members==grouped_files-1 && reopened.shared_base_info().bases==1 &&
      prior.read("target",measured_offset,kNativeExtentBytes)==grouped_bytes(0),
      "indexed overwrite lost the prior group or retained a stale reference");
  for(unsigned i=1;i<grouped_files;++i)
    check(reopened.read(grouped_file(i),measured_offset,kNativeExtentBytes)==grouped_bytes(i),
        "indexed overwrite changed another grouped file");
  std::cout<<"indexed_grouped_prior_members="<<grouped_files
      <<" indexed_grouped_remaining_members="<<reopened.shared_base_info().members<<'\n';
}
constexpr std::uint32_t fixture_quantum_chunks=2,fixture_sweep_chunks=16;
struct IndexedWriteFixture {
  std::uint64_t capacity=8U*1024*1024;
  StoreCompressionOptions compression=fixture_compression;
};
GpuLz4ResidentCost diagnostic_uniform_cost(const HipDiscoveredRoute& route) {
  return {.fixed_bytes=256ULL<<20,.input_copies=6,.slab_copies=2,.output_copies=3,.per_chunk_bytes=64,
      .configuration=route.identity,.profiled_chunk_bytes=kNativeExtentBytes,.maximum_grid_x=fixture_quantum_chunks,
      .kernel_ns_per_chunk=10000000,.qualified_kernel_quantum_ns=fixture_quantum_chunks*10000000ULL,.qualified=true};
}
NativeAmdWriteSample indexed_trial(const HipDiscoveredRoute* route,bool sweep,
    const std::vector<std::byte>& expected,bool expect_uniform_advice,
    bool prove_owned_input=false,NativeAmdWriteShape* shape=nullptr,bool grouped=false,bool late_activation=false,
    IndexedWriteFixture fixture={}) {
  constexpr auto quantum_chunks=fixture_quantum_chunks;
  check(!expected.empty() && expected.size()%kNativeExtentBytes==0,"incomplete comparison extent");
  const auto chunks=static_cast<std::uint32_t>(expected.size()/kNativeExtentBytes);
  auto source=expected;
  if(!route) {
    NativeAmdWriteSample sample;
    {
      Harness h(std::nullopt,true,true,grouped,fixture.capacity,fixture.compression);
      const auto target=grouped?h.open(L"\\target"):h.create(L"\\target");
      const auto prior=h.adapter->snapshot();
      ULONG count=0;FSP_FSCTL_FILE_INFO info{};
      const auto primed=h.fs.Interface->Write(&h.fs,target,source.data(),0,static_cast<ULONG>(source.size()),
          FALSE,FALSE,&count,&info);
      if(primed!=STATUS_SUCCESS || count!=source.size())
        std::cerr<<"indexed_cpu_prime status="<<static_cast<std::uint32_t>(primed)
            <<" transferred="<<count<<" image_bytes="<<h.image->extent()<<'\n';
      check(primed==STATUS_SUCCESS && count==source.size(),"indexed CPU priming write");
      const auto started=std::chrono::steady_clock::now();
      const auto status=h.fs.Interface->Write(&h.fs,target,source.data(),measured_offset,
          static_cast<ULONG>(source.size()),FALSE,FALSE,&count,&info);
      const auto elapsed=std::chrono::steady_clock::now()-started;
      check(status==STATUS_SUCCESS && count==source.size() && info.FileSize==measured_offset+source.size(),
          "indexed CPU write outcome");
      h.close(target);
      indexed_oracle(h,prior,expected,grouped);
      if(shape)*shape={static_cast<std::uint32_t>(source.size()),
          h.image->contract().write_failure_unit_bytes,fixture.compression};
      sample={static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
          h.image->extent(),detail::record_digest(expected),true,false,false};
    }
    check_indexed_retirement();
    sample.retirement_confirmed=true;
    return sample;
  }
  check(store_lz4_available()&&gpu_lz4_cpu_verification_available(),"independent indexed CPU decoder unavailable");
  NativeDeferredWriteOptions options;
  options.hint_query=hint;options.response_sink=send;
  options.allow_gpu=true;options.workload_qualified=true;options.helper_independent=true;
  options.binding.helper=route->helper;options.binding.configuration=route->identity;
  options.binding.ordinal=static_cast<int>(route->ordinal);
  options.binding.runtime_sha256=route->runtime_sha256;
  options.binding.capture_advice_receipt=true;
  options.budget=diagnostic_worker_budget;options.deadline_ms=options.budget.timeout_ms;
  const auto& cost=options.cost=diagnostic_uniform_cost(*route);
  options.window={16,cost.qualified_kernel_quantum_ns};
  if(sweep) {
    uniform_lz4::Layout quantum{};
    check(uniform_lz4::make_layout(quantum_chunks*kNativeExtentBytes,kNativeExtentBytes,quantum),
        "AMD sweep quantum layout");
    options.binding.sweep=NativeUniformSweepGrant{quantum_chunks,quantum.total_bytes};
  }
  const GpuHealthEvidence observed{route->identity,1,0,true,true,route->memory};
  options.controller_health=observed;
  options.controller_route_pins=route->pins;
  NativeAmdWriteSample sample;
  {
    Harness h(late_activation?std::nullopt:std::optional{options},true,true,grouped,fixture.capacity,fixture.compression);
    const auto target=grouped?h.open(L"\\target"):h.create(L"\\target");
    const auto prior=h.adapter->snapshot();
    const auto marker=native_fixture::data(71,492);
    if(late_activation) {
      auto before=h.create(L"\\before-route");
      auto bytes=marker;
      ULONG written=0;FSP_FSCTL_FILE_INFO before_info{};
      check(h.fs.Interface->Write(&h.fs,before,bytes.data(),0,static_cast<ULONG>(bytes.size()),FALSE,FALSE,
          &written,&before_info)==STATUS_SUCCESS && written==bytes.size(),"first write waited for the AMD route");
      h.close(before);
      options.measured_shape=NativeAmdWriteShape{static_cast<std::uint32_t>(source.size()),
          h.image->contract().write_failure_unit_bytes,fixture.compression};
      check(!NativeDeferredWriteService::measured_write_funded(options,observed,observed.memory.captured_ms),
          "measured sweep was funded with kernel costs alone");
      // The live fixture exercises route activation independently of performance qualification.
      options.cost.sweep_timing=GpuLz4SweepTiming{3000000000,250000000};
      auto mismatch=options;
      ++mismatch.measured_shape->foreground.zstd_level;
      check(h.controller->offer_qualified_foreground_route(std::move(mismatch)),
          "running controller did not queue the incompatible measurement");
      eventually([&]{return h.controller->view().foreground_activation==NativeDeferredActivation::refused;},
          "different foreground policy activated a measured route");
      auto partial=options;
      partial.binding.sweep.reset();
      check(h.controller->offer_qualified_foreground_route(std::move(partial)),
          "running controller did not receive partial device funding");
      eventually([&]{return h.controller->view().foreground_activation==NativeDeferredActivation::refused;},
          "partial device funding activated an exact measured route");
      check(h.controller->offer_qualified_foreground_route(options),"running controller did not queue its AMD route");
      eventually([&]{return h.controller->view().foreground_activation==NativeDeferredActivation::active;},
          "running controller did not activate its AMD route");
      check(!h.controller->offer_qualified_foreground_route(options),"owner allowed a second live write service");
    }
    eventually([&]{return GetTickCount64()-route->memory.captured_ms>kGpuMaxBudgetAgeMs;},
        "initial probe should expire before testing on-demand collection");
    if(shape)check(shape->matches_storage(fixture.compression,h.image->contract().write_failure_unit_bytes) &&
        shape->input_bytes==expected.size(),"CPU and AMD write geometry differs");
    const auto single=plan_gpu_lz4_window(expected.size(),kNativeExtentBytes,observed,observed.memory.captured_ms,
        cost,options.window);
    const auto planned=options.binding.sweep?plan_gpu_lz4_sweep_window(expected.size(),kNativeExtentBytes,
        observed,observed.memory.captured_ms,cost,options.window,*options.binding.sweep,options.deadline_ms):single;
    std::cout<<"indexed_gpu_plan gate="<<static_cast<unsigned>(planned.code)
        <<" input_bytes="<<planned.layout.input_bytes<<" single_launch_bytes="<<single.layout.input_bytes
        <<" device_peak_bytes="<<planned.quantum.total_bytes<<" host_peak="<<planned.host_peak_bytes<<'\n';
    check(planned.code==GpuGateCode::admitted && planned.layout.input_bytes==expected.size() &&
        planned.quantum.chunks==(std::min)(chunks,quantum_chunks) &&
        (!sweep || chunks<=quantum_chunks || single.layout.input_bytes<expected.size()),
        "indexed HIP finite work window not admitted");
    ULONG count=0;FSP_FSCTL_FILE_INFO info{};
    check(h.fs.Interface->Write(&h.fs,target,source.data(),0,static_cast<ULONG>(source.size()),FALSE,FALSE,
        &count,&info)==STATUS_SUCCESS && count==expected.size(),"absent fresh GPU budget delayed CPU write");
    eventually([&]{return h.controller->view().gpu_budget_queries==1;},
        "ordinary write did not trigger GPU budget observation");
    const auto expected_hint=hints.load();
    const auto started=std::chrono::steady_clock::now();
    check(h.fs.Interface->Write(&h.fs,target,source.data(),measured_offset,static_cast<ULONG>(source.size()),FALSE,FALSE,
        &count,&info)==STATUS_PENDING && count==0 && info.FileSize==0,"indexed HIP callback did not defer");
    if(prove_owned_input)std::fill(source.begin(),source.end(),std::byte{0xff});
    std::chrono::steady_clock::time_point completed;
    const auto response=h.reply(0,[]{},&completed);
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
        completed-started).count();
    check(response.Kind==FspFsctlTransactWriteKind && response.Hint==expected_hint &&
        response.IoStatus.Status==STATUS_SUCCESS && response.IoStatus.Information==expected.size() &&
        response.Rsp.Write.FileInfo.FileSize==measured_offset+expected.size(),"indexed HIP completion facts");
    auto stopped=NativeDeferredStop::running;
    eventually([&]{stopped=h.adapter->stop_deferred_writes_hosted();return stopped!=NativeDeferredStop::running;},
        "indexed HIP worker retirement deadline");
    const auto stats=h.adapter->deferred_write_stats();
    const auto receipt=h.adapter->deferred_write_receipt();
    std::cout<<"indexed_gpu_outcome submitted="<<stats.worker_submitted<<" advised="<<stats.advised
        <<" forced="<<stats.cpu_forced<<" retired="<<stats.retirement_confirmed
        <<" candidates="<<(receipt?receipt->candidate_count:0)<<" response_ns="<<ns;
    if(receipt) {
      std::cout<<" gate="<<static_cast<unsigned>(receipt->worker.gate);
      std::cout<<" worker_ms="<<receipt->worker.process_elapsed_ms
          <<" worker_peak_commit="<<receipt->worker.process_peak_commit_bytes;
      if(receipt->worker.exchange)
        std::cout<<" exchange="<<static_cast<unsigned>(*receipt->worker.exchange);
      if(receipt->worker.exit_code)std::cout<<" worker_exit="<<*receipt->worker.exit_code;
    }
    std::cout<<'\n';
    h.close(target);
    indexed_oracle(h,prior,expected,grouped);
    if(late_activation) {
      const auto reopened=NativeFilesystemSnapshot::open_image(h.image,{NativeImageKind::indexed_paged,h.capacity});
      check(reopened.read("before-route",0,static_cast<std::uint32_t>(marker.size()))==marker,
          "live route activation changed the first CPU write");
      std::cout<<"live_foreground_activation cpu_before=exact shape_refused=1 activated=1 partial_refused=1\n";
    }
    if(stats.advised)
      eventually([&]{return h.controller->view().gpu_budget_queries==2;},
          "verified GPU completion did not request a fresh budget");
    const auto observations=h.controller->view();
    std::cout<<"indexed_gpu_budget_queries="<<observations.gpu_budget_queries
        <<" refused="<<observations.gpu_budget_refusals<<'\n';
    check(observations.gpu_budget_refusals==0,"AMD budget observer rejected its trusted adapter");
    check(stopped==NativeDeferredStop::quiescent && stats.retirement_confirmed && stats.thread_exited &&
        stats.worker_submitted==1 && stats.advised==1 && stats.response_attempts==1 && receipt &&
        receipt->candidate_count==chunks && receipt->stored_lz4_candidates<=chunks && receipt->advice.captured &&
        (!expect_uniform_advice || receipt->advice.uniform_payloads),
        "indexed HIP verified candidacy or retirement");
    std::cout<<"indexed_gpu_write sweep="<<sweep<<" quantum_chunks="<<planned.quantum.chunks
        <<" source_bytes="<<expected.size()<<" offset="<<measured_offset
        <<" worker_payload_bytes="<<receipt->candidate_bytes
        <<" stored_lz4_candidates="<<receipt->stored_lz4_candidates;
    if(expect_uniform_advice) {
      const auto matched=verified_image_records(h,*receipt,measured_offset);
      check(receipt->stored_lz4_candidates==chunks && matched==2*receipt->stored_lz4_candidates,
          "committed advice count differs from independently verified image records");
      std::cout<<" matched_copies="<<matched;
    }
    std::cout<<" image_bytes="<<h.image->extent()<<" response_ns="<<ns<<'\n';
    sample={static_cast<std::uint64_t>(ns),h.image->extent(),detail::record_digest(expected),true,false,true,
        receipt->stored_lz4_candidates};
  }
  check_indexed_retirement();
  check(!GetModuleHandleW(route->runtime_basename.c_str()),"HIP runtime entered the filesystem process");
  sample.retirement_confirmed=true;
  return sample;
}
const HipDiscoveredRoute& require_indexed_route(const HipRouteDiscoveryResult& discovery) {
  std::cout<<"indexed_gpu_discovery status="<<static_cast<unsigned>(discovery.status)
      <<" queries="<<discovery.identity_queries<<" worker_exit="<<discovery.worker_exit<<'\n';
  check(discovery.status==HipDiscoveryStatus::ready && discovery.route && discovery.route->pins,
      "indexed AMD route not admitted");
  const auto& route=*discovery.route;
  auto changed=route.runtime_sha256;changed[0]^=std::byte{1};
  check(!route.pins->matches(route.helper,route.identity,changed,static_cast<int>(route.ordinal),route.memory),
      "different runtime digest was accepted by pinned route");
  return route;
}
int gpu_indexed(const std::filesystem::path& worker,std::wstring_view digest,const wchar_t* runtime,bool sweep) {
  check(digest.size()==64,"verified worker SHA256 required");
  gpu_probe_fixture::Budget controller_budget;
  HipRouteDiscoveryRequest request;
  request.helper=worker;request.runtime_basename=runtime;request.worker=diagnostic_worker_budget;
  request.maximum_identity_queries=4;
  for(std::size_t i=0;i<request.helper_sha256.size();++i)
    request.helper_sha256[i]=static_cast<std::byte>(16*hex_digest_digit(digest[2*i])+hex_digest_digit(digest[2*i+1]));
  const auto discovery=discover_system_hip_route(request);
  const auto input=indexed_source(sweep?fixture_sweep_chunks:fixture_quantum_chunks);
  (void)indexed_trial(&require_indexed_route(discovery),sweep,input,true,true);
  std::cout<<"parent_peak_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
int compare_indexed_amd_writes(const std::vector<std::byte>& input,bool synthetic,bool grouped,IndexedWriteFixture fixture) {
  gpu_probe_fixture::Budget controller_budget;
  std::cout<<"indexed_corpus="<<(!synthetic)<<" input_bytes="<<input.size()
      <<" mode="<<static_cast<unsigned>(fixture.compression.mode)<<" zstd_level="<<fixture.compression.zstd_level<<'\n';
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"installed route did not pin its compute module");
  std::array<NativeAmdWriteComparison,3> pairs{};
  NativeAmdWriteShape shape{};
  for(std::size_t i=0;i<pairs.size();++i) {
    auto& pair=pairs[i];
    pair.amd_first=i%2!=0;
    if(pair.amd_first)pair.amd=indexed_trial(&route,true,input,synthetic,false,&shape,grouped,false,fixture);
    pair.cpu=indexed_trial(nullptr,true,input,false,false,&shape,grouped,false,fixture);
    if(!pair.amd_first)pair.amd=indexed_trial(&route,true,input,synthetic,false,&shape,grouped,false,fixture);
    std::cout<<"indexed_pair="<<i+1<<" amd_first="<<pair.amd_first<<" cpu_ns="<<pair.cpu.elapsed_ns
        <<" amd_ns="<<pair.amd.elapsed_ns<<" cpu_image_bytes="<<pair.cpu.occupied_bytes
        <<" amd_image_bytes="<<pair.amd.occupied_bytes
        <<" amd_stored_lz4_candidates="<<pair.amd.stored_lz4_candidates<<'\n';
  }
  const NativeAmdWriteEvidence evidence{route.identity,shape,pairs};
  const auto preference=evaluate_native_amd_writes(evidence);
  check(preference!=NativeAmdWritePreference::unqualified,"matched installed AMD observations failed");
  GpuLz4ResidentCost unqualified;
  check(!qualify_native_amd_writes(route,evidence,unqualified,{},diagnostic_worker_budget,5000),
      "write outcomes alone qualified an unprofiled launch");
  std::cout<<"indexed_preference="<<(preference==NativeAmdWritePreference::amd?"amd":"cpu")
      <<" parent_peak_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
std::vector<std::byte> read_indexed_write_corpus(const std::filesystem::path& corpus,std::uint64_t maximum) {
  check(corpus.is_absolute(),"corpus path must be absolute");
  const auto file=ReadOnlyFile::open(corpus);
  const auto bytes=file.size();
  check(bytes && bytes<=maximum && bytes%kNativeExtentBytes==0,"comparison corpus must fill complete native extents");
  std::vector<std::byte> input(static_cast<std::size_t>(bytes));
  file.read_exact_at(0,input.data(),input.size());
  return input;
}
int compare_installed_amd(const std::filesystem::path& corpus={},bool grouped=false) {
  auto input=corpus.empty()?indexed_source(fixture_sweep_chunks):
      read_indexed_write_corpus(corpus,fixture_sweep_chunks*kNativeExtentBytes);
  check(input.size()==fixture_sweep_chunks*kNativeExtentBytes,"comparison corpus must fill the declared fixture exactly");
  return compare_indexed_amd_writes(input,corpus.empty(),grouped,{});
}
int compare_file_policy(const std::filesystem::path& corpus,bool cpu_only) {
  constexpr auto maximum=kNativeMaxDirtyExtents*kNativeExtentBytes;
  const auto input=read_indexed_write_corpus(corpus,maximum);
  // Two writes and both payload copies, plus room for test image metadata.
  const IndexedWriteFixture fixture{4ULL*input.size()+2U*1024*1024,kWinFspFileForegroundCompression};
  if(cpu_only) {
    NativeAmdWriteShape shape{};
    const auto sample=indexed_trial(nullptr,true,input,false,false,&shape,false,false,fixture);
    check(sample.cpu_readback_verified && sample.retirement_confirmed && !sample.device_executed &&
        !sample.stored_lz4_candidates,"file policy CPU reference was not independently verified");
    std::cout<<"file_policy_cpu bytes="<<shape.input_bytes<<" isolation_unit="<<shape.isolation_unit
        <<" mode="<<static_cast<unsigned>(shape.foreground.mode)<<" zstd_level="<<shape.foreground.zstd_level
        <<" image_bytes="<<sample.occupied_bytes<<" response_ns="<<sample.elapsed_ns<<'\n';
    return 0;
  }
  return compare_indexed_amd_writes(input,false,false,fixture);
}
int installed_late_foreground() {
  gpu_probe_fixture::Budget controller_budget;
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"live foreground route has no pinned uniform compute module");
  const auto sample=indexed_trial(&route,true,indexed_source(fixture_sweep_chunks),true,true,nullptr,false,true);
  check(sample.device_executed && sample.stored_lz4_candidates && sample.cpu_readback_verified && sample.retirement_confirmed,
      "live foreground route did not settle the AMD write");
  return 0;
}
enum class SweepLaunch {immediate,prepared,discard};
struct RecordedSweepBatch {
  std::vector<VerifiedUniformLz4Batch> results;
  std::uint64_t elapsed_ns=0,child_ms=0,ready_ns=0,request_to_verified_ns=0;
  std::uint32_t launches=0;
  bool discarded=false;
};
std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point start) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count());
}
struct RecordedSweepProfile {
  UniformLz4SweepProfiledReply reply;
  std::uint64_t total_ns=0,fixed_ns=0;
};
template<class Accept>
auto measure_installed_sweep(const HipDiscoveredRoute& route,const PreparedUniformLz4& batch,
    std::uint32_t quantum_chunks,std::uint64_t request_id,std::wstring_view command,std::size_t extra_reply_bytes,
    Accept&& accept) {
  const auto started=std::chrono::steady_clock::now();
  auto cost=diagnostic_uniform_cost(route);
  cost.maximum_grid_x=quantum_chunks;
  cost.qualified_kernel_quantum_ns=quantum_chunks*cost.kernel_ns_per_chunk;
  uniform_lz4::Layout quantum{};
  check(uniform_lz4::make_layout(std::uint64_t(quantum_chunks)*kNativeExtentBytes,kNativeExtentBytes,quantum),
      "profile quantum geometry");
  const NativeUniformSweepGrant grant{quantum_chunks,quantum.total_bytes};
  const NativeUniformWorkerBinding binding{.helper=route.helper,.configuration=route.identity,
      .ordinal=static_cast<int>(route.ordinal),.runtime_sha256=route.runtime_sha256,.sweep=grant};
  check(route.pins->device_ready() && WorkerExchangeSession::available(),"profile device not available");
  WindowsGpuBudgetSampler sampler(route.memory.luid,route.memory.node_mask);
  const auto before=sampler.sample();
  check(before.status==WindowsGpuBudgetStatus::success,"profile GPU budget unavailable");
  const GpuHealthEvidence evidence{route.identity,request_id,0,true,true,before.snapshot};
  const auto plan=plan_gpu_lz4_sweep_window(batch.layout().input_bytes,kNativeExtentBytes,
      evidence,GetTickCount64(),cost,{16,cost.qualified_kernel_quantum_ns},grant,diagnostic_worker_budget.timeout_ms);
  check(plan.code==GpuGateCode::admitted && !plan.remaining_input_bytes && plan.quantum.chunks==quantum_chunks &&
      uniform_worker_host_budget_covers(batch.limits(),diagnostic_worker_budget,plan.host_peak_bytes),
      "profile could not admit the whole requested sweep");
  auto dispatch=prepare_native_uniform_dispatch(binding,evidence,batch,WorkerExecution::hip,&grant);
  const WorkerChannelCapacity channel{batch.limits().request_bytes,batch.limits().reply_bytes+extra_reply_bytes};
  dispatch.arguments[0]=command;
  dispatch.arguments[6]=std::to_wstring(channel.reply_bytes);
  auto frame=std::make_shared<const WorkerRequestSnapshot>(encode_uniform_lz4_request(batch,request_id));
  const auto worker=WorkerExchangeSession::run_sized(binding.helper,dispatch.arguments,std::move(frame),channel,
      diagnostic_worker_budget,{},route.pins);
  const auto retired=WorkerExchangeSession::available();
  if(worker.code!=WorkerExchangeCode::completed || !worker.process.exit_confirmed || !retired)
    std::cerr<<"sweep_profile_worker_failure request="<<request_id<<" quantum="<<quantum_chunks
        <<" code="<<static_cast<unsigned>(worker.code)<<" worker_exit="<<worker.process.exit_code
        <<" worker_ms="<<worker.process.elapsed_ms<<" exit_confirmed="<<worker.process.exit_confirmed
        <<" retired="<<retired<<'\n';
  check(worker.code==WorkerExchangeCode::completed && worker.process.exit_confirmed && retired,
      "profiled worker did not publish and retire");
  const UniformLz4ProfileBinding profile{request_id,route.ordinal,route.memory.node_mask,
      grant.maximum_chunks,static_cast<std::uint32_t>(kNativeExtentBytes),grant.maximum_device_bytes,route.memory.luid};
  auto receipt=accept(worker.response,profile,dispatch.windows);
  const auto after=sampler.sample();
  check(after.status==WindowsGpuBudgetStatus::success && after.snapshot.captured_ms>before.snapshot.captured_ms &&
      check_gpu_memory_budget(after.snapshot,route.memory.luid,route.memory.node_mask,GetTickCount64(),
          dispatch.device_peak_bytes)==GpuGateCode::admitted,"profile completion budget unavailable");
  return std::pair{std::move(receipt),elapsed_ns(started)};
}
RecordedSweepProfile profile_installed_sweep(const HipDiscoveredRoute& route,const PreparedUniformLz4& batch,
    std::uint32_t quantum_chunks,std::uint64_t request_id,UniformLz4SweepProfileMode mode) {
  const auto command=mode==UniformLz4SweepProfileMode::timeline?
      L"--driver-uniform-lz4-sweep-timeline-adapter":L"--driver-uniform-lz4-sweep-profile-adapter";
  auto [reply,duration]=measure_installed_sweep(route,batch,quantum_chunks,request_id,command,
      uniform_lz4_sweep_profile_bytes(mode),[&](UniformLz4Bytes response,const auto& binding,auto windows){
        auto accepted=accept_uniform_lz4_sweep_profile_reply(response,batch,binding,WorkerExecution::hip,mode);
        check(accepted.trace.completed_windows==windows,"profile window count differs from dispatch");
        return accepted;
      });
  const auto& trace=reply.trace;
  check(trace.total_wall_ns<=duration,"profile total disagrees with parent");
  std::uint64_t window_phases=0;
  for(auto phase=static_cast<unsigned>(UniformSweepPhase::admission);
      phase<=static_cast<unsigned>(UniformSweepPhase::completion);++phase)
    window_phases+=trace.wall_ns[phase];
  std::cout<<"sweep_profile request="<<request_id<<" quantum="<<quantum_chunks
      <<" windows="<<trace.completed_windows<<" parent_ns="<<duration<<" fixed_ns="<<duration-window_phases
      <<" setup_ns="<<trace.wall_ns[static_cast<unsigned>(UniformSweepPhase::setup)]
      <<" upload_ns="<<trace.wall_ns[static_cast<unsigned>(UniformSweepPhase::upload)]
      <<" kernel_wait_ns="<<trace.wall_ns[static_cast<unsigned>(UniformSweepPhase::kernel_wait)]
      <<" download_ns="<<trace.wall_ns[static_cast<unsigned>(UniformSweepPhase::download)]
      <<" retirement_ns="<<trace.wall_ns[static_cast<unsigned>(UniformSweepPhase::retirement)]
      <<" maximum_window_ns="<<trace.maximum_quantum_wall_ns<<" maximum_kernel_wait_ns="<<trace.maximum_kernel_wall_ns
      <<" encoded_bytes="<<reply.candidates.payload.size()<<" verified=1 retired=1\n";
  if(trace.timeline){
    const auto report=[&](const char* phase,const UniformLz4KernelInterval& interval){
      std::cout<<"sweep_kernel request="<<request_id<<" phase="<<phase<<" window="<<interval.window
          <<" host_ns="<<interval.host_wall_ns<<" launch_ns="<<interval.launch_call_ns
          <<" sync_ns="<<interval.sync_call_ns<<" stream_ns="<<interval.stream_interval_ns<<'\n';
    };
    report("first",trace.timeline->first);
    if(trace.completed_windows>1)report("slowest_later",trace.timeline->slowest_later);
  }
  return {std::move(reply),duration,duration-window_phases};
}
int profile_indexed_cpu_sizes() {
  gpu_probe_fixture::Budget controller_budget;
  constexpr std::array sizes{1U,2U,4U,8U,16U,32U,static_cast<unsigned>(kNativeMaxDirtyExtents)};
  for(std::size_t trial=0;trial<kNativeAmdWriteMinimumPairs;++trial) {
    for(std::size_t lane=0;lane<sizes.size();++lane) {
      const auto size=sizes[trial%2?sizes.size()-lane-1:lane];
      const auto input=indexed_source(size);
      const IndexedWriteFixture fixture{4ULL*input.size()+2U*1024*1024,kWinFspFileForegroundCompression};
      NativeAmdWriteShape shape;
      const auto sample=indexed_trial(nullptr,true,input,false,false,&shape,false,false,fixture);
      check(sample.cpu_readback_verified && sample.retirement_confirmed && !sample.device_executed,
          "CPU scaling trial did not independently verify the memory image");
      std::cout<<"indexed_cpu_memory trial="<<trial+1<<" bytes="<<shape.input_bytes
          <<" mode="<<static_cast<unsigned>(shape.foreground.mode)<<" zstd_level="<<shape.foreground.zstd_level
          <<" response_ns="<<sample.elapsed_ns<<" occupied_bytes="<<sample.occupied_bytes
          <<" cpu_readback=1 retired=1\n";
    }
  }
  std::cout<<"indexed_cpu_memory_done peak_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
int compare_profiled_sweep_quanta(std::span<const std::byte> input,UniformLz4SweepProfileMode mode) {
  gpu_probe_fixture::Budget controller_budget;
  check(input.size()==fixture_sweep_chunks*kNativeExtentBytes,"profile requires the complete matched input");
  uniform_lz4::Layout layout{};
  check(uniform_lz4::make_layout(input.size(),kNativeExtentBytes,layout),"profile source layout");
  const auto batch=PreparedUniformLz4::prepare(input,uniform_lz4_limits(layout));
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"profile requires pinned compute module");
  std::uint64_t sequence=0;
  for(unsigned pair=0;pair<3;++pair) {
    RecordedSweepProfile narrow,wide;
    const bool wide_first=pair%2!=0;
    if(wide_first)wide=profile_installed_sweep(route,batch,4,++sequence,mode);
    narrow=profile_installed_sweep(route,batch,2,++sequence,mode);
    if(!wide_first)wide=profile_installed_sweep(route,batch,4,++sequence,mode);
    check(narrow.reply.candidates.sizes==wide.reply.candidates.sizes &&
        narrow.reply.candidates.payload==wide.reply.candidates.payload,"launch size changed the verified GPU result");
    std::cout<<"sweep_profile_pair pair="<<pair+1<<" wide_first="<<wide_first
        <<" narrow_ns="<<narrow.total_ns<<" wide_ns="<<wide.total_ns
        <<" narrow_fixed_ns="<<narrow.fixed_ns<<" wide_fixed_ns="<<wide.fixed_ns
        <<" identical_payload=1\n";
  }
  std::cout<<"sweep_profile_done parent_peak_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
int compare_stream_bound_sweeps(const std::filesystem::path& corpus) {
  gpu_probe_fixture::Budget controller_budget;
  const auto input=read_indexed_write_corpus(corpus,fixture_sweep_chunks*kNativeExtentBytes);
  check(input.size()==fixture_sweep_chunks*kNativeExtentBytes,"Stream sweep requires the complete matched corpus");
  uniform_lz4::Layout geometry{};
  check(uniform_lz4::make_layout(input.size(),kNativeExtentBytes,geometry),"Stream sweep source layout");
  const auto batch=PreparedUniformLz4::prepare(input,uniform_lz4_limits(geometry));
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"Stream sweep requires pinned compute module");
  std::uint64_t sequence=0;
  const auto measure=[&](bool stream_bound) {
    const auto id=++sequence;
    const auto command=stream_bound?L"--driver-uniform-lz4-stream-sweep-adapter":L"--driver-uniform-lz4-sweep-adapter";
    auto sample=measure_installed_sweep(route,batch,4,id,command,0,
        [&](UniformLz4Bytes response,const auto&,auto windows){
          check(windows==4,"Stream sweep changed its four-window plan");
          return accept_uniform_lz4_reply(response,batch,id,WorkerExecution::hip);
        });
    std::cout<<"stream_sweep request="<<id<<" stream_bound="<<stream_bound<<" parent_ns="<<sample.second
        <<" encoded_bytes="<<sample.first.payload.size()<<" verified=1 retired=1\n";
    return sample;
  };
  for(unsigned pair=0;pair<3;++pair){
    const bool stream_first=pair%2!=0;
    auto first=measure(stream_first);
    auto second=measure(!stream_first);
    check(first.first.sizes==second.first.sizes && first.first.payload==second.first.payload,
        "Transfer order changed the verified encoding");
    std::cout<<"stream_sweep_pair pair="<<pair+1<<" stream_first="<<stream_first
        <<" separated_ns="<<(stream_first?second.second:first.second)
        <<" stream_bound_ns="<<(stream_first?first.second:second.second)<<" identical_payload=1\n";
  }
  std::cout<<"stream_sweep_done parent_peak_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
RecordedSweepBatch run_sweep_batch(const HipDiscoveredRoute& route,
    std::span<const UniformLz4BatchInput> requests,bool grouped,std::uint64_t& sequence,
    SweepLaunch launch=SweepLaunch::immediate) {
  check(grouped || launch==SweepLaunch::immediate,"prepared sweep must use one complete exchange");
  constexpr auto width=kNativeExtentBytes;
  uniform_lz4::Layout quantum{};
  check(uniform_lz4::make_layout(fixture_quantum_chunks*width,width,quantum),"batch diagnostic quantum");
  const NativeUniformSweepGrant grant{fixture_quantum_chunks,quantum.total_bytes};
  const auto cost=diagnostic_uniform_cost(route);
  const GpuLz4WindowPolicy policy{16,cost.qualified_kernel_quantum_ns};
  const NativeUniformWorkerBinding binding{.helper=route.helper,.configuration=route.identity,
      .ordinal=static_cast<int>(route.ordinal),.runtime_sha256=route.runtime_sha256,.sweep=grant};
  WindowsGpuBudgetSampler budget(route.memory.luid,route.memory.node_mask);
  RecordedSweepBatch measured;
  const auto start=std::chrono::steady_clock::now();
  while(!requests.empty()){
    const auto work=grouped?requests:requests.first(1);
    check(route.pins->device_ready() && WorkerExchangeSession::available(),"batch route unavailable");
    const auto observed=budget.sample();
    check(observed.status==WindowsGpuBudgetStatus::success,"batch budget observation failed");
    const GpuHealthEvidence evidence{route.identity,++sequence,0,true,true,observed.snapshot};
    const auto planned=plan_uniform_lz4_batch(work,width,evidence,GetTickCount64(),cost,policy,grant,
        diagnostic_worker_budget.timeout_ms);
    check(planned.requests==work.size(),"budget did not cover the matched complete requests");
    const auto prepared=PreparedUniformLz4Batch::prepare(work,planned);
    const NativeUniformSweepGrant admitted{planned.window.quantum.chunks,planned.window.quantum.total_bytes};
    auto dispatch=prepare_native_uniform_dispatch(binding,evidence,prepared.source(),WorkerExecution::hip,&admitted);
    const auto& limits=prepared.source().limits();
    const WorkerChannelCapacity channel{limits.request_bytes,limits.reply_bytes};
    const auto frame=[&] {
      return std::make_shared<const WorkerRequestSnapshot>(encode_uniform_lz4_request(prepared.source(),sequence));
    };
    WorkerExchangeResult result;
    auto submitted=std::chrono::steady_clock::now();
    if(launch==SweepLaunch::immediate) {
      result=WorkerExchangeSession::run_sized(binding.helper,dispatch.arguments,frame(),
          channel,diagnostic_worker_budget,{},route.pins);
    } else {
      dispatch.arguments.front()=L"--driver-uniform-lz4-prepared-adapter";
      const auto setup=std::chrono::steady_clock::now();
      auto worker=WorkerExchangeSession::start_prepared_sized(binding.helper,dispatch.arguments,
          channel,diagnostic_worker_budget,route.pins);
      if(!worker.wait_prepared(diagnostic_worker_budget.timeout_ms)) {
        const auto* failure=worker.try_result();
        std::cerr<<"prepared_sweep_unavailable route="<<(failure?static_cast<unsigned>(failure->code):999)
            <<" worker_exit="<<(failure?failure->process.exit_code:0)<<'\n';
        check(false,"prepared device did not become ready");
      }
      measured.ready_ns=elapsed_ns(setup);
      check(!worker.try_result(),"readiness exposed a reply");
      submitted=std::chrono::steady_clock::now();
      if(launch==SweepLaunch::discard)check(worker.request_cancel(),"unable to discard prepared sweep");
      else {
        const auto fresh=budget.sample();
        check(fresh.status==WindowsGpuBudgetStatus::success &&
            check_gpu_memory_budget(fresh.snapshot,route.memory.luid,route.memory.node_mask,GetTickCount64(),
                dispatch.device_peak_bytes)==GpuGateCode::admitted,"prepared dispatch budget failed");
        check(worker.submit_prepared(frame()),"unable to supply the prepared device request");
      }
      check(WaitForSingleObject(worker.supervisor_wait_handle(),diagnostic_worker_budget.timeout_ms)==WAIT_OBJECT_0 &&
          worker.retirement_confirmed() && worker.try_result(),"prepared sweep retirement unconfirmed");
      result=*worker.try_result();
      if(launch==SweepLaunch::discard) {
        check(result.code==WorkerExchangeCode::cancelled && result.process.exit_confirmed && result.process.exit_code==0 &&
            !result.publication_observed && result.response.empty() && WorkerExchangeSession::available(),
            "unused device preparation did not retire without publishing");
        measured.discarded=true;
        measured.child_ms=result.process.elapsed_ms;
        measured.elapsed_ns=elapsed_ns(start);
        return measured;
      }
    }
    const auto retired=WorkerExchangeSession::available();
    if(result.code!=WorkerExchangeCode::completed || !result.process.exit_confirmed || !retired)
      std::cerr<<"sweep_batch_worker_failure grouped="<<grouped<<" sequence="<<sequence
          <<" requests="<<work.size()<<" code="<<static_cast<unsigned>(result.code)
          <<" worker_exit="<<result.process.exit_code<<" worker_ms="<<result.process.elapsed_ms
          <<" exit_confirmed="<<result.process.exit_confirmed<<" retired="<<retired<<'\n';
    check(result.code==WorkerExchangeCode::completed && result.process.exit_confirmed && retired,
        "batch worker did not successfully retire");
    auto verified=VerifiedUniformLz4Batch::accept(result.response,prepared,sequence,WorkerExecution::hip);
    const auto completed=budget.sample();
    check(completed.status==WindowsGpuBudgetStatus::success &&
        completed.snapshot.captured_ms>observed.snapshot.captured_ms &&
        check_gpu_memory_budget(completed.snapshot,route.memory.luid,route.memory.node_mask,GetTickCount64(),
            dispatch.device_peak_bytes)==GpuGateCode::admitted,"batch completion budget failed");
    measured.request_to_verified_ns+=elapsed_ns(submitted);
    measured.results.push_back(std::move(verified));
    measured.launches+=dispatch.windows;
    measured.child_ms+=result.process.elapsed_ms;
    requests=requests.subspan(work.size());
  }
  measured.elapsed_ns=elapsed_ns(start);
  return measured;
}
int compare_installed_detached_writes(const std::filesystem::path& corpus,bool grouped,bool repeated=false) {
  gpu_probe_fixture::Budget controller_budget;
  const auto file=ReadOnlyFile::open(corpus);
  check(file.size() && file.size()<=fixture_sweep_chunks*kNativeExtentBytes && file.size()%kNativeExtentBytes==0,
      "Detached write fixture must contain complete admitted extents");
  std::vector<std::byte> source(static_cast<std::size_t>(file.size()));
  file.read_exact_at(0,source.data(),source.size());
  const auto available=observe_memory_volume_headroom();
  const auto allowance=usable_memory_volume_headroom(available)/sized_worker_memory_share_divisor(diagnostic_worker_budget);
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const NativeAmdStoragePolicy storage{system.dwPageSize,{StoreCompressionMode::fast,1,false}};
  const auto input=std::span<const std::byte>(source);
  std::vector<UniformLz4BatchInput> requests;
  const auto extents=source.size()/kNativeExtentBytes;
  if(grouped) {
    check(extents>=2 && (!repeated || extents%2==0),"Grouped comparison needs two complete compatible writes");
    const auto first=extents/2*kNativeExtentBytes;
    requests={{1,input.first(first)},{2,input.subspan(first)}};
  } else requests={{1,input}};
  const auto chunks=(std::min<std::uint32_t>)(static_cast<std::uint32_t>(extents),fixture_quantum_chunks);
  uniform_lz4::Layout quantum{};
  check(uniform_lz4::make_layout(std::uint64_t(chunks)*kNativeExtentBytes,kNativeExtentBytes,quantum),
      "Detached write quantum");
  const NativeUniformSweepGrant grant{chunks,quantum.total_bytes};
  const auto cpu=record_native_amd_cpu_batch(storage,requests,allowance,nullptr,[]{return false;});
  check(cpu && cpu->transactions==requests.size() && !cpu->device_executed && !cpu->stored_lz4_candidates && cpu->cpu_readback_verified &&
      cpu->retirement_confirmed,"Detached CPU write did not verify");
  std::cout<<"detached_cpu_write writes="<<cpu->transactions<<" bytes="<<source.size()<<" occupied="<<cpu->occupied_bytes
      <<" complete_ns="<<cpu->elapsed_ns<<" cpu_verified=1 retired=1\n"<<std::flush;
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  const std::array launches{NativeAmdTrialLaunch::immediate,NativeAmdTrialLaunch::prepared,
      NativeAmdTrialLaunch::prepared_requests};
  for(const auto launch:std::span(launches).first(repeated?launches.size():2)) {
    const bool prepared=launch!=NativeAmdTrialLaunch::immediate;
    const bool separate=launch==NativeAmdTrialLaunch::prepared_requests;
    const auto measured=record_native_amd_device_batch(storage,requests,route,grant,diagnostic_worker_budget,
        allowance,nullptr,launch,{});
    if(!measured.sample)
      std::cerr<<"detached_amd_write_refusal prepared="<<prepared<<" code="<<static_cast<unsigned>(measured.exchange)
          <<" worker_exit="<<measured.worker_exit<<" retired="<<measured.worker_retired
          <<" ready_ns="<<measured.ready_ns<<" exchange_ns="<<measured.exchange_ns<<'\n';
    check(measured.sample && measured.sample->transactions==cpu->transactions &&
        measured.sample->input_digest==cpu->input_digest && measured.sample->device_executed &&
        measured.sample->cpu_readback_verified && measured.sample->retirement_confirmed && measured.worker_retired &&
        (prepared==(measured.ready_ns!=0)) && (!separate || measured.admitted_requests==requests.size()),
        "Detached Radeon write did not verify the same source");
    const auto& sample=*measured.sample;
    std::cout<<"detached_amd_write prepared="<<prepared<<" writes="<<sample.transactions
        <<" bytes="<<source.size()<<" occupied="<<sample.occupied_bytes
        <<" complete_ns="<<sample.elapsed_ns<<" ready_ns="<<measured.ready_ns<<" exchange_ns="<<measured.exchange_ns
        <<" stored_candidates="<<sample.stored_lz4_candidates<<" separate_requests="<<separate
        <<" admitted_requests="<<measured.admitted_requests<<" cpu_verified=1 retired=1\n";
  }
  check(MemoryVolumeDomain::cleanup_healthy() && WorkerExchangeSession::available() &&
      !GetModuleHandleW(route.runtime_basename.c_str()),"Detached write retained a runtime lease");
  std::cout<<"detached_write_comparison complete=1 scoped_gpu=1 parent_runtime_loaded=0 parent_peak="
      <<controller_budget.peak_commit()<<'\n';
  return 0;
}
int compare_installed_arriving_burst(std::wstring_view mode) {
  const bool single=mode==L"single";
  const bool duplicate=mode==L"duplicate";
  check(single || duplicate || mode==L"later","Unknown arriving write fixture");
  gpu_probe_fixture::Budget controller_budget;
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const auto memory=observe_memory_volume_headroom();
  const auto allowance=usable_memory_volume_headroom(memory)/sized_worker_memory_share_divisor(diagnostic_worker_budget);
  const NativeAmdStoragePolicy storage{system.dwPageSize,{StoreCompressionMode::fast,1,false}};
  constexpr std::uint32_t maximum_requests=3;
  const auto plan=plan_native_amd_write_burst(storage,kNativeExtentBytes,maximum_requests,
      diagnostic_worker_budget,allowance);
  check(plan.has_value(),"Arriving burst lacks a complete grant");
  const NativeUniformSweepGrant grant{plan->work.chunks,plan->work.total_bytes};
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  const auto domain_capacity=allowance-allowance%system.dwPageSize;
  auto domain=MemoryVolumeDomain::create(domain_capacity);
  constexpr std::uint64_t capacity=4ULL*1024*1024;
  std::size_t admitted=0;
  {
    auto head=domain.create_store(kVolumeHeadBytes);
    auto image=domain.create_store(capacity,storage.isolation_unit);
    StoreIdentity volume_id{},image_id{};
    volume_id[0]=std::byte{4};
    image_id[0]=std::byte{5};
    FSP_FILE_SYSTEM fs{};
    auto source=WinFspNativeWritable::create_managed_indexed(head,volume_id,image,image_id,{capacity},storage.foreground);
    check(source->enable_amd_write_demand(),"Could not observe ordinary source writes");
    fs.Version=static_cast<UINT16>(sizeof(fs));
    fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();
    source->bind_callbacks(fs,Harness::origin);
    auto security=native_security_fixture::descriptor();
    const auto input=[](unsigned ordinal) {
      auto bytes=indexed_source(1);
      bytes.front()^=static_cast<std::byte>(ordinal);
      return bytes;
    };
    const auto commit=[&](PCWSTR name,unsigned ordinal,std::size_t observations) {
      auto bytes=input(ordinal);
      PVOID file=nullptr;
      FSP_FSCTL_FILE_INFO info{};
      check(fs.Interface->Create(&fs,const_cast<PWSTR>(name),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,
          FILE_ATTRIBUTE_NORMAL,security.data(),0,&file,&info)==STATUS_SUCCESS && file,"Source callback Create");
      for(std::size_t i=0;i<observations;++i) {
        ULONG written=0;
        check(fs.Interface->Write(&fs,file,bytes.data(),0,static_cast<ULONG>(bytes.size()),FALSE,FALSE,
            &written,&info)==STATUS_SUCCESS && written==bytes.size(),"Source callback did not commit the complete write");
      }
      fs.Interface->Cleanup(&fs,file,nullptr,0);
      fs.Interface->Close(&fs,file);
      const auto demand=source->take_amd_write_demand();
      check(demand.has_value(),"Missing completed write demand");
      const auto current=observe_memory_volume_headroom();
      const auto now=GetTickCount64();
      const NativeMaintenanceObservation observation{ordinal,now,current.physical_total,current.physical_available,
          current.commit_total,current.commit_available,1000,0,1000,true,false};
      const std::atomic<bool> stopped{false};
      auto captured=source->capture_amd_write_group(*demand,observation,now,source->foreground_activity(),
          stopped,false,kNativeExtentBytes);
      check(captured.size()==1 && captured[0].sequence==demand->source_sequence && captured[0].bytes==bytes,
          "The committed callback write did not capture");
      return std::move(captured.front());
    };
    const auto first=commit(L"\\first",1,kNativeAmdWriteMinimumPairs);
    std::uint64_t later_sequence=0;
    std::uint32_t boundaries=0;
    const auto next=[&](const NativeAmdWriteBoundary& boundary,detail::NativeCapturedWrite& out) {
      const auto index=boundary.completed_requests;
      check(index==++boundaries && boundary.first_ready_ns && boundary.previous_service_ns &&
          boundary.elapsed_ns>=boundary.first_ready_ns && !WorkerExchangeSession::available(),
          "Later capture did not run while the exact worker was ready");
      if(single || index==2)return PreparedRequestAction::finish;
      out=commit(L"\\later",2,1);
      later_sequence=out.sequence;
      if(duplicate)out.sequence=first.sequence;
      return PreparedRequestAction::submit;
    };
    const auto measured=record_native_amd_device_burst(storage,{first.sequence,first.bytes},maximum_requests,
        route,grant,diagnostic_worker_budget,allowance,&domain,{},next);
    admitted=measured.admitted_requests;
    if(!measured.sample)
      std::cerr<<"arriving_burst status="<<static_cast<unsigned>(measured.exchange)
          <<" worker_exit="<<measured.worker_exit<<" retired="<<measured.worker_retired
          <<" admitted="<<admitted<<'\n';

    check(measured.worker_retired && WorkerExchangeSession::available() &&
        !GetModuleHandleW(route.runtime_basename.c_str()),"The arriving write worker did not retire");
    const auto original=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,capacity});
    original.verify_all();
    const auto original_first=original.read("first",0,kNativeExtentBytes);
    check(original_first==first.bytes,"The first acknowledged write changed");
    std::vector<std::byte> original_later;
    std::vector<UniformLz4BatchInput> inputs{{first.sequence,original_first}};
    if(later_sequence) {
      original_later=original.read("later",0,kNativeExtentBytes);
      check(original_later==input(2),"The later acknowledged write changed");
      inputs.push_back({later_sequence,original_later});
    }
    const auto cpu=record_native_amd_cpu_batch(storage,inputs,allowance,&domain,[]{return false;});
    check(cpu && cpu->cpu_readback_verified && cpu->retirement_confirmed &&
        cpu->transactions==inputs.size() && domain.usage().stores==2,"CPU did not recover the original captures");
    if(duplicate) {
      check(measured.exchange==WorkerExchangeCode::cancelled && !measured.sample && admitted==1 && boundaries==1,
          "Duplicate late identity exposed a partial replay");
    }else {
      check(measured.exchange==WorkerExchangeCode::completed && measured.sample && measured.sample->device_executed &&
          measured.sample->cpu_readback_verified && measured.sample->retirement_confirmed &&
          measured.sample->stored_lz4_candidates==inputs.size() && measured.sample->input_digest==cpu->input_digest &&
          measured.sample->transactions==inputs.size() && admitted==inputs.size() && boundaries==inputs.size(),
          "A deterministic native replay changed the supplied prefix");
    }
    std::cout<<"arriving_burst maximum="<<maximum_requests<<" admitted="<<admitted
        <<" later_commits="<<bool(later_sequence)<<" callbacks="<<boundaries<<" replayed="
        <<(measured.sample?measured.sample->transactions:0)<<" stored="
        <<(measured.sample?measured.sample->stored_lz4_candidates:0)
        <<" ready_ns="<<measured.ready_ns<<" total_device_ns="<<measured.exchange_ns
        <<" original_cpu_verified=1 independent_reopen="<<bool(measured.sample)
        <<" duplicate_cancelled="<<duplicate<<" retired=1\n";
  }
  check(MemoryVolumeDomain::cleanup_healthy() && domain.usage().stores==0 && domain.usage().resident==0,
      "Arriving write fixture retained a memory store");
  return 0;
}

int compare_installed_prepared_sweep(const std::filesystem::path& corpus) {
  gpu_probe_fixture::Budget controller_budget;
  const auto file=ReadOnlyFile::open(corpus);
  check(file.size()==fixture_sweep_chunks*kNativeExtentBytes,"prepared sweep corpus must match the full fixture");
  std::vector<std::byte> source(static_cast<std::size_t>(file.size()));
  file.read_exact_at(0,source.data(),source.size());
  const std::array requests{UniformLz4BatchInput{1,source}};
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"prepared sweep requires a pinned compute module");
  std::uint64_t sequence=0;
  for(unsigned pair=0;pair<3;++pair) {
    const bool prepared_first=pair%2!=0;
    RecordedSweepBatch immediate,prepared;
    if(prepared_first)prepared=run_sweep_batch(route,requests,true,sequence,SweepLaunch::prepared);
    immediate=run_sweep_batch(route,requests,true,sequence);
    if(!prepared_first)prepared=run_sweep_batch(route,requests,true,sequence,SweepLaunch::prepared);
    const auto a=immediate.results.at(0).reply(0),b=prepared.results.at(0).reply(0);
    check(immediate.launches==prepared.launches && std::ranges::equal(a.sizes,b.sizes) &&
        std::ranges::equal(a.payload,b.payload),"prepared route changed verified compressed bytes");
    std::cout<<"prepared_sweep_pair pair="<<pair+1<<" prepared_first="<<prepared_first
        <<" immediate_ns="<<immediate.elapsed_ns<<" prepared_ns="<<prepared.elapsed_ns
        <<" ready_ns="<<prepared.ready_ns<<" request_to_verified_ns="<<prepared.request_to_verified_ns
        <<" immediate_request_to_verified_ns="<<immediate.request_to_verified_ns
        <<" launches="<<prepared.launches<<" encoded_bytes="<<b.payload.size()
        <<" identical_payload=1 independent_cpu_verified=1 retired=1\n";
  }
  const auto discarded=run_sweep_batch(route,requests,true,sequence,SweepLaunch::discard);
  check(discarded.discarded && !discarded.launches && !GetModuleHandleW(route.runtime_basename.c_str()) &&
      WorkerExchangeSession::available(),"prepared discard loaded the parent runtime or retained a worker");
  std::cout<<"prepared_sweep_discard ready_ns="<<discarded.ready_ns<<" elapsed_ns="<<discarded.elapsed_ns
      <<" worker_ms="<<discarded.child_ms<<" published=0 retired=1 parent_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
int compare_installed_sweep_batch(const std::filesystem::path& corpus){
  gpu_probe_fixture::Budget controller_budget;
  const auto input=ReadOnlyFile::open(corpus);
  constexpr auto width=kNativeExtentBytes;
  check(input.size()==fixture_sweep_chunks*width,"sweep batch corpus must match the full fixture");
  std::vector<std::byte> source(static_cast<std::size_t>(input.size()));
  input.read_exact_at(0,source.data(),source.size());
  const auto bytes=std::span<const std::byte>(source);
  const std::array requests{UniformLz4BatchInput{20,bytes.first(4*width)},UniformLz4BatchInput{21,bytes.subspan(4*width,6*width)},
      UniformLz4BatchInput{24,bytes.subspan(10*width)}};
  const auto discovery=discover_installed_hip_route(diagnostic_worker_budget,4);
  const auto& route=require_indexed_route(discovery);
  check(route.pins->uniform_module_pinned(),"sweep batch requires a pinned compute module");
  std::uint64_t sequence=0;
  for(unsigned pair=0;pair<3;++pair){
    const bool batch_first=pair%2!=0;
    RecordedSweepBatch separate,batch;
    if(batch_first)batch=run_sweep_batch(route,requests,true,sequence);
    separate=run_sweep_batch(route,requests,false,sequence);
    if(!batch_first)batch=run_sweep_batch(route,requests,true,sequence);
    check(batch.results.size()==1 && separate.results.size()==requests.size() &&
        batch.launches==separate.launches && batch.results.front().size()==requests.size(),
        "batched candidate count or physical launches changed");
    std::size_t compressed=0;
    for(std::size_t i=0;i<requests.size();++i) {
      const auto a=separate.results.at(i).reply(0),b=batch.results.front().reply(i);
      check(a.request_id==requests[i].request_id && b.request_id==a.request_id &&
          std::ranges::equal(a.sizes,b.sizes) && std::ranges::equal(a.payload,b.payload),
          "batch changed the exact verified bytes for an independent request");
      compressed+=b.payload.size();
    }
    std::cout<<"sweep_request_batch pair="<<pair+1<<" batch_first="<<batch_first<<" members="<<requests.size()
        <<" gpu_launches="<<batch.launches<<" separate_workers="<<separate.results.size()
        <<" batch_workers="<<batch.results.size()<<" separate_ns="<<separate.elapsed_ns<<" batch_ns="<<batch.elapsed_ns
        <<" separate_child_ms="<<separate.child_ms<<" batch_child_ms="<<batch.child_ms
        <<" compressed_bytes="<<compressed<<" cpu_verified=1 identical_payload=1 retired=1\n";
  }
  check(!GetModuleHandleW(route.runtime_basename.c_str()) && WorkerExchangeSession::available(),
      "batch parent loaded the HIP runtime or retained a worker");
  std::cout<<"sweep_request_batch_peak parent_commit="<<controller_budget.peak_commit()<<'\n';
  return 0;
}
#endif
}
int wmain(int argc,wchar_t** argv){try{
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-sweep-batch")return compare_installed_sweep_batch(argv[2]);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-prepared")return compare_installed_prepared_sweep(argv[2]);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-detached-writes")
    return compare_installed_detached_writes(argv[2],false);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-detached-write-batch")
    return compare_installed_detached_writes(argv[2],true);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-resident-write-batch")
    return compare_installed_detached_writes(argv[2],true,true);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-arriving-burst")
    return compare_installed_arriving_burst(argv[2]);
  if(argc==2 && std::wstring_view(argv[1])==L"--live-installed-late-foreground")return installed_late_foreground();
  if(argc==2 && std::wstring_view(argv[1])==L"--compare-installed-amd")return compare_installed_amd();
  if(argc==2 && std::wstring_view(argv[1])==L"--compare-installed-amd-grouped")
    return compare_installed_amd({},true);
  if(argc==2 && std::wstring_view(argv[1])==L"--verify-grouped-cpu") {
    (void)indexed_trial(nullptr,true,indexed_source(fixture_sweep_chunks),false,false,nullptr,true);
    return 0;
  }
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-corpus")
    return compare_installed_amd(argv[2]);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-file-policy")
    return compare_file_policy(argv[2],false);
  if(argc==3 && std::wstring_view(argv[1])==L"--compare-installed-amd-stream-sweep-corpus")
    return compare_stream_bound_sweeps(argv[2]);
  if(argc==2 && std::wstring_view(argv[1])==L"--profile-indexed-cpu-sizes")return profile_indexed_cpu_sizes();
  if(argc==2 && std::wstring_view(argv[1])==L"--profile-installed-amd-memory-timeline")
    return compare_profiled_sweep_quanta(indexed_source(fixture_sweep_chunks),UniformLz4SweepProfileMode::timeline);
  if(argc==3 && std::wstring_view(argv[1])==L"--profile-installed-amd-sweep-corpus")
    return compare_profiled_sweep_quanta(read_indexed_write_corpus(argv[2],fixture_sweep_chunks*kNativeExtentBytes),
        UniformLz4SweepProfileMode::aggregate);
  if(argc==3 && std::wstring_view(argv[1])==L"--profile-installed-amd-sweep-timeline-corpus")
    return compare_profiled_sweep_quanta(read_indexed_write_corpus(argv[2],fixture_sweep_chunks*kNativeExtentBytes),
        UniformLz4SweepProfileMode::timeline);
  if(argc==3 && std::wstring_view(argv[1])==L"--verify-file-policy-cpu")
    return compare_file_policy(argv[2],true);
  if(argc==2) {
    const std::wstring_view mode(argv[1]);
    if(mode==L"--verify-installed-amd-absent" || mode==L"--verify-installed-amd-untrusted") {
      const auto result=discover_installed_hip_route(diagnostic_worker_budget,4);
      const auto expected=mode==L"--verify-installed-amd-absent"?
          HipDiscoveryStatus::package_unavailable:HipDiscoveryStatus::worker_untrusted;
      check(result.status==expected && !result.route && !result.identity_queries,
          "absent or untrusted installed package attempted device discovery");
      std::cout<<"indexed_gpu_discovery status="<<static_cast<unsigned>(result.status)
          <<" queries="<<result.identity_queries<<'\n';
      return 0;
    }
  }
  if(argc==5) {
    const std::wstring_view mode(argv[1]);
    if(mode==L"--gpu-indexed-once" || mode==L"--gpu-indexed-sweep")
      return gpu_indexed(argv[2],argv[3],argv[4],mode==L"--gpu-indexed-sweep");
  }
#endif
  check(argc==2,"trusted CPU helper required");const std::filesystem::path worker(argv[1]);
  check(worker.is_absolute(),"absolute CPU helper required");
  transport_required();
  for(bool fault:{false,true})deferred_activity_notification(fault);
  native_amd_worker_plan();
  native_amd_write_preference();
  detached_write_batch(worker);
  for(const bool owned:{false,true})ordinary_amd_write_demand(owned);
  refused_foreground_offer();
  budget_demand(worker);
  partial_measured_window_uses_cpu(worker);
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
