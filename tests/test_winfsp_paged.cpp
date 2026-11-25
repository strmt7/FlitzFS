#include "winfsp_native.hpp"
#include "winfsp_session.hpp"
#include "native_lz4_read_worker.hpp"
#include "native_fs_fixture.hpp"
#include "anchor_search_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "../src/native_lz4_decode_wire.hpp"
#include "../src/native_indexed_volume.hpp"
#include "../src/native_name_index.hpp"
#include "../src/store_codec.hpp"
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
class BlockingIo final:public ConcurrentRam {
 public:
  using ConcurrentRam::ConcurrentRam;
  mutable std::mutex mutex;mutable std::condition_variable changed;
  mutable std::atomic<bool> armed{false};mutable bool entered=false,released=false,expired=false;
  std::atomic<bool> reported_damage{false};
  bool backing_degraded() const noexcept override {return reported_damage.load(std::memory_order_relaxed);}
  void read_exact(std::uint64_t n,std::span<std::byte> bytes) const override {
    if(armed.exchange(false)) {
      std::unique_lock lock(mutex);
      entered=true;changed.notify_all();
      if(!changed.wait_for(lock,std::chrono::seconds(5),[&]{return released;})) {
        expired=true;
        throw std::runtime_error("controlled callback deadline");
      }
    }
    ConcurrentRam::read_exact(n,bytes);
  }
  void release(){std::lock_guard lock(mutex);released=true;changed.notify_all();}
};
struct Harness {
  std::shared_ptr<Ram> head,image,advice;std::unique_ptr<WinFspNativeWritable> adapter;FSP_FILE_SYSTEM fs{};
  std::shared_ptr<BlockingIo> image_io;
  Bytes security;Clock clock;
  explicit Harness(unsigned unit,const Bytes* previous=nullptr,const Bytes* old_head=nullptr,const Bytes* old_advice=nullptr,
      const NativeDeferredWriteOptions* deferred=nullptr,bool indexed=false,
      StoreCompressionOptions compression={StoreCompressionMode::raw,1,false},bool read_ahead=false,
      NativeVolumeHeadLayout head_layout=NativeVolumeHeadLayout::legacy,
      PagedStoreBankLayout image_layout=PagedStoreBankLayout::shared):
      head(ram(unit)),image(ram(unit)),advice(ram(unit)) {
    image_io=std::make_shared<BlockingIo>(image);
    if(previous) {
      image->live=image->stable=*previous;head->live=head->stable=*old_head;advice->live=advice->stable=*old_advice;
      adapter=WinFspNativeWritable::resume_managed_retired(head,id(90),1,[&](const auto& key)->std::shared_ptr<StoreByteIo>{
          if(key!=id(1))throw std::runtime_error("fixture image identity");return image_io;},
          {StoreCompressionMode::raw,1,false},advice,1);
    } else if(indexed)adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),image_io,id(1),
        {kStoreMaxPhysicalBytes},compression,advice,{},NativeIndexedDataLayout::extents,head_layout,image_layout);
    else adapter=WinFspNativeWritable::create_managed_paged(head,id(90),image_io,id(1),
        {kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false},advice);
    if(deferred)adapter->enable_deferred_writes_hosted(*deferred);
    if(read_ahead)require(adapter->enable_native_lz4_read_ahead(),"read-ahead fixture refused its codec");
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
  void write(PVOID context,const Bytes& bytes,std::uint64_t offset=0){ULONG count=0;FSP_FSCTL_FILE_INFO info{};
    require(fs.Interface->Write(&fs,context,const_cast<std::byte*>(bytes.data()),offset,static_cast<ULONG>(bytes.size()),
        FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==bytes.size(),"paged SDK Write");}
  void read(PVOID context,const Bytes& expected,std::uint64_t offset=0) {
    Bytes actual(expected.size());ULONG count=0;
    require(fs.Interface->Read(&fs,context,actual.data(),offset,static_cast<ULONG>(actual.size()),&count)==STATUS_SUCCESS &&
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
  auto step(){const auto observed=clock.next();return adapter->advance_background_maintenance(observed,clock.now);}
  auto finish(){for(unsigned i=0;i<1000;++i){const auto r=step();require(r.state!=WinFspMaintenanceState::unavailable,
      "paged opportunity unavailable");if(r.progress.phase==NativeMaintenancePhase::idle && !adapter->background_maintenance_pending())return r;}
    throw std::runtime_error("paged group did not quiesce");}
  void populate(){for(unsigned i=0;i<4;++i){const auto context=create(i);write(context,family(i));read(context,family(i));close(context);}}
};
void sequential_read_ahead() {
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  constexpr std::uint64_t extent=kNativeExtentBytes;
  Harness h(4096,nullptr,nullptr,nullptr,nullptr,true,fast,true);
  std::array<Bytes,5> blocks;
  Bytes file;
  for(std::size_t i=0;i<blocks.size();++i) {
    blocks[i]=native_lz4_fixture::source();
    blocks[i].front()=static_cast<std::byte>(i);
    file.insert(file.end(),blocks[i].begin(),blocks[i].end());
  }
  const auto context=h.create(43);
  h.write(context,file);
  const auto writes=h.image->write_bytes;
  h.read(context,blocks[0]);
  require(!h.adapter->native_lz4_read_ahead_pending(),"first read offered speculation");
  h.read(context,blocks[1],extent);
  require(h.adapter->native_lz4_read_ahead_pending(),"adjacent reads did not offer their following extent");
  const auto before=h.image->read_bytes;
  const auto admitted=h.clock.next();
  const auto demand_epoch=h.adapter->foreground_activity();
  auto denied=admitted;
  denied.commit_available=0;
  require(!h.adapter->advance_native_lz4_read_ahead(denied,h.clock.now,demand_epoch),"commit pressure admitted read-ahead");
  denied=admitted;denied.cpu_busy_ticks=denied.cpu_total_ticks;
  require(!h.adapter->advance_native_lz4_read_ahead(denied,h.clock.now,demand_epoch),"CPU pressure admitted read-ahead");
  require(!h.adapter->advance_native_lz4_read_ahead(admitted,h.clock.now+1000,demand_epoch) &&
      h.image->read_bytes==before,"refused read-ahead performed backing I/O");

  require(h.adapter->advance_native_lz4_read_ahead(admitted,h.clock.now,demand_epoch) &&
      !h.adapter->native_lz4_read_ahead_pending(),"funded speculation failed to prepare a single frame");
  const auto capture_bytes=h.image->read_bytes-before;
  auto reads=h.image->read_bytes;
  h.read(context,blocks[2],2*extent);
  const auto ready_bytes=h.image->read_bytes-reads;
  require(h.adapter->native_lz4_read_ahead_hits()==1,"next SDK demand did not consume ready bytes");
  reads=h.image->read_bytes;
  h.read(context,blocks[2],2*extent);
  const auto ordinary_bytes=h.image->read_bytes-reads;
  require(h.adapter->native_lz4_read_ahead_hits()==1 && ordinary_bytes>ready_bytes &&
      !h.adapter->native_lz4_read_ahead_pending(),"random reread failed its exact CPU miss");

  h.read(context,blocks[3],3*extent);
  require(h.adapter->native_lz4_read_ahead_pending(),"resumed sequential stream did not forecast");
  reads=h.image->read_bytes;
  const auto next_epoch=h.adapter->foreground_activity();
  const auto later=h.clock.next();
  require(!h.adapter->advance_native_lz4_read_ahead(admitted,h.clock.now-1000,next_epoch) && h.image->read_bytes==reads,
      "one observation funded two prefetches");
  require(!h.adapter->advance_native_lz4_read_ahead(later,h.clock.now,demand_epoch) && h.image->read_bytes==reads,
      "old foreground epoch authorized new backing work");
  h.adapter->discard_native_lz4_read_ahead(demand_epoch);
  require(h.adapter->native_lz4_read_ahead_pending(),"old foreground epoch discarded a new prediction");
  h.adapter->discard_native_lz4_read_ahead(next_epoch);
  require(!h.adapter->native_lz4_read_ahead_pending(),"backing refusal kept the same prediction queued");

  h.read(context,blocks[0]);
  h.read(context,blocks[1],extent);
  h.adapter->discard_native_lz4_read_ahead(h.adapter->foreground_activity(),true);
  require(!h.adapter->take_native_lz4_read_ahead_sample(h.adapter->foreground_activity()),
      "feedback target queued speculative I/O");
  h.read(context,blocks[0]);
  h.read(context,blocks[1],extent);
  require(h.adapter->take_native_lz4_read_ahead_sample(h.adapter->foreground_activity())==0,
      "reads outside refused extent lowered backing estimate");
  h.read(context,blocks[2],2*extent);
  const auto measured_epoch=h.adapter->foreground_activity();
  const auto demand_sample=h.adapter->take_native_lz4_read_ahead_sample(measured_epoch);
  require(demand_sample && *demand_sample>0 && h.adapter->take_native_lz4_read_ahead_sample(measured_epoch)==0,
      "matching CPU demand failed to supply one measured sample");

  h.adapter->discard_native_lz4_read_ahead(measured_epoch,true);
  detail::NativeLz4CapturedFrame captured;
  require(h.adapter->capture_native_lz4_read(context,3*extent,kNativeExtentBytes,captured),
      "fixture could not capture ready read of the refused extent");
  auto ready=detail::NativeLz4ReadyReads::decode_captured(captured);
  require(h.adapter->publish_native_lz4_reads(ready),"fixture ready read publication refused");
  h.read(context,blocks[3],3*extent);
  require(h.adapter->native_lz4_read_hits()==1 &&
      h.adapter->take_native_lz4_read_ahead_sample(h.adapter->foreground_activity())==0,
      "ready result contributed a demand cost without reading the stored frame");

  require(h.image->write_bytes==writes,"read-ahead wrote backing bytes before foreground edit");
  h.read(context,blocks[0]);
  h.read(context,blocks[1],extent);
  const auto contested=h.clock.next();
  const auto contested_epoch=h.adapter->foreground_activity();
  h.image_io->armed=true;
  bool prepared=false;
  const auto now=h.clock.now;
  std::jthread prepare([&]{prepared=h.adapter->advance_native_lz4_read_ahead(contested,now,contested_epoch);});
  {
    std::unique_lock lock(h.image_io->mutex);
    require(h.image_io->changed.wait_for(lock,std::chrono::seconds(5),[&]{return h.image_io->entered;}),
        "read-ahead did not reach controlled backing");
  }
  auto replacement=blocks[2];
  replacement.front()^=std::byte{0x71};
  Bytes before_edit(replacement.size()),after_edit(replacement.size());
  NTSTATUS first_read=STATUS_UNSUCCESSFUL,write_status=STATUS_UNSUCCESSFUL,last_read=STATUS_UNSUCCESSFUL;
  ULONG old_bytes=0,written=0,new_bytes=0;
  std::atomic<bool> foreground_completed{false};
  std::jthread demand([&]{
    first_read=h.fs.Interface->Read(&h.fs,context,before_edit.data(),2*extent,
        static_cast<ULONG>(before_edit.size()),&old_bytes);
    FSP_FSCTL_FILE_INFO info{};
    write_status=h.fs.Interface->Write(&h.fs,context,replacement.data(),2*extent,
        static_cast<ULONG>(replacement.size()),FALSE,FALSE,&written,&info);
    last_read=h.fs.Interface->Read(&h.fs,context,after_edit.data(),2*extent,
        static_cast<ULONG>(after_edit.size()),&new_bytes);
    foreground_completed.store(true,std::memory_order_release);
  });
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  while(!foreground_completed.load(std::memory_order_acquire) && std::chrono::steady_clock::now()<deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto completed_before_release=foreground_completed.load(std::memory_order_acquire);
  h.image_io->release();
  demand.join();prepare.join();
  require(completed_before_release && !h.image_io->expired && first_read==STATUS_SUCCESS &&
      old_bytes==before_edit.size() && before_edit==blocks[2] && write_status==STATUS_SUCCESS &&
      written==replacement.size() && last_read==STATUS_SUCCESS && new_bytes==after_edit.size() &&
      after_edit==replacement,"a stalled capture blocked acknowledged foreground I/O");
  require(!prepared && h.adapter->native_lz4_read_ahead_hits()==1,
      "read-ahead published from the superseded backing view");
  h.read(context,replacement,2*extent);
  h.adapter->cancel_background_maintenance();
  h.read(context,blocks[3],3*extent);
  require(!h.adapter->native_lz4_read_ahead_pending() && h.adapter->native_lz4_read_ahead_hits()==1,
      "retired maintenance resumed the optional read-ahead");
  h.close(context);
  require(!h.adapter->native_lz4_read_ahead_pending(),"closed context left speculative work");
  std::cout<<"winfsp_sequential_lz4 ready_hit=1 pressure_refused=1 backing_discard=1 foreground_superseded=1"
      <<" exact_cpu_feedback=1 unrelated_reads_ignored=1 ready_read_ignored=1 single_use_sample=1"
      <<" foreground_read_write_before_release=1 changed_bytes_exact=1"
      <<" capture_backing_bytes="<<capture_bytes<<" ready_backing_bytes="<<ready_bytes
      <<" ordinary_backing_bytes="<<ordinary_bytes<<'\n';
}
void group_read_timing() {
  detail::NativeGroupReadTiming timing;
  constexpr std::uint64_t startup=900,dispatch=30;
  require(timing.start_lead(startup)==startup && timing.submit_lead(startup,dispatch,250,false)==650,
      "An unready worker did not reserve the remaining qualified startup");
  timing.observe_accepted(4800,4700,false);
  require(timing.start_lead(startup)==4800 && timing.submit_lead(startup,dispatch,4500,false)==300 &&
      timing.submit_lead(startup,dispatch,4500,true)==dispatch,
      "An early submission contaminated ready dispatch timing");
  require(timing.submit_lead(startup,dispatch,5000,false)==4800,
      "An overdue estimate was accepted as a readiness signal");
  timing.observe_accepted(12000,45,true);
  require(timing.start_lead(startup)==4800 &&
      timing.submit_lead(startup,dispatch,880,true)==45 &&
      timing.submit_lead(startup,60,880,true)==60,
      "Waiting after readiness affected startup or the dispatch floor");
  detail::NativeGroupReadTiming dispatch_only;
  dispatch_only.observe_accepted(5000,1200,true);
  require(dispatch_only.start_lead(startup)==1200,
      "A read worker launched with less lead than its verified dispatch needs");
  timing.observe_accepted(4900,4800,false);
  require(timing.start_lead(startup)==4900 && timing.submit_lead(startup,dispatch,4900,true)==45,
      "A later cold exchange replaced the ready dispatch observation");
  timing.observe_unfinished(5200,false);
  timing.observe_unfinished(4900,false);
  require(timing.start_lead(startup)==5200 && timing.submit_lead(startup,dispatch,5000,false)==200 &&
      timing.submit_lead(startup,dispatch,5000,true)==45,
      "An unfinished cold exchange failed to preserve its lower bound");
  timing.observe_unfinished(90,true);
  timing.observe_unfinished(70,true);
  require(timing.start_lead(startup)==5200 && timing.submit_lead(startup,dispatch,5000,true)==90,
      "An unfinished ready exchange affected the wrong phase or lost its lower bound");
  timing.observe_accepted(10000,60,true);
  require(timing.start_lead(startup)==5200 && timing.submit_lead(startup,dispatch,5000,true)==60,
      "A completed ready exchange failed to refresh its phase estimate");
  std::cout<<"winfsp_group_timing startup_observed=1 ready_dispatch_observed=1 overdue_refused=1"
      <<" ready_wait_excluded=1 unfinished_cold_and_ready=1 completed_refresh=1\n";
}
void group_read_window() {
  constexpr auto extent=kNativeExtentBytes;
  const auto forecast=[] {
    WinFspReadAheadWork result;
    result.offset=7*kNativeExtentBytes;
    result.bytes=2*kNativeExtentBytes;
    result.hint_created_ms=10000;
    result.observed_lead_ms=750;
    return result;
  };
  auto later=forecast();
  constexpr WinFspGroupReadWindow device{4,1218,2};
  detail::plan_group_read_window(later,device,6,4,750,10040);
  require(later.offset==9*extent && later.bytes==4*extent &&
      later.observed_lead_ms==1500 && later.has_read_lead(1218,10040),
      "Measured device startup did not select the next funded grouped range");
  auto immediate=forecast();
  detail::plan_group_read_window(immediate,{4,600,2},6,4,750,10040);
  require(immediate.offset==7*extent && immediate.bytes==4*extent,
      "A useful immediate read forecast moved to a later range");
  auto variable=forecast();
  detail::plan_group_read_window(variable,device,6,4,0,10040);
  require(variable.offset==7*extent && variable.bytes==2*extent,
      "Unstable request widths extrapolated a grouped forecast");
  auto overdue=forecast();
  detail::plan_group_read_window(overdue,device,6,4,750,11100);
  require(overdue.offset==7*extent && overdue.bytes==2*extent,
      "A late hint exceeded the demonstrated grouped range");
  auto limited=forecast();
  detail::plan_group_read_window(limited,device,3,4,750,10040);
  require(limited.offset==7*extent && limited.bytes==2*extent,
      "The later group had too little demonstrated demand");
  auto partial_forecast=forecast();
  partial_forecast.bytes=extent;
  partial_forecast.observed_lead_ms=12;
  detail::plan_group_read_window(partial_forecast,{4,400,2},5,4,256,10010);
  require(partial_forecast.offset==9*extent && partial_forecast.bytes==3*extent &&
      partial_forecast.observed_lead_ms==524,
      "Small immediate lead was confused with the time needed to read an extent");
  std::cout<<"winfsp_group_window skipped_frames=2 useful_frames=4 unstable_refused=1 overdue_refused=1 short_refused=1\n";
}
struct GroupReadFixtureOptions {
  bool mixed=false,expanded=false,variable_widths=false,cpu_prefix=false,learned_lead=false,segmented=false;
  bool small_requests=false;
};

void group_read_ahead(std::optional<NativeLz4ReadWorkerProfile> profile={},GroupReadFixtureOptions options={}) {
  const auto [mixed,expanded,variable_widths,cpu_prefix,learned_lead,segmented,small_requests]=options;
  constexpr auto extent=kNativeExtentBytes;
  constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  std::vector<Bytes> blocks(learned_lead?28U:expanded?14U+static_cast<unsigned>(variable_widths):10U);
  Bytes file;
  const auto base=family(0);
  for(std::size_t i=0;i<blocks.size();++i) {
    blocks[i]=base;
    if(mixed) {
      if(i%2)blocks[i]=calibrated_extent(base,static_cast<unsigned>(i));
      else anchor_search_fixture::apply_sparse_xor_variation(blocks[i],static_cast<unsigned>(2*i));
    }else {
      const auto bits=data(extent,32019+static_cast<unsigned>(i));
      for(std::size_t at=0;at<extent;++at)blocks[i][at]^=bits[at]&std::byte{1};
    }
    file.insert(file.end(),blocks[i].begin(),blocks[i].end());
  }
  // Select the committed reference before reopening. Optimizer reference
  // choices are covered separately and do not determine this read fixture.
  auto seed=std::make_unique<Harness>(4096,nullptr,nullptr,nullptr,nullptr,true,raw,false,
      NativeVolumeHeadLayout::compact,PagedStoreBankLayout::compact);
  require(seed->head->extent()==8192 &&
      std::string_view(reinterpret_cast<const char*>(seed->image->live.data()),8)=="FLTXPG02",
      "grouped read did not start on compact checkpoints");
  const auto initial=seed->create(47);
  seed->write(initial,file);
  seed->close(initial);
  const auto unrelated=data(91,237);
  const auto sibling=seed->create(46);
  seed->write(sibling,unrelated);
  seed->close(sibling);
  auto head=seed->head,image=seed->image,advice=seed->advice;
  auto backing=seed->image_io;
  seed.reset();
  {
    const auto generation=PagedObjectStoreSnapshot::open(backing,limits).info().generation;
    auto writer=NativeFilesystemWriter::resume(
        PagedObjectStoreWriter::resume(backing,id(1),generation,limits,raw));
    const auto inode=writer.snapshot().stat("f47").inode;
    std::vector<NativeExtentSelection> selected(blocks.size());
    for(std::size_t i=0;i<selected.size();++i)selected[i]={inode,i};
    require(writer.compress_shared_base(selected.front(),selected).applied,
        "Grouped callback fixture did not publish its reference");
  }
  Harness h(4096,&image->stable,&head->stable,&advice->stable,nullptr,true,raw,true);
  PVOID context=nullptr,other_context=nullptr;
  FSP_FSCTL_FILE_INFO info{};
  std::wstring name=L"\\f47";
  require(h.fs.Interface->Open(&h.fs,name.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_ALL_ACCESS,&context,&info)==STATUS_SUCCESS,
      "Grouped callback fixture reopen");
  name=L"\\f46";
  require(h.fs.Interface->Open(&h.fs,name.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_ALL_ACCESS,&other_context,&info)==STATUS_SUCCESS,
      "Unrelated callback fixture reopen");
  const auto read_unrelated=[&] {
    FSP_FSCTL_FILE_INFO current{};
    require(h.fs.Interface->GetFileInfo(&h.fs,other_context,&current)==STATUS_SUCCESS &&
        current.FileSize==unrelated.size(),"Unrelated metadata query changed the file");
    h.read(other_context,unrelated);
  };
  static constexpr auto width=2U;
  const bool warm=profile && profile->prepared_group_submit_lead_ms!=0;
  const auto read=[&](unsigned first,unsigned count=width) {
    const auto begin=file.begin()+first*extent;
    h.read(context,Bytes(begin,begin+count*extent),first*extent);
  };
  constexpr auto slice=4096U;
  const auto read_slice=[&](std::uint32_t offset) {
    h.read(context,Bytes(file.begin()+offset,file.begin()+offset+slice),offset);
  };
  const auto writes=h.image->write_bytes;
  const auto first_group=expanded?1U:0U;
  constexpr auto grouped_history_end=4U*extent;
  if(expanded && !profile && !variable_widths) {
    read(0);read(width);
    const auto sample=h.clock.next();
    const std::atomic<bool> running{false};
    const auto partly_grouped=h.adapter->capture_native_lz4_read_ahead(sample,h.clock.now,
        h.adapter->foreground_activity(),running,{},WinFspGroupReadWindow{2*width,0});
    require(partly_grouped && partly_grouped->groups && partly_grouped->bytes==3*extent &&
        partly_grouped->groups->members().size()==3,"Raw prefix inflated grouped demand coverage");
  }
  if(small_requests) {
    for(auto offset=extent;offset<grouped_history_end;offset+=slice) {
      if(offset!=extent)Sleep(20);
      read_slice(offset);
      if(offset<2*extent)
        require(h.adapter->native_lz4_read_ahead_pending()==(offset+slice==2*extent),
            "Small reads did not wait for and then forecast after one complete grouped extent");
    }
  }else if(variable_widths) {
    const auto pause=warm?750U:50U;
    // The first extent is the base and does not establish grouped demand.
    read(first_group,1);
    Sleep(pause);
    read(first_group+1,3);
    Sleep(pause);
    read(first_group+2*width);
  }else {
    read(first_group);
    if(warm)Sleep(750);
    read(first_group+width);
    if(warm) {
      Sleep(750);
      read(first_group+2*width);
    }
  }
  const auto predicted=small_requests?5U:first_group+(warm || variable_widths?3U:2U)*width;
  const auto members=expanded?2U*width:width;
  const auto after=predicted+members;
  require(h.adapter->native_lz4_read_ahead_pending(),"Grouped demand did not forecast its next extent");
  const std::atomic<bool> stop{false};
  auto admitted=h.clock.next();
  if(variable_widths || small_requests)admitted.captured_ms=h.clock.now=GetTickCount64();
  if(expanded && !profile && !variable_widths) {
    auto unfunded=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,h.adapter->foreground_activity(),
        stop,{},WinFspGroupReadWindow{members,(std::numeric_limits<std::uint64_t>::max)()});
    require(unfunded && unfunded->groups && unfunded->groups->members().size()==width &&
        !unfunded->expanded_group_window,"Unfunded group read captured an expanded window");
    read(first_group);
    read(first_group+width);
    admitted=h.clock.next();
  }
  const auto epoch=h.adapter->foreground_activity();
  std::unique_ptr<NativeLz4ReadWorker> worker;
  std::uint32_t timeout=0;
  const auto observe_memory=[&] {
    MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
    require(GlobalMemoryStatusEx(&memory)!=0,"Live group fixture memory telemetry missing");
    admitted.captured_ms=h.clock.now=GetTickCount64();
    admitted.physical_total=memory.ullTotalPhys;admitted.physical_available=memory.ullAvailPhys;
    admitted.commit_limit=memory.ullTotalPageFile;admitted.commit_available=memory.ullAvailPageFile;
  };
  if(profile) {
    observe_memory();
    timeout=profile->worker.timeout_ms;
    worker=std::make_unique<NativeLz4ReadWorker>(std::move(*profile));
  }
  auto allowance=WinFspGroupReadWindow{};
  if(small_requests)allowance=worker?worker->group_read_window():WinFspGroupReadWindow{members,100,2};
  else if(expanded)allowance=worker?worker->group_read_window():WinFspGroupReadWindow{members,variable_widths?1U:0U};
  bool forecast_seen=false;
  auto work=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,epoch,stop,
      [&](const auto& forecast) {
        if(variable_widths)std::cout<<"winfsp_group_widths forecast_frames="<<forecast.bytes/extent
            <<" observed_lead_ms="<<forecast.observed_lead_ms<<" expanded="<<forecast.expanded_group_window<<'\n';
        require(!forecast_seen && forecast.owner==h.adapter.get() && forecast.foreground_epoch==epoch &&
            forecast.offset==predicted*extent && forecast.bytes==members*extent &&
            forecast.expanded_group_window==(expanded || small_requests) && !forecast.groups && forecast.frames.empty(),
            "Grouped forecast has the wrong reservation or already owns a payload");
        forecast_seen=true;
        if(warm)require(worker->prepare_group(forecast,admitted,stop),"Live grouped AMD preparation refused");
      },allowance);
  require(forecast_seen && work && work->frames.empty() && work->groups && work->groups->bases().size()==1 &&
      work->groups->members().size()==members && work->groups->mixed_methods()==mixed,
      "Grouped WinFsp forecast did not capture its protected inputs");
  const auto member_offset=work->groups->members()[static_cast<std::size_t>(cpu_prefix)].record.primary;
  const auto publish_device=[&] {
    worker->observe(*h.adapter,false);
    if(const auto wake=static_cast<HANDLE>(worker->wake_handle()))
      require(WaitForSingleObject(wake,timeout+2500)==WAIT_OBJECT_0,"Packed-group AMD retirement wait");
    worker->observe(*h.adapter,false);
    observe_memory();
    if(!worker->ready_to_verify()) {
      const auto failure=worker->stats();
      std::cerr<<"Packed group worker exit="<<failure.last_worker_exit_code<<" failures="<<failure.failures<<'\n';
    }
    require(worker->ready_to_verify() && worker->verify_and_publish(*h.adapter,admitted,h.clock.now,stop),
        "Packed-group AMD preparation failed");
  };
  const auto read_skipped_extent=[&] {
    for(auto offset=grouped_history_end;offset<predicted*extent;offset+=slice)read_slice(offset);
    require(!h.adapter->native_lz4_read_ahead_hits(),"Intervening small reads consumed an unpublished result");
  };
  if(worker) {
    require(worker->start(*work,admitted,stop),"Live grouped AMD submission refused");
    if(small_requests)read_skipped_extent();
    if(cpu_prefix) {
      read(predicted,1);
      require(!h.adapter->native_lz4_read_ahead_hits(),"CPU prefix used an unpublished device result");
    }
    read_unrelated();
    publish_device();
    const auto stats=worker->stats();
    require(stats.submitted==1 && stats.verified_frames==members && stats.retirement_confirmed &&
        !stats.pending && stats.prepared_groups_started==static_cast<unsigned>(warm) &&
        stats.prepared_groups_submitted==static_cast<unsigned>(warm) && worker->stop(),
        "Grouped AMD session did not retire its selected mode");
    std::cout<<"winfsp_group_hip mixed="<<mixed<<" prepared="<<warm<<" verified="<<stats.verified_frames
        <<" worker_ms="<<stats.last_worker_elapsed_ms<<" prepared_submit_ms="<<stats.last_prepared_submit_ms
        <<" exact_child_retired=1\n";
  }else {
    auto ready=detail::NativeGroupReadyReads::decode_captured(*work->groups);
    if(small_requests)read_skipped_extent();
    if(cpu_prefix) {
      read(predicted,1);
      require(!h.adapter->native_lz4_read_ahead_hits(),"CPU prefix unexpectedly used the unpublished result");
    }
    read_unrelated();
    require(h.adapter->foreground_activity()!=epoch &&
        h.adapter->read_ahead_state(*work)==WinFspReadAheadState::current &&
        h.adapter->publish_read_ahead(*work,{},std::move(ready),stop),
        cpu_prefix?"Sequential CPU prefix superseded the remaining grouped read":"Unrelated callbacks superseded a verified read result");
  }
  read_unrelated();
  const auto first_ready=predicted+static_cast<unsigned>(cpu_prefix);
  auto ready_members=after-first_ready;
  if(segmented || small_requests) {
    // Observe reads of one protected member without injecting a failure.
    h.image->corrupt_read_offset=member_offset;
    h.image->corrupt_read_number=(std::numeric_limits<std::uint64_t>::max)();
    const auto read_slices=[&] {
      h.image->matching_reads=0;
      const auto before=h.image->read_bytes;
      for(auto offset=first_ready*extent;offset<after*extent;offset+=slice)
        read_slice(offset);
      return std::pair{h.image->read_bytes-before,h.image->matching_reads};
    };
    const auto prepared=read_slices();
    const auto cpu=read_slices();
    h.image->corrupt_read_offset=Ram::no_read_fault;
    std::cout<<"segmented_group_read callbacks="<<(after-first_ready)*extent/slice
        <<" delivered_frames="<<h.adapter->native_lz4_read_ahead_hits()
        <<" prepared_backing_bytes="<<prepared.first<<" cpu_backing_bytes="<<cpu.first
        <<" prepared_member_reads="<<prepared.second<<" cpu_member_reads="<<cpu.second
        <<" device="<<bool(worker)<<" small_request_training="<<small_requests
        <<" skipped_frames="<<(small_requests?predicted-grouped_history_end/extent:0)<<'\n';
    require(!prepared.second && cpu.second==1 && prepared.first<cpu.first &&
        h.adapter->native_lz4_read_ahead_hits()==ready_members && h.image->write_bytes==writes,
        "Segmented demand reread a prepared or retained CPU member");
    if(!worker) {
      const auto offset=first_ready*extent;
      read_slice(offset);
      file[offset]^=std::byte{1};
      h.write(context,Bytes{file[offset]},offset);
      read_slice(offset);
    }
    h.close(other_context);
    h.close(context);
    auto snapshot=h.adapter->snapshot();
    snapshot.verify_all();
    require(snapshot.read("f47",0,file.size())==file,"Independent CPU read lost segmented group bytes");
    return;
  }
  unsigned callbacks=0;
  for(auto first=first_ready;first<after;) {
    const auto count=(std::min)(after-first,variable_widths?(first==first_ready?3U:1U):width);
    read(first,count);
    first+=count;
    ++callbacks;
    require(h.adapter->native_lz4_read_ahead_hits()==first-first_ready && h.image->write_bytes==writes,
        "Grouped callback failed to hit or issued a write");
  }

  auto end=after;
  if(learned_lead) {
    auto window=worker?worker->group_read_window():WinFspGroupReadWindow{members,0,width};
    if(worker)require(window.required_lead_ms>=200 && window.required_lead_ms<=3500,
        "Measured worker startup outside the paired fixture interval");
    const auto pause=worker?static_cast<DWORD>(window.required_lead_ms*5/8):80U;
    std::array<std::uint64_t,3> begin{},complete{};
    for(unsigned index=0;index<3;++index) {
      if(index)Sleep(pause);
      begin[index]=GetTickCount64();
      read(end+index*width,width);
      complete[index]=GetTickCount64();
    }
    const auto immediate=end+3*width;
    const auto later=immediate+width;
    admitted=h.clock.next();
    if(worker)observe_memory();
    else {
      admitted.captured_ms=h.clock.now=GetTickCount64();
      // Bound the two internal callback gaps using the timestamps around each call.
      // The fixture requirement falls after the next read and before the later one.
      const auto upper=(std::min)(complete[1]-begin[0],complete[2]-begin[1]);
      const auto lower=(std::min)(begin[1]-complete[0],begin[2]-complete[1]);
      window.required_lead_ms=upper+1;
      require(window.required_lead_ms+h.clock.now-begin[2]<=2*lower,"Fixture gaps did not bound a later request");
    }
    auto later_work=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,h.adapter->foreground_activity(),
        stop,[&](const auto& forecast) {
          require(forecast.offset==later*extent && forecast.bytes==members*extent &&
              forecast.has_read_lead(window.required_lead_ms,h.clock.now),
              "Learned startup did not move the forecast by one grouped request");
          if(worker)require(worker->prepare_group(forecast,admitted,stop),"Learned grouped preparation refused");
        },window);
    require(later_work && later_work->groups && later_work->groups->members().size()==members,
        "Learned grouped request did not capture the later range");
    const auto new_lead=later_work->observed_lead_ms;
    if(worker)require(worker->start(*later_work,admitted,stop),"Learned grouped request did not submit");
    read(immediate,width);
    require(h.adapter->native_lz4_read_ahead_hits()==ready_members,"Intervening demand used the later window");
    if(worker)publish_device();
    else {
      auto ready=detail::NativeGroupReadyReads::decode_captured(*later_work->groups);
      require(ready && h.adapter->publish_read_ahead(*later_work,{},std::move(ready),stop),
          "Intervening demand invalidated the later sequential grouped window");
    }
    for(unsigned first=later;first<later+members;first+=width)read(first,width);
    ready_members+=members;
    callbacks+=members/width;
    end=later+members;
    require(h.adapter->native_lz4_read_ahead_hits()==ready_members && h.image->write_bytes==writes,
        "Learned grouped delivery differed or wrote to the backing");
    if(worker) {
      const auto stats=worker->stats();
      require(stats.submitted==2 && stats.published==2 && stats.verified_frames==2U*members &&
          stats.retirement_confirmed && worker->stop(),"Learned grouped delivery did not pass exact retirement");
      std::cout<<"winfsp_group_learned required_startup_ms="<<window.required_lead_ms
          <<" scheduled_lead_ms="<<new_lead<<" intervening_cpu_frames="<<width
          <<" later_device_frames="<<members<<" exact_child_retired=1\n";
    }else std::cout<<"winfsp_group_later_callback intervening_cpu_frames="<<width
        <<" later_ready_frames="<<members<<" backing_writes=0\n";
  }

  admitted=h.clock.next();
  const auto next=h.adapter->foreground_activity();
  auto superseded=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,next,stop);
  require(superseded && superseded->groups,"Missing second grouped forecast");
  auto late=detail::NativeGroupReadyReads::decode_captured(*superseded->groups);
  read(end,superseded->bytes/extent);
  require(late && h.adapter->read_ahead_state(*superseded)==WinFspReadAheadState::demand_served &&
      !h.adapter->publish_read_ahead(*superseded,{},std::move(late),stop) &&
      h.adapter->native_lz4_read_ahead_hits()==ready_members,
      "A fully served range was not identified or accepted late grouped output");
  if(!worker) {
    const auto foreground=h.adapter->foreground_activity();
    admitted=h.clock.next();
    const auto cancelled=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,foreground,stop,
        [&](const auto&) {read(end+width);});
    require(!cancelled && h.adapter->foreground_activity()!=foreground,
        "Grouped forecast did not yield before capture");
  }
  auto unrelated_current=unrelated;
  if(!worker) {
    if(cpu_prefix) {
      read(0);
      read(width);
      admitted=h.clock.next();
      auto skipped=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,h.adapter->foreground_activity(),
          stop,{},WinFspGroupReadWindow{4,0});
      require(skipped && skipped->groups,"Seek fixture did not capture a grouped window");
      auto skipped_ready=detail::NativeGroupReadyReads::decode_captured(*skipped->groups);
      read(2*width+1,1);
      require(skipped_ready && h.adapter->read_ahead_state(*skipped)==WinFspReadAheadState::superseded &&
          !h.adapter->publish_read_ahead(*skipped,{},std::move(skipped_ready),stop),
          "A nonsequential read kept an old grouped window");
    }
    read(0);
    if(variable_widths)require(!h.adapter->native_lz4_read_ahead_pending(),"Seek kept the previous read forecast");
    read(width);
    admitted=h.clock.next();
    auto old=h.adapter->capture_native_lz4_read_ahead(admitted,h.clock.now,h.adapter->foreground_activity(),stop);
    require(old && old->groups,"Checkpoint change fixture did not capture its input");
    auto old_ready=detail::NativeGroupReadyReads::decode_captured(*old->groups);
    unrelated_current.front()^=std::byte{0x4f};
    h.write(other_context,unrelated_current);
    require(old_ready && h.adapter->read_ahead_state(*old)==WinFspReadAheadState::superseded &&
        !h.adapter->publish_read_ahead(*old,{},std::move(old_ready),stop),"A changed checkpoint accepted old read work");
    read(2*width);
    require(h.adapter->native_lz4_read_ahead_hits()==ready_members,"Changed checkpoint delivered a speculative result");
  }
  h.close(other_context);
  h.close(context);
  auto snapshot=h.adapter->snapshot();
  require(snapshot.read("f47",0,file.size())==file &&
      snapshot.read("f46",0,unrelated_current.size())==unrelated_current,"Independent CPU read changed callback bytes");
  snapshot.verify_all();
  std::cout<<"winfsp_ready_groups mixed="<<mixed<<" ready_hits="<<ready_members<<" late_refused=1 cpu_readback=1 backing_writes=0"
      <<" unrelated_callbacks_retained=1 checkpoint_change_refused="<<!worker
      <<" expanded="<<expanded<<" callbacks="<<callbacks<<" variable_widths="<<variable_widths
      <<" cpu_prefix="<<cpu_prefix<<'\n';
}

NativeLz4ReadWorkerProfile group_gpu_profile(wchar_t** arguments) {
  const auto ascii=[](std::wstring_view value) {
    require(value.size()==64 && std::ranges::all_of(value,[](auto c){
      return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');
    }),"Invalid fixture SHA");
    return std::string(value.begin(),value.end());
  };
  const auto decimal=[](std::wstring_view value,std::uint32_t maximum) {
    require(!value.empty() && value.size()<=10 && std::ranges::all_of(value,[](auto c){return c>=L'0' && c<=L'9';}),
        "Invalid fixture adapter integer");
    const auto parsed=std::stoul(std::wstring(value));
    require(parsed<=maximum,"Fixture adapter integer out of range");
    return static_cast<std::uint32_t>(parsed);
  };
  const std::wstring_view luid=arguments[7];
  require(luid.size()==16,"Fixture adapter LUID length");
  const auto digit=[](wchar_t value) {
    if(value>=L'0' && value<=L'9')return static_cast<unsigned>(value-L'0');
    require(value>=L'a' && value<=L'f',"Invalid fixture LUID digit");
    return static_cast<unsigned>(value-L'a'+10);
  };
  NativeLz4ReadWorkerProfile profile;
  profile.helper=arguments[2];profile.runtime_name=arguments[3];profile.architecture=arguments[4];
  profile.runtime_sha256=ascii(arguments[5]);profile.helper_sha256=ascii(arguments[6]);
  profile.module_sha256=ascii(arguments[8]);
  for(std::size_t i=0;i<profile.luid.size();++i)
    profile.luid[i]=static_cast<std::byte>(16*digit(luid[2*i])+digit(luid[2*i+1]));
  profile.ordinal=decimal(arguments[9],63);
  profile.node_mask=decimal(arguments[10],(std::numeric_limits<std::uint32_t>::max)());
  profile.wave_mask=decimal(arguments[11],3);
  profile.worker={160,1200,14000,16};
  profile.minimum_frames=2;profile.maximum_frames=2;
  profile.maximum_slab_bytes=512*1024;
  profile.packed_groups=true;
  return profile;
}

void batched_read_ahead() {
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  constexpr std::uint32_t extent=kNativeExtentBytes,first=8,frames=4;
  Harness h(4096,nullptr,nullptr,nullptr,nullptr,true,fast,true);
  Bytes file;
  for(std::uint32_t i=0;i<first+frames;++i) {
    auto block=native_lz4_fixture::source();
    block.front()=static_cast<std::byte>(i);
    file.insert(file.end(),block.begin(),block.end());
  }
  const auto part=[&](std::uint32_t start,std::uint32_t count) {
    return Bytes(file.begin()+start*extent,file.begin()+(start+count)*extent);
  };
  const auto context=h.create(45);
  h.write(context,file);
  std::wstring name=L"\\f45";
  PVOID alternate=nullptr;
  FSP_FSCTL_FILE_INFO info{};
  require(h.fs.Interface->Open(&h.fs,name.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_ALL_ACCESS,
      &alternate,&info)==STATUS_SUCCESS && alternate,"second SDK handle could not reopen the batch file");
  const auto writes=h.image->write_bytes;
  const auto left=part(first,2),right=part(first+2,2);
  auto before=h.image->read_bytes;
  h.read(context,left,first*extent);
  h.read(context,right,(first+2)*extent);
  const auto ordinary=h.image->read_bytes-before;

  before=h.image->read_bytes;
  for(std::uint32_t i=0;i<frames;++i) {
    detail::NativeLz4CapturedFrame frame;
    require(h.adapter->capture_native_lz4_read(context,(first+i)*extent,extent,frame),
        "independent capture refused a selected batch frame");
  }
  const auto independent=h.image->read_bytes-before;

  const auto train=[&] {
    h.read(context,part(0,frames));
    h.read(context,part(frames,frames),frames*extent);
    require(h.adapter->native_lz4_read_ahead_pending(),"adjacent wide reads did not offer their next window");
  };
  train();
  before=h.image->read_bytes;
  const auto observation=h.clock.next();
  require(h.adapter->advance_native_lz4_read_ahead(observation,h.clock.now,h.adapter->foreground_activity()),
      "wide speculation did not prepare the captured batch");
  const auto shared=h.image->read_bytes-before;
  before=h.image->read_bytes;
  h.read(context,left,first*extent);
  require(h.adapter->native_lz4_read_ahead_hits()==2 && !h.adapter->native_lz4_read_ahead_pending(),
      "first demand lost remaining batch or offered overlapping work");
  h.read(context,right,(first+2)*extent);
  const auto prepared=h.image->read_bytes-before;
  require(h.adapter->native_lz4_read_ahead_hits()==frames && shared<independent && shared+prepared<ordinary,
      "the complete prepared path did not reuse the captured metadata and frames");

  train();
  auto limited=h.clock.next();
  const auto divisor=h.adapter->group_maintenance_policy().memory_share_divisor;
  limited.physical_available=limited.physical_total/16+
      divisor*(detail::kStoreDecoderMemoryCap+7ULL*kStoreCommitUnit);
  require(h.adapter->advance_native_lz4_read_ahead(limited,h.clock.now,h.adapter->foreground_activity()),
      "partial memory grant did not fund a prefix of the window");
  h.read(context,left,first*extent);
  h.read(context,right,(first+2)*extent);
  require(h.adapter->native_lz4_read_ahead_hits()==frames+1 && h.image->write_bytes==writes,
      "read-ahead exceeded its partial grant or wrote backing data");

  train();
  const auto degraded=h.clock.next();
  require(h.adapter->advance_native_lz4_read_ahead(degraded,h.clock.now,h.adapter->foreground_activity()),
      "batch could not prepare before the backing reported damage");
  before=h.image->read_bytes;
  h.image_io->reported_damage=true;
  h.read(context,left,first*extent);
  h.read(context,right,(first+2)*extent);
  h.image_io->reported_damage=false;
  const auto degraded_backing=h.image->read_bytes-before;
  require(degraded_backing==ordinary,"reported damage did not restore the ordinary backing verification");

  train();
  const auto hits_before_write=h.adapter->native_lz4_read_ahead_hits();
  const auto revised=h.clock.next();
  require(h.adapter->advance_native_lz4_read_ahead(revised,h.clock.now,h.adapter->foreground_activity()),
      "batch could not prepare the pre-overwrite range");
  for(std::uint32_t i=0;i<frames;++i)file[(first+i)*extent]^=std::byte{0x7f};
  h.write(alternate,file);
  before=h.image->read_bytes;
  h.read(context,part(first,2),first*extent);
  h.read(context,part(first+2,2),(first+2)*extent);
  const auto revised_backing=h.image->read_bytes-before;
  require(h.adapter->native_lz4_read_ahead_hits()==hits_before_write && revised_backing>prepared,
      "a second handle advanced the checkpoint but the old batch still supplied bytes");
  h.close(alternate);
  h.close(context);
  std::cout<<"winfsp_batched_lz4 frames="<<frames<<" partial_grant_frames=1 exact=1 second_handle_cpu=1"
      <<" revised_backing_bytes="<<revised_backing<<" degraded_backing_bytes="<<degraded_backing
      <<" capture_independent_bytes="<<independent<<" capture_shared_bytes="<<shared
      <<" ordinary_backing_bytes="<<ordinary<<" ready_backing_bytes="<<prepared
      <<" total_prepared_bytes="<<shared+prepared<<'\n';
}

void ready_lz4_reads() {
  if(!store_lz4_available()){
    std::cout<<"winfsp_ready_lz4 cpu_decoder=0\n";
    return;
  }
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  {
    const auto head=ram(4096),image=ram(4096);
    const auto unsupported=WinFspNativeWritable::create_managed_indexed(
        head,id(90),image,id(1),{kStoreMaxPhysicalBytes},fast);
    const auto reads=image->read_calls;
    require(!unsupported->enable_native_lz4_read_ahead() && image->read_calls==reads,
        "read-ahead admitted an unqualified backing or probed it");
  }
  Harness h(4096,nullptr,nullptr,nullptr,nullptr,true,fast);
  const auto context=h.create(42);
  const auto plain=native_lz4_fixture::source();
  detail::NativeLz4CapturedFrame captured;
  require(!h.adapter->capture_native_lz4_read(context,0,kNativeExtentBytes,captured),
      "capture adopted a provisional context");
  h.write(context,plain);
  require(h.adapter->capture_native_lz4_read(context,0,kNativeExtentBytes,captured),
      "controller could not capture its active SDK frame");

  constexpr detail::NativeLz4DecodeGrant grant{1,256U*1024U};
  const auto exchange=detail::NativeLz4ReadExchange::prepare_captured({&captured,1},91,grant);
  const auto request=detail::decode_native_lz4_request(exchange.request()->bytes(),grant,1);
  detail::StoreCodecDecoder cpu;
  const detail::NativeLz4DecodedFrame frame{captured.key(),
      cpu.decode(detail::kStoreLz4,captured.encoded(),captured.key().decoded_bytes)};
  const auto reply=detail::encode_native_lz4_reply(request,{&frame,1},WorkerExecution::cpu_semantics);
  auto ready=exchange.accept(reply,WorkerExecution::cpu_semantics);
  auto before=h.image->read_bytes;
  h.read(context,plain);
  const auto ordinary_bytes=h.image->read_bytes-before;
  require(h.adapter->publish_native_lz4_reads(ready) && !ready,"callback owner refused verified completion");
  before=h.image->read_bytes;
  h.read(context,plain);
  const auto ready_bytes=h.image->read_bytes-before;
  require(h.adapter->native_lz4_read_hits()==1 && ordinary_bytes>=ready_bytes+captured.encoded().size(),
      "SDK read skipped the verified ready result or fetched its payload");
  before=h.image->read_bytes;
  h.read(context,plain);
  require(h.adapter->native_lz4_read_hits()==1 && h.image->read_bytes-before==ordinary_bytes,
      "SDK read consumed one result twice or skipped its CPU fallback");

  ready=exchange.accept(reply,WorkerExecution::cpu_semantics);
  require(h.adapter->publish_native_lz4_reads(ready) && !ready,"callback owner rejected second verification");
  auto changed=plain;
  changed[1]^=std::byte{1};
  h.write(context,changed);
  h.read(context,changed);
  require(h.adapter->native_lz4_read_hits()==0,"SDK read accepted the prior file revision");
  h.close(context);
  require(!h.adapter->capture_native_lz4_read(context,0,kNativeExtentBytes,captured) && captured.encoded().empty(),
      "capture reused a closed context");
  h.adapter->snapshot().verify_all();
  std::cout<<"winfsp_ready_lz4 cpu_decoder=1 ready_hit=1 cpu_miss_exact=1 stale_after_write_rejected=1"
      <<" provisional_and_closed_refused=1 ordinary_backing_bytes="<<ordinary_bytes
      <<" ready_backing_bytes="<<ready_bytes<<'\n';
  sequential_read_ahead();
  batched_read_ahead();
}

void callbacks(unsigned unit) {
  Harness h(unit);h.populate();const auto workload=h.adapter->snapshot().workload_info();
  auto context=h.create(4);h.write(context,family(4));
  auto result=h.finish();
  require(result.progress.published_groups==1 && h.adapter->snapshot().shared_base_info().members==5,
      "SDK-created data not automatically grouped");h.read(context,family(4));h.close(context);
  // Closing a context can update namespace metadata; reuse content hashes.
  result=h.finish();const auto reads=h.image->read_calls,calls=h.image->calls;
  for(unsigned i=0;i<20;++i)require(h.step().state==WinFspMaintenanceState::unchanged,"unchanged callback volume not idle");
  require(h.image->read_calls==reads && h.image->calls==calls,"unchanged callback opportunity accessed backing");
  require(h.adapter->snapshot().workload_info()==NativeWorkloadInfo{true,workload.committed_input_bytes+kNativeExtentBytes},
      "callback groups minted accepted input");
  auto snapshot=h.adapter->snapshot();snapshot.verify_all();
  require(snapshot.image_storage().kind==NativeImageKind::paged && snapshot.shared_base_info().enabled,"wrong callback storage format");
  h.fs.Interface->DispatcherStopped(&h.fs,TRUE);const auto before=h.image->read_calls;
  require(h.step().state==WinFspMaintenanceState::unavailable && !h.adapter->background_maintenance_pending() &&
      h.image->read_calls==before,"stopped callback owner accessed storage");
  std::cout<<"paged_callbacks unit="<<unit<<" members=5 groups="<<result.progress.published_groups<<" input="
      <<snapshot.workload_info().committed_input_bytes<<"\n";
}
void directory_pages(bool indexed=false) {
  Harness h(4096,nullptr,nullptr,nullptr,nullptr,indexed);
  const unsigned children=indexed?384:38;
  const ULONG wide_buffer=indexed?64*1024:8192;
  const auto folder=h.create(L"\\Folder",true);
  const auto early=h.create(L"\\Folder\\(first)",false);
  const auto payload=data(23,719);
  h.write(early,payload);
  h.close(early);
  for(unsigned i=100;i<100+children;++i)
    h.close(h.create(L"\\Folder\\Item-"+std::to_wstring(i),false));
  if(indexed) {
    h.close(h.create(L"\\Neighbour",true));
    h.close(h.create(L"\\Neighbour\\Excluded",false));
  }
  const auto reference=h.adapter->snapshot().list("Folder");
  require(reference.size()==children+1,"owned directory test fixture");
  const auto writes=h.image->writes;

  struct Row {std::wstring name;std::uint64_t inode,size;};
  struct Page {std::vector<Row> rows;std::uint64_t backing_bytes=0;bool eof=false;};
  const auto page=[&](PWSTR marker,ULONG capacity) {
    alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,64*1024> output{};
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
  const auto all=page(nullptr,wide_buffer);
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
  const auto remainder=page(marker.data(),wide_buffer);
  require(remainder.eof && remainder.rows.size()==children && remainder.rows.front().name==L"Item-100",
      "short output continuation repeated or skipped a child");
  std::wstring folded=L"iTeM-115";
  const auto after=page(folded.data(),wide_buffer);
  require(after.eof && after.rows.size()==children-16 && after.rows.front().name==L"Item-116",
      "case-insensitive marker is not exclusive");
  require(one.backing_bytes<all.backing_bytes && h.image->writes==writes,
      "short enumeration scanned all entries or changed backing");
  if(indexed) {
    auto narrow_marker=dotdot;
    const auto following=static_cast<ULONG>(sizeof(FSP_FSCTL_DIR_INFO)+24);
    std::uint64_t backing_bytes=0;
    for(std::size_t i=0;i<reference.size();++i) {
      const auto batch=page(narrow_marker.data(),following);
      require(batch.rows.size()==1 &&
          batch.rows[0].name==(i?L"Item-"+std::to_wstring(99+i):L"(first)") &&
          batch.rows[0].inode==reference[i].inode &&
          batch.rows[0].size==reference[i].logical_bytes && (i+1==reference.size() || !batch.eof),
          "indexed narrow directory buffer repeated or skipped a checked child");
      narrow_marker=batch.rows[0].name;
      backing_bytes+=batch.backing_bytes;
    }
    const auto end=page(narrow_marker.data(),following);
    require(end.rows.empty() && end.eof && h.image->writes==writes,
        "indexed end marker returned a neighbour or changed storage");
    backing_bytes+=end.backing_bytes;
    std::cout<<"indexed_narrow_directory entries="<<reference.size()<<" complete_backing_bytes="<<backing_bytes
        <<" wide_backing_bytes="<<all.backing_bytes<<" writes=0\n";
  }
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
  h.adapter->cancel_background_maintenance();const auto before=h.image->calls;
  require(!h.adapter->background_maintenance_pending() && h.step().state==WinFspMaintenanceState::unavailable &&
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
  require(h.step().state==WinFspMaintenanceState::busy && h.image->read_calls==reads && h.image->calls==calls,
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
    require(!stats.native_pending && stats.worker_busy && h.step().state==WinFspMaintenanceState::busy &&
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
    require(adapter->background_maintenance_pending()==paged,"resume silently promoted legacy format or lost grouping");
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
void indexed_backfill_opportunities() {
  Harness h(4096);
  h.close(h.create(L"\\old",true));
  for(unsigned i=0;i<4;++i) {
    const auto handle=h.create(L"\\old\\member"+std::to_wstring(i),false);
    h.write(handle,family(i));
    h.close(handle);
  }
  auto head=ram(4096),backing=ram(4096);
  constexpr PagedStoreLimits capacity{kStoreMaxPhysicalBytes};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto migrated=h.adapter->snapshot().create_indexed_copy(backing,id(71),capacity,raw);
  {auto owner=NativeVolumeHeadWriter::create(head,id(70),migrated);}
  auto adapter=WinFspNativeWritable::resume_indexed_retired(head,id(70),1,
      [&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo> {
        require(key==id(71),"backfill selected the wrong volume image");
        return backing;
      },raw);
  const auto reads=backing->read_calls,writes=backing->calls;
  require(adapter->background_maintenance_pending() && backing->read_calls==reads,
      "legacy directory backfill was missing or read storage during discovery");
  Clock clock;
  auto observation=clock.next();
  observation.foreground_pending=true;
  const auto deferred=adapter->advance_background_maintenance(observation,clock.now);
  require(deferred.progress.deferral==NativeMaintenanceDeferral::foreground &&
      backing->read_calls==reads && backing->calls==writes,"backfill bypassed foreground admission");
  unsigned opportunities=0;
  while(adapter->background_maintenance_pending() && ++opportunities<300) {
    observation=clock.next();
    require(adapter->advance_background_maintenance(observation,clock.now).state!=WinFspMaintenanceState::unavailable,
        "indexed backfill fenced the callback adapter");
  }
  require(!adapter->background_maintenance_pending(),"indexed backfill did not quiesce");
  const auto objects=PagedObjectStoreSnapshot::open(backing,capacity);
  require((objects.read(1).at(93)&std::byte{2})!=std::byte{0},"idle opportunities did not complete directory backfill");
  const auto after=adapter->snapshot();
  after.verify_all();
  require(after.shared_base_info().members==4,"directory backfill starved normal collective compression");
  for(unsigned i=0;i<4;++i)require(after.read("old/member"+std::to_string(i),0,kNativeExtentBytes)==family(i),
      "idle directory backfill changed grouped content");
  const auto settled=backing->read_calls;
  observation=clock.next();
  require(adapter->advance_background_maintenance(observation,clock.now).state==WinFspMaintenanceState::unchanged &&
      backing->read_calls==settled,"completed backfill retried on an unchanged volume");
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
    require(!session.background_maintenance_pending(),"ready injected session admitted group work");
    // The path is inert data passed ONLY to the injected functions above.
    // No native preflight/create/dispatcher/mount function is in this provider.
    const auto started=session.start({L"R:\\FlitzFS-model\\unused",true,true});
    if(!fail){require(started==STATUS_SUCCESS && session.background_maintenance_pending(),"paged injected lifecycle start");
      Clock clock;const auto o=clock.next();require(session.advance_background_maintenance(o,clock.now).state==WinFspMaintenanceState::advanced,
          "session did not forward group opportunity");session.stop();}
    else require(started==STATUS_OBJECT_NAME_COLLISION && session.state()==SessionState::failed,"injected mount refusal lost ownership");
    require(!session.background_maintenance_pending() && provider.callback_alive && !provider.fs.UserContext &&
        std::string(provider.calls.data(),provider.count)==(fail?"pcsmtd":"pcsmtrd"),"paged lifecycle did not retire in order");
    session.cancel_background_maintenance();
  }
  Lifecycle::active=nullptr;
}
void indexed_callbacks(unsigned unit,NativeIndexedDataLayout layout=NativeIndexedDataLayout::extents) {
  auto head=ram(unit),backing=ram(unit);
  constexpr PagedStoreLimits capacity{kStoreMaxPhysicalBytes};
  const auto volume=id(70),image_id=id(71);
  const auto resolver=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo> {
    require(key==image_id,"indexed SDK restart resolved another image");
    return backing;
  };
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,volume,backing,image_id,capacity,raw,{}, {},layout);
  FSP_FILE_SYSTEM fs{};
  const auto bind=[&] {
    fs={};
    fs.Version=static_cast<UINT16>(sizeof(fs));
    fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();
    adapter->bind_callbacks(fs,[]()->UINT32{return 0;});
  };
  bind();
  require(adapter->mount_names_supported() && adapter->parameters().MaxComponentLength==255,
      "indexed SDK name profile differs from the Windows ordinal table or component bound");
  SIZE_T security_bytes=0;
  UINT32 attributes=0;
  wchar_t root[]=L"\\";
  require(fs.Interface->GetSecurityByName(&fs,root,&attributes,nullptr,&security_bytes)==STATUS_BUFFER_OVERFLOW,
      "indexed SDK root descriptor length");
  Bytes security(security_bytes);
  require(fs.Interface->GetSecurityByName(&fs,root,&attributes,security.data(),&security_bytes)==STATUS_SUCCESS,
      "indexed SDK root descriptor");
  const auto create=[&](PWSTR path,bool directory=false) {
    void* context=nullptr;
    FSP_FSCTL_FILE_INFO output{};
    require(fs.Interface->Create(&fs,path,directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE,
        FILE_ALL_ACCESS,directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL,
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
  constexpr UINT32 open_file=(1U<<24)|FILE_NON_DIRECTORY_FILE;
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

  wchar_t unicode_name[]=L"\\\u00e9chantillon\U0001f600";
  wchar_t unicode_alias[]=L"\\\u00c9CHANTILLON\U0001f600";
  const auto unicode=create(unicode_name);
  auto unicode_bytes=data(19,937);
  const auto prior_extents=adapter->snapshot().allocation_usage().live_extents;
  require(fs.Interface->Write(&fs,unicode,unicode_bytes.data(),0,static_cast<ULONG>(unicode_bytes.size()),
      FALSE,FALSE,&bytes,&info)==STATUS_SUCCESS && bytes==unicode_bytes.size(),"indexed SDK Unicode write");
  const auto unicode_id=info.IndexNumber;
  if(layout==NativeIndexedDataLayout::inline_small_files) {
    constexpr ULONG sparse_size=static_cast<ULONG>(detail::kIndexedInlineFileBytes+1);
    require(adapter->snapshot().allocation_usage().live_extents==prior_extents &&
        fs.Interface->SetFileSize(&fs,unicode,sparse_size,FALSE,&info)==STATUS_SUCCESS &&
        info.FileSize==sparse_size && adapter->snapshot().allocation_usage().live_extents==prior_extents+1,
        "indexed SDK inline file did not move its data on growth");
    auto expected=unicode_bytes;
    expected.resize(sparse_size);
    Bytes actual(sparse_size);
    require(fs.Interface->Read(&fs,unicode,actual.data(),0,sparse_size,&bytes)==STATUS_SUCCESS &&
        bytes==sparse_size && actual==expected,"indexed SDK sparse growth changed the inline prefix");
    require(fs.Interface->SetFileSize(&fs,unicode,unicode_bytes.size(),FALSE,&info)==STATUS_SUCCESS &&
        adapter->snapshot().allocation_usage().live_extents==prior_extents,
        "indexed SDK shrink did not restore the inline layout");
  }
  require(fs.Interface->Rename(&fs,unicode,unicode_name,unicode_alias,FALSE)==STATUS_SUCCESS,
      "indexed SDK Unicode case-only rename");
  cleanup(unicode,0);close(unicode);
  std::wstring long_name=L"\\"+std::wstring(255,L'\u6f22');
  const auto long_file=create(long_name.data());
  cleanup(long_file,0);close(long_file);
  void* root_context=nullptr;
  require(fs.Interface->Open(&fs,root,(1U<<24)|FILE_DIRECTORY_FILE,FILE_ALL_ACCESS,&root_context,&info)==STATUS_SUCCESS &&
      root_context,"indexed SDK Unicode enumeration root");
  alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,1024> directory{};
  require(fs.Interface->ReadDirectory(&fs,root_context,nullptr,keep+1,directory.data(),
      static_cast<ULONG>(directory.size()),&bytes)==STATUS_SUCCESS,"indexed SDK Unicode spelling enumeration");
  const auto* row=reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(directory.data());
  require(row->Size>=sizeof(FSP_FSCTL_DIR_INFO) && bytes>=row->Size &&
      std::wstring_view(row->FileNameBuf,(row->Size-sizeof(FSP_FSCTL_DIR_INFO))/sizeof(wchar_t))==unicode_alias+1,
      "indexed SDK Unicode enumeration lost the renamed spelling");
  require(fs.Interface->ReadDirectory(&fs,root_context,nullptr,unicode_name+1,directory.data(),
      static_cast<ULONG>(directory.size()),&bytes)==STATUS_SUCCESS,"indexed SDK Unicode directory marker");
  require(bytes>=row->Size && row->Size==sizeof(FSP_FSCTL_DIR_INFO)+255*sizeof(wchar_t) &&
      std::wstring_view(row->FileNameBuf,255)==std::wstring_view(long_name).substr(1),
      "indexed SDK directory enumeration did not preserve the 255-unit name");
  cleanup(root_context,0);close(root_context);
  wchar_t broken[]={L'\\',static_cast<wchar_t>(0xd800),0};
  void* malformed=nullptr;
  const auto prior_writes=backing->calls;
  require(fs.Interface->Create(&fs,broken,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,security.data(),
      0,&malformed,&info)==STATUS_OBJECT_NAME_INVALID && !malformed && backing->calls==prior_writes,
      "indexed SDK accepted an unpaired surrogate");

  wchar_t tree_name[]=L"\\tree",tree_destination[]=L"\\directory-with-a-longer-name";
  wchar_t child_name[]=L"\\tree\\member",child_destination[]=L"\\directory-with-a-longer-name\\member";
  const auto tree=create(tree_name,true);
  const auto child=create(child_name);
  require(fs.Interface->Write(&fs,child,unicode_bytes.data(),0,static_cast<ULONG>(unicode_bytes.size()),
      FALSE,FALSE,&bytes,&info)==STATUS_SUCCESS && bytes==unicode_bytes.size(),"indexed SDK tree fixture write");
  const auto member=info.IndexNumber;
  require(fs.Interface->Rename(&fs,tree,tree_name,tree_destination,FALSE)==STATUS_SUCCESS,"indexed SDK tree move");
  wchar_t missing_name[]=L"\\directory-with-a-longer-name\\missing";
  const auto rename_checks_writes=backing->calls;
  require(fs.Interface->GetSecurityByName(&fs,child_name,nullptr,nullptr,nullptr)==STATUS_OBJECT_PATH_NOT_FOUND &&
      fs.Interface->GetSecurityByName(&fs,missing_name,nullptr,nullptr,nullptr)==STATUS_OBJECT_NAME_NOT_FOUND &&
      fs.Interface->Rename(&fs,child,child_destination,child_name,FALSE)==STATUS_OBJECT_PATH_NOT_FOUND &&
      backing->calls==rename_checks_writes,"indexed SDK missing ancestor or file returned the wrong status");
  void* moved=nullptr;
  require(fs.Interface->Open(&fs,child_destination,open_file,FILE_ALL_ACCESS,&moved,&info)==STATUS_SUCCESS &&
      moved && info.IndexNumber==member,"indexed SDK tree move changed the member identity");
  Bytes same(unicode_bytes.size());
  require(fs.Interface->Read(&fs,child,same.data(),0,static_cast<ULONG>(same.size()),&bytes)==STATUS_SUCCESS &&
      bytes==same.size() && same==unicode_bytes,"indexed SDK tree move changed data behind an existing handle");
  cleanup(moved,0);close(moved);
  cleanup(child,0);close(child);
  cleanup(tree,0);close(tree);

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
  require(fs.Interface->Open(&fs,gone,open_file,FILE_ALL_ACCESS,&removed,&info)==STATUS_OBJECT_NAME_NOT_FOUND && !removed,
      "indexed SDK recovery exposed the detached name");
  void* unicode_reopened=nullptr;
  require(fs.Interface->Open(&fs,unicode_name,open_file,FILE_ALL_ACCESS,&unicode_reopened,&info)==STATUS_SUCCESS &&
      unicode_reopened && info.IndexNumber==unicode_id,"indexed SDK reopen lost the Unicode alias or inode");
  Bytes unicode_actual(unicode_bytes.size());
  require(fs.Interface->Read(&fs,unicode_reopened,unicode_actual.data(),0,static_cast<ULONG>(unicode_actual.size()),
      &bytes)==STATUS_SUCCESS && bytes==unicode_actual.size() && unicode_actual==unicode_bytes,
      "indexed SDK Unicode restart changed file contents");
  cleanup(unicode_reopened,0);close(unicode_reopened);
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
  if(unit==4096) {
    constexpr unsigned blocks=129,per_write=32;
    wchar_t retired_name[]=L"\\retire";
    auto* retired=create(retired_name);
    for(unsigned first=0;first<blocks;first+=per_write) {
      const auto count=(std::min)(per_write,blocks-first);
      Bytes input(count*kNativeExtentBytes);
      for(unsigned i=0;i<count;++i)input[i*kNativeExtentBytes]=std::byte{0x29};
      require(fs.Interface->Write(&fs,retired,input.data(),std::uint64_t(first)*kNativeExtentBytes,
          static_cast<ULONG>(input.size()),FALSE,FALSE,&bytes,&info)==STATUS_SUCCESS && bytes==input.size(),
          "indexed retirement extent write");
    }
    const auto before_retirement=adapter->snapshot();
    const auto keep_extents=before_retirement.allocation_usage().live_extents-blocks;
    require(fs.Interface->SetDelete(&fs,retired,retired_name,TRUE)==STATUS_SUCCESS,"indexed retirement delete intent");
    cleanup(retired,FspCleanupDelete);close(retired);
    const auto reads=backing->read_calls,writes=backing->calls;
    require(adapter->background_maintenance_pending() && backing->read_calls==reads && backing->calls==writes,
        "indexed reaper pending discovery did backing I/O");
    Clock clock;
    const auto expect_deferral=[&](const NativeMaintenanceObservation& observed,std::uint64_t now,
        NativeMaintenanceDeferral reason) {
      const auto status=adapter->advance_background_maintenance(observed,now);
      require(!status.reclaimed && status.progress.deferral==reason &&
          backing->read_calls==reads && backing->calls==writes,"refused indexed reclamation touched backing");
    };
    auto observation=clock.next();
    expect_deferral(observation,clock.now+1000,NativeMaintenanceDeferral::observation);
    expect_deferral(observation,clock.now,NativeMaintenanceDeferral::observation);
    observation=clock.next();observation.foreground_pending=true;
    expect_deferral(observation,clock.now,NativeMaintenanceDeferral::foreground);
    observation=clock.next();observation.cpu_busy_ticks=900;
    expect_deferral(observation,clock.now,NativeMaintenanceDeferral::cpu);
    observation=clock.next();observation.physical_available=1;
    expect_deferral(observation,clock.now,NativeMaintenanceDeferral::memory);
    unsigned transactions=0;
    auto previous=keep_extents+blocks;
    while(previous>keep_extents) {
      require(transactions<4 && adapter->background_maintenance_pending(),"indexed reaper did not drain");
      observation=clock.next();
      const auto status=adapter->advance_background_maintenance(observation,clock.now);
      require(status.reclaimed,"admitted indexed reclamation did not publish");
      const auto left=adapter->snapshot().allocation_usage().live_extents;
      require(left>=keep_extents && left<previous && previous-left<=64,"indexed reaper exceeded its transaction");
      previous=left;++transactions;
    }
    const auto finished=adapter->snapshot();finished.verify_all();
    require(transactions==3 && previous==keep_extents &&
        before_retirement.read("retire",(blocks-1)*kNativeExtentBytes,1)==Bytes{std::byte{0x29}} &&
        finished.read("keep",eof,static_cast<std::uint32_t>(tail.size()))==tail,
        "indexed reaper changed a pinned snapshot or unrelated live data");
    const auto settled_reads=backing->read_calls,settled_writes=backing->calls;
    observation=clock.next();
    // A completed reaper can hand the next opportunity to pending group work.
    // Refuse that work independently so the retired queue's I/O can be checked.
    observation.cpu_busy_ticks=observation.cpu_total_ticks;
    const auto settled=adapter->advance_background_maintenance(observation,clock.now);
    require(!settled.reclaimed && (settled.state==WinFspMaintenanceState::unchanged ||
        settled.progress.deferral==NativeMaintenanceDeferral::cpu) &&
        backing->read_calls==settled_reads && backing->calls==settled_writes,"settled indexed reaper polled backing");
    std::cout<<"indexed_sdk_reclamation extents="<<blocks<<" transactions="<<transactions<<" refused_observations=5\n";
  }
  fs.UserContext=nullptr;
  std::cout<<"indexed_winfsp_callbacks unit="<<unit<<" large_write_bytes="<<initial.size()
      <<" reopened_eof="<<eof+tail.size()<<" occupied_bytes="<<occupied
      <<" inline_small_files="<<(layout==NativeIndexedDataLayout::inline_small_files)<<'\n';
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
    for(unsigned i=0;i<1000 && probe.adapter->background_maintenance_pending();++i)probe.step();cuts=probe.image->calls-start;}
  require(cuts>3 && cuts<256,"callback publication fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    Harness h(unit,&bytes,&head,&advice);ready(h);h.image->failure=mode;h.image->fail_at=h.image->calls+cut;
    WinFspMaintenanceResult result;
    for(unsigned i=0;i<1000;++i){result=h.step();if(h.adapter->requires_offline())break;}
    require(h.adapter->requires_offline() && !h.adapter->background_maintenance_pending(),"failed callback group did not fence owner");
    const auto reads=h.image->read_calls,calls=h.image->calls;
    require(h.step().state==WinFspMaintenanceState::unavailable && h.image->read_calls==reads && h.image->calls==calls,
        "offline group retried backing");
    auto stable=image(h.image->stable,unit);auto snapshot=NativeFilesystemSnapshot::open_image(stable,{NativeImageKind::paged,kStoreMaxPhysicalBytes});
    for(unsigned i=0;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"callback interruption lost acknowledged bytes");
    snapshot.verify_all();++faults;
  }
  std::cout<<"paged_callback_faults unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
}
int wmain(int argc,wchar_t** argv){try {
  const auto mode=argc>1?std::wstring_view(argv[1]):std::wstring_view{};
  const bool segmented=mode==L"--hip-segmented-group-reads";
  const bool small_requests=mode==L"--hip-small-stream-group-reads";
  const bool learned_lead=mode==L"--hip-prepared-learned-group-reads";
  const bool cpu_prefix=learned_lead || mode==L"--hip-prepared-partial-mixed-group-reads";
  const bool variable_widths=cpu_prefix || mode==L"--hip-prepared-variable-mixed-group-reads";
  const bool expanded=variable_widths || mode==L"--hip-prepared-expanded-mixed-group-reads";
  const bool warm=expanded || mode==L"--hip-prepared-group-reads" || mode==L"--hip-prepared-mixed-group-reads";
  const bool device=warm || segmented || small_requests || mode==L"--hip-group-reads" || mode==L"--hip-mixed-group-reads";
  const bool timing=device && argc==13 && std::wstring_view(argv[12])==L"--timed-exchange";
  app_fixture::Budget budget(true,timing);
  if(device && (argc==12 || timing)) {
    auto profile=group_gpu_profile(argv);
    const bool mixed=expanded || mode==L"--hip-mixed-group-reads" || mode==L"--hip-prepared-mixed-group-reads";
    profile.mixed_groups=mixed;
    if(expanded) {
      profile.maximum_frames=4;
      profile.maximum_slab_bytes=1024*1024;
    }
    if(warm || small_requests)profile.minimum_lead_ms=100;
    if(warm) {
      profile.prepared_group_submit_lead_ms=20;
    }
    group_read_ahead(std::move(profile),{mixed,expanded,variable_widths,cpu_prefix,learned_lead,segmented,small_requests});
    return 0;
  }
  if(argc!=2)throw std::runtime_error("Explicit callback/fault mode required");
  if(mode==L"--callbacks"){
    callbacks(4096);callbacks(65536);directory_pages();directory_pages(true);selected_change();foreground_busy();
    deferred_response_busy();resume_formats();indexed_backfill_opportunities();lifecycle();indexed_callbacks(4096);indexed_callbacks(65536);
  }
  else if(mode==L"--inline-files"){
    indexed_callbacks(4096,NativeIndexedDataLayout::inline_small_files);
    indexed_callbacks(65536,NativeIndexedDataLayout::inline_small_files);
  }
  else if(mode==L"--ready-lz4-reads")ready_lz4_reads();
  else if(mode==L"--ready-group-reads")group_read_ahead();
  else if(mode==L"--ready-segmented-group-reads")group_read_ahead({},{.segmented=true});
  else if(mode==L"--ready-small-stream-group-reads")group_read_ahead({},{.small_requests=true});
  else if(mode==L"--ready-mixed-group-reads"){
    group_read_timing();group_read_window();
    GroupReadFixtureOptions options{.mixed=true};
    group_read_ahead({},options);
    options.expanded=true;group_read_ahead({},options);
    options.variable_widths=true;group_read_ahead({},options);
    options.cpu_prefix=true;group_read_ahead({},options);
    options.learned_lead=true;group_read_ahead({},options);
  }
  else if(mode==L"--faults-4096")publication_faults(4096);
  else if(mode==L"--faults-65536")publication_faults(65536);
  else if(mode==L"--profile-indexed-directory")directory_pages(true);
  else throw std::runtime_error("Unknown callback qualification mode");
  std::cout<<"PASS: "<<checks<<" paged WinFsp SDK checks; "<<faults<<" injected group failures; modeled CPU observations; memory only, no mount/driver/GPU\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
