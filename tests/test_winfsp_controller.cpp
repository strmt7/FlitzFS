#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "winfsp_memory_volume.hpp"
#include "anchor_batch_profile.hpp"
#include "anchor_search_fixture.hpp"
#include "flitzfs/paged_store.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/native_hybrid_backing.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/windows/hip_worker_exit.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "paused_capture_backing.hpp"
#include "dense_file_fixture.hpp"
#include "native_corpus_fixture.hpp"
#include "native_security_fixture.hpp"
#include "../src/native_group_codec.hpp"
#include "../src/native_indexed_volume.hpp"
#include "../src/native_capture.hpp"
#include "../src/native_maintenance_source.hpp"
#include "../src/store_codec.hpp"
#include "../src/record_digest.hpp"
#include "volume_range_fixture.hpp"
#include "windows_app_budget.hpp"
#include <bcrypt.h>
#include <pdh.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstring>
#include <fstream>
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
#include "../src/windows/native_anchor_launch.hpp"
#include "../src/windows/gpu_budget.hpp"
#endif
#include <iostream>
#include <limits>
#include <optional>
#include <latch>
#include <thread>
#include <utility>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
inline constexpr auto queued_anchor_members=1+(kAnchorProfileMaxQueuedDispatches-1)*
    anchor_match::dispatch_member_limit(anchor_match::BatchOutput::anchors);
#endif
DWORD handle_count() {DWORD count=0;if(!GetProcessHandleCount(GetCurrentProcess(),&count))throw std::runtime_error("handle telemetry");return count;}
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
void anchor_launch_budget() {
  constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
  GpuMemorySnapshot observed;
  observed.valid = observed.amd_hardware = true;
  observed.luid[0] = std::byte{1};
  observed.node_mask = 1;
  observed.captured_ms = 100;
  observed.dedicated_bytes = 8 * gib;
  observed.local_budget = 6 * gib;
  observed.nonlocal_budget = 8 * gib;
  observed.physical_total = observed.commit_limit = 8 * gib;
  observed.physical_available = observed.commit_available = 7 * gib;

  NativeAnchorSearchOptions options;
  options.route.memory = observed;
  options.worker = {128, 2'000, 5'000, 16};
  options.minimum_members = 20;
  options.maximum_members = 128;
  options.adaptive_batch_sizing = false;
  constexpr std::array<std::uint32_t,4> members{12, 24, 30, 44};
  const auto full = plan_native_anchor_launch(members, options, observed, observed.captured_ms, false);
  const auto three = plan_native_anchor_launch(std::span(members).first(3),
      options, observed, observed.captured_ms, false);
  const auto two = plan_native_anchor_launch(std::span(members).first(2),
      options, observed, observed.captured_ms, false);
  require(full && full->batch.groups.size() == 4 && three && three->batch.groups.size() == 3 &&
      two && two->batch.groups.size() == 2, "Wide memory grant did not admit complete groups");

  const auto three_bytes = required_sized_worker_headroom(three->capacity, three->worker);
  const auto two_bytes = required_sized_worker_headroom(two->capacity, two->worker);
  require(three_bytes > two_bytes && two_bytes > 0, "Group staging did not charge the complete transport");
  const auto limited = [&](std::uint64_t available) {
    auto snapshot = observed;
    snapshot.physical_available = snapshot.commit_available = available;
    return plan_native_anchor_launch(members, options, snapshot, snapshot.captured_ms, false);
  };
  const auto at_limit = limited(three_bytes);
  require(at_limit && at_limit->batch == three->batch && at_limit->transfer == three->transfer,
      "A funded prefix changed its group identities or geometry");
  const auto below_limit = limited(three_bytes - 1);
  require(below_limit && below_limit->batch == two->batch, "Prefix planning did not choose the largest funded batch");
  require(!limited(two_bytes - 1), "Prefix planning accepted fewer than the approved minimum members");
  require(!plan_native_anchor_launch(members, options, observed,
      observed.captured_ms + kGpuMaxBudgetAgeMs + 1, false), "Stale budget admitted a GPU batch");

  auto queued_options=options;
  queued_options.minimum_members=1;
  constexpr std::array one_dispatch{anchor_match::max_members};
  constexpr std::array two_dispatches{anchor_match::max_members+1};
  const auto unprofiled=plan_native_anchor_launch(two_dispatches,queued_options,observed,observed.captured_ms,false);
  queued_options.dispatches_per_wait=kAnchorProfileMaxQueuedDispatches;
  const auto one_wait=plan_native_anchor_launch(one_dispatch,queued_options,observed,observed.captured_ms,false);
  const auto two_wait=plan_native_anchor_launch(two_dispatches,queued_options,observed,observed.captured_ms,false);
  const auto full_wait=plan_native_anchor_launch(members,queued_options,observed,observed.captured_ms,false);
  require(unprofiled && unprofiled->dispatches_per_wait==1 && one_wait && one_wait->dispatches_per_wait==1 &&
      one_wait->capacity.reply_bytes==one_wait->batch.reply_bytes, "Single dispatch required an optional profile");
  require(two_wait && two_wait->dispatches_per_wait==2 && two_wait->batch==unprofiled->batch &&
      two_wait->transfer==unprofiled->transfer &&
      two_wait->capacity.reply_bytes==unprofiled->capacity.reply_bytes+kAnchorBatchProfileBytes,
      "A smaller funded wait group wasn't profiled");
  require(full_wait && full_wait->dispatches_per_wait==kAnchorProfileMaxQueuedDispatches,
      "Wait group exceeded the requested dispatch limit");
  auto no_trailer=observed;
  no_trailer.physical_available=no_trailer.commit_available=
      required_sized_worker_headroom(unprofiled->capacity,unprofiled->worker);
  const auto funded=plan_native_anchor_launch(two_dispatches,queued_options,no_trailer,no_trailer.captured_ms,false);
  require(funded && funded->dispatches_per_wait==1 && funded->batch==unprofiled->batch &&
      funded->capacity.reply_bytes==unprofiled->capacity.reply_bytes,
      "Optional wait profile displaced a funded unprofiled batch");

  auto measured_options=options;
  measured_options.minimum_members=1;
  measured_options.adaptive_batch_sizing=true;
  constexpr std::array<std::uint32_t,5> uniform_groups{4,4,4,4,4};
  const auto bootstrap=plan_native_anchor_launch(std::span(uniform_groups).first(1),measured_options,
      observed,observed.captured_ms,false);
  require(bootstrap && bootstrap->profiled,"Initial copy batch did not fund its setup trace");
  auto fixed_options=measured_options;
  fixed_options.adaptive_batch_sizing=false;
  const auto fixed_batch=plan_native_anchor_launch(uniform_groups,fixed_options,observed,observed.captured_ms,false);
  fixed_options.profile_copy_batches=true;
  const auto measured_fixed=plan_native_anchor_launch(uniform_groups,fixed_options,observed,observed.captured_ms,false);
  require(fixed_batch && !fixed_batch->profiled && measured_fixed && measured_fixed->profiled &&
      measured_fixed->batch==fixed_batch->batch && measured_fixed->transfer==fixed_batch->transfer &&
      measured_fixed->dispatches_per_wait==1 &&
      measured_fixed->capacity.reply_bytes==fixed_batch->capacity.reply_bytes+kAnchorBatchProfileBytes,
      "Fixed-size phase observation changed the planned work");
  const auto measured_launches=bootstrap->batch.launches;
  NativeAnchorBatchTiming setup_timing;
  setup_timing.record_verified(2100,measured_launches,1'850'000'000,uniform_groups.front());
  const auto stop_ms=measured_options.worker.timeout_ms;
  const auto split_limit=setup_timing.launch_limit(stop_ms,uniform_groups.front());
  const auto mixed_limit=setup_timing.launch_limit(stop_ms,0);
  const auto amortized=plan_native_anchor_launch(uniform_groups,measured_options,observed,
      observed.captured_ms,false,split_limit);
  const auto charged_per_launch=plan_native_anchor_launch(uniform_groups,measured_options,observed,
      observed.captured_ms,false,mixed_limit);
  require(split_limit && mixed_limit && *split_limit>*mixed_limit &&
      setup_timing.launch_limit(stop_ms,uniform_groups.front()+1)==mixed_limit &&
      amortized && amortized->profiled && amortized->dispatches_per_wait==1 &&
      amortized->capacity.reply_bytes==amortized->batch.reply_bytes+kAnchorBatchProfileBytes &&
      amortized->batch.groups.size()==uniform_groups.size() &&
      charged_per_launch && charged_per_launch->batch.groups.size()==1,
      "Startup evidence failed to admit a bounded identical-shape burst");
  constexpr std::array<std::uint32_t,6> mixed_groups{4,4,4,4,4,6};
  const auto matching=plan_timed_native_anchor_launch(mixed_groups,measured_options,observed,
      observed.captured_ms,false,setup_timing);
  require(matching && matching->batch==amortized->batch,
      "A larger trailing group suppressed the affordable calibrated prefix");
  constexpr std::array<std::uint32_t,6> smaller_groups{4,2,4,3,2,6};
  const auto staged_mix=plan_timed_native_anchor_launch(smaller_groups,measured_options,observed,
      observed.captured_ms,false,setup_timing);
  require(staged_mix && staged_mix->batch.groups.size()==5 &&
      staged_mix->batch.groups[1].whole.members==2 && staged_mix->batch.groups[3].whole.members==3,
      "Smaller groups did not share the verified setup");
  auto mixed_timing=setup_timing;
  mixed_timing.record_verified(1950,4*measured_launches,1'500'000'000,uniform_groups.front(),false);
  require(mixed_timing.calibrated_members()==uniform_groups.front() &&
      mixed_timing.launch_limit(stop_ms,uniform_groups.front())==split_limit,
      "A cheaper mixed job weakened the measured maximum group price");
  mixed_timing.record_verified(3300,2*measured_launches,1'850'000'000,uniform_groups.front(),false);
  require(mixed_timing.calibrated_members()==uniform_groups.front() &&
      mixed_timing.launch_limit(stop_ms,uniform_groups.front())<split_limit,
      "Slower mixed work failed to tighten the shared budget");
  mixed_timing.record_verified(3300,2*measured_launches,1'850'000'000,6,false);
  require(!mixed_timing.calibrated_members(),"A larger mixed job extended the verified envelope");
  auto narrow_slab=observed;
  const auto local_reserve=gpu_memory_envelope(observed).local_reserve;
  narrow_slab.local_usage=observed.local_budget-local_reserve-
      anchor_match::device_bytes(uniform_groups.front()-1,measured_options.output);
  const auto narrow_limit=plan_native_anchor_launch(uniform_groups,measured_options,narrow_slab,
      narrow_slab.captured_ms,false,mixed_limit);
  const auto narrowed=plan_timed_native_anchor_launch(uniform_groups,measured_options,narrow_slab,
      narrow_slab.captured_ms,false,setup_timing);
  require(narrow_limit && narrow_limit->transfer.members_per_transfer<uniform_groups.front() &&
      narrowed && narrowed->batch==narrow_limit->batch && narrowed->transfer==narrow_limit->transfer,
      "A narrowed staging slab retained the calibrated complete-group setup estimate");
  NativeAnchorBatchTiming larger_mixed_grant;
  larger_mixed_grant.record_verified(2500,6*measured_launches,1'500'000'000,uniform_groups.front());
  const auto mixed=plan_timed_native_anchor_launch(mixed_groups,measured_options,observed,
      observed.captured_ms,false,larger_mixed_grant);
  require(mixed && mixed->batch.groups.size()==mixed_groups.size() &&
      mixed->batch.groups.back().whole.members==mixed_groups.back(),
      "A calibrated prefix displaced a larger funded mixed batch");
  NativeAnchorBatchTiming expensive_setup;
  expensive_setup.record_verified(4000,4*measured_launches,3'200'000'000,uniform_groups.front());
  const auto whole_limit=expensive_setup.launch_limit(stop_ms,0);
  const auto misleading=plan_native_anchor_launch(mixed_groups,measured_options,observed,
      observed.captured_ms,false,whole_limit);
  require(misleading && misleading->batch.groups.size()<=uniform_groups.size() &&
      !plan_timed_native_anchor_launch(mixed_groups,measured_options,observed,observed.captured_ms,false,expensive_setup),
      "A mixed estimate admitted an unaffordable calibrated prefix");
  expensive_setup.record_verified(4000,4*measured_launches,3'800'000'000,uniform_groups.front());
  require(expensive_setup.launch_limit(stop_ms,0)==0 &&
      !plan_timed_native_anchor_launch(mixed_groups,measured_options,observed,observed.captured_ms,false,expensive_setup),
      "A setup cost that consumes the whole allowance admitted mixed work");
  require(setup_timing.launch_limit(2000,uniform_groups.front())==0,
      "Startup-aware sizing did not charge setup against the deadline");
  setup_timing.record_verified(2600,measured_launches,1'850'000'000,uniform_groups.front());
  const auto slow_profile=setup_timing.launch_limit(stop_ms,uniform_groups.front());
  require(slow_profile && *slow_profile>=measured_launches && *slow_profile<2*measured_launches,
      "Slower verified incremental work did not shrink the burst");
  const auto priced_small=plan_timed_native_anchor_launch(std::span(smaller_groups).subspan(1),measured_options,
      observed,observed.captured_ms,false,setup_timing);
  require(priced_small && priced_small->batch.groups.size()==1 && priced_small->batch.groups[0].whole.members==2,
      "The planner priced a smaller group below the full calibrated group");
  setup_timing.record_verified(2600,measured_launches,2'600'000'000,uniform_groups.front());
  require(setup_timing.launch_limit(stop_ms,uniform_groups.front())==setup_timing.launch_limit(stop_ms,0),
      "An inconsistent profile displaced the complete-cost estimate");

  options.output=anchor_match::BatchOutput::wide_anchors;
  require(!plan_native_anchor_launch(members,options,observed,observed.captured_ms,false),
      "Wide copy admitted without an approved dispatch size");
  options.maximum_dispatch_members=12;
  const auto widened=plan_native_anchor_launch(members,options,observed,observed.captured_ms,false);
  require(widened && widened->batch.quantum_members==12 && widened->batch.launches<full->batch.launches &&
      widened->capacity.request_bytes==full->capacity.request_bytes &&
      widened->capacity.reply_bytes==full->capacity.reply_bytes,"Wide copy did not reuse the existing bounded transfer");
  using Output=anchor_match::BatchOutput;
  auto reference_options=options;
  reference_options.prefer_reference=true;
  const auto priced=plan_native_anchor_launch(members,reference_options,observed,observed.captured_ms,true);
  const auto retained=plan_native_anchor_launch(members,reference_options,observed,observed.captured_ms,false);
  require(priced && priced->batch.output==Output::reference_xor && priced->batch.reference_candidates>=2,
      "Qualified reference profile skipped a funded reference search");
  require(retained && retained->batch==widened->batch && retained->transfer==widened->transfer,
      "A retained base lost its approved copy route");
  auto copy_headroom=observed;
  copy_headroom.physical_available=copy_headroom.commit_available=required_sized_worker_headroom(widened->capacity,widened->worker);
  const auto copy=plan_native_anchor_launch(members,reference_options,copy_headroom,copy_headroom.captured_ms,true);
  require(copy && copy->batch==widened->batch && copy->transfer==widened->transfer,
      "A larger reference reply displaced the funded copy batch");
  NativeAnchorBatchTiming timing;
  const auto timeout=reference_options.worker.timeout_ms;
  require(!timing.launch_limit(timeout),"An unobserved route claimed a timing budget");
  reference_options.output=Output::anchors;
  reference_options.maximum_dispatch_members=4;
  constexpr std::array<std::uint32_t,5> families{18,18,18,18,18};
  const auto reference_full=plan_native_anchor_launch(families,reference_options,observed,observed.captured_ms,true);
  require(reference_full && reference_full->batch.groups.size()==families.size() &&
      reference_full->batch.output==Output::reference_xor,"Timing fixture did not offer the full reference batch");
  timing.record_verified(4000,reference_full->batch.launches);
  const auto margin=timing.launch_limit(timeout);
  const auto prefix=plan_native_anchor_launch(families,reference_options,observed,observed.captured_ms,true,margin);
  require(margin && prefix && prefix->batch.output==Output::reference_xor &&
      prefix->batch.groups.size()<families.size() && prefix->batch.launches<=*margin &&
      prefix->batch.reference_candidates>=2 && prefix->worker.timeout_ms==timeout,
      "A measured launch bound failed to reserve a deadline margin for a whole reference prefix");
  timing.record_verified(3000,17);
  const auto slow=timing.launch_limit(timeout);
  require(slow && *slow<*margin,"Slower verified work failed to shrink the launch grant");
  timing.record_verified(100,17);
  const auto recovered=timing.launch_limit(timeout);
  require(recovered && *recovered>=*slow && *recovered<*margin &&
      !plan_native_anchor_launch(families,reference_options,observed,observed.captured_ms,true,0),
      "One fast sample bypassed the bound or zero paid work admitted a launch");
  options.maximum_dispatch_members=options.maximum_members+1;
  require(!plan_native_anchor_launch(members,options,observed,observed.captured_ms,false),
      "Wide copy exceeded the approved batch work");
  std::cout << "anchor launch prefix: wide=4 at_budget=3 below_budget=2 below_minimum=0\n";
}
#endif
template<class F>void rejected(F&& run,const char* why) {
  bool refused=false;
  try{run();}catch(const std::exception&){refused=true;}
  require(refused,why);
}
void windows_processor_windows() {
  const auto delta=detail::windows_processor_delta;
  const detail::WindowsProcessorTimes start{70,100,40};
  const auto work=delta(start,{85,125,55});
  require(work && work->total==40 && work->busy==25,"Native CPU sample did not account for kernel idle ticks");
  const auto idle=delta(start,{95,125,40});
  require(idle && idle->total==25 && idle->busy==0,"Native idle sample reported busy ticks");
  const auto busy=delta(start,{70,125,50});
  require(busy && busy->total==35 && busy->busy==35,"Native busy sample lost active ticks");
  require(!delta(start,start) && !delta(start,{69,130,55}) && !delta(start,{70,99,55}) &&
      !delta(start,{70,101,39}) && !delta(start,{90,119,55}),"Invalid CPU counter sequence was admitted");
  require(!delta({0,0,0},{0,(std::numeric_limits<std::uint64_t>::max)(),1}),"Overflowing CPU sample was admitted");
}
void backing_maintenance_window() {
  using Operation=detail::MaintenanceBackingOperation;
  using Choice=detail::MaintenanceFollowup;
  detail::MaintenanceBackingUse use,isolated;
  {
    const detail::MaintenanceBackingScope tracking(use);
    {
      detail::MaintenanceBackingCall written(Operation::write,39);
      written.complete();
    }
    {
      const detail::MaintenanceBackingScope separate(isolated);
      detail::MaintenanceBackingCall reads(Operation::read,91);
      reads.complete();
    }
    detail::MaintenanceBackingCall partial(Operation::write,17);
  }
  {
    detail::MaintenanceBackingCall ordinary(Operation::write,100);
    ordinary.complete();
  }
  const auto written=use[Operation::write];
  require(written.attempts==2 && written.completed==1 && written.attempted_bytes==56 &&
      written.completed_bytes==39 && written.elapsed_ns>0 && use[Operation::read].attempts==0 &&
      isolated[Operation::read].completed_bytes==91,"maintenance sample mixed scopes or counted an incomplete write");
  detail::MaintenanceBackingTiming timing;
  constexpr auto window=10'000'000ULL;
  constexpr auto elapsed=3'000'000ULL;
  constexpr auto previous=2'000'000ULL;
  const auto fits=[&](std::uint64_t spent,std::uint64_t last) {
    return detail::maintenance_followup(window,spent,last,timing.predicted_ns());
  };
  require(fits(elapsed,previous)==Choice::admit && fits(window,previous)==Choice::elapsed &&
      fits(elapsed,window)==Choice::elapsed,"unobserved backing replaced the wall-time gate");
  timing.observe(24'000'000);
  require(fits(elapsed,previous)==Choice::backing,"slow backing did not restrict a continued idle step");
  timing.observe(0);
  require(timing.predicted_ns()==24'000'000,"missing backing evidence reset the recent cost");
  timing.observe(1'000'000);
  require(fits(elapsed,previous)==Choice::backing,"one fast step erased recent backing contention");
  for(unsigned sample=0;sample<4;++sample)timing.observe(1'000'000);
  require(fits(elapsed,previous)==Choice::admit && fits(window,previous)==Choice::elapsed,
      "repeated fast backing evidence did not recover the remaining-window admission");
  std::cout<<"maintenance_backing_gate slower_refused=1 missing_preserved=1 recovery_after_observation=1"
      <<" wall_gate_preserved=1 partial_writes_separate=1 isolated_scopes=1\n";
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
void sample_native_pdh(unsigned samples) {
  PDH_HQUERY query=nullptr;
  PDH_HCOUNTER counter=nullptr;
  require(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS,"Native PDH test query");
  const auto added=PdhAddEnglishCounterW(query,L"\\Processor Information(_Total)\\% Processor Time",0,&counter);
  auto collected=added;
  if(added==ERROR_SUCCESS)for(unsigned index=0;index<samples;++index) {
    if(index)Sleep(1010);
    collected=PdhCollectQueryData(query);
    if(collected!=ERROR_SUCCESS)break;
  }
  const auto closed=PdhCloseQuery(query);
  require(added==ERROR_SUCCESS && collected==ERROR_SUCCESS && closed==ERROR_SUCCESS,"Native PDH test lifecycle");
}
void initialize_pdh_handles() {
  const auto cold=handle_count();
  sample_native_pdh(1);
  const auto first=handle_count();
  sample_native_pdh(1);
  const auto stable=handle_count();
  std::cout<<"native_pdh_handles cold="<<cold<<" first="<<first<<" stable="<<stable<<'\n';
  require(first==stable,"Repeated native PDH initialization retained handles");
}
void initialize_long_pdh_window() {
  const auto initial=handle_count();
  sample_native_pdh(35);
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
void callback_cleanup_close(Provider& p,PVOID context,bool written=false) {
  FSP_FSCTL_TRANSACT_REQ request;
  FSP_FSCTL_TRANSACT_RSP response;
  std::memset(&request,0,sizeof(request));
  std::memset(&response,0,sizeof(response));
  request.Kind=FspFsctlTransactCleanupKind;
  request.Req.Cleanup.UserContext2=reinterpret_cast<UINT64>(context);
  request.Req.Cleanup.SetArchiveBit=written;
  request.Req.Cleanup.SetLastWriteTime=written;
  request.Req.Cleanup.SetChangeTime=written;
  require(FspFileSystemOpCleanup(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Cleanup");
  request.Kind=FspFsctlTransactCloseKind;
  request.Req.Close.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpClose(&p.fs,&request,&response)==STATUS_SUCCESS,"controller SDK Close");
}
void callback_file(Provider& p,unsigned n,const Bytes& bytes,std::size_t quantum=kNativeMaxReadBytes) {
  const auto* table=p.fs.Interface;SIZE_T security_size=0;UINT32 attributes=0;
  require(table->GetSecurityByName(&p.fs,const_cast<PWSTR>(L"\\"),&attributes,nullptr,&security_size)==STATUS_BUFFER_OVERFLOW,
      "controller root security size");Bytes security(security_size);
  require(table->GetSecurityByName(&p.fs,const_cast<PWSTR>(L"\\"),&attributes,security.data(),&security_size)==STATUS_SUCCESS,
      "controller root security");
  auto name=L"\\f"+std::to_wstring(n);PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
  require(table->Create(&p.fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&context,&info)==STATUS_SUCCESS,"controller SDK Create");
  for(std::size_t offset=0;offset<bytes.size();offset+=quantum) {
    const auto length=static_cast<ULONG>((std::min)(quantum,bytes.size()-offset));
    ULONG count=0;
    require(table->Write(&p.fs,context,const_cast<std::byte*>(bytes.data()+offset),offset,length,
        FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==length,"controller SDK Write");
  }
  Bytes out(bytes.size());ULONG count=0;require(table->Read(&p.fs,context,out.data(),0,static_cast<ULONG>(out.size()),&count)==STATUS_SUCCESS &&
      count==out.size() && out==bytes,"controller SDK byte readback");
  callback_cleanup_close(p,context);
}
// Adds one sampled stall to the selected idle or demand read.
class DelayedLookaheadBacking final:public StoreByteIo {
 public:
  explicit DelayedLookaheadBacking(std::shared_ptr<StoreByteIo> bytes):bytes_(std::move(bytes)) {}
  bool concurrent_reads_supported() const noexcept override{return bytes_->concurrent_reads_supported();}
  std::uint64_t extent() const override{return bytes_->extent();}
  StoreIoContract contract() const noexcept override{return bytes_->contract();}
  void admit_write(std::uint64_t bytes) override{bytes_->admit_write(bytes);}
  void write_exact(std::uint64_t offset,std::span<const std::byte> bytes) override{bytes_->write_exact(offset,bytes);}
  void flush() override{bytes_->flush();}
  void read_exact(std::uint64_t offset,std::span<std::byte> bytes) const override {
    auto& delay=GetCurrentThreadId()==foreground_?delay_demand_:delay_idle_;
    if(delay.exchange(false)) {
      detail::MaintenanceBackingCall wait(detail::MaintenanceBackingOperation::control);
      std::this_thread::sleep_for(std::chrono::milliseconds(45));
      wait.complete();
    }
    bytes_->read_exact(offset,bytes);
  }
  void delay_next_idle_read() noexcept{delay_idle_=true;}
  void delay_next_demand_read() noexcept{delay_demand_=true;}
 private:
  std::shared_ptr<StoreByteIo> bytes_;
  const DWORD foreground_=GetCurrentThreadId();
  mutable std::atomic<bool> delay_idle_{false},delay_demand_{false};
};
std::shared_ptr<NativeHybridBacking> select_capture_hybrid(NativeFileSession& session,MemoryVolumeDomain& domain,
    std::shared_ptr<StoreByteIo> base,StoreIdentity next,std::uint64_t capacity,StoreCompressionOptions compression) {
  auto workspace=domain.create_store(capacity/2);
  auto tail=domain.create_store(capacity);
  auto work=NativeHybridBacking::prepare(session,workspace,std::move(base),tail,next,capacity,capacity,compression);
  for(unsigned step=0;!work.advance(session);++step)
    require(step<8192,"capture fixture could not prepare a sealed image");
  std::shared_ptr<NativeHybridBacking> image;
  require(work.publish(session,image) && image->concurrent_reads_supported(),
      "capture fixture could not select its concurrent sealed image");
  return image;
}

void capture_during_hybrid_tail_write() {
  constexpr std::uint64_t capacity=8ULL*1024*1024;
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto domain=MemoryVolumeDomain::create(3*capacity+kVolumeHeadBytes);
  auto head=domain.create_store(kVolumeHeadBytes),source=domain.create_store(capacity);
  auto base=std::make_shared<test::PausedCaptureBacking>(domain.create_store(capacity/2));
  auto session=NativeFileSession::create_indexed_volume(head,id(90),source,id(1),{capacity},fast);
  session.create_entry("visible");
  const auto file=session.open("visible",{true,true,false});
  const auto original=native_lz4_fixture::source();
  auto body=data(2*kStoreMaxObjectBytes,157);
  std::ranges::copy(original,body.begin());
  for(std::size_t at=0;at<body.size();at+=kStoreMaxObjectBytes)
    session.write(file,at,std::span(body).subspan(at,kStoreMaxObjectBytes));
  auto image=select_capture_hybrid(session,domain,base,id(2),capacity,fast);
  image->verify_backing();
  auto lease=session.lease_native_lz4_capture(file,0,kNativeExtentBytes);
  require(lease!=nullptr,"sealed image did not expose the normal LZ4 capture lease");
  std::atomic<bool> interrupt{false};
  auto capture=std::async(std::launch::async,[&]{
    return lease->capture_range(kNativeExtentBytes,2*kNativeExtentBytes,[&]{return interrupt.load();});
  });
  require(base->wait_until_held(),"LZ4 capture did not reach the sealed base");
  require(session.read(file,0,kNativeExtentBytes)==original,"held capture impeded exact foreground read");
  auto edited=original;
  edited.back()^=std::byte{0xa9};
  session.write(file,0,edited);
  require(session.read(file,0,kNativeExtentBytes)==edited &&
      capture.wait_for(std::chrono::seconds(0))==std::future_status::timeout,
      "held capture impeded publication to the tail");
  interrupt=true;
  base->release();
  require(capture.get().empty() && base->capture_reads()==1 && !image->backing_degraded(),
      "cancelled capture retried the base or marked a healthy image degraded");
  detail::StoreCodecDecoder cpu;
  const auto exact=[&](const detail::NativeLz4CaptureLease& from,const Bytes& expected) {
    detail::NativeLz4CapturedFrame frame;
    require(from.capture(frame) && frame.key().image==id(2),"sealed capture did not bind its selected identity");
    const auto decoded=cpu.decode(detail::kStoreLz4,frame.encoded(),frame.key().decoded_bytes);
    require(decoded.size()>=expected.size() && std::ranges::equal(std::span(decoded).last(expected.size()),expected),
        "captured version did not independently decode on CPU");
    return frame.key();
  };
  const auto latest=session.lease_native_lz4_capture(file,0,kNativeExtentBytes);
  require(latest!=nullptr && exact(*lease,original)!=exact(*latest,edited),
      "old and new sealed captures used the same frame identity");
  session.cleanup(file,false);
  session.close(file);
  std::cout<<"hybrid_lz4_capture held_base=1 foreground_tail_write=1 cancellation_backing_calls=1"
      <<" healthy_after_cancel=1 old_and_new_cpu_exact=1\n";
}

void read_capture_pins_selected_image() {
  constexpr std::uint64_t capacity=8ULL*1024*1024;
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto domain=MemoryVolumeDomain::create(capacity+kVolumeHeadBytes);
  auto head=domain.create_store(kVolumeHeadBytes);
  auto image=domain.create_store(capacity);
  const auto original=native_lz4_fixture::source();
  auto replacement=original;
  replacement.back()^=std::byte{79};
  std::unique_ptr<detail::NativeLz4CaptureLease> lease;
  {
    auto session=NativeFileSession::create_indexed_volume(head,id(90),image,id(1),{capacity},fast);
    session.create_entry("retained");
    const auto file=session.open("retained",{true,true,true});
    session.write(file,0,original);
    lease=session.lease_native_lz4_capture(file,0,kNativeExtentBytes);
    require(lease!=nullptr,"memory image refused the selected capture");
    session.write(file,0,replacement);
    require(session.read(file,0,kNativeExtentBytes)==replacement,
        "ordinary reader lost the acknowledged write while the capture held the previous view");
    session.cleanup(file,false);
    session.close(file);
  }
  const auto before=domain.usage();
  require(domain.retire_store(image)==MemoryStoreRetirement::pinned && domain.usage().resident==before.resident,
      "retirement discarded bytes held by a pending capture");
  detail::NativeLz4CapturedFrame frame;
  require(lease->capture(frame),"closed writer invalidated the pinned capture");
  detail::StoreCodecDecoder cpu;
  const auto decoded=cpu.decode(detail::kStoreLz4,frame.encoded(),frame.key().decoded_bytes);
  require(decoded.size()>=original.size() &&
      std::ranges::equal(std::span(decoded).last(original.size()),original),
      "the capture returned a different version of the retained extent");
  lease.reset();
  while(domain.retire_store(image)==MemoryStoreRetirement::advanced) {}
  const auto after=domain.usage();
  require(!image && before.stores==after.stores+1 && before.resident>after.resident &&
      before.reserved==after.reserved+capacity,"finished capture kept its memory image reserved");
  std::cout<<"read_capture_pin retained_selected_version=1 later_write_exact=1 protected_from_retirement=1"
      <<" retired_after_release=1 reclaimed_locked_bytes="<<before.resident-after.resident<<'\n';
}
void live_read_ahead_admission() {
  require(store_lz4_available(),"live read-ahead requires the pinned LZ4 decoder");
  read_capture_pins_selected_image();
  require(MemoryVolumeDomain::cleanup_healthy(),"capture fixture failed to retire its memory domain");
  constexpr std::uint64_t extent=kNativeExtentBytes,image_capacity=8ULL*1024*1024;
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto domain=MemoryVolumeDomain::create(image_capacity+kVolumeHeadBytes);
  auto head=domain.create_store(kVolumeHeadBytes);
  auto image=std::make_shared<DelayedLookaheadBacking>(domain.create_store(image_capacity));
  NativeMaintenancePolicy qualification;
  qualification.maximum_cpu_busy_ppm=500'000;
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),{image_capacity},fast,{},qualification);
  require(adapter->enable_native_lz4_read_ahead(),"read-ahead controller could not enable the checked route");
  auto* route=adapter.get();
  Provider provider;
  Provider::active=&provider;
  WinFspVolumeController controller(std::move(adapter),provider.api(),plan(),domain);
  await([&]{return controller.view().state==SessionState::running;},5000,"read-ahead controller startup deadline");

  std::array<Bytes,3> frames{Bytes(extent),Bytes(extent),Bytes(extent)};
  // Two logical holes establish sequential access. The trailing stored extent
  // isolates read-ahead from discovery of cross-file compression candidates.
  const auto motif=data(extent/8,917);
  for(std::size_t offset=0;offset<extent;offset+=motif.size())
    std::ranges::copy(motif,frames.back().begin()+static_cast<std::ptrdiff_t>(offset));
  Bytes file;
  for(const auto& frame:frames)file.insert(file.end(),frame.begin(),frame.end());
  callback_file(provider,0,file,kNativeExtentBytes);
  await([&]{return controller.view().payload_scrub_passes>0 && !route->background_maintenance_pending();},
      12000,"initial maintenance never returned idle credit");
  auto* fs=&provider.fs;
  wchar_t name[]=L"\\f0";
  PVOID context=nullptr;
  FSP_FSCTL_FILE_INFO info{};
  constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
  require(fs->Interface->Open(fs,name,open_file,FILE_READ_DATA,&context,&info)==STATUS_SUCCESS &&
      context && info.FileSize==file.size(),"read-ahead controller opened wrong content");
  const auto demand=[&](std::size_t i) {
    Bytes output(extent);
    ULONG count=0;
    require(fs->Interface->Read(fs,context,output.data(),i*extent,static_cast<ULONG>(output.size()),&count)==
        STATUS_SUCCESS && count==output.size() && output==frames[i],"controller demand bytes differ");
  };
  demand(0);demand(1);
  try {
    await([&]{return controller.view().read_ahead_prepared==1;},12000,"fast backing did not prepare the next frame");
  }catch(...) {
    const auto state=controller.view();
    std::cerr<<"read-ahead admission status host="<<static_cast<unsigned>(state.host)
        <<" phase="<<static_cast<unsigned>(state.maintenance.phase)
        <<" deferral="<<static_cast<unsigned>(state.maintenance.deferral)
        <<" pending="<<route->native_lz4_read_ahead_pending()<<" quanta="<<state.maintenance_quanta
        <<" backing_refusals="<<state.read_ahead_backing_refusals<<'\n';
    throw;
  }
  const auto fast_sample=controller.view().maintenance_backing;
  demand(2);
  require(route->native_lz4_read_ahead_hits()==1,"controller did not deliver the prepared frame");
  // Replace the next forecast with a new run over the same backing and arm
  // one slow call before its target becomes eligible.
  demand(0);
  image->delay_next_idle_read();
  demand(1);
  await([&]{return controller.view().read_ahead_prepared==2;},12000,"slow sample did not complete its admitted frame");
  const auto slow_sample=controller.view().maintenance_backing;
  require(slow_sample.elapsed_ns()>fast_sample.elapsed_ns()+40'000'000,"stall was absent from backing observations");
  demand(2);
  demand(0);demand(1);
  await([&]{return controller.view().read_ahead_backing_refusals==1;},12000,"slow backing cost did not block speculation");
  const auto refused=controller.view();
  require(refused.read_ahead_prepared==2 && refused.maintenance_backing.elapsed_ns()==slow_sample.elapsed_ns(),
      "backing refusal performed speculative storage operations");
  // A refused prediction is inert until ordinary reads supply another hint.
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  const auto quiet=controller.view();
  require(quiet.maintenance_quanta==refused.maintenance_quanta &&
      quiet.read_ahead_demand_samples==0 && quiet.maintenance_backing.elapsed_ns()==slow_sample.elapsed_ns(),
      "refused read-ahead woke on a timer without demand");

  image->delay_next_demand_read();
  demand(2);
  require(route->native_lz4_read_ahead_hits()==2,"declined read-ahead failed to select ordinary CPU read");
  demand(0);demand(1);
  await([&]{return controller.view().read_ahead_backing_refusals==2;},12000,"slow CPU demand did not keep the backing limit");
  const auto slow_demand=controller.view();
  require(slow_demand.read_ahead_demand_samples==1 && slow_demand.read_ahead_prepared==2 &&
      slow_demand.maintenance_backing.elapsed_ns()==slow_sample.elapsed_ns(),
      "matching slow demand re-admitted speculation or caused probe I/O");

  auto refusals=slow_demand.read_ahead_backing_refusals;
  WinFspControllerView recovered;
  for(unsigned i=0;i<8;++i) {
    demand(2);demand(0);demand(1);
    await([&]{
      recovered=controller.view();
      return recovered.read_ahead_backing_refusals>refusals || recovered.read_ahead_prepared>2;
    },12000,"successful demand produced no read-ahead decision");
    require(recovered.read_ahead_demand_samples==i+2,"controller lost or replayed a demand sample");
    if(recovered.read_ahead_prepared>2)break;
    require(recovered.maintenance_backing.elapsed_ns()==slow_sample.elapsed_ns(),
        "controller performed backing I/O to learn recovery");
    refusals=recovered.read_ahead_backing_refusals;
  }
  require(recovered.read_ahead_prepared==3,"ordinary fast reads did not re-admit speculation");
  demand(2);
  require(route->native_lz4_read_ahead_hits()==3,"readmission failed to deliver exact prepared bytes");
  callback_cleanup_close(provider,context);
  controller.stop_and_join();
  require(controller.view().retired && !controller.view().controller_failed &&
      provider.fs.UserContext==nullptr,"read-ahead controller did not retire");
  std::cout<<"read_ahead_controller prepared=3 exact_hits=3 backing_refused="
      <<recovered.read_ahead_backing_refusals<<" refused_backing_calls=0"
      <<" modeled_stall_ns="<<slow_sample.elapsed_ns()-fast_sample.elapsed_ns()
      <<" demand_samples="<<recovered.read_ahead_demand_samples
      <<" slow_demand_kept_limit=1 timer_only_retries=0 demand_recovered=1 exact_cpu_fallback=1 owner_retired=1\n";
}
enum class AmdReadFixtureLayout {mirrored,prepared,automatic_sealed,reopened_pair,reopened_dense};
int live_amd_read_ahead(char** argv,bool refuse,AmdReadFixtureLayout layout=AmdReadFixtureLayout::mirrored) {
  const bool sealed=layout!=AmdReadFixtureLayout::mirrored;
  const bool automatic=layout==AmdReadFixtureLayout::automatic_sealed;
  const bool dense=layout==AmdReadFixtureLayout::reopened_dense;
  const bool paired=layout==AmdReadFixtureLayout::reopened_pair || dense;
  require(store_lz4_available(),"owned AMD read-ahead requires a qualified CPU LZ4 decoder");
  const auto number=[](std::string_view value) {
    std::uint32_t result=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    require(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size(),"invalid AMD read fixture number");
    return result;
  };
  const auto wide=[](std::string_view value){return std::wstring(value.begin(),value.end());};
  const std::string_view raw_luid(argv[7]);
  require(raw_luid.size()==16,"invalid AMD read fixture LUID");
  const auto digit=[](char value) {
    if(value>='0' && value<='9')return unsigned(value-'0');
    if(value>='a' && value<='f')return unsigned(value-'a'+10);
    throw std::invalid_argument("invalid AMD read fixture LUID digit");
  };
  NativeLz4ReadWorkerProfile profile;
  profile.helper=argv[2];
  profile.helper_sha256=argv[3];
  profile.runtime_name=wide(argv[4]);
  profile.runtime_sha256=argv[5];
  profile.module_sha256=argv[6];
  for(std::size_t i=0;i<profile.luid.size();++i)
    profile.luid[i]=static_cast<std::byte>(16*digit(raw_luid[2*i])+digit(raw_luid[2*i+1]));
  profile.architecture=refuse?L"gfx0":wide(argv[8]);
  profile.ordinal=number(argv[9]);
  profile.wave_mask=number(argv[10]);
  profile.node_mask=1;
  profile.worker={160,1200,14000,16};
  profile.minimum_frames=profile.maximum_frames=2;
  profile.maximum_slab_bytes=256U*1024U;
  constexpr std::uint64_t extent=kNativeExtentBytes,image_capacity=8ULL*1024*1024;
  constexpr std::size_t window=2*extent;
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto owned=automatic?MemoryVolumeBacking::create(true):MemoryVolumeBacking{};
  std::optional<MemoryVolumeDomain> standalone;
  if(!automatic && !paired)
    standalone.emplace(MemoryVolumeDomain::create((sealed?3:1)*image_capacity+kVolumeHeadBytes));
  auto* domain=automatic?owned.domain.get():standalone?&*standalone:nullptr;
  std::shared_ptr<StoreByteIo> head,image;
  std::shared_ptr<SparseVolumeRam> paired_backing;
  std::shared_ptr<DenseTailRam> dense_backing;
  NativeMaintenancePolicy qualification;
  qualification.maximum_cpu_busy_ppm=500'000;
  std::unique_ptr<WinFspNativeWritable> adapter;
  if(automatic)adapter=WinFspNativeWritable::create_indexed_memory(owned,fast,qualification);
  else if(dense) {
    dense_backing=std::make_shared<DenseTailRam>();
    dense_backing->bytes.failure_unit=4096;
    dense_backing->bytes.capacity=2*image_capacity+2*1024*1024;
    adapter=WinFspNativeWritable::create_file_volume(dense_backing,id(90),id(1),{image_capacity},fast,
        dense_backing->bytes.capacity);
  }
  else if(paired) {
    paired_backing=std::make_shared<SparseVolumeRam>(4096,2*image_capacity+2*1024*1024,true);
    adapter=WinFspNativeWritable::create_file_volume(paired_backing,id(90),id(1),{image_capacity},fast);
  } else {
    head=domain->create_store(kVolumeHeadBytes);
    image=domain->create_store(image_capacity);
    adapter=WinFspNativeWritable::create_managed_indexed(
        head,id(90),image,id(1),{image_capacity},fast,{},qualification);
  }
  Bytes file(3*window);
  for(std::size_t i=0;i<2;++i) {
    const auto motif=data(extent/8,917+static_cast<unsigned>(i));
    for(std::size_t at=0;at<extent;at+=motif.size())
      std::ranges::copy(motif,file.begin()+static_cast<std::ptrdiff_t>(2*window+i*extent+at));
  }
  Provider provider;
  Provider::active=&provider;
  std::shared_ptr<NativeHybridBacking> selected;
  if(sealed && !automatic) {
    {
      WinFspNativeSession seed(std::move(adapter),provider.api());
      require(seed.start(plan())==STATUS_SUCCESS,"sealed AMD source callbacks did not start");
      callback_file(provider,0,file,static_cast<ULONG>(extent));
      callback_file(provider,1,data(kStoreMaxObjectBytes,227),static_cast<ULONG>(extent));
      if(paired)callback_file(provider,2,Bytes(4*1024*1024),static_cast<ULONG>(extent));
      seed.stop();
      require(seed.write_restart_ready(),"sealed AMD source owner did not retire");
    }
    if(paired) {
      {
        std::shared_ptr<StoreByteIo> packed=dense?std::static_pointer_cast<StoreByteIo>(dense_backing):paired_backing;
        auto builder=NativeVolumeFile::resume(std::move(packed),id(90),raw,NativeDispositionPolicy::windows_global);
        require(builder.persistent_reclamation_pending(true),"paired AMD source did not fund its seal");
        using Step=NativeVolumeReclaimProgress;
        bool selected_pair=false;
        for(unsigned turn=0;turn<4096;++turn) {
          const auto step=builder.advance_persistent_reclamation([]{return id(2);},true);
          if(step==Step::published) {
            selected_pair=builder.volume_info().selection.sealed_range.has_value();
            break;
          }
          require(step==Step::copying || step==Step::ready,"paired AMD source could not prepare its seal");
        }
        require(selected_pair,"paired AMD source did not select a sealed image");
        const auto inactive_pending=[&]{
          return dense?builder.persistent_reclamation_pending():
              paired_backing->allocated_extent(kNativeVolumeFileImageOffset,image_capacity)!=0;
        };
        for(unsigned turn=0;inactive_pending();++turn) {
          require(turn<8 && builder.advance_persistent_reclamation({})==Step::retiring,
              "paired AMD setup could not retire the old range");
        }
        if(dense) {
          const auto before=builder.dense_file_space().value().file_end;
          unsigned work=0;
          for(;work<32 && builder.persistent_reclamation_pending(true);++work) {
            const auto step=builder.advance_persistent_reclamation({},true);
            if(step==Step::idle)break;
            require(step==Step::retiring,"dense AMD backing could not retire its outer tail");
          }
          const auto after=builder.dense_file_space().value().file_end;
          require(work && work<32 && after<before,"dense AMD backing retained its old physical extent");
          std::cout<<"amd_dense_storage original_extent="<<before<<" retained_extent="<<after
              <<" maintenance_steps="<<work<<'\n';
        }
      }
      if(dense) {
        dense_backing=DenseTailRam::replay(*dense_backing);
        adapter=WinFspNativeWritable::resume_file_volume(dense_backing,id(90),raw);
      } else {
        paired_backing=paired_backing->replay();
        adapter=WinFspNativeWritable::resume_file_volume(paired_backing,id(90),raw);
      }
    } else {
      {
        auto builder=NativeFileSession::resume_closed_indexed_volume(head,id(90),1,
            [&](const StoreIdentity& choice){return choice==id(1)?image:nullptr;},fast,
            NativeDispositionPolicy::windows_global);
        selected=select_capture_hybrid(builder,*domain,domain->create_store(image_capacity/2),
            id(2),image_capacity,fast);
      }
      adapter=WinFspNativeWritable::resume_indexed_retired(head,id(90),2,
          [&](const StoreIdentity& choice){return choice==id(2)?selected:nullptr;},fast,{},0,qualification);
    }
  }
  auto* route=adapter.get();
  std::optional<WinFspVolumeController> controller;
  if(domain)controller.emplace(std::move(adapter),provider.api(),plan(),*domain,
      std::nullopt,nullptr,std::move(profile));
  else controller.emplace(std::move(adapter),provider.api(),plan(),std::nullopt,nullptr,std::move(profile));
  auto& owner=*controller;
  await([&]{return owner.view().state==SessionState::running;},5000,"AMD read controller startup");
  require(!owner.view().amd_read.configuration_refused,"AMD read package did not verify");
  if(!sealed || automatic)callback_file(provider,0,file,static_cast<ULONG>(extent));
  if(automatic)callback_file(provider,1,data(kStoreMaxObjectBytes,227),static_cast<ULONG>(extent));
  PVOID context=nullptr;
  FSP_FSCTL_FILE_INFO info{};
  wchar_t name[]=L"\\f0";
  auto* fs=&provider.fs;
  require(fs->Interface->Open(fs,name,FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_READ_DATA,&context,&info)==STATUS_SUCCESS &&
      context && info.FileSize==file.size(),"AMD read fixture opened the wrong file");
  if(automatic) {
    // A first data callback adopts the ordinary Open before maintenance can
    // move the selected image. Keep this same context across publication.
    std::byte first{};
    ULONG moved=0;
    require(fs->Interface->Read(fs,context,&first,0,1,&moved)==STATUS_SUCCESS &&
        moved==1 && first==file.front(),"AMD read fixture could not adopt its open context");
  }
  const auto startup_timeout=automatic?85000:(paired?35000:12000);
  try {
    await([&]{
      const auto status=owner.view();
      if(!status.payload_scrub_passes || route->background_maintenance_pending())return false;
      require(!automatic || status.sealed_compactions==1,"maintenance quiesced without selecting a sealed image");
      return true;
    },startup_timeout,"AMD read fixture initial maintenance did not quiesce");
  }catch(...) {
    const auto state=owner.view();
    const auto volume=route->snapshot();
    const auto input=volume.workload_info();
    const auto contexts=route->stats();
    std::cerr<<"AMD read startup sealed="<<sealed<<" host="<<static_cast<unsigned>(state.host)
        <<" quanta="<<state.maintenance_quanta<<" sealed_swaps="<<state.sealed_compactions
        <<" compactions="<<state.compactions<<" compaction_failures="<<state.compaction_failures
        <<" scrub_checks="<<state.payload_scrub_checks
        <<" scrub_passes="<<state.payload_scrub_passes<<" scrub_faults="<<state.payload_scrub_faults
        <<" phase="<<static_cast<unsigned>(state.maintenance.phase)
        <<" deferral="<<static_cast<unsigned>(state.maintenance.deferral)
        <<" attempted="<<state.maintenance.attempted_groups<<" disabled="<<state.maintenance_disabled
        <<" failed="<<state.controller_failed<<" offline="<<state.offline
        <<" input_enabled="<<input.enabled<<" committed_input="<<input.committed_input_bytes
        <<" image_bytes="<<volume.storage_info().physical_end<<" contexts="<<contexts.contexts
        <<" latched="<<contexts.latched_inodes<<'\n';
    throw;
  }
  const auto demand=[&](unsigned index) {
    Bytes output(window);
    ULONG moved=0;
    require(fs->Interface->Read(fs,context,output.data(),index*window,static_cast<ULONG>(output.size()),&moved)==
        STATUS_SUCCESS && moved==output.size() &&
        std::equal(output.begin(),output.end(),file.begin()+static_cast<std::ptrdiff_t>(index*window)),
        "AMD read fixture callback bytes differed");
  };
  const auto train=[&]{demand(0);demand(1);};
  train();
  if(refuse) {
    await([&]{return owner.view().amd_read.failures!=0;},15000,"unsupported AMD worker did not retire");
    const auto outcome=owner.view().amd_read;
    if(!outcome.retirement_confirmed || !outcome.configuration_refused ||
        hip_worker_exit_category(outcome.last_worker_exit_code)!=
            static_cast<std::uint32_t>(HipWorkerExit::unsupported_configuration))
      std::cerr<<"AMD refusal submitted="<<outcome.submitted<<" published="<<outcome.published
          <<" exit="<<outcome.last_worker_exit_code<<" retired="<<outcome.retirement_confirmed
          <<" refused="<<outcome.configuration_refused<<'\n';
    require(outcome.submitted==1 && outcome.published==0 && outcome.retirement_confirmed &&
        outcome.configuration_refused && hip_worker_exit_category(outcome.last_worker_exit_code)==
            static_cast<std::uint32_t>(HipWorkerExit::unsupported_configuration),
        "AMD read refusal did not fence the qualified child");
    demand(2);
    require(route->native_lz4_read_ahead_hits()==0,"worker refusal left GPU-ready data");
    const auto before_cpu=owner.view();
    train();
    try {
      await([&]{return owner.view().read_ahead_prepared>before_cpu.read_ahead_prepared;},12000,
          "worker refusal did not preserve CPU preparation");
    }catch(...) {
      const auto status=owner.view();
      std::cerr<<"amd_refusal_followup immediate_cpu=1 prepared_before="<<before_cpu.read_ahead_prepared
          <<" prepared_after="<<status.read_ahead_prepared<<" backing_refusals="<<status.read_ahead_backing_refusals
          <<" demand_samples="<<status.read_ahead_demand_samples<<" host="<<static_cast<unsigned>(status.host)
          <<" submission_count="<<status.amd_read.submitted<<" quanta="<<status.maintenance_quanta
          <<" deferral="<<static_cast<unsigned>(status.maintenance.deferral)<<'\n';
      throw;
    }
    demand(2);
    const auto recovered=owner.view().amd_read;
    require(route->native_lz4_read_ahead_hits()==2 && recovered.submitted==1 &&
        recovered.budget_queries==outcome.budget_queries,
        "a refused worker was probed again or its CPU successor was lost");
  }else {
    await([&]{
      const auto result=owner.view().amd_read;
      return result.published || result.failures;
    },15000,"real AMD read worker did not finish");
    const auto complete=owner.view().amd_read;
    if(complete.failures)std::cerr<<"AMD read worker exit="<<complete.last_worker_exit_code<<'\n';
    require(complete.submitted==1 && complete.published==1 && complete.verified_frames==2 &&
        complete.retirement_confirmed && !complete.failures,"real AMD result was not published by the controller");
    demand(2);
    require(route->native_lz4_read_ahead_hits()==2,"ordinary callbacks did not consume both GPU frames");
    train();
    await([&]{return owner.view().amd_read.submitted==2 && owner.view().amd_read.pending;},
        12000,"second AMD window never entered its supervised flight");
    demand(2);
    await([&]{
      const auto state=owner.view().amd_read;
      return state.superseded==1 && state.retirement_confirmed;
    },8000,"CPU read did not retire the superseded AMD window");
    require(route->native_lz4_read_ahead_hits()==2 && owner.view().amd_read.published==1,
        "superseded GPU frames reached the ordinary path");
    train();
    await([&]{return owner.view().amd_read.submitted==3 && owner.view().amd_read.pending;},
        12000,"AMD read fixture did not start the stop-time flight");
  }
  if(refuse)callback_cleanup_close(provider,context);
  owner.stop_and_join();
  const auto state=owner.view();
  require(state.retired && state.amd_read.retirement_confirmed && !state.controller_failed &&
      provider.fs.UserContext==nullptr,"owned AMD read controller did not retire");
  if(!refuse)require(state.amd_read.submitted==3 && state.amd_read.superseded==2 &&
      state.amd_read.published==1,"stop did not cancel the last AMD read flight");
  if(paired) {
    std::shared_ptr<StoreByteIo> recovered=dense?
        std::static_pointer_cast<StoreByteIo>(DenseTailRam::replay(*dense_backing)):paired_backing->replay();
    const auto reopened=NativeVolumeFile::inspect(std::move(recovered),id(90));
    const auto& snapshot=reopened.filesystem();
    snapshot.verify_all();
    require(reopened.info().selection.epoch==2 && reopened.info().selection.image_identity==id(2) &&
        reopened.info().selection.sealed_range && !reopened.info().degraded && snapshot.list().size()==3 &&
        snapshot.read("f0",0,static_cast<std::uint32_t>(file.size()+1))==file &&
        snapshot.read("f1",0,kStoreMaxObjectBytes)==data(kStoreMaxObjectBytes,227),
        "reopened paired AMD fixture lost its selected image or CPU bytes");
    std::cout<<"paired_sealed_amd reopened=1 independent_cpu_projection=1\n";
  }
  if(automatic) {
    owned.release_unselected_after_join();
    require(state.sealed_compactions==1 && owned.selected_image_sealed(),
        "automatic selection was lost after controller shutdown");
    {
      Provider replacement;
      Provider::active=&replacement;
      WinFspNativeSession reopened(WinFspNativeWritable::resume_memory_volume(
          owned,NativeImageKind::indexed_paged,fast),replacement.api());
      require(reopened.start(plan())==STATUS_SUCCESS,"the selected hybrid could not reopen for ordinary writes");
      auto* mounted=&replacement.fs;
      PVOID writer=nullptr;
      FSP_FSCTL_FILE_INFO reopened_info{};
      require(mounted->Interface->Open(mounted,name,FILE_NON_DIRECTORY_FILE|(1U<<24),
          FILE_READ_DATA|FILE_WRITE_DATA,&writer,&reopened_info)==STATUS_SUCCESS && writer,
          "could not reopen the selected file for writing");
      auto edit=data(979,117);
      constexpr auto offset=2*window-19;
      ULONG written=0;
      require(mounted->Interface->Write(mounted,writer,edit.data(),offset,static_cast<ULONG>(edit.size()),
          FALSE,FALSE,&written,&reopened_info)==STATUS_SUCCESS && written==edit.size(),
          "write to the reopened tail failed");
      std::ranges::copy(edit,file.begin()+offset);
      callback_cleanup_close(replacement,writer);
      reopened.stop();
      require(reopened.write_restart_ready(),"reopened selected image did not retire");
    }
    Provider::active=&provider;
    auto view=owned.open_read_only();
    require(view.read(L"\\f0",0,static_cast<std::uint32_t>(file.size()))==file,
        "automatic sealed AMD fixture lost independent CPU projection after shutdown");
    std::cout<<"automatic_sealed_amd switches=1 original_context=1 reopened_write=1 independent_cpu_projection=1\n";
  }
  std::cout<<"winfsp_owned_amd_read sealed="<<(sealed?1:0)<<" refusal="<<(refuse?1:0)<<" submitted="<<state.amd_read.submitted
      <<" published="<<state.amd_read.published<<" verified_frames="<<state.amd_read.verified_frames
      <<" superseded="<<state.amd_read.superseded<<" failures="<<state.amd_read.failures
      <<" worker_exit="<<state.amd_read.last_worker_exit_code<<" budget_queries="<<state.amd_read.budget_queries
      <<" exact_cpu_fallback=1 child_and_owner_retired=1 stopped_pending="<<(refuse?0:1)<<" checks="<<checks<<'\n';
  return 0;
}
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
int live_installed_native_read(bool missing) {
  const auto qualified=installed_native_read_profile();
  require(qualified && qualified->minimum_frames==2 && qualified->maximum_frames==2 && store_lz4_available(),
      "The installed native-read fixture requires a two-frame AMD package");
  constexpr std::uint64_t extent=kNativeExtentBytes,capacity=8ULL*1024*1024;
  constexpr std::size_t window=2*extent;
  auto domain=MemoryVolumeDomain::create(capacity+kVolumeHeadBytes);
  auto head=domain.create_store(kVolumeHeadBytes);
  auto image=domain.create_store(capacity);
  NativeMaintenancePolicy policy;
  policy.maximum_cpu_busy_ppm=500'000;
  auto source=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),{capacity},
      {StoreCompressionMode::fast,1,false},{},policy);
  auto* adapter=source.get();
  Provider provider;
  Provider::active=&provider;
  WinFspVolumeController owner(std::move(source),provider.api(),plan(),domain);
  await([&]{return owner.view().state==SessionState::running;},5000,"Installed read controller startup");
  auto bytes=data(4*window,1937);
  for(std::size_t i=0;i<2;++i) {
    const auto motif=data(extent/8,917+static_cast<unsigned>(i));
    for(std::size_t pos=0;pos<extent;pos+=motif.size())
      std::ranges::copy(motif,bytes.begin()+static_cast<std::ptrdiff_t>(3*window+i*extent+pos));
  }
  callback_file(provider,0,bytes,window);
  try {
    await([&]{return owner.view().payload_scrub_passes && !adapter->background_maintenance_pending();},
        18000,"Installed read initial maintenance did not quiesce");
  }catch(...) {
    const auto setup=owner.view();
    std::cerr<<"installed_read_setup quanta="<<setup.maintenance_quanta<<" scrub_passes="<<setup.payload_scrub_passes
        <<" phase="<<static_cast<int>(setup.maintenance.phase)<<" deferral="<<static_cast<int>(setup.maintenance.deferral)
        <<" unavailable="<<setup.maintenance_disabled<<" host="<<static_cast<int>(setup.host)<<'\n';
    throw;
  }
  PVOID context=nullptr;
  FSP_FSCTL_FILE_INFO info{};
  wchar_t name[]=L"\\f0";
  auto* fs=&provider.fs;
  require(fs->Interface->Open(fs,name,FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_READ_DATA,&context,&info)==STATUS_SUCCESS &&
      context && info.FileSize==bytes.size(),"Installed read opened a different file");
  const auto demand=[&](unsigned index) {
    Bytes found(window);
    ULONG moved=0;
    require(fs->Interface->Read(fs,context,found.data(),index*window,static_cast<ULONG>(found.size()),&moved)==
        STATUS_SUCCESS && moved==found.size() &&
        std::equal(found.begin(),found.end(),bytes.begin()+static_cast<std::ptrdiff_t>(index*window)),
        "Installed read bytes differ");
  };
  const auto prepare=[&](DWORD gap) {
    demand(0);
    if(gap)Sleep(gap);
    demand(1);
    if(gap)Sleep(gap);
    const auto before=owner.view().read_ahead_prepared;
    demand(2);
    return before;
  };
  const auto cpu_turn=[&](DWORD gap) {
    const auto prior=prepare(gap);
    await([&]{return owner.view().read_ahead_prepared>prior;},12000,"Installed route did not prepare CPU frames");
    const auto hits=adapter->native_lz4_read_ahead_hits();
    demand(3);
    require(adapter->native_lz4_read_ahead_hits()==hits+2,"CPU preparation did not supply the two requested frames");
  };
  cpu_turn(0);
  require(owner.view().installed_read_attempts==0 && owner.view().installed_read_lead_refusals &&
      !owner.view().amd_read.submitted,"Unqualified read cadence touched the AMD package");
  constexpr DWORD paced_gap=1500;
  require(qualified->minimum_lead_ms<paced_gap,"The fixture gap does not cover the approved lead");
  const auto initial=prepare(paced_gap);
  try {
    await([&]{return owner.view().installed_read_status.has_value();},20000,"Installed read acquisition did not retire");
  }catch(...) {
    const auto state=owner.view();
    std::cerr<<"installed_read_admission attempts="<<state.installed_read_attempts
        <<" shape_refusals="<<state.installed_read_shape_refusals<<" lead_refusals="<<state.installed_read_lead_refusals
        <<" resource_refusals="<<state.installed_read_resource_refusals
        <<" cpu_hits="<<adapter->native_lz4_read_ahead_hits()<<'\n';
    throw;
  }
  const auto acquired=owner.view();
  const auto expected=missing?HipDiscoveryStatus::package_unavailable:HipDiscoveryStatus::ready;
  if(acquired.installed_read_status!=expected)
    std::cerr<<"installed_read_identity status="<<static_cast<int>(*acquired.installed_read_status)
        <<" queries="<<acquired.installed_read_queries<<" exit="<<acquired.installed_read_worker_exit<<'\n';
  require(acquired.installed_read_attempts==1 && acquired.installed_read_status==expected,
      "Installed read package selection was unexpected");
  require(missing?acquired.installed_read_queries==0:acquired.installed_read_queries>0,
      "Incorrect installed identity query count");
  bool learned_cpu=false;
  await([&]{return owner.view().read_ahead_prepared>initial;},12000,"Acquisition did not preserve CPU read-ahead");
  demand(3);
  if(missing) {
    Sleep(qualified->worker.timeout_ms);
    cpu_turn(paced_gap);
    require(owner.view().installed_read_attempts==1 && !owner.view().amd_read.submitted,
        "Unchanged missing package was retried after its evidence interval");
  }else {
    cpu_turn(0);
    require(owner.view().amd_read.lead_refusals && !owner.view().amd_read.budget_queries && !owner.view().amd_read.submitted,
        "A short read interval reached the installed GPU budget or worker");
    const auto hits=adapter->native_lz4_read_ahead_hits();
    prepare(paced_gap);
    await([&]{return owner.view().amd_read.published || owner.view().amd_read.failures;},18000,
        "Installed native decoder did not finish");
    require(owner.view().amd_read.published==1 && owner.view().amd_read.verified_frames==2 &&
        owner.view().amd_read.retirement_confirmed,"Installed route did not publish verified GPU frames");
    demand(3);
    require(adapter->native_lz4_read_ahead_hits()==hits+2,"Callbacks did not consume the installed AMD result");
    const auto completed=owner.view().amd_read;
    if(completed.last_turnaround_ms>2*std::uint64_t{paced_gap}) {
      cpu_turn(paced_gap);
      const auto observed=owner.view().amd_read;
      require(observed.submitted==completed.submitted && observed.budget_queries==completed.budget_queries &&
          observed.lead_refusals>completed.lead_refusals,"The measured turnaround did not keep a later read on CPU");
      learned_cpu=true;
    }
  }
  callback_cleanup_close(provider,context);
  owner.stop_and_join();
  const auto last=owner.view();
  require(last.retired && last.amd_read.retirement_confirmed && !last.controller_failed &&
      provider.fs.UserContext==nullptr,"Installed read controller did not retire");
  std::cout<<"installed_native_read missing="<<missing<<" acquisitions="<<last.installed_read_attempts
      <<" identity_queries="<<last.installed_read_queries<<" gpu_submitted="<<last.amd_read.submitted
      <<" gpu_verified="<<last.amd_read.verified_frames<<" lead_refusals="<<last.amd_read.lead_refusals
      <<" worker_ms="<<last.amd_read.last_worker_elapsed_ms<<" turnaround_ms="<<last.amd_read.last_turnaround_ms
      <<" learned_cpu="<<learned_cpu<<" byte_exact=1 retired=1\n";
  return 0;
}
#endif
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
void dense_file_retirement_with_due_scrub() {
  using Progress=NativeVolumeReclaimProgress;
  constexpr PagedStoreLimits limits{8ULL*1024*1024};
  constexpr auto physical_capacity=2*limits.maximum_physical_bytes+2*1024*1024;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto volume=id(90);
  auto backing=std::make_shared<DenseTailRam>();
  backing->bytes.failure_unit=4096;
  backing->bytes.capacity=physical_capacity;
  auto expected=data(kNativeExtentBytes,920);
  {
    Provider provider;Provider::active=&provider;
    WinFspNativeSession writer(WinFspNativeWritable::create_file_volume(
        backing,volume,id(1),limits,raw,physical_capacity),provider.api());
    require(writer.start(plan())==STATUS_SUCCESS,"dense retirement producer did not start");
    callback_file(provider,0,expected);
    for(unsigned i=1;i<8;++i) {
      wchar_t name[]=L"\\f0";
      constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
      PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
      require(provider.fs.Interface->Open(&provider.fs,name,open_file,FILE_WRITE_DATA,&context,&info)==STATUS_SUCCESS &&
          context,"dense retirement producer could not open its file");
      expected=data(expected.size(),920+i);
      ULONG count=0;
      require(provider.fs.Interface->Write(&provider.fs,context,expected.data(),0,static_cast<ULONG>(expected.size()),
          FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==expected.size(),"dense retirement producer could not write");
      callback_cleanup_close(provider,context);
    }
    writer.stop();
    require(writer.write_restart_ready(),"dense retirement producer did not retire");
  }
  {
    auto writer=NativeVolumeFile::resume(backing,volume,raw,NativeDispositionPolicy::windows_global);
    for(unsigned step=0;writer.persistent_reclamation_pending();++step) {
      require(step<128,"dense preparation did not retire its old image");
      const auto result=writer.advance_persistent_reclamation([]{return id(2);});
      require(result==Progress::copying || result==Progress::ready || result==Progress::published ||
          result==Progress::retiring,"dense preparation failed");
    }
    require(writer.volume_info().selection.epoch==2 && writer.persistent_reclamation_pending(true),
        "dense preparation did not leave physical tail work");
  }
  const auto before=backing->extent();
  Provider provider;Provider::active=&provider;
  WinFspNativeSession session(WinFspNativeWritable::resume_file_volume(backing,volume,raw),provider.api());
  require(session.start(plan())==STATUS_SUCCESS,"dense retirement fixture did not start");
  NativeMaintenanceObservation observed{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
  const auto advance=[&] {
    ++observed.sequence;observed.captured_ms+=1000;
    return session.advance_background_maintenance(observed,observed.captured_ms,{},WinFspMaintenanceContinuation::none,true);
  };
  const auto writes=backing->bytes.calls;
  require(advance().progress.deferral==NativeMaintenanceDeferral::foreground && backing->bytes.calls==writes,
      "dense retirement wrote under foreground demand");
  observed.foreground_pending=false;
  for(unsigned step=0;step<24 && backing->extent()>=before;++step)(void)advance();
  require(backing->extent()<before,"a due scrub blocked release of retired dense backing");
  std::optional<PagedPayloadCheckpointObservation> verified;
  std::uint64_t checked_objects=0;
  for(unsigned step=0;step<32 && !verified;++step) {
    const auto scan=advance().payload_scrub;
    checked_objects+=scan.examined_objects;
    verified=scan.verified_checkpoint;
  }
  require(verified && checked_objects,"scrub did not verify the released backing's checkpoint");
  session.stop();
  require(session.write_restart_ready(),"dense retirement owner did not retire");
  const auto reopened=NativeVolumeFile::inspect(DenseTailRam::replay(*backing),volume);
  require(reopened.info().selection.epoch==2 && verified->image==reopened.info().selection.image_identity &&
      verified->generation==reopened.filesystem().storage_info().generation &&
      reopened.filesystem().read("f0",0,kNativeExtentBytes)==expected,"dense release changed selected bytes");
  std::cout<<"dense_release_scrub observed_objects="<<checked_objects<<" exact_generation=1 independent_bytes=1\n";
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
      if(!step.examined_objects)continue;
      require(step.state==Scrub::checked || step.state==Scrub::complete,"scrub fixture found corruption in a clean volume");
      if(reopen) {
        require(step.first_examined_object_id>saved.position.after &&
            step.first_examined_object_id<=saved.position.through && !step.verified_checkpoint,
            "SDK restart discarded its advisory cursor or certified a skipped prefix");
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
    constexpr std::uint64_t stamp=133700000000000011ULL;
    const auto security=native_security_fixture::descriptor();
    const NativeVolumeRootMetadata root{{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY},security};
    auto session=NativeVolumeFile::create(backing,volume,id(1),limits,raw,
        NativeDispositionPolicy::windows_global,root);
    session.create_entry("portable",false,{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_ARCHIVE});
    const auto file=session.open("portable",{true,true,true});
    session.write(file,0,bytes);
    session.reserve_result(file,kNativeAllocationUnit);
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
  catch(const std::runtime_error& error){refused=std::string_view(error.what())=="missing restart descriptor";}
  require(refused && backing->writes==writes && backing->flushes==barriers,
      "invalid Windows metadata allowed repair before refusal");
  const auto verified=NativeVolumeFile::inspect(backing,volume);
  require(verified.filesystem().storage_info().degraded_checkpoint &&
      verified.filesystem().read("portable",0,8192)==bytes,"platform refusal changed the independently readable source");
  std::cout<<"paired_reopen_windows_admission before_repair=1 surviving_bytes=1\n";
}

std::pair<std::uint64_t,std::uint64_t> indexed_windows_reopen_profile() {
  using detail::IndexedExtentInput;
  using detail::IndexedInodeInput;
  using detail::IndexedVolumeBuilder;
  constexpr unsigned files=4096,batch=32;
  constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  constexpr std::uint64_t stamp=133700000000000011ULL;
  const auto volume=id(90),image_id=id(1);
  const auto security=native_security_fixture::descriptor();
  const auto name=[](unsigned index){return "f"+std::to_string(100000+index);};
  const auto payload=[](unsigned index) {
    auto bytes=data(71,1+index);
    bytes.back()=std::byte{0x55};
    return bytes;
  };
  auto head=std::make_shared<Ram>(),body=std::make_shared<Ram>();
  head->failure_unit=body->failure_unit=4096;
  {
    IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(body,image_id,limits,raw),
        1,files+3,{true,files*71},2);
    const NativeBasicMetadata basic{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY};
    const NativeBasicMetadata file_basic{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_ARCHIVE};
    const std::array<IndexedInodeInput,2> directories{{
        {1,0,0,1,1,true,"",basic,security},
        {2,1,0,1,1,true,"Data",basic,security}}};
    builder.append(directories);
    for(unsigned start=0;start<files;start+=batch) {
      std::vector<IndexedInodeInput> inodes;
      std::vector<IndexedExtentInput> extents;
      std::vector<Bytes> contents;
      inodes.reserve(batch);
      extents.reserve(batch);
      contents.reserve(batch);
      for(unsigned i=start;i<start+batch;++i) {
        inodes.push_back({i+3,2,71,1,1,false,name(i),file_basic,security,kNativeAllocationUnit});
        contents.push_back(payload(i));
        extents.push_back({i+3,0,contents.back()});
      }
      builder.append(inodes,extents);
    }
    const auto snapshot=builder.finish();
    const auto selected=NativeVolumeHeadWriter::create(head,volume,snapshot);
    require(selected.info().selection.epoch==1,"profile must use the initial selector");
  }
  const auto resolve=[&](const auto& expected)->std::shared_ptr<StoreByteIo> {
    return expected==image_id?body:nullptr;
  };
  const auto prior=body->read_bytes,read_calls=body->read_calls;
  const auto writes=body->write_bytes+head->write_bytes,flushes=body->flushes,head_flushes=head->flushes;
  Provider provider;Provider::active=&provider;
  std::uint64_t pre_flush_reads=0,post_flush_reads=0;
  {
    WinFspNativeSession session(WinFspNativeWritable::resume_indexed_retired(head,volume,1,resolve,raw),provider.api());
    pre_flush_reads=body->read_bytes_at_flush-prior;
    post_flush_reads=body->read_bytes-body->read_bytes_at_flush;
    std::cout<<"indexed_windows_reopen files="<<files<<" image_extent_bytes="<<body->extent()
        <<" pre_image_flush_read_bytes="<<pre_flush_reads
        <<" post_image_flush_read_bytes="<<post_flush_reads
        <<" image_read_calls="<<body->read_calls-read_calls<<" image_flushes="<<body->flushes-flushes
        <<" head_flushes="<<head->flushes-head_flushes<<" backing_write_bytes="<<body->write_bytes+head->write_bytes-writes<<'\n';
    require(body->write_bytes+head->write_bytes==writes && body->flushes==flushes+1 && head->flushes==head_flushes+1,
        "healthy Windows reopen changed backing bytes or repeated a barrier");
    require(session.start(plan())==STATUS_SUCCESS,"reopened Windows callback provider start");
    auto* fs=&provider.fs;
    constexpr unsigned samples=64;
    std::uint64_t query_bytes=0,query_calls=0;
    for(unsigned i=0;i<files;i+=files/samples) {
      const auto filename=name(i);
      auto query_path=L"\\Data\\"+std::wstring(filename.begin(),filename.end());
      PVOID context=nullptr;FSP_FSCTL_FILE_INFO opened{},metadata{};
      require(fs->Interface->Open(fs,query_path.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_READ_DATA,
          &context,&opened)==STATUS_SUCCESS && context,"metadata profile file open");
      const auto bytes=body->read_bytes,calls=body->read_calls;
      const auto before_write=body->write_bytes+head->write_bytes;
      require(fs->Interface->GetFileInfo(fs,context,&metadata)==STATUS_SUCCESS &&
          metadata.IndexNumber==i+3 && metadata.FileSize==71 && metadata.AllocationSize==kNativeAllocationUnit &&
          metadata.FileAttributes==FILE_ATTRIBUTE_ARCHIVE && metadata.CreationTime==stamp &&
          metadata.LastAccessTime==stamp && metadata.LastWriteTime==stamp && metadata.ChangeTime==stamp,
          "indexed GetFileInfo changed verified metadata");
      query_bytes+=body->read_bytes-bytes;query_calls+=body->read_calls-calls;
      require(body->write_bytes+head->write_bytes==before_write,"file metadata query wrote backing bytes");
      callback_cleanup_close(provider,context);
    }
    std::cout<<"indexed_windows_get_file_info files="<<files<<" queries="<<samples
        <<" backing_read_bytes="<<query_bytes<<" backing_read_calls="<<query_calls<<" backing_write_bytes=0\n";
    const auto last=name(files-1);
    auto path=L"\\Data\\"+std::wstring(last.begin(),last.end());
    FSP_FSCTL_FILE_INFO found{};PVOID handle=nullptr;
    require(fs->Interface->Open(fs,path.data(),FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_READ_DATA,&handle,&found)==STATUS_SUCCESS &&
        handle && found.IndexNumber==files+2 && found.FileSize==71,"reopened Windows file metadata");
    Bytes output(71);ULONG count=0;
    require(fs->Interface->Read(fs,handle,output.data(),0,static_cast<ULONG>(output.size()),&count)==STATUS_SUCCESS &&
        count==output.size() && output==payload(files-1),"reopened Windows file bytes");
    callback_cleanup_close(provider,handle);
    callback_file(provider,0,data(83,718));
    session.stop();
    require(session.write_restart_ready() && provider.fs.UserContext==nullptr,"profile left the provider active");
  }
  const auto cpu=NativeVolumeHeadView::open(head,volume,resolve);
  for(unsigned i=0;i<files;++i)
    require(cpu.filesystem().read("Data/"+name(i),0,71)==payload(i),"CPU reopen lost an original file");
  require(cpu.filesystem().read("f0",0,83)==data(83,718),"CPU reopen lost an acknowledged Windows write");
  std::cout<<"indexed_windows_reopen cpu_files="<<files+1<<" callback_read=1 subsequent_write=1 retired=1\n";
  return {pre_flush_reads,post_flush_reads};
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
  constexpr PagedStoreLimits limits{16ULL*1024*1024};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  Provider provider;Provider::active=&provider;
  auto file=std::make_shared<SparseVolumeRam>(4096);
  auto adapter=WinFspNativeWritable::create_file_volume(file,id(90),id(1),limits,raw);
  std::unique_ptr<void,decltype(&CloseHandle)> activity(CreateEventW(nullptr,FALSE,FALSE,nullptr),&CloseHandle);
  require(activity!=nullptr,"paired-file activity event");
  adapter->set_maintenance_event(activity.get());
  auto* source=adapter.get();
  WinFspNativeSession session(std::move(adapter),provider.api());
  require(session.start(plan())==STATUS_SUCCESS,"paired-file native SDK start");
  auto expected=data(15731,901);
  callback_file(provider,0,expected);
  for(unsigned i=0;i<160;++i) {
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
  const auto step=[&](bool continued,std::uint64_t age_ms=0,Continuation route=Continuation::compaction) {
    ++observed.sequence;
    observed.continued=continued;
    if(!continued)observed.captured_ms+=1000;
    return session.advance_background_maintenance(observed,observed.captured_ms+age_ms,{},
        continued?route:Continuation::none);
  };
  const auto refuse=[&](Deferral reason,std::uint64_t age_ms=0) {
    const auto writes=file->io.calls;
    require(step(true,age_ms).progress.deferral==reason && file->io.calls==writes,
        "paired-file continuation performed deferred backing work");
  };
  const auto open_context=[&](UINT32 options,UINT32 access) {
    wchar_t name[]=L"\\f0";
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO metadata{};
    require(provider.fs.Interface->Open(&provider.fs,name,options,access,&context,&metadata)==STATUS_SUCCESS &&
        context,"paired-file maintenance fixture could not open a context");
    return context;
  };
  constexpr auto ordinary_open=FILE_NON_DIRECTORY_FILE|(1U<<24);
  const auto before=file->occupied();
  unsigned private_steps=0,publish_boundaries=0;
  {
    std::optional<NativeFilesystemSnapshot> retained(source->snapshot());
    std::optional<NativeFilesystemSnapshot> second(source->snapshot());
    const auto writes=file->io.calls;
    require(step(false).progress.deferral==Deferral::foreground && file->io.calls==writes,
        "paired-file compaction ran under foreground load");
    observed.foreground_pending=false;
    refuse(Deferral::observation); // A reused sample cannot start a new image.

    const auto defer_provisional=[&](UINT32 options,UINT32 access) {
      const auto context=open_context(options,access);
      const auto writes=file->io.calls;
      const auto result=step(false);
      require(result.continuation!=Continuation::compaction && file->io.calls==writes,
          "paired-file maintenance crossed an unresolved disposition");
      provider.fs.Interface->Close(&provider.fs,context);
      require(!source->requires_offline() && file->io.calls==writes,
          "abandoned provisional context performed a cleanup mutation");
    };
    defer_provisional(ordinary_open|FILE_DELETE_ON_CLOSE,DELETE);
    defer_provisional(FILE_NON_DIRECTORY_FILE|(4U<<24),FILE_WRITE_DATA);

    const auto reader=open_context(ordinary_open,FILE_READ_DATA);
    const auto abandoned_open=open_context(ordinary_open,FILE_READ_DATA);
    auto progress=step(false);
    require(progress.continuation==Continuation::compaction,
        "untouched ordinary opens blocked paired-file preparation");
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
        retained->read("f0",0,50000)==expected,"paired-file publication boundaries or pinned source changed");
    Bytes readback(expected.size());
    ULONG read=0;
    require(provider.fs.Interface->Read(&provider.fs,reader,readback.data(),0,
        static_cast<ULONG>(readback.size()),&read)==STATUS_SUCCESS && read==readback.size() && readback==expected,
        "untouched WinFsp reader changed at its first read after image publication");
    callback_cleanup_close(provider,reader);
    const auto cancel_writes=file->io.calls;
    provider.fs.Interface->Close(&provider.fs,abandoned_open);
    require(!source->requires_offline() && file->io.calls==cancel_writes,
        "unadopted open could not retire without writes after image publication");
    const auto previous=expected;
    expected[0]^=std::byte{0x42};
    wchar_t name[]=L"\\f0";
    constexpr auto open_file=FILE_NON_DIRECTORY_FILE|(1U<<24);
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO metadata{};
    require(provider.fs.Interface->Open(&provider.fs,name,open_file,FILE_WRITE_DATA,&context,&metadata)==STATUS_SUCCESS &&
        context,"new selected range could not open while old readers remained active");
    ULONG written=0;
    require(provider.fs.Interface->Write(&provider.fs,context,expected.data(),0,1,FALSE,FALSE,&written,&metadata)==STATUS_SUCCESS &&
        written==1,"new selected range could not acknowledge a write while old readers remained active");
    callback_cleanup_close(provider,context);
    require(source->snapshot().read("f0",0,50000)==expected,"selected range lost its foreground edit");
    require(WaitForSingleObject(activity.get(),0)==WAIT_OBJECT_0,"foreground activity was not signaled");
    const auto changes=file->io.calls;
    for(unsigned tick=0;tick<32 && session.background_maintenance_pending();++tick)(void)step(false);
    require(!session.background_maintenance_pending() && file->io.calls==changes &&
        second->read("f0",0,50000)==previous,"pinned range prevented the owner from settling available work");
    const auto reads=file->io.read_calls;
    for(unsigned tick=0;tick<64;++tick)
      require(!session.background_maintenance_pending(),"an unchanged old pin repeatedly scheduled retirement");
    require(file->io.read_calls==reads,"checking a pinned range performed backing reads");
    retained.reset();
    require(WaitForSingleObject(activity.get(),0)==WAIT_TIMEOUT && !session.background_maintenance_pending(),
        "dropping one reader signaled retirement while another reader still pinned the range");
    std::thread release([snapshot=std::exchange(second,std::nullopt)]() mutable {snapshot.reset();});
    release.join();
    require(WaitForSingleObject(activity.get(),0)==WAIT_OBJECT_0 &&
        WaitForSingleObject(activity.get(),0)==WAIT_TIMEOUT && session.background_maintenance_pending() &&
        file->io.calls==changes && !file->range_reclaims,"the last reader did not wake retirement exactly once");
  }
  refuse(Deferral::observation); // An unpinned range still needs its first fresh capture.
  require(step(true,0,Continuation::range_retirement).progress.deferral==Deferral::observation &&
      !file->range_reclaims,"an unauthorized continuation reclaimed the old range");
  auto retirement=step(false);
  require(retirement.continuation==Continuation::range_retirement && file->range_reclaims==1,
      "a checked range release did not admit its next chunk");
  const auto reclaims=file->range_reclaims;
  retirement=step(true,0,Continuation::range_retirement);
  require(!retirement.compaction_failed && retirement.progress.deferral==Deferral::none && file->range_reclaims>reclaims,
      "continued release did not retire another chunk");
  if(retirement.continuation==Continuation::range_retirement) {
    const auto before_foreground=file->io.calls;
    observed.foreground_pending=true;
    require(step(true,0,Continuation::range_retirement).progress.deferral==Deferral::foreground &&
        file->io.calls==before_foreground,"range continuation performed work under foreground demand");
    observed.foreground_pending=false;
  }
  for(unsigned tick=0;tick<8 && file->allocated_extent(kNativeVolumeFileImageOffset,limits.maximum_physical_bytes);++tick)
    (void)step(false);
  const auto settled_calls=file->io.calls;
  require(step(true,0,Continuation::range_retirement).progress.deferral==Deferral::observation &&
      file->io.calls==settled_calls,"range continuation escaped its finished retirement");
  require(file->range_reclaims && file->occupied()<before,"the native idle owner did not reclaim the old range");
  session.stop();
  require(session.write_restart_ready(),"paired-file dispatcher did not retire");
  require(WaitForSingleObject(activity.get(),0)!=WAIT_FAILED,"cannot clear the prior dispatcher activity");
  require(NativeVolumeFile::inspect(file->replay()).filesystem().read("f0",0,50000)==expected,
      "reclaimed SDK volume lost acknowledged bytes on independent CPU reopen");
  auto reopened=WinFspNativeWritable::resume_file_volume(file->replay(),id(90),raw);
  reopened->set_maintenance_event(activity.get());
  std::optional<NativeFilesystemSnapshot> escaped(reopened->snapshot());
  reopened.reset();
  require(escaped->read("f0",0,50000)==expected,"snapshot lost its backing at owner shutdown");
  require(WaitForSingleObject(activity.get(),0)==WAIT_TIMEOUT,"unexpected activity after owner shutdown");
  escaped.reset();
  require(WaitForSingleObject(activity.get(),0)==WAIT_TIMEOUT,"retired owner was signaled by an escaped reader");
  std::cout<<"paired_file_sdk_idle private_steps="<<private_steps<<" fresh_publish_boundaries="<<publish_boundaries
      <<" published=1 prior_readers_preserved=1 reclaimed=1 cpu_reopen=1"
      <<" pinned_wait_quiescent=1 foreground_write_while_pinned=1 last_reader_woke=1"
      <<" release_after_owner_quiet=1 continued_range_release=1\n";
}
enum class ColdFixtureMode { basic, public_corpus, shared_budget };
void volume_file_screened_cold(std::span<const Bytes> fixture,StoreCompressionOptions foreground,
    ColdFixtureMode mode=ColdFixtureMode::basic) {
  const bool profile=mode!=ColdFixtureMode::basic;
  std::uint64_t input_bytes=0;
  for(const auto& file:fixture)input_bytes+=file.size();
  const auto aligned=(input_bytes+kStoreCommitUnit-1)/kStoreCommitUnit*kStoreCommitUnit;
  const PagedStoreLimits limits{8ULL*1024*1024+(profile?2*aligned:0)};
  Provider provider;Provider::active=&provider;
  const auto capacity=profile?kNativeVolumeFileImageOffset+3*limits.maximum_physical_bytes:kStoreMaxPhysicalBytes;
  auto file=std::make_shared<SparseVolumeRam>(4096,capacity);
  auto adapter=WinFspNativeWritable::create_file_volume(file,id(90),id(1),limits,foreground);
  auto* volume=adapter.get();
  WinFspNativeSession session(std::move(adapter),provider.api());
  require(session.start(plan())==STATUS_SUCCESS,"screened compaction SDK start");
  unsigned number=0;
  for(const auto& bytes:fixture)callback_file(provider,number++,bytes,profile?1024*1024:kNativeMaxReadBytes);
  const auto original=volume->snapshot().storage_info();
  auto ordinary=original;
  if(profile) {
    auto control=std::make_shared<Ram>();
    control->failure_unit=4096;
    control->capacity=limits.maximum_physical_bytes;
    // This new fixture has not started maintenance. Its first image starts
    // immediately after the selector and advice ranges. Give the synchronous
    // compactor an independent backing with its own available writer lease.
    constexpr auto start=kNativeVolumeFileImageOffset;
    require(file->io.stable.size()>=start && original.physical_end<=file->io.stable.size()-start,
        "cold profile source image bounds");
    const auto bytes=std::span(file->io.stable).subspan(start,original.physical_end);
    auto source_image=std::make_shared<Ram>();
    source_image->failure_unit=4096;
    source_image->capacity=limits.maximum_physical_bytes;
    source_image->live.assign(bytes.begin(),bytes.end());
    const auto source=NativeFilesystemSnapshot::open_image(source_image,
        {NativeImageKind::indexed_paged,limits.maximum_physical_bytes});
    auto writer=NativeFilesystemWriter::compact(source,control,id(61),foreground);
    ordinary=writer.snapshot().storage_info();
  }
  const auto occupied_before=file->occupied();
  auto peak_occupied=occupied_before;
  NativeMaintenanceObservation observed{1,1000,16ULL<<30,12ULL<<30,32ULL<<30,24ULL<<30,1000,0,1000,true,true};
  const auto step=[&] {
    ++observed.sequence;observed.captured_ms+=1000;
    return session.advance_background_maintenance(observed,observed.captured_ms);
  };
  const auto reads=file->io.read_calls,writes=file->io.write_bytes;
  const auto read_bytes=file->io.read_bytes;
  require(step().progress.deferral==NativeMaintenanceDeferral::foreground &&
      file->io.read_calls==reads && file->io.write_bytes==writes,"idle work touched storage under foreground admission");
  observed.foreground_pending=false;
  WinFspMaintenanceResult progress;
  bool saw_budget_boundary=false,published=false;
  unsigned steps=0,compaction_quanta=0;
  std::uint64_t groups_published=0;
  const auto started=std::chrono::steady_clock::now();
  for(;steps<(profile?12'000U:1800U) && (profile || !published) && session.background_maintenance_pending();++steps) {
    progress=step();
    published=published || progress.compacted;
    if(progress.continuation==WinFspMaintenanceContinuation::compaction)++compaction_quanta;
    groups_published=(std::max)(groups_published,progress.progress.published_groups);
    peak_occupied=(std::max)(peak_occupied,file->occupied());
    require(!progress.compaction_failed && progress.progress.phase!=NativeMaintenancePhase::failed,
        "ordinary SDK cold work failed");
    if(!saw_budget_boundary && progress.progress.phase==NativeMaintenancePhase::preparing &&
        progress.continuation==WinFspMaintenanceContinuation::none) {
      const auto saved_writes=file->io.write_bytes;
      ++observed.sequence;observed.continued=true;
      const auto replay=session.advance_background_maintenance(observed,observed.captured_ms,{},
          WinFspMaintenanceContinuation::compaction);
      observed.continued=false;
      require(replay.progress.deferral==NativeMaintenanceDeferral::observation && file->io.write_bytes==saved_writes,
          "continued preparation spent the durable write allowance");
      saw_budget_boundary=true;
    }
  }
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
  const auto after=volume->snapshot().storage_info();
  if(profile) {
    require(!session.background_maintenance_pending(),"public cold profile did not settle");
    require(after.stored_payload_bytes<=after.physical_end && ordinary.stored_payload_bytes<=ordinary.physical_end,
        "invalid payload accounting");
    std::cout<<"cold_profile logical_bytes="<<input_bytes<<" files="<<fixture.size()
        <<" original_image="<<original.physical_end<<" retained_compacted_image="<<ordinary.physical_end
        <<" retained_payload_bytes="<<ordinary.stored_payload_bytes<<" retained_container_bytes="
        <<ordinary.physical_end-ordinary.stored_payload_bytes
        <<" selected_image="<<after.physical_end<<" selected_payload_bytes="<<after.stored_payload_bytes
        <<" selected_container_bytes="<<after.physical_end-after.stored_payload_bytes
        <<" fixed_metadata_range="<<kNativeVolumeFileImageOffset<<" occupied_before="<<occupied_before
        <<" occupied_peak="<<peak_occupied<<" occupied_after="<<file->occupied()
        <<" optimization_read_bytes="<<file->io.read_bytes-read_bytes<<" optimization_writes="<<file->io.write_bytes-writes
        <<" reclaim_model_writes="<<file->reclaimed_model_write_bytes
        <<" steps="<<steps<<" compaction_quanta="<<compaction_quanta<<" group_publications="<<groups_published
        <<" maintenance_ns="<<elapsed<<" compacted="<<published<<" fresh_budget_boundary="<<saw_budget_boundary<<'\n';
  } else {
    require(saw_budget_boundary && published && after.physical_end<original.physical_end,
        "ordinary SDK maintenance did not shrink the append-mostly image");
  }
  if(mode==ColdFixtureMode::shared_budget)
    require(published && saw_budget_boundary && after.stored_payload_bytes<ordinary.stored_payload_bytes,
        "advisory discovery consumed the cold rewrite budget");
  session.stop();
  auto reopened=NativeVolumeFile::inspect(file->replay(),id(90));
  const auto& snapshot=reopened.filesystem();
  snapshot.verify_all();
  require(session.write_restart_ready(),"screened SDK controller did not retire");
  number=0;
  for(const auto& bytes:fixture)
    require(snapshot.read("f"+std::to_string(number++),0,static_cast<std::uint32_t>(bytes.size()))==bytes,
        "screened SDK independent readback failed");
  std::cout<<"screened_cold_sdk compacted="<<published<<" foreground_yield=1 fresh_budget_boundary="
      <<saw_budget_boundary<<" exact_readback=1 old_image="<<original.physical_end<<" new_image="<<after.physical_end<<'\n';
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
  const bool prefix_extension=mode=="prefix-extension";
  const bool changing=mode=="changed-extents" || prefix_extension;
  const bool device_not_ready=mode=="device-not-ready";
  const bool missing_reference=mode=="reference-missing";
  const bool reference=mode=="reference-xor" || missing_reference;
  const bool two_planes=mode=="xor-wide" || mode=="xor-narrow";
  const bool xor_encoding=two_planes || mode=="xor" || mode=="xor-cpu";
  const bool packed=mode=="batch" || mode=="batch-yield" || xor_encoding;
  const unsigned files_per_group=prefix_extension?6:reference?anchor_search_fixture::indexed_small_extents_per_family:
      queued?queued_anchor_members+1:installed && (packed || restoring)?3:5;
  const unsigned groups=restoring?4:(packed || device_not_ready)?2:1;
  const unsigned seed_files=reference?2U:0U;
  const unsigned initial_files=reference?seed_files:prefix_extension?3:changing?2:restoring?2*files_per_group:groups*files_per_group;
  const bool yield=mode=="yield" || mode=="batch-yield" || mode=="queued-yield";
  const auto count=seed_files+groups*files_per_group;
  const auto before=handle_count();
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  if(changing)head->failure_unit=image->failure_unit=4096;
  Provider provider;Provider::active=&provider;
  NativeMaintenancePolicy qualification;
  qualification.maximum_cpu_busy_ppm=500000;
  const auto compression=prefix_extension?StoreCompressionOptions{StoreCompressionMode::automatic,3,true}:
      StoreCompressionOptions{StoreCompressionMode::raw,1,false};
  auto adapter=WinFspNativeWritable::create_managed_indexed(
      head,id(90),image,id(1),{kStoreMaxPhysicalBytes},compression,{},qualification);
  std::vector<Bytes> originals;
  originals.reserve(count);
  if(reference) {
    const auto seed=data(kNativeExtentBytes,61571);
    originals={seed,seed};
    originals.back()[4096]^=std::byte{0x15};
  }
  for(unsigned group=0;group<groups;++group) {
    const auto variant=restoring && group>=2?group-2:group;
    const auto seed=prefix_extension?3819:714+variant*5731+(restoring && group<2?99991:0);
    const auto base=data(kNativeExtentBytes,seed);
    for(unsigned i=0;i<files_per_group;++i) {
      auto derived=base;
      if(reference)anchor_search_fixture::apply_full_reference_variation(derived,base,i,files_per_group);
      else if(prefix_extension) {
        if(i)anchor_search_fixture::apply_prefix_revision(derived,base,i);
      } else if(i && xor_encoding) {
        const auto variation=data(kNativeExtentBytes,1511+group*103+i);
        const auto mask=two_planes?std::byte{0x30}:std::byte{1};
        for(std::size_t at=0;at<base.size();++at)derived[at]^=variation[at]&mask;
      } else if(i && ((restoring && group<2) || (changing && i<initial_files)))
        derived[kNativeExtentBytes/8+(i+1)*719]^=std::byte{0x67};
      else if(i)derived=anchor_search_fixture::fragmented(base,i);
      if(!reference && !two_planes)std::copy_n(base.begin(),kNativeExtentBytes/8,derived.begin());
      originals.push_back(std::move(derived));
    }
  }
  WinFspControllerView result;
  bool signalled=false,restored=false;
  std::array<unsigned,static_cast<std::size_t>(NativeMaintenanceDeferral::capacity)+1> deferrals{};
  std::uint64_t last_quantum=0,settled_ms=0,visits_before_edits=0,prefixes_before_edits=0;
  const auto visits=[](const auto& progress){return progress.sampled_extents+progress.reused_features;};
  {
    WinFspVolumeController controller(std::move(adapter),provider.api(),plan(),std::move(options));
    await([&]{return controller.view().state==SessionState::running;},5000,"AMD controller startup deadline");
    for(unsigned i=0;i<initial_files;++i)callback_file(provider,i,originals[i]);
    if(changing && !prefix_extension)
      for(unsigned i=initial_files;i<count;++i)callback_file(provider,i,data(1024,44000+i));
    if(read_after_verification) {
      require(!controller.view().anchor_search.submitted,"GPU launched before baseline reads");
      foreground_read_samples(provider,1,originals[1],"before_launch");
    }
    // Each group is staged and published across admitted idle observations.
    const auto started=GetTickCount64();
    const auto deadline=started+(queued?85000ULL:57000ULL)*(groups+static_cast<unsigned>(reference));
    do {
      result=controller.view();
      if(result.maintenance_quanta!=last_quantum) {
        last_quantum=result.maintenance_quanta;
        ++deferrals.at(static_cast<std::size_t>(result.maintenance.deferral));
      }
      const bool finished=reference?signalled && result.maintenance.published_groups>=2:
          changing?signalled && result.maintenance.published_groups>=2 &&
          result.maintenance.phase==NativeMaintenancePhase::idle:result.maintenance.published_groups>=groups;
      if(finished || result.retired || result.maintenance_disabled ||
          (mode=="stop" && result.installed_anchor_attempts))break;
      if(changing && !signalled && result.maintenance.published_groups==1 &&
          result.maintenance.phase==NativeMaintenancePhase::idle) {
        require(!result.anchor_search.submitted,"Seed group unexpectedly used the AMD worker");
        visits_before_edits=visits(result.maintenance);
        prefixes_before_edits=result.maintenance.prefixed_zstd_members;
        for(unsigned i=initial_files;i<count;++i) {
          if(prefix_extension)callback_file(provider,i,originals[i]);
          else replace_callback_file(provider,i,originals[i]);
        }
        signalled=true;
      }
      if(reference && !signalled && result.maintenance.published_groups==1 &&
          result.installed_anchor_status==(missing_reference?HipDiscoveryStatus::package_unavailable:HipDiscoveryStatus::ready)) {
        require(!result.anchor_search.submitted,"Seed group used the device reference route");
        for(unsigned i=initial_files;i<count;++i)callback_file(provider,i,originals[i]);
        signalled=true;
      }
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
  const auto accepted_files=restored || ((changing || reference) && signalled)?count:initial_files;
  for(unsigned i=0;i<accepted_files;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
      "AMD controller changed accepted file bytes");
  std::cout<<"anchor_owner mode="<<mode<<" host="<<static_cast<int>(result.host)
      <<" quanta="<<result.maintenance_quanta<<" groups="<<result.maintenance.published_groups
      <<" continued_steps="<<result.continued_preparation_steps<<" settled_ms="<<settled_ms
      <<" maximum_quantum_ns="<<result.maximum_quantum_ns
      <<" declined_groups="<<result.maintenance.declined_groups<<" phase="<<static_cast<int>(result.maintenance.phase)<<" sampled="<<result.maintenance.sampled_extents
      <<" attempted="<<result.maintenance.attempted_groups<<" cpu_limit_ppm="<<qualification.maximum_cpu_busy_ppm
      <<" submitted="<<result.anchor_search.submitted<<" verified="<<result.anchor_search.verified
      <<" submitted_queued_batches="<<result.anchor_search.submitted_queued_batches
      <<" verified_queued_batches="<<result.anchor_search.verified_queued_batches
      <<" dispatches_per_wait="<<result.anchor_search.queued_dispatches_per_wait
      <<" submitted_groups="<<result.anchor_search.submitted_groups<<" verified_groups="<<result.anchor_search.verified_groups
      <<" submitted_kernel_launches="<<result.anchor_search.submitted_kernel_launches
      <<" largest_dispatch_members="<<result.anchor_search.largest_dispatch_members
      <<" copy_suggested_ranges="<<result.anchor_search.copy_suggested_ranges
      <<" copy_compared_ranges="<<result.anchor_search.copy_compared_ranges
      <<" copy_suggested_bytes="<<result.anchor_search.copy_suggested_bytes
      <<" copy_compared_bytes="<<result.anchor_search.copy_compared_bytes
      <<" declined="<<result.anchor_search.declined<<" yielded="<<result.anchor_search.yielded
      <<" integrity_failures="<<result.anchor_search.integrity_failures
      <<" empty_xor_batches="<<result.anchor_search.empty_xor_batches
      <<" reference_batches="<<result.anchor_search.reference_batches
      <<" reselected_groups="<<result.anchor_search.reselected_groups
      <<" advised_members="<<result.maintenance.advised_members_published
      <<" installed_attempts="<<result.installed_anchor_attempts
      <<" metadata_queries="<<result.installed_anchor_queries
      <<" installed_status="<<(result.installed_anchor_status?static_cast<int>(*result.installed_anchor_status):-1)
      <<" handles_before="<<before<<" handles_after="<<handle_count()
      <<" retired=true cpu_readback_verified=true deferrals=";
  for(const auto occurrences:deferrals)std::cout<<occurrences<<',';
  std::cout<<'\n';
  if(reference) {
    require(signalled && result.maintenance.published_groups==2 && result.installed_anchor_attempts==1 &&
        !result.anchor_search.integrity_failures,"Installed reference cohort did not settle");
    if(missing_reference)
      require(result.installed_anchor_status==HipDiscoveryStatus::package_unavailable &&
          !result.installed_anchor_queries && !result.anchor_search.submitted && !result.maintenance.advised_members_published,
          "Missing reference package failed independent CPU continuation");
    else
      require(result.installed_anchor_status==HipDiscoveryStatus::ready && result.installed_anchor_queries &&
          result.anchor_search.submitted==1 && result.anchor_search.reference_batches==1 &&
          result.anchor_search.reselected_groups && result.maintenance.advised_members_published,
          "Installed owner did not use independently verified device reference advice");
    auto compacted=std::make_shared<Ram>();compacted->failure_unit=image->failure_unit;
    (void)NativeFilesystemWriter::compact(snapshot,compacted,id(3),{StoreCompressionMode::raw,1,false});
    const auto reopened=NativeFilesystemSnapshot::open_image(compacted,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    for(unsigned i=0;i<count;++i)
      require(reopened.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"Installed compacted reference changed data");
    std::cout<<"installed_reference mode="<<mode<<" files="<<count<<" compacted_bytes="<<compacted->extent()
        <<" exact_compacted_cpu_readback=true\n";
    return 0;
  }
  if(prefix_extension) {
    const auto info=snapshot.shared_base_info();
    const auto reused_prefixes=result.maintenance.prefixed_zstd_members-prefixes_before_edits;
    std::cout<<"anchor_prefix_extension groups="<<result.maintenance.published_groups
        <<" submissions="<<result.anchor_search.submitted<<" verified="<<result.anchor_search.verified
        <<" bases="<<info.bases<<" files="<<info.members<<" reused_prefixes="<<reused_prefixes
        <<" independent_cpu_readback=true"<<std::endl;
    require(signalled && result.anchor_search.verified>0 &&
        !result.anchor_search.integrity_failures && info.bases==1 && info.members==count && reused_prefixes>0,
        "Protected prefix reuse was not verified after the AMD exchange");
    return 0;
  }
  if(changing) {
    const auto info=snapshot.shared_base_info();
    const auto edit_visits=visits(result.maintenance)-visits_before_edits;
    std::cout<<"owner_changed_extents visits_before="<<visits_before_edits<<" edit_visits="<<edit_visits
        <<" bases="<<info.bases<<" members="<<info.members<<" independent_cpu_readback=true\n";
    require(signalled && result.anchor_search.submitted==1 && result.anchor_search.verified_groups==1 &&
        result.maintenance.published_groups==2 && edit_visits<=count-initial_files+1 &&
        info.bases==1 && info.members==count,"Live AMD incremental grouping or extent accounting differed");
    return 0;
  }
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
  if(mode=="collective-dense")return IndexedBatchPattern::collective_dense;
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
  std::uint64_t full_ns=0,sampled_ns=0;
};

template<class Read>
RevisionReadProfile profile_revision_reads(const Ram& backing,std::span<const Bytes> files,Read&& read,
    unsigned passes=1) {
  RevisionReadProfile result;
  auto read_start=backing.read_bytes;
  for(unsigned pass=0;pass<passes;++pass) {
    for(std::size_t i=0;i<files.size();++i) {
      const auto start=std::chrono::steady_clock::now();
      const auto bytes=read(i,0,static_cast<std::uint32_t>(files[i].size()));
      result.full_ns+=elapsed_ns(start);
      require(bytes==files[i],"Whole revision read differs");
      result.full_logical+=files[i].size();
    }
  }
  result.full_backing=backing.read_bytes-read_start;
  read_start=backing.read_bytes;
  for(unsigned pass=0;pass<passes;++pass) {
    for(std::size_t i=0;i<files.size();++i) {
      const auto count=(std::min)(files[i].size(),std::size_t{4096});
      const auto last=files[i].size()-count;
      for(const auto offset:{std::size_t{0},last/2,last}) {
        const auto start=std::chrono::steady_clock::now();
        const auto bytes=read(i,offset,static_cast<std::uint32_t>(count));
        result.sampled_ns+=elapsed_ns(start);
        require(std::ranges::equal(bytes,std::span(files[i]).subspan(offset,count)),
            "Sampled revision read differs");
        result.sampled_logical+=count;
      }
    }
  }
  result.sampled_backing=backing.read_bytes-read_start;
  return result;
}

RevisionReadProfile profile_revision_reads(const NativeFilesystemSnapshot& snapshot,
    const Ram& backing,std::span<const Bytes> files) {
  return profile_revision_reads(backing,files,[&](std::size_t i,std::uint64_t offset,std::uint32_t count) {
    return snapshot.read("f"+std::to_string(i),offset,count);
  });
}
RevisionReadProfile profile_revision_session_reads(NativeFilesystemWriter writer,
    const Ram& backing,std::span<const Bytes> files,unsigned passes=1) {
  auto session=NativeFileSession::create(std::move(writer));
  std::vector<NativeFileHandle> handles;
  handles.reserve(files.size());
  for(std::size_t i=0;i<files.size();++i)
    handles.push_back(session.open("f"+std::to_string(i),{true,false,false}));
  const auto profile=profile_revision_reads(backing,files,[&](std::size_t i,std::uint64_t offset,std::uint32_t count) {
    return session.read(handles[i],offset,count);
  },passes);
  for(const auto& handle:handles) {
    session.cleanup(handle,false);
    session.close(handle);
  }
  return profile;
}

void restrict_fixture_anchor_launch(std::span<const NativeSharedBaseSearchWindow> windows,
    NativeAnchorSearchOptions& options) {
  std::vector<std::uint32_t> members;
  for (const auto& window : windows) members.push_back(static_cast<std::uint32_t>(window.members().size()));
  const auto& identity = options.route.memory;
  const auto observed = query_windows_gpu_budget(identity.luid, identity.node_mask);
  require(windows.size() > 1 && observed.status == WindowsGpuBudgetStatus::success,
      "Limited-memory fixture requires live headroom and several complete groups");
  const auto& snapshot = observed.snapshot;
  const auto available = (std::min)(snapshot.physical_available, snapshot.commit_available);
  const auto original = options.worker.memory_share_divisor;
  constexpr std::uint64_t mib = 1024ULL * 1024;
  const auto upper = static_cast<std::uint32_t>((std::min)(available / (options.worker.memory_mib * mib),
      std::uint64_t((std::numeric_limits<std::uint32_t>::max)())));
  const auto count_for = [&](std::uint32_t divisor) {
    options.worker.memory_share_divisor = divisor;
    const auto planned = plan_native_anchor_launch(members, options, snapshot, snapshot.captured_ms, false);
    return planned ? planned->batch.groups.size() : std::size_t{0};
  };
  require(upper > original && count_for(original) == windows.size(),
      "Live headroom cannot fund the unrestricted fixture");
  const auto edge_for = [&](std::size_t groups) {
    auto low = original, high = upper;
    while (low < high) {
      const auto mid = low + (high - low + 1) / 2;
      if (count_for(mid) >= groups) low = mid;
      else high = mid - 1;
    }
    return low;
  };
  const auto full = edge_for(windows.size());
  const auto partial = edge_for(1);
  require(partial > full, "Live resource grant has no usable partial range");
  const auto divisor = full + (partial - full + 1) / 2;
  const auto expected = count_for(divisor);
  require(expected && expected < windows.size(), "Fixture budget did not select a strict prefix");
  std::cout << "anchor limited-memory fixture staged=" << windows.size() << " planned=" << expected
      << " divisor=" << divisor << '\n';
}

enum class BatchOverlapTrial {normal,partial,full_wait,full_advance};
enum class BatchForegroundTrial {normal,cancel_active,read_after_retirement,edit_after_retirement,read_or_cpu,recover_after_deadline};

int live_multiwindow_anchor(std::optional<NativeAnchorSearchOptions> options,
    const std::filesystem::path& bank_path,std::uint64_t discovery_ns,IndexedBatchPattern pattern,
    bool limited_memory=false,BatchOverlapTrial overlap=BatchOverlapTrial::normal,
    BatchForegroundTrial foreground=BatchForegroundTrial::normal,bool adaptive=false) {
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  const bool revision=revision_corpus(pattern);
  const bool require_phase_profile=options && options->profile_copy_batches;
  const bool shifted_bias=pattern==IndexedBatchPattern::shifted_bias;
  const bool tile_reference=pattern==IndexedBatchPattern::xor_tile_reference;
  const bool sparse_reference=pattern==IndexedBatchPattern::xor_sparse_reference;
  const bool full_reference=pattern==IndexedBatchPattern::xor_full_reference;
  const bool all_planes=pattern==IndexedBatchPattern::sparse_xor_all_planes;
  const bool sparse_xor=pattern==IndexedBatchPattern::sparse_xor || all_planes || sparse_reference || full_reference;
  const bool dense_collective=pattern==IndexedBatchPattern::collective_dense;
  const bool collective=pattern==IndexedBatchPattern::collective || dense_collective;
  const bool sparse_device=options && options->output==anchor_match::BatchOutput::sparse_xor;
  const bool collective_device=options && options->output==anchor_match::BatchOutput::collective;
  const bool reference_device=options &&
      (options->output==anchor_match::BatchOutput::reference_xor || options->prefer_reference);
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
  auto originals=revision?read_revision_corpus(bank_path):anchor_search_fixture::indexed_batch_files(bank,pattern);
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
  RevisionReadProfile baseline_reads,baseline_session_reads;
  session.reset();
  {
    const auto closed=NativeVolumeHeadView::open(head,id(90),resolve);
    auto copy=std::make_shared<Ram>();copy->failure_unit=image->failure_unit;
    {
      auto writer=NativeFilesystemWriter::compact(closed.filesystem(),copy,id(2),compression);
      if(revision)baseline_session_reads=profile_revision_session_reads(std::move(writer),*copy,originals);
    }
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
  if(options)options->adaptive_batch_sizing=adaptive;
  auto restricted=limited_memory?options:std::nullopt;
  std::size_t limited_groups=0;
  const bool full_trial=overlap==BatchOverlapTrial::full_wait || overlap==BatchOverlapTrial::full_advance;
  if(full_trial) {
    require(options && revision,"Full overlap fixture needs an AMD revision workload");
    // Fund a small complete GPU batch while leaving other discovered groups.
    options->maximum_members=4;
  }
  std::unique_ptr<NativeAnchorSearchController> anchor;
  if(options)anchor=std::make_unique<NativeAnchorSearchController>(std::move(*options));
  if(anchor && !adaptive && !revision && bank.empty() && !shifted_bias && !alternate_reference && !sparse_xor && !collective) {
    const auto now=GetTickCount64();
    require(!anchor->stage_grant(now,members) && anchor->stage_grant(now,members+families+1)==members+families,
        "Wide route used a legacy cap or ignored its funded range");
  }
  NativeMaintenanceProgress progress;
  bool cancelled=false,retired_reply_retained=false,deadline_fallback=false;
  bool recovery_rebound=false;
  const bool edit_after_retirement=foreground==BatchForegroundTrial::edit_after_retirement;
  const bool recover_after_deadline=foreground==BatchForegroundTrial::recover_after_deadline;
  const bool allow_cpu=foreground==BatchForegroundTrial::read_or_cpu || recover_after_deadline;
  const bool intervene_after_retirement=edit_after_retirement || allow_cpu ||
      foreground==BatchForegroundTrial::read_after_retirement;
  std::uint64_t activity=1;
  std::uint64_t sequence=0,worker_ns=0,cpu_groups_before_gpu_reply=0,fully_submitted=0;
  std::uint64_t packed_payload_bytes=0,sampled_bias_members=0;
  std::size_t sparse_bodies=0,delta_bodies=0,dense_bodies=0;
  unsigned packed_widths=0;
  const auto next_observation=[&] {
    MEMORYSTATUSEX host{};host.dwLength=sizeof(host);
    require(GlobalMemoryStatusEx(&host)!=0,"Cannot confirm current memory headroom");
    // Model idle time to exercise the larger batch quickly. HIP admission and
    // the physical and commit observations still use the real host.
    return NativeMaintenanceObservation{++sequence,1000*sequence,host.ullTotalPhys,host.ullAvailPhys,
        host.ullTotalPageFile,host.ullAvailPageFile,1000,0,1000,true,false};
  };
  for(unsigned i=0;i<5200 && maintenance.pending();++i) {
    const auto observation=next_observation();
    const auto resources=detail::maintenance_resources(observation,observation.captured_ms,policy);
    require(resources.has_value(),"Cannot establish a modeled opportunity with live memory");
    const auto funded=detail::maintenance_group_member_limit(*resources,true);
    if(cancelled)anchor->observe(activity,GetTickCount64());
    const auto grant=anchor && !cancelled?anchor->stage_grant(GetTickCount64(),funded):0;
    const auto replies=anchor?anchor->replies():std::span<const NativeSharedBaseSearchReply>{};
    progress=maintenance.advance(observation,observation.captured_ms,{grant,nullptr,anchor!=nullptr,replies});
    if(progress.phase==NativeMaintenancePhase::failed || progress.deferral==NativeMaintenanceDeferral::memory ||
        progress.deferral==NativeMaintenanceDeferral::capacity)
      std::cerr<<"multiwindow refusal phase="<<static_cast<unsigned>(progress.phase)
          <<" deferral="<<static_cast<unsigned>(progress.deferral)<<" sampled="<<progress.sampled_extents
          <<" published="<<progress.published_groups<<" declined="<<progress.declined_groups
          <<" image_bytes="<<image->extent()<<" physical_available="<<observation.physical_available
          <<" commit_available="<<observation.commit_available<<'\n';
    require(progress.phase!=NativeMaintenancePhase::failed && progress.deferral!=NativeMaintenanceDeferral::memory &&
        progress.deferral!=NativeMaintenanceDeferral::capacity,"Large batch preparation or publication failed");
    if(!anchor)continue;
    anchor->settle_reply_checks();
    if(!maintenance.has_retained_search())anchor->discard_reply();
    if(!progress.search_batch_ready)continue;
    const auto windows=maintenance.search_batch();
    if(restricted) {
      restrict_fixture_anchor_launch(windows,*restricted);
      limited_groups=windows.size();
      anchor=std::make_unique<NativeAnchorSearchController>(std::move(*restricted));
      restricted.reset();
    }
    const auto started=std::chrono::steady_clock::now();
    anchor->start(windows,activity);
    if(full_trial) {
      const auto pending=anchor->pending_prefix();
      require(std::ranges::equal(pending,windows) && !pending.empty(),
          "The comparison needs a completely admitted GPU batch");
      ++fully_submitted;
    }
    if(foreground==BatchForegroundTrial::cancel_active) {
      require(!cancelled && anchor->waiting(),"Cancellation fixture needs an outstanding GPU exchange");
      anchor->observe(++activity,GetTickCount64(),NativeAnchorOpportunity::foreground);
      cancelled=true;
      continue; // Admit independent CPU work while the supervisor retires the worker.
    }
    if(intervene_after_retirement && !retired_reply_retained && !deadline_fallback) {
      const auto terminal=anchor->wake_handle();
      require(terminal && WaitForSingleObject(terminal,8000)==WAIT_OBJECT_0 && WorkerExchangeSession::available(),
          "The retired reply fixture needs confirmed worker and supervisor exit");
      const auto reader=session->open("f0",{true,false,false});
      require(session->read(reader,0,originals.front().size())==originals.front(),
          "Retired reply interfered with foreground CPU read");
      session->cleanup(reader,false);session->close(reader);
      anchor->observe(++activity,GetTickCount64());
      const auto reserved=anchor->pending_prefix();
      const auto expected=std::span(windows).first(adaptive?1:windows.size());
      const bool retained=!reserved.empty() && std::ranges::equal(reserved,expected);
      const auto observed=anchor->stats();
      std::cout<<"retired_reply_trial retained="<<retained<<" pending_groups="<<reserved.size()
          <<" verified_before_idle="<<observed.verified
          <<" transport_status="<<(observed.last_exchange?static_cast<int>(*observed.last_exchange):-1)
          <<" worker_exit_code="<<observed.last_worker_exit_code
          <<" worker_elapsed_ms="<<observed.last_worker_elapsed_ms
          <<" submitted_groups="<<observed.submitted_groups
          <<" submitted_launches="<<observed.submitted_kernel_launches
          <<" retirement_ms="<<observed.last_retirement_elapsed_ms
          <<" publication_observed="<<observed.last_publication_observed
          <<" replies_before_idle="<<anchor->replies().size()<<std::endl;
      deadline_fallback=allow_cpu && !retained && observed.last_exchange==WorkerExchangeCode::timed_out;
      require((retained || (deadline_fallback && reserved.empty())) && !anchor->waiting() &&
          anchor->replies().empty() && !observed.verified,
          "Foreground read discarded or prematurely collected the retired AMD reply");
      retired_reply_retained=retained;
      if(edit_after_retirement) {
        anchor->observe(activity,GetTickCount64(),NativeAnchorOpportunity::idle);
        require(!anchor->replies().empty() && !anchor->replies().front().packed_bodies.empty() &&
            !anchor->stats().verified,"The edit fixture needs collected, unverified packed advice");
        const auto selected=std::span(originals.front()).first(kNativeExtentBytes);
        require(std::ranges::any_of(windows,[&](const auto& window) {
          return std::ranges::equal(window.base(),selected) ||
              std::ranges::any_of(window.members(),[&](const auto& member){return std::ranges::equal(member,selected);});
        }),"The edit must change a member of the retired batch");
        originals.front()[0]^=std::byte{0x27};
        const auto writer=session->open("f0",{true,true,false});
        session->write(writer,0,selected.first(1));
        session->cleanup(writer,false);session->close(writer);
        anchor->observe(++activity,GetTickCount64(),NativeAnchorOpportunity::foreground);
        require(!anchor->replies().empty() && !anchor->stats().verified,
            "Foreground edit dropped the entire retired batch or admitted unverified bytes");
        std::cout<<"retired_reply_edit selected_extent_changed=1 packed_reply_retained=1\n";
      }
    }
    bool can_overlap=overlap==BatchOverlapTrial::partial || overlap==BatchOverlapTrial::full_advance;
    await([&] {
      anchor->observe(activity,GetTickCount64(),NativeAnchorOpportunity::idle);
      if(!anchor->waiting())return true;
      if(can_overlap) {
        const auto pending=anchor->pending_prefix();
        require(!pending.empty(),"The overlap trial lost its pending AMD reservation");
        const auto observation=next_observation();
        const auto published=progress.published_groups;
        progress=maintenance.advance(observation,observation.captured_ms,{0,nullptr,true,{},pending});
        require(progress.phase!=NativeMaintenancePhase::failed && progress.deferral!=NativeMaintenanceDeferral::memory &&
            progress.deferral!=NativeMaintenanceDeferral::capacity,"Unsubmitted CPU group failed to advance");
        cpu_groups_before_gpu_reply+=progress.published_groups-published;
        can_overlap=progress.deferral!=NativeMaintenanceDeferral::other_maintenance;
      }
      return false;
    },8000,"Multiwindow AMD worker did not retire");
    worker_ns+=elapsed_ns(started);
    if(modular || alternate_reference || sparse_xor || collective || reference_device)for(const auto& reply:anchor->replies()) {
      if((revision || adaptive) && reply.packed_bodies.empty())continue; // Copy advice has no packed payload.
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
        if(method==NativePackedGroupMethod::xor_planes)++dense_bodies;
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
  if(foreground==BatchForegroundTrial::cancel_active)
    require(cancelled && stats.submitted==1 && stats.yielded==1 && !stats.verified &&
        progress.published_groups && !progress.advised_members_published,
        "Foreground cancellation did not settle through CPU grouping");
  if(foreground==BatchForegroundTrial::read_after_retirement)
    require(retired_reply_retained && stats.verified && !stats.yielded &&
        (adaptive || progress.advised_members_published),
        "Idle continuation failed to publish the preserved AMD advice");
  if(deadline_fallback) {
    require(stats.submitted==1 && !stats.verified && !stats.verified_timing_samples &&
        !progress.advised_members_published && progress.published_groups==families,
        "CPU grouping failed to complete the workload after a retired GPU deadline");
    std::cout<<"deadline_fallback cpu_groups="<<progress.published_groups<<"\n";
  }
  if(recover_after_deadline && deadline_fallback) {
    const auto target=anchor->recovery_target();
    require(target && anchor->take_deadline_recovery() && !anchor->take_deadline_recovery() &&
        WorkerExchangeSession::available(),"A retired deadline did not authorize exactly one qualification");
    const auto profile=installed_anchor_profile();
    require(profile.has_value(),"Deadline recovery requires an installed profile");
    auto binding=discover_installed_hip_anchor_route();
    auto check_status=binding.status;
    if(binding.status==HipDiscoveryStatus::ready && binding.route && binding.route->identity==target->identity) {
      require(!anchor->rebind(*binding.route),"Discovery alone reopened the expired configuration");
      const auto checked=qualify_native_anchor_recovery(*target,*binding.route,profile->worker);
      check_status=checked.status;
      if(checked.proof) {
        require(checked.status==HipDiscoveryStatus::ready && anchor->rebind(std::move(*binding.route),&*checked.proof) &&
            !anchor->recovery_target() && !anchor->take_deadline_recovery() &&
            anchor->stage_grant(UINT64_MAX,std::size_t{detail::anchor_match_batch_member_limit()}+1),
            "Independent device verification could not reopen the same configuration");
        recovery_rebound=true;
      }else {
        require(!anchor->rebind(std::move(*binding.route)) && anchor->recovery_target()==target,
            "Failed recovery verification changed the quarantine");
      }
    }
    require(WorkerExchangeSession::available(),"Deadline qualification did not retire");
    std::cout<<"deadline_recovery rebound="<<recovery_rebound<<" check_status="<<static_cast<unsigned>(check_status)
        <<" one_attempt=1 retired=1\n";
  }
  if(edit_after_retirement)
    require(retired_reply_retained && stats.submitted==1 && stats.reference_batches==1 &&
        progress.reselected_reference_groups+1==families && progress.advised_members_published,
        "The retired edit fixture used stale advice or lost advice for unchanged groups");
  if(limited_memory)
    require(limited_groups && stats.submitted==1 && stats.verified==1 &&
        stats.verified_groups>0 && stats.verified_groups<limited_groups &&
        progress.published_groups>stats.verified_groups,
        "Limited-memory fixture did not settle AMD and CPU groups together");
  if(overlap==BatchOverlapTrial::partial)
    require(cpu_groups_before_gpu_reply && cpu_groups_before_gpu_reply<=limited_groups-stats.verified_groups,
        "The overlap trial did not publish unsubmitted groups before AMD verification");
  if(full_trial)
    require(fully_submitted && stats.verified==fully_submitted &&
        (cpu_groups_before_gpu_reply>0)==(overlap==BatchOverlapTrial::full_advance),
        "The completely reserved AMD batch did not produce the expected CPU overlap");
  if(dense_overflow) {
    require(stats.submitted==1 && stats.empty_xor_batches==1 && !stats.verified &&
        !progress.advised_members_published,"Dense slots unexpectedly handled all active planes");
  }else if(anchor && shifted_bias) {
    std::cout<<"shifted_bias_reply submitted="<<stats.submitted<<" verified="<<stats.verified
        <<" empty="<<stats.empty_xor_batches<<" published="<<progress.advised_members_published<<'\n';
    require(stats.submitted && (stats.verified || stats.empty_xor_batches || packed_payload_bytes),
        "Shifted-bias comparison lacked a completed AMD exchange");
  } else if(anchor && !adaptive && !revision && !edit_after_retirement && !deadline_fallback) {
    require(stats.verified && progress.advised_members_published,"Large batch lost verified advice");
  }
  if(anchor && !adaptive && !deadline_fallback && !revision && bank.empty() && !modular && !dense_overflow &&
      !collective && !edit_after_retirement)
    require(stats.submitted==1 && stats.verified_groups==families &&
        (tile_reference || sparse_xor || progress.advised_members_published==members),
        "Related AMD work did not fill one verified multiwindow batch");
  if(adaptive && !deadline_fallback) {
    require(stats.submitted==1 && stats.verified_groups>=1 && stats.verified_timing_samples==1 &&
        stats.learned_launch_limit && (revision || (stats.verified_groups==1 && progress.published_groups>=families)),
        "The first measured AMD batch failed to leave the other complete groups on the CPU");
    std::cout<<"adaptive_batch measured_samples="<<stats.verified_timing_samples
        <<" learned_launch_limit="<<*stats.learned_launch_limit
        <<" worker_elapsed_ms="<<stats.last_worker_elapsed_ms
        <<" setup_ns="<<stats.last_profiled_setup_ns
        <<" launches="<<stats.submitted_kernel_launches<<" published_groups="<<progress.published_groups<<'\n';
  }
  if(require_phase_profile)
    require(stats.last_verified_copy_profile.has_value(),"AMD comparison lacked a verified phase trace");
  if(const auto& phase=stats.last_verified_copy_profile;phase && require_phase_profile) {
    std::cout<<"verified_phase_profile total_worker_wall_ns="<<phase->total_wall_ns;
    for(std::size_t i=0;i<hip_phase_names.size();++i)
      std::cout<<' '<<hip_phase_names[i]<<"_ns="<<phase->wall_ns[i];
    std::cout<<'\n';
  }
  if(tile_reference || sparse_xor)
    require(progress.published_groups==families,"Packed XOR did not group the complete cohort");
  if(anchor && modular && !shifted_bias)
    require(packed_widths==15 && packed_payload_bytes,"AMD modular batch did not cover all word widths");
  if(collective_device)
    require(stats.submitted==1 && sparse_bodies && delta_bodies && stats.verified_groups==families,
        "The shared AMD exchange did not verify both codec choices");
  if(!revision && (sparse_device || reference_device) && !edit_after_retirement && !deadline_fallback && !adaptive)
    require(sparse_bodies && stats.verified,"AMD sparse device advice was not independently accepted");
  if(reference_device && full_reference && !edit_after_retirement && !deadline_fallback && !adaptive)
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
  const auto grouped_root=PagedObjectStoreSnapshot::open(image,{kStoreMaxPhysicalBytes}).read(1);
  auto packed=std::make_shared<Ram>();packed->failure_unit=image->failure_unit;
  RevisionReadProfile grouped_session_reads;
  {
    auto writer=NativeFilesystemWriter::compact(closed.filesystem(),packed,id(3),compression);
    if(revision || dense_collective)
      grouped_session_reads=profile_revision_session_reads(std::move(writer),*packed,originals,dense_collective?16U:1U);
  }
  const auto reopened=NativeFilesystemSnapshot::open_image(packed,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
  const auto audit_bytes_before=packed->read_bytes,audit_calls_before=packed->read_calls;
  reopened.verify_all();
  if(revision)std::cout<<"grouped_replica_audit backing_read_bytes="<<packed->read_bytes-audit_bytes_before
      <<" backing_read_calls="<<packed->read_calls-audit_calls_before<<'\n';
  RevisionReadProfile grouped_reads;
  if(revision)grouped_reads=profile_revision_reads(reopened,*packed,originals);
  else for(std::size_t i=0;i<originals.size();++i)
    require(reopened.read("f"+std::to_string(i),0,originals[i].size())==originals[i],
        "Large AMD batch changed independently reopened file bytes");
  const auto compact_verify_ns=elapsed_ns(verify_start);
  require(revision || packed->extent()<baseline_bytes,"Multiwindow maintenance did not reduce complete compacted size");
  const auto root=PagedObjectStoreSnapshot::open(packed,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  // Advice can compete with a denser representation. Compaction must
  // preserve the capability that the writer actually selected.
  require(root[8]==grouped_root[8],"Compaction changed the selected indexed capability");
  if(revision) {
    std::cout<<"revision_reads zstd="<<(compression.mode==StoreCompressionMode::automatic?store_zstd_version():0)
        <<" full_logical="<<grouped_reads.full_logical<<" full_input_backing="<<baseline_reads.full_backing
        <<" full_grouped_backing="<<grouped_reads.full_backing<<" sampled_logical="<<grouped_reads.sampled_logical
        <<" sampled_input_backing="<<baseline_reads.sampled_backing<<" sampled_grouped_backing="<<grouped_reads.sampled_backing<<'\n';
    std::cout<<"revision_session_reads full_logical="<<grouped_session_reads.full_logical
        <<" full_input_backing="<<baseline_session_reads.full_backing
        <<" full_grouped_backing="<<grouped_session_reads.full_backing
        <<" sampled_logical="<<grouped_session_reads.sampled_logical
        <<" sampled_input_backing="<<baseline_session_reads.sampled_backing
        <<" sampled_grouped_backing="<<grouped_session_reads.sampled_backing<<'\n';
  }
  if(dense_collective)std::cout<<"collective_dense_session_reads full_ns="<<grouped_session_reads.full_ns
      <<" full_logical="<<grouped_session_reads.full_logical<<" full_backing="<<grouped_session_reads.full_backing
      <<" sampled_ns="<<grouped_session_reads.sampled_ns<<" sampled_logical="<<grouped_session_reads.sampled_logical
      <<" sampled_backing="<<grouped_session_reads.sampled_backing<<" verified=true\n";
  const auto cohort=revision?"revision-corpus":dense_collective?"collective-dense":
      collective?(bank.empty()?"collective-synthetic":"collective-public"):
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
      <<" submitted_groups="<<stats.submitted_groups<<" verified_groups="<<stats.verified_groups
      <<" cpu_groups_before_gpu_reply="<<cpu_groups_before_gpu_reply
      <<" fully_submitted_batches="<<fully_submitted
      <<" foreground_cancelled="<<cancelled
      <<" submitted_queued_batches="<<stats.submitted_queued_batches
      <<" verified_queued_batches="<<stats.verified_queued_batches<<" dispatches_per_wait="<<stats.queued_dispatches_per_wait
      <<" queued_worker_wall_ns="<<stats.queued_worker_wall_ns<<" queued_kernel_wait_ns="<<stats.queued_kernel_wait_ns
      <<" submitted_kernel_launches="<<stats.submitted_kernel_launches
      <<" largest_dispatch_members="<<stats.largest_dispatch_members
      <<" empty_batches="<<stats.empty_xor_batches
      <<" discovery_ns="<<discovery_ns<<" maintenance_ns="<<maintenance_ns<<" compact_verify_ns="<<compact_verify_ns
      <<" input_compacted_bytes="<<baseline_bytes<<" grouped_compacted_bytes="<<packed->extent()
      <<" packed_widths="<<packed_widths<<" packed_payload_bytes="<<packed_payload_bytes
      <<" sampled_bias_members="<<sampled_bias_members
      <<" sparse_bodies="<<sparse_bodies<<" delta_bodies="<<delta_bodies<<" dense_bodies="<<dense_bodies
      <<" reference_batches="<<stats.reference_batches<<" reselected_groups="<<stats.reselected_groups
      <<" cpu_reference_pairs="<<progress.cpu_reference_pairs<<" published_references_changed="<<progress.reselected_reference_groups
      <<" section_reference_groups="<<progress.section_reference_groups
      <<" packed_reference_groups="<<progress.packed_reference_groups
      <<" prefixed_zstd_members="<<progress.prefixed_zstd_members
      <<" retired=true independent_cpu_readback=true modeled_idle_time=true input_blake3=";
  constexpr char digits[]="0123456789abcdef";
  for(const auto byte:detail::record_digest(file_digests)) {
    const auto value=std::to_integer<unsigned>(byte);
    std::cout<<digits[value>>4]<<digits[value&15];
  }
  std::cout<<'\n';
  if(recover_after_deadline)
    require(deadline_fallback && recovery_rebound,"The deadline fixture did not recover the original AMD route");
  return 0;
}

int live_installed_retired_reply(BatchForegroundTrial foreground,bool adaptive=false) {
  const auto profile=installed_anchor_profile();
  require(profile && profile->prefer_reference,"The retired reply fixture needs an installed reference profile");
  const auto begun=std::chrono::steady_clock::now();
  auto binding=discover_installed_hip_anchor_route();
  require(binding.status==HipDiscoveryStatus::ready && binding.route,"Installed AMD route is unavailable");
  const auto discovery_ns=elapsed_ns(begun);
  auto budget=profile->worker;
  if(foreground==BatchForegroundTrial::recover_after_deadline) {
    // Exercise a real process deadline with a healthy device and untouched
    // driver. Discovery and the recovery worker use the installed budget.
    budget.timeout_ms=(std::max)(1U,budget.timeout_ms/4);
    std::cout<<"deadline_fixture worker_timeout_ms="<<budget.timeout_ms<<'\n';
  }
  return live_multiwindow_anchor(NativeAnchorSearchOptions{
      .route=std::move(*binding.route),.worker=budget,.minimum_members=profile->minimum_members,
      .maximum_members=profile->maximum_members,.duty_ppm=profile->duty_ppm,.output=profile->output,
      .maximum_dispatch_members=profile->maximum_dispatch_members,.dispatches_per_wait=profile->dispatches_per_wait,
      .prefer_reference=profile->prefer_reference}, {},discovery_ns,IndexedBatchPattern::xor_full_reference,
      false,BatchOverlapTrial::normal,foreground,adaptive);
}

int live_installed_recovery_check() {
  using Status=HipDiscoveryStatus;
  const auto profile=installed_anchor_profile();
  require(profile.has_value(),"Recovery check requires an installed AMD profile");
  auto binding=discover_installed_hip_anchor_route();
  require(binding.status==Status::ready && binding.route,"Recovery check could not bind the installed AMD route");
  const NativeAnchorRecoveryTarget target{binding.route->identity,1};
  auto other=target;
  other.identity[0]^=std::byte{1};
  const auto refused=qualify_native_anchor_recovery(other,*binding.route,profile->worker);
  require(refused.status==Status::configuration_quarantined && !refused.proof &&
      WorkerExchangeSession::available(),"Another configuration could enter the recovery check");
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled=qualify_native_anchor_recovery(target,*binding.route,profile->worker,stop.get_token());
  require(cancelled.status==Status::cancelled && !cancelled.proof &&
      WorkerExchangeSession::available(),"A cancelled recovery check launched a worker");
  const auto checked=qualify_native_anchor_recovery(target,*binding.route,profile->worker);
  require(checked.status==Status::ready && checked.proof && WorkerExchangeSession::available(),
      "Installed route did not produce an independently verified recovery proof");
  std::cout<<"installed_recovery_check cpu_oracle=exact route_identity_checked=1 retired=1\n";
  return 0;
}

int live_mixed_anchor_setup(const HipDiscoveredRoute& route,ProcessBudget budget,const std::filesystem::path& input) {
  constexpr std::array<std::uint32_t,11> widths{4,2,4,3,4,2,4,3,4,2,4};
  constexpr auto capacity=32ULL*1024*1024;
  constexpr StoreCompressionOptions compression{StoreCompressionMode::raw,1,false};
  const auto source=ReadOnlyFile::open(input);
  require(source.size()>=widths.size()*kNativeExtentBytes,"Mixed anchor fixture requires a complete public source");
  std::vector<Bytes> originals;
  std::vector<std::string> names;
  for(std::size_t group=0;group<widths.size();++group) {
    Bytes base(kNativeExtentBytes);
    source.read_exact_at(group*kNativeExtentBytes,base.data(),base.size());
    for(std::uint32_t member=0;member<=widths[group];++member) {
      auto bytes=base;
      if(member)for(std::size_t j=0;j<96;++j)bytes[4096*member+j]^=std::byte{29};
      names.push_back("m"+std::to_string(originals.size()));
      originals.push_back(std::move(bytes));
    }
  }

  const auto lane=[&](bool gpu) {
    auto head=std::make_shared<Ram>(),live=std::make_shared<Ram>();
    head->failure_unit=live->failure_unit=4096;
    std::optional<NativeFileSession> session(NativeFileSession::create_indexed_volume(
        head,id(90),live,id(1),{capacity},compression));
    for(std::size_t index=0;index<originals.size();++index) {
      session->create_entry(names[index]);
      const auto handle=session->open(names[index],{true,true,false});
      session->write(handle,0,originals[index]);
      session->cleanup(handle,false);
      session->close(handle);
    }
    const auto resolve=[&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?live:nullptr;};
    session.reset();
    const auto before=NativeVolumeHeadView::open(head,id(90),resolve);
    std::uint64_t baseline_bytes=0;
    if(!gpu) {
      auto baseline=std::make_shared<Ram>();
      baseline->failure_unit=live->failure_unit;
      (void)NativeFilesystemWriter::compact(before.filesystem(),baseline,id(3),compression);
      baseline_bytes=baseline->extent();
    }
    session.emplace(NativeFileSession::resume_closed_indexed_volume(head,id(90),1,resolve,compression));
    NativeGroupMaintenance maintenance(*session);
    const auto begun=std::chrono::steady_clock::now();
    const auto members=static_cast<std::uint32_t>(originals.size()-widths.size());
    const NativeAnchorSearchOptions options{.route=route,.worker=budget,.minimum_members=2,
        .maximum_members=members,.duty_ppm=100000,.maximum_dispatch_members=2};
    std::unique_ptr<NativeAnchorSearchController> anchor;
    if(gpu)anchor=std::make_unique<NativeAnchorSearchController>(options);
    const auto run=[&](std::span<const NativeSharedBaseSearchWindow> batch) {
      anchor->start(batch,1);
      const auto terminal=anchor->wake_handle();
      require(terminal && WaitForSingleObject(terminal,8000)==WAIT_OBJECT_0,"Mixed anchor worker did not settle");
      anchor->observe(1,GetTickCount64(),NativeAnchorOpportunity::idle);
      require(WorkerExchangeSession::available() && anchor->stats().retirement_confirmed,
          "Mixed anchor worker was still owned after its reply");
    };
    NativeMaintenanceProgress progress;
    bool compared=false;
    for(std::uint64_t sequence=1;sequence<=4096 && maintenance.pending();++sequence) {
      MEMORYSTATUSEX host{};
      host.dwLength=sizeof(host);
      require(GlobalMemoryStatusEx(&host)!=0,"Cannot confirm current memory headroom");
      const NativeMaintenanceObservation observation{sequence,sequence*1000,host.ullTotalPhys,host.ullAvailPhys,
          host.ullTotalPageFile,host.ullAvailPageFile,1000,0,1000,true,false};
      const auto replies=anchor?anchor->replies():std::span<const NativeSharedBaseSearchReply>{};
      progress=maintenance.advance(observation,observation.captured_ms,
          {compared?0U:members,nullptr,true,replies});
      require(progress.phase!=NativeMaintenancePhase::failed &&
          progress.deferral!=NativeMaintenanceDeferral::memory &&
          progress.deferral!=NativeMaintenanceDeferral::capacity,"Mixed group preparation or publication failed");
      if(!progress.search_batch_ready || compared)continue;
      compared=true;
      if(!anchor)continue;
      auto windows=maintenance.search_batch();
      require(windows.size()>=3,"Mixed copy fixture did not discover enough complete groups");
      const auto member_count=[](const auto& window){return window.members().size();};
      const auto largest=std::ranges::max_element(windows,{},member_count);
      const auto training=*largest;
      windows.erase(largest);
      const auto different=std::find_if(std::next(windows.begin()),windows.end(),[&](const auto& window) {
        return member_count(window)!=member_count(windows.front());
      });
      require(different!=windows.end(),"Mixed copy fixture needs distinct group sizes");
      std::rotate(std::next(windows.begin()),different,std::next(different));
      std::vector<std::uint32_t> counts;
      for(const auto& window:windows)counts.push_back(static_cast<std::uint32_t>(member_count(window)));
      run(std::span{&training,1});
      const auto trained=anchor->stats();
      require(trained.verified_groups==1 && trained.verified_timing_samples==1 &&
          trained.last_profiled_setup_ns && trained.last_verified_copy_profile,"Could not verify the initial GPU setup");
      NativeAnchorBatchTiming complete;
      const auto profiled_ms=(trained.last_verified_copy_profile->total_wall_ns+999999)/1000000;
      complete.record_verified((std::max)(profiled_ms,trained.last_worker_elapsed_ms),
          static_cast<std::uint32_t>(trained.submitted_kernel_launches));
      anchor->discard_reply();
      await([&]{return anchor->stage_grant(GetTickCount64(),originals.size())!=0;},62000,
          "Mixed GPU fixture did not pass its real pacing gate");
      const auto pressure=query_windows_gpu_budget(route.memory.luid,route.memory.node_mask);
      require(pressure.status==WindowsGpuBudgetStatus::success,"Cannot price the confirmation batch");
      const auto fallback=plan_native_anchor_launch(counts,options,pressure.snapshot,
          GetTickCount64(),false,complete.launch_limit(budget.timeout_ms));
      run(windows);
      const auto settled=anchor->stats();
      const auto cold_groups=fallback?fallback->batch.groups.size():0;
      const auto verified_groups=anchor->replies().size();
      std::cout<<"mixed_setup verified_groups="<<verified_groups<<" full_cost_groups="<<cold_groups
          <<" bootstrap_setup_ns="<<trained.last_profiled_setup_ns<<" bootstrap_elapsed_ms="<<trained.last_worker_elapsed_ms
          <<" mixed_elapsed_ms="<<settled.last_worker_elapsed_ms<<" observations="<<settled.verified_timing_samples
          <<" retired="<<settled.retirement_confirmed<<" training_members="<<member_count(training)<<" offered_members=";
      for(const auto count:counts)std::cout<<count<<',';
      std::cout<<std::endl;
      require(verified_groups>cold_groups && verified_groups>=2 &&
          settled.verified_groups==verified_groups+1 && settled.verified_timing_samples==2,
          "Verified setup did not fund more heterogeneous groups");
    }
    // Suggested copies compete against the native CPU encodings. The
    // device can verify a group without winning the final byte comparison.
    require(compared && !maintenance.pending() && progress.published_groups>0,
        "Mixed group publication did not finish");
    if(anchor)require(anchor->stop(),"Mixed anchor controller failed to retire");
    session.reset();
    auto packed=std::make_shared<Ram>();
    packed->failure_unit=live->failure_unit;
    const auto after=NativeVolumeHeadView::open(head,id(90),resolve);
    (void)NativeFilesystemWriter::compact(after.filesystem(),packed,id(2),compression);
    const auto verified=NativeFilesystemSnapshot::open_image(packed,{NativeImageKind::indexed_paged,capacity});
    verified.verify_all();
    for(std::size_t index=0;index<originals.size();++index)
      require(verified.read(names[index],0,kNativeExtentBytes)==originals[index] &&
          before.filesystem().read(names[index],0,kNativeExtentBytes)==originals[index],"Mixed group changed an original extent");
    if(baseline_bytes)require(packed->extent()<baseline_bytes,"Mixed grouping did not reduce complete occupancy");
    std::cout<<"mixed_setup_image gpu="<<gpu<<" files="<<originals.size()<<" initial_compacted_bytes="<<baseline_bytes
        <<" whole_bytes="<<packed->extent()<<" groups="<<progress.published_groups
        <<" advised_members="<<progress.advised_members_published
        <<" elapsed_ns="<<elapsed_ns(begun)<<" verified=1"<<std::endl;
    return packed->extent();
  };
  const auto cpu=lane(false);
  const auto amd=lane(true);
  require(amd<=cpu && WorkerExchangeSession::available(),"Mixed GPU grouping expanded the CPU result or retained the worker");
  return 0;
}

int live_anchor_owner(int argc,char** argv) {
  const std::string_view mode(argv[6]);
  const bool mixed_setup=mode=="mixed-setup";
  const auto multiwindow=multiwindow_pattern(mode);
  const auto restriction=argc==9?std::string_view(argv[8]):std::string_view{};
  const bool wide_copy=restriction=="wide-copy";
  const bool reference_xor=restriction=="reference-xor";
  const bool measure_startup=restriction=="startup-profile";
  const bool measure_batch=restriction=="batch-profile";
  const bool cancel_after_submit=restriction=="cancel-after-submit";
  auto overlap=BatchOverlapTrial::normal;
  if(restriction=="limited-overlap")overlap=BatchOverlapTrial::partial;
  if(restriction=="full-overlap")overlap=BatchOverlapTrial::full_advance;
  if(restriction=="full-wait")overlap=BatchOverlapTrial::full_wait;
  const bool limited_memory=restriction=="limited-memory" || overlap==BatchOverlapTrial::partial;
  const bool queued=mode=="queued" || mode=="queued-yield" || mode=="verified-read" || mode=="verified-edit" ||
      restriction=="queued" || wide_copy;
  const bool xor_mode=mode=="xor" || mode=="xor-wide" || mode=="xor-narrow";
  require(mode=="accept" || mode=="yield" || mode=="both" || mode=="batch" || mode=="batch-yield" || queued ||
      mode=="changed-extents" || mode=="prefix-extension" || xor_mode || multiwindow || (mixed_setup && argc==8),
      "Expected an AMD ownership trial");
  require(argc!=9 || (multiwindow && revision_corpus(*multiwindow) &&
      (restriction=="queued" || limited_memory || wide_copy || reference_xor ||
          cancel_after_submit || measure_startup || measure_batch || overlap!=BatchOverlapTrial::normal)),
      "The requested comparison needs a revision corpus");
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
  if(mixed_setup)return live_mixed_anchor_setup(*binding.route,profile,argv[7]);
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
    const auto output=wide_copy || reference_xor?anchor_match::BatchOutput::wide_anchors:
        revision_corpus(*multiwindow)?anchor_match::BatchOutput::anchors:
        mode=="xor-full-reference"?anchor_match::BatchOutput::reference_xor:
        (mode=="collective" || mode=="collective-dense")?anchor_match::BatchOutput::collective:
        sparse?anchor_match::BatchOutput::sparse_xor:
        modular?anchor_match::BatchOutput::modular_delta:anchor_match::BatchOutput::wide_packed_xor;
    return live_multiwindow_anchor(NativeAnchorSearchOptions{
        .route=*binding.route,.worker=profile,.minimum_members=minimum,
        .maximum_members=static_cast<std::uint32_t>(members+families),.output=output,
        .maximum_dispatch_members=wide_copy || reference_xor?12U:small_batch?4U:modular?16U:0U,
        .dispatches_per_wait=queued?kAnchorProfileMaxQueuedDispatches:1U,.prefer_reference=reference_xor,
        .profile_copy_batches=measure_startup || measure_batch},
        argc>=8?std::filesystem::path(argv[7]):std::filesystem::path{},
        discovery_ns,*multiwindow,limited_memory,overlap,
        cancel_after_submit?BatchForegroundTrial::cancel_active:BatchForegroundTrial::normal,measure_startup);
  }
  const std::uint32_t members=(mode=="changed-extents" || mode=="prefix-extension")?3:
      queued?queued_anchor_members:(mode=="batch" || mode=="batch-yield" || xor_mode)?8:4;
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
void memory_hybrid_placement() {
  using Phase=NativeHybridBacking::BuildPhase;
  constexpr auto image_limit=8ULL<<20;
  constexpr auto workspace_reservation=4ULL<<20;
  constexpr auto selector_reservation=2*kStoreCommitUnit;
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto domain=MemoryVolumeDomain::create(2*image_limit+selector_reservation);
  auto head=domain.create_store(selector_reservation),source=domain.create_store(image_limit);
  auto workspace=domain.create_store(workspace_reservation);
  const auto volume=fresh_volume_identity(),previous=fresh_volume_identity(),replacement=fresh_volume_identity();
  auto contents=data(2'860'912,311);
  std::shared_ptr<StoreByteIo> base,tail;
  std::shared_ptr<NativeHybridBacking> selected;
  std::uint64_t base_reserved=0,tail_reserved=0,peak=0,old_bytes=0,new_bytes=0;
  unsigned workspace_steps=0,retirement_steps=0;
  {
    auto session=NativeFileSession::create_indexed_volume(head,volume,source,previous,{image_limit},raw);
    session.create_entry("visible");
    const auto file=session.open("visible",{true,true,false});
    for(std::size_t at=0;at<contents.size();at+=kStoreMaxObjectBytes)
      session.write(file,at,std::span<const std::byte>(contents).subspan(at,
          (std::min)(std::size_t{kStoreMaxObjectBytes},contents.size()-at)));
    old_bytes=source->extent();
    std::optional<NativeFilesystemSnapshot> pinned(session.snapshot());
    auto work=NativeHybridBacking::prepare(session,workspace,source->contract(),replacement,image_limit,image_limit,raw);
    const auto advance=[&](Phase phase) {
      for(unsigned steps=0;work.phase()!=phase;++steps) {
        require(steps<8192,"owned hybrid preparation did not yield or reach the requested phase");
        (void)work.advance(session);
        peak=(std::max)(peak,domain.usage().resident);
      }
    };
    advance(Phase::awaiting_base);
    base_reserved=work.required_store_bytes();
    base=domain.create_store(base_reserved);
    work.provide_store(session,base);
    advance(Phase::awaiting_tail);
    tail_reserved=work.required_store_bytes();
    require(base_reserved+tail_reserved<=image_limit &&
        domain.usage().reserved+tail_reserved>domain.usage().capacity,
        "fixture does not require release of private staging before tail placement");
    while(workspace) {
      const auto bytes=domain.usage().resident;
      require(domain.retire_store(workspace)!=MemoryStoreRetirement::pinned &&
          bytes-domain.usage().resident<=kStoreMaxObjectBytes && ++workspace_steps<16,
          "checked workspace did not release in bounded quanta");
    }
    tail=domain.create_store(tail_reserved);
    work.provide_store(session,tail);
    advance(Phase::ready);
    require(work.publish(session,selected),"allocator-backed image was not selected");
    new_bytes=selected->occupied();
    require(new_bytes<old_bytes && session.volume_info().selection.image_identity==replacement &&
        session.read(file,0,static_cast<std::uint32_t>(contents.size()))==contents &&
        domain.retire_store(source)==MemoryStoreRetirement::pinned,
        "owned hybrid mismatched an open file or discarded a pinned image");
    const auto patch=data(2117,519);
    const auto offset=kNativeExtentBytes-13;
    session.write(file,offset,patch);
    require(pinned->read("visible",offset,patch.size())!=patch &&
        session.read(file,offset,static_cast<std::uint32_t>(patch.size()))==patch && base->extent()==base_reserved,
        "tail write modified retained bytes or changed the base");
    std::ranges::copy(patch,contents.begin()+offset);
    pinned.reset();
    while(source) {
      const auto bytes=domain.usage().resident;
      require(domain.retire_store(source)!=MemoryStoreRetirement::pinned &&
          bytes-domain.usage().resident<=kStoreMaxObjectBytes && ++retirement_steps<16,
          "old owned mirror did not retire in bounded quanta");
    }
    domain.verify_resident();
    session.cleanup(file,false);
    session.close(file);
  }
  selected.reset();
  auto opened=NativeHybridBacking::open(base,tail,replacement,image_limit,image_limit);
  auto checked=NativeVolumeHeadView::open(head,volume,[&](const StoreIdentity& chosen) {
    return chosen==replacement?opened:nullptr;
  });
  require(checked.info().selection.epoch==2 &&
      checked.filesystem().read("visible",0,contents.size())==contents &&
      domain.usage().stores==3,"independent CPU reopen lost data or retained a staged store");
  domain.verify_resident();
  std::cout<<"owned_hybrid_placement old_bytes="<<old_bytes<<" new_bytes="<<new_bytes
      <<" base_reserved="<<base_reserved<<" tail_reserved="<<tail_reserved<<" peak_resident="<<peak
      <<" workspace_steps="<<workspace_steps<<" retirement_steps="<<retirement_steps
      <<" source_pinned=1 open_handle=1 cpu_reopen=1\n";
}

void automatic_memory_sealing(bool change_source,bool rotate_sealed=false) {
  constexpr std::uint64_t capacity=8ULL*1024*1024;
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  MemoryVolumeBacking backing;
  backing.layout={2*capacity+kVolumeHeadBytes,capacity,0};
  backing.domain=std::make_unique<MemoryVolumeDomain>(MemoryVolumeDomain::create(backing.layout.domain_capacity));
  backing.head=backing.domain->create_store(kVolumeHeadBytes);
  backing.image=backing.domain->create_store(capacity);
  backing.volume_identity=id(90);
  backing.image_identity=id(1);
  auto bytes=data(2'861'348,167);
  const auto length=static_cast<std::uint32_t>(bytes.size());
  std::uint64_t original=0,occupied=0,peak=0;
  unsigned retire_steps=0;
  {
    auto session=NativeFileSession::create_indexed_volume(backing.head,backing.volume_identity,backing.image,
        backing.image_identity,{backing.layout.image_capacity},raw);
    require(session.enable_workload_accounting().enabled,"automatic sealing fixture has no committed-input accounting");
    session.create_entry("visible");
    const auto handle=session.open("visible",{true,true,false});
    for(std::size_t at=0;at<bytes.size();at+=kStoreMaxObjectBytes)
      session.write(handle,at,std::span(bytes).subspan(at,
          (std::min)(std::size_t{kStoreMaxObjectBytes},bytes.size()-at)));
    original=backing.image->extent();
    std::optional<NativeFilesystemSnapshot> saved(session.snapshot());
    require(!backing.compaction_pending(session) && backing.compaction_pending(session,true),
        "new workload did not qualify exactly as cold sealing");
    require(backing.advance_compaction(session,true)==MemoryCompactionStep::copying,
        "qualified sealing did not reserve a workspace");
    if(change_source) {
      bytes[13]^=std::byte{0x31};
      session.write(handle,13,std::span<const std::byte>(bytes).subspan(13,1));
      require(backing.advance_compaction(session,true)==MemoryCompactionStep::retiring,
          "a changed source did not retire private preparation");
      require(backing.advance_compaction(session,true)==MemoryCompactionStep::retiring &&
          backing.image_identity==id(1) && !backing.compaction_pending(session,true) &&
          session.read(handle,0,length)==bytes,"unfunded retry replaced or lost acknowledged data");
      session.cleanup(handle,false);
      session.close(handle);
      std::cout<<"automatic_memory_seal stale_cancelled=1 unchanged_input_quiet=1 exact=1\n";
      return;
    }
    bool published=false;
    for(unsigned n=0;!published && n<8192;++n) {
      const auto step=backing.advance_compaction(session,true);
      require(step==MemoryCompactionStep::copying || step==MemoryCompactionStep::ready ||
          step==MemoryCompactionStep::published,"automatic sealing could not finish its prepared image");
      const auto allocation=backing.domain->usage();
      require(allocation.resident<=allocation.reserved && allocation.reserved<=allocation.capacity,
          "automatic sealing exceeded its memory domain");
      peak=(std::max)(peak,allocation.resident);
      published=step==MemoryCompactionStep::published;
    }
    require(published && backing.selected_image_sealed() && backing.domain->owns_image(*backing.image) &&
        session.volume_info().selection.image_identity==backing.image_identity,
        "automatic sealing did not select the registered composite");
    occupied=dynamic_cast<const NativeHybridBacking&>(*backing.image).occupied();
    const auto pinned=backing.domain->usage().resident;
    require(occupied<original && backing.advance_compaction(session,true)==MemoryCompactionStep::pinned &&
        backing.domain->usage().resident==pinned && saved->read("visible",0,length)==bytes &&
        session.read(handle,0,length)==bytes,"automatic selection broke a retained reader or open file");
    const auto edit=data(3119,547);
    const auto offset=kNativeExtentBytes-19;
    session.write(handle,offset,edit);
    require(saved->read("visible",offset,edit.size())!=edit &&
        session.read(handle,offset,static_cast<std::uint32_t>(edit.size()))==edit,
        "selected tail did not isolate the acknowledged overwrite");
    std::ranges::copy(edit,bytes.begin()+offset);
    saved.reset();
    while(backing.compaction_pending(session,true)) {
      const auto before=backing.domain->usage().resident;
      require(backing.advance_compaction(session,true)==MemoryCompactionStep::retiring &&
          before-backing.domain->usage().resident<=kStoreMaxObjectBytes && ++retire_steps<20,
          "released snapshot did not allow bounded retirement");
    }
    if(rotate_sealed) {
      std::optional<NativeFilesystemSnapshot> old_hybrid(session.snapshot());
      session.truncate(handle,0);
      const auto reduced=data(117'372,47);
      session.write(handle,0,reduced);
      require(backing.compaction_pending(session),"deletions did not qualify recovery of old hybrid space");
      bool copied=false;
      for(unsigned n=0;!copied && n<8192;++n) {
        const auto step=backing.advance_compaction(session);
        require(step==MemoryCompactionStep::copying || step==MemoryCompactionStep::ready ||
            step==MemoryCompactionStep::published,"ordinary reclaim failed to rotate a selected hybrid");
        copied=step==MemoryCompactionStep::published;
      }
      require(copied && !backing.selected_image_sealed() &&
          backing.advance_compaction(session)==MemoryCompactionStep::pinned &&
          old_hybrid->read("visible",0,length)==bytes,
          "hybrid backing parts retired ahead of their old snapshot");
      old_hybrid.reset();
      unsigned old_steps=0;
      while(backing.compaction_pending(session)) {
        const auto before=backing.domain->usage().resident;
        require(backing.advance_compaction(session)==MemoryCompactionStep::retiring &&
            before-backing.domain->usage().resident<=kStoreMaxObjectBytes && ++old_steps<20,
            "retired composite did not release one bounded span");
      }
      require(backing.domain->usage().stores==2 &&
          session.read(handle,0,static_cast<std::uint32_t>(reduced.size()))==reduced,
          "retired composite retained a component or lost selected bytes");
      bytes=reduced;
      std::cout<<"automatic_memory_seal composite_retirement_steps="<<old_steps
          <<" old_composite_pinned=1 both_components_released=1 exact=1\n";
    }
    backing.domain->verify_resident();
    session.cleanup(handle,false);
    session.close(handle);
  }
  auto portable=backing.open_read_only();
  require(portable.read(L"\\visible",0,static_cast<std::uint32_t>(bytes.size()))==bytes && retire_steps>1,
      "read-only projection did not retain the selected base and tail");
  std::cout<<"automatic_memory_seal old_bytes="<<original<<" occupied_bytes="<<occupied
      <<" peak_locked_bytes="<<peak<<" retirement_steps="<<retire_steps
      <<" reader_pin=1 open_handle=1 cpu_projection=1\n";
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
    std::array<std::byte,4> zero{};
    std::array<std::byte,4> untouched{std::byte{9},std::byte{9},std::byte{9},std::byte{9}};
    detail::MaintenanceBackingUse use;
    {
      const detail::MaintenanceBackingScope tracking(use);
      backing.image->write_exact(4*page,marker);
      backing.image->flush();
      backing.image->read_exact(2*page,untouched);
    }
    backing.domain->verify_resident();
    using Operation=detail::MaintenanceBackingOperation;
    require(use[Operation::write].completed_bytes==marker.size() &&
        use[Operation::flush].completed==1 && use[Operation::read].completed_bytes==untouched.size(),
        "actual locked-memory backing did not supply exact maintenance costs");
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

enum class PairedWorkspaceFixture {file,locked_memory,denied_memory};
struct PairedWorkspaceResult {
  std::uint64_t written=0,allocated=0;
};
PairedWorkspaceResult volume_file_sealed_callbacks(PairedWorkspaceFixture workspace=PairedWorkspaceFixture::file,
    std::uint32_t unit=4096) {
  const PagedStoreLimits limits{(unit==4096?8ULL:16ULL)*1024*1024};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto backing=std::make_shared<SparseVolumeRam>(unit,2*limits.maximum_physical_bytes+2*1024*1024);
  std::optional<MemoryVolumeDomain> occupied_domain;
  if(workspace==PairedWorkspaceFixture::denied_memory)
    occupied_domain.emplace(MemoryVolumeDomain::create(kStoreCommitUnit));
  auto expected=data(2'861'348,167);
  std::uint64_t attempted_backing_writes=0;
  constexpr auto edit_at=kNativeExtentBytes-13;
  Provider provider;Provider::active=&provider;
  {
    auto adapter=WinFspNativeWritable::create_file_volume(backing,id(90),id(1),limits,raw);
    if(workspace!=PairedWorkspaceFixture::file)adapter->enable_paired_seal_memory();
    WinFspNativeSession session(std::move(adapter),provider.api());
    require(session.start(plan())==STATUS_SUCCESS,"paired seal SDK fixture could not start");
    callback_file(provider,0,expected,kStoreMaxObjectBytes);
    callback_file(provider,1,Bytes((unit==4096?2U:3U)*1024*1024),kStoreMaxObjectBytes);
    if(unit!=4096)callback_file(provider,2,Bytes(3U*1024*1024),kStoreMaxObjectBytes);
    wchar_t name[]=L"\\f0";
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO metadata{};
    auto* fs=&provider.fs;
    require(fs->Interface->Open(fs,name,FILE_NON_DIRECTORY_FILE|(1U<<24),FILE_READ_DATA|FILE_WRITE_DATA,
        &context,&metadata)==STATUS_SUCCESS && context,"could not open the file before paired sealing");
    ULONG bytes=0;
    std::byte first{};
    require(fs->Interface->Read(fs,context,&first,0,1,&bytes)==STATUS_SUCCESS && bytes==1 &&
        first==expected.front(),"paired seal SDK fixture did not adopt the open");
    NativeMaintenanceObservation observed;
    observed.valid=true;observed.physical_total=16ULL<<30;observed.physical_available=12ULL<<30;
    observed.commit_limit=32ULL<<30;observed.commit_available=24ULL<<30;
    observed.cpu_total_ticks=1000;observed.cpu_window_ms=1000;
    const auto before_writes=backing->io.write_bytes;
    bool selected=false;
    WinFspMaintenanceResult last;
    for(unsigned turn=0;!selected && session.background_maintenance_pending() && turn<3200;++turn) {
      ++observed.sequence;observed.captured_ms+=observed.cpu_window_ms;
      last=session.advance_background_maintenance(observed,observed.captured_ms);
      require(!last.compaction_failed && last.progress.phase!=NativeMaintenancePhase::failed,
          "paired SDK compaction failed");
      selected=last.sealed_image;
    }
    if(!selected)std::cerr<<"paired SDK seal unit="<<unit<<" workspace="<<static_cast<unsigned>(workspace)
        <<" phase="<<static_cast<unsigned>(last.progress.phase)<<" deferral="<<static_cast<unsigned>(last.progress.deferral)<<'\n';
    require(selected,"ordinary SDK maintenance did not select a funded sealed image");
    require(bool(backing->range_reclaims)==(workspace!=PairedWorkspaceFixture::locked_memory),
        "sealed SDK fixture did not place scratch on the admitted provider");
    attempted_backing_writes=backing->io.write_bytes-before_writes;
    auto edit=data(3001,913);
    require(fs->Interface->Write(fs,context,edit.data(),edit_at,static_cast<ULONG>(edit.size()),
        FALSE,FALSE,&bytes,&metadata)==STATUS_SUCCESS && bytes==edit.size(),"open context could not write the selected tail");
    std::ranges::copy(edit,expected.begin()+edit_at);
    Bytes output(expected.size());
    require(fs->Interface->Read(fs,context,output.data(),0,static_cast<ULONG>(output.size()),&bytes)==STATUS_SUCCESS &&
        bytes==expected.size() && output==expected,"same SDK context did not read the new image");
    callback_cleanup_close(provider,context);
    session.stop();
    require(session.write_restart_ready(),"paired seal SDK fixture did not retire");
  }
  const auto view=NativeVolumeFile::inspect(backing->replay(),id(90));
  require(view.info().selection.sealed_range && view.filesystem().read("f0",0,
      static_cast<std::uint32_t>(expected.size()))==expected,"CPU inspection lost the SDK tail write");
  occupied_domain.reset();
  if(workspace!=PairedWorkspaceFixture::file) {
    {auto free=MemoryVolumeDomain::create(kStoreCommitUnit);}
    require(MemoryVolumeDomain::cleanup_healthy(),"paired seal scratch did not release its Windows lease");
  }
  const auto allocated=backing->occupied();
  std::cout<<"paired_sealed_sdk selected=1 held_context=1 stored_tail_edit=1 independent_cpu_reopen=1"
      <<" workspace="<<static_cast<unsigned>(workspace)<<" unit="<<unit<<" attempted_backing_write_bytes="
      <<attempted_backing_writes<<" allocated_bytes="<<allocated<<" memory_lease_retired=1\n";
  return {attempted_backing_writes,allocated};
}

void concurrent_memory_store_accounting() {
  constexpr auto capacity=8*kStoreCommitUnit;
  auto domain=MemoryVolumeDomain::create(2*capacity);
  const auto first_bytes=data(kStoreCommitUnit,429);
  const auto second_bytes=data(kStoreCommitUnit,719);
  const auto fill=[&](const Bytes& bytes) {
    auto store=domain.create_store(capacity);
    for(std::uint64_t offset=0;offset<capacity;offset+=bytes.size())
      store->write_exact(offset,bytes);
    return store;
  };
  std::shared_ptr<StoreByteIo> first;
  std::exception_ptr failure;
  std::latch start{1};
  std::jthread writer([&] {
    start.wait();
    try{first=fill(first_bytes);}
    catch(...){failure=std::current_exception();}
  });
  start.count_down();
  auto second=fill(second_bytes);
  writer.join();
  if(failure)std::rethrow_exception(failure);
  const auto usage=domain.usage();
  require(usage.stores==2 && usage.reserved==2*capacity && usage.resident==2*capacity,
      "Parallel memory store growth lost ownership accounting");
  Bytes output(kStoreCommitUnit);
  first->read_exact(capacity-kStoreCommitUnit,output);
  require(output==first_bytes,"Parallel first store changed bytes");
  second->read_exact(capacity-kStoreCommitUnit,output);
  require(output==second_bytes,"Parallel second store changed bytes");
  domain.verify_resident();
  first.reset();second.reset();
  const auto retired=domain.usage();
  require(!retired.stores && !retired.reserved && !retired.resident,
      "Parallel stores did not retire their resident pages");
}

#ifdef FLITZFS_INDEXED_HIP_FIXTURE
void amd_write_arrival_trial(std::string_view mode,const std::filesystem::path& public_input={}) {
  const bool mixed=mode=="mixed";
  const bool geometry=mode=="geometry";
  const bool oversized=mode=="oversized";
  const bool later=mode=="later" || mixed || oversized;
  const bool receive_later=later && !oversized;
  const bool stop_waiting=mode=="stop";
  const auto profile=installed_uniform_write_trial_profile();
  require((later || stop_waiting || geometry || mode=="drain") && profile && profile->await_later_writes &&
      (!(mixed || oversized) || profile->allow_smaller_writes) && (mixed || geometry || public_input.empty()) &&
      installed_uniform_write_files_present(),"Arriving write trial requires its staged version 6 profile");
  require(!geometry || (profile->maximum_batch_chunks>=4 && profile->maximum_batch_chunks<=
      kStoreMaxTransactionBytes/kNativeExtentBytes),"Geometry trial exceeds its fixture's native write envelope");
  const std::uint64_t image_capacity=(geometry?32ULL:8ULL)*1024*1024;
  constexpr std::uint64_t allowance=32ULL*1024*1024;
  auto domain=MemoryVolumeDomain::create(image_capacity+kVolumeHeadBytes+allowance);
  auto head=domain.create_store(kVolumeHeadBytes);
  auto image=domain.create_store(image_capacity);
  NativeMaintenancePolicy policy;
  policy.maximum_cpu_busy_ppm=500'000;
  auto source=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),{image_capacity},
      {StoreCompressionMode::fast,1,false},{},policy);
  Provider p;
  Provider::active=&p;
  WinFspVolumeController owner(std::move(source),p.api(),plan(),domain);
  await([&]{return owner.view().state==SessionState::running;},5000,"Arriving write controller did not start");
  const auto* table=p.fs.Interface;
  const auto security=native_security_fixture::descriptor();
  const auto make_input=[](unsigned seed,std::size_t chunks) {
    const auto block=data(kNativeExtentBytes/8,seed);
    Bytes result(chunks*kNativeExtentBytes);
    for(std::size_t offset=0;offset<result.size();offset+=block.size())
      std::ranges::copy(block,result.begin()+offset);
    return result;
  };
  auto initial=make_input(918,mixed?2:1);
  const auto following_chunks=geometry?profile->maximum_batch_chunks:oversized?2U:1U;
  auto following=make_input(919,following_chunks);
  if(!public_input.empty()) {
    const auto file=ReadOnlyFile::open(public_input);
    require(file.size()>=initial.size()+following.size(),"Write trial fixture needs a complete public source");
    file.read_exact_at(0,initial.data(),initial.size());
    file.read_exact_at(initial.size(),following.data(),following.size());
  }
  const auto commit=[&](PCWSTR name,Bytes& bytes,std::size_t count,bool close=true) {
    PVOID handle=nullptr;
    FSP_FSCTL_FILE_INFO info{};
    require(table->Create(&p.fs,const_cast<PWSTR>(name),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
        const_cast<std::byte*>(security.data()),0,&handle,&info)==STATUS_SUCCESS && handle,"Arriving write Create");
    for(std::size_t i=0;i<count;++i) {
      if(i)Sleep(1120); // Span the one-second host CPU observation window.
      ULONG written=0;
      require(table->Write(&p.fs,handle,bytes.data(),0,static_cast<ULONG>(bytes.size()),FALSE,FALSE,&written,&info)==
          STATUS_SUCCESS && written==bytes.size(),"Arriving ordinary CPU Write");
    }
    if(close)callback_cleanup_close(p,handle,true);
    return handle;
  };
  commit(L"\\first",initial,kNativeAmdWriteMinimumPairs);
  await([&]{const auto view=owner.view();return view.amd_write_waiting || view.amd_write_trial.has_value();},13000,
      "Owner did not reach its arriving write boundary");
  const auto parked=owner.view();
  if(!parked.amd_write_waiting && parked.amd_write_trial) {
    const auto& failed=*parked.amd_write_trial;
    std::cerr<<"arriving_owner_status="<<static_cast<unsigned>(failed.status)
        <<" requests="<<failed.device.admitted_requests<<" ready_ns="<<failed.device.ready_ns<<'\n';
  }
  require(parked.amd_write_waiting,"Owner did not park a completed request");
  if(later)commit(L"\\later",following,1);
  if(stop_waiting)owner.stop_and_join();
  else await([&]{return owner.view().amd_write_trial.has_value();},9000,"Arriving trial did not settle");
  const auto view=owner.view();
  require(view.amd_write_trial && view.amd_write_trial_attempts==1 && !view.amd_write_waiting &&
      !view.foreground_activation,"Arriving trial lost ownership or offered a foreground route");
  const auto& trial=*view.amd_write_trial;
  const auto& device=trial.device;
  const std::size_t expected=receive_later?2:1;
  const auto expected_bytes=initial.size()+(receive_later?following.size():0);
  std::cout<<"arriving_owner status="<<static_cast<unsigned>(trial.status)
      <<" requests="<<device.admitted_requests<<" cpu_writes="<<trial.reference.sample.transactions
      <<" gpu_writes="<<(device.sample?device.sample->transactions:0)
      <<" stored="<<(device.sample?device.sample->stored_lz4_candidates:0)
      <<" verified_input_bytes="<<device.verified_input_bytes
      <<" exchange="<<static_cast<unsigned>(device.exchange)<<" worker_exit="<<device.worker_exit
      <<" parked=1 ready_ns="<<device.ready_ns<<" device_wall_ns="<<trial.device_wall_ns
      <<" cpu_wall_ns="<<trial.reference.wall_ns<<" retired="<<device.worker_retired<<'\n';
  if(stop_waiting) {
    require(trial.status==HipDiscoveryStatus::cancelled && !device.sample && device.worker_retired,
        "Shutdown did not cancel and retire the parked worker");
  }else {
    require(trial.status==HipDiscoveryStatus::ready && device.sample && device.matched_cpu && device.worker_retired &&
        device.admitted_requests==expected && trial.reference.sample.transactions==expected &&
        device.sample->cpu_readback_verified && device.sample->transactions==expected &&
        device.sample->input_digest==trial.reference.sample.input_digest &&
        trial.reference.input_bytes==expected_bytes && device.verified_input_bytes==expected_bytes &&
        (public_input.empty()?device.sample->stored_lz4_candidates==expected_bytes/kNativeExtentBytes:
            device.sample->stored_lz4_candidates<=expected_bytes/kNativeExtentBytes),
        "Arriving worker did not verify the same writes as CPU");
    if(geometry) {
      const auto now=GetTickCount64();
      const auto eligible=trial.next_eligible_ms;
      require(eligible>now && eligible-now<=900'000,"Geometry trial pacing outside the fixture's runtime budget");
      std::cout<<"write_geometry first_bytes="<<initial.size()<<" next_eligible_ms="<<eligible
          <<" cooldown_ms="<<eligible-now<<std::endl;
      commit(L"\\before-due",following,kNativeAmdWriteMinimumPairs);
      require(GetTickCount64()<eligible && owner.view().amd_write_trial_attempts==1,
          "Second write shape bypassed the shared pacing gate");
      while(GetTickCount64()<eligible)Sleep(1000);
      commit(L"\\repeat-small",initial,kNativeAmdWriteMinimumPairs);
      Sleep(2000);
      require(owner.view().amd_write_trial_attempts==1,"Completed small shape consumed a second device trial");
      // The before-due writes filled this size's CPU history. Keep this last
      // acknowledged handle quiet until its comparison has settled.
      const auto large_handle=commit(L"\\large",following,1,false);
      await([&] {
        const auto state=owner.view();
        return state.amd_write_trial_attempts>=2 && state.amd_write_trial &&
            state.amd_write_trial->reference.demand.shape.input_bytes==following.size();
      },18000,"Larger write did not settle on the same controller");
      const auto state=owner.view();
      const auto& large_trial=*state.amd_write_trial;
      const auto& result=large_trial.device;
      std::cout<<"write_geometry attempts="<<state.amd_write_trial_attempts<<" status="<<static_cast<unsigned>(large_trial.status)
          <<" verified_input_bytes="<<result.verified_input_bytes<<" exchange="<<static_cast<unsigned>(result.exchange)
          <<" worker_exit="<<result.worker_exit<<" readiness_ns="<<result.ready_ns<<" discovery_ns="<<large_trial.discovery_ns
          <<" device_wall_ns="<<large_trial.device_wall_ns
          <<" cpu_wall_ns="<<large_trial.reference.wall_ns<<" retired="<<result.worker_retired<<std::endl;
      callback_cleanup_close(p,large_handle,true);
      require(state.amd_write_trial_attempts==2 && !state.amd_write_waiting && !state.foreground_activation &&
          large_trial.status==HipDiscoveryStatus::ready && result.sample && result.worker_retired &&
          result.sample->device_executed && result.sample->cpu_readback_verified && !result.admitted_requests &&
          result.verified_input_bytes==following.size() && large_trial.reference.input_bytes==following.size() &&
          result.sample->transactions==1 && result.sample->input_digest==large_trial.reference.sample.input_digest,
          "Larger write did not verify as a sealed comparison");
    }
    owner.stop_and_join();
  }
  const auto snapshot=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,image_capacity});
  snapshot.verify_all();
  require(snapshot.read("first",0,initial.size())==initial &&
      (!later || snapshot.read("later",0,following.size())==following) &&
      (!geometry || (snapshot.read("before-due",0,following.size())==following &&
          snapshot.read("repeat-small",0,initial.size())==initial && snapshot.read("large",0,following.size())==following)) &&
      WorkerExchangeSession::available(),"Original acknowledged writes did not survive the arriving trial");
  std::cout<<"arriving_originals verified=1 source_files="<<(geometry?4:later?2:1)<<" retained_after_stop=0\n";
}
#endif

enum class AmdWriteReplayMode {cpu,paired,interrupt};
void amd_write_cpu_replay(bool controller_owned,bool grouped=false,AmdWriteReplayMode mode=AmdWriteReplayMode::cpu) {
  if(!gpu_lz4_cpu_verification_available())return;
  constexpr std::uint64_t image_capacity=8ULL*1024*1024,allowance=32ULL*1024*1024;
  auto domain=MemoryVolumeDomain::create(image_capacity+kVolumeHeadBytes+allowance);
  auto head=domain.create_store(kVolumeHeadBytes);
  auto image=domain.create_store(image_capacity);
  NativeMaintenancePolicy policy;
  policy.maximum_cpu_busy_ppm=500'000;
  auto source=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),{image_capacity},
      {StoreCompressionMode::fast,1,false},{},policy);
  auto& adapter=*source;
  Provider p;
  Provider::active=&p;
  std::optional<WinFspVolumeController> owner;
  if(controller_owned) {
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
    require(installed_uniform_write_files_present(),"CPU replay needs the verified staged uniform package");
#endif
    owner.emplace(std::move(source),p.api(),plan(),domain);
    await([&]{return owner->view().state==SessionState::running;},5000,"CPU replay controller did not start");
  } else {
    require(adapter.enable_amd_write_demand(),"CPU replay demand not enabled");
    p.fs.Version=static_cast<UINT16>(sizeof(p.fs));
    p.fs.UmFileContextIsUserContext2=TRUE;
    p.fs.Interface=&WinFspNativeWritable::callbacks();
    adapter.bind_callbacks(p.fs,Provider::origin);
  }
  const auto* table=p.fs.Interface;
  auto security=native_security_fixture::descriptor();
  wchar_t name[]=L"\\replay";
  FSP_FSCTL_FILE_INFO info{};
  PVOID handle=nullptr;
  require(table->Create(&p.fs,name,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&handle,&info)==STATUS_SUCCESS && handle,"CPU replay Create");
  std::uint32_t device_write_extents=8;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if(const auto profile=installed_uniform_write_trial_profile())
    device_write_extents=(std::min)(device_write_extents,profile->maximum_batch_chunks/2);
#endif
  const auto make_input=[&](unsigned seed) {
    if(mode==AmdWriteReplayMode::cpu)return data(2*kNativeExtentBytes,seed);
    const auto prefix=data(kNativeExtentBytes/8,seed);
    Bytes input(device_write_extents*kNativeExtentBytes);
    for(std::size_t offset=0;offset<input.size();offset+=prefix.size())
      std::ranges::copy(prefix,input.begin()+offset);
    return input;
  };
  auto bytes=make_input(912);
  const auto write_to=[&](PVOID target,std::span<const std::byte> input) {
    ULONG count=0;
    require(table->Write(&p.fs,target,const_cast<std::byte*>(input.data()),0,static_cast<ULONG>(input.size()),
        FALSE,FALSE,&count,&info)==STATUS_SUCCESS && count==input.size(),"CPU replay ordinary Write");
  };
  const auto write=[&](std::span<const std::byte> input){write_to(handle,input);};
  Bytes peer,stale,foreground;
  const auto create_peer=[&](wchar_t* path) {
    PVOID target=nullptr;
    require(table->Create(&p.fs,path,FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
        security.data(),0,&target,&info)==STATUS_SUCCESS && target,"CPU group peer Create");
    return target;
  };
  if(grouped) {
    wchar_t peer_name[]=L"\\peer",stale_name[]=L"\\stale";
    peer=make_input(913);
    const auto peer_handle=create_peer(peer_name);
    write_to(peer_handle,peer);
    callback_cleanup_close(p,peer_handle,true);
    stale=make_input(914);
    const auto stale_handle=create_peer(stale_name);
    write_to(stale_handle,stale);
    stale[0]^=std::byte{0xff};
    write_to(stale_handle,std::span(stale).first(1)); // Invalidate the eligible receipt of this inode only.
    callback_cleanup_close(p,stale_handle,true);
  }
  for(std::size_t i=0;i<(grouped?1:kNativeAmdWriteMinimumPairs);++i)write(bytes);
  if(grouped) {
    callback_cleanup_close(p,handle,true);
    handle=nullptr;
  }
  NativeAmdWriteSample sample;
  if(owner) {
    await([&]{const auto view=owner->view();return view.amd_cpu_trial_attempts || view.maintenance_disabled;},
        6000,"Controller did not admit its detached CPU trial");
    require(owner->view().amd_cpu_trial.has_value(),"Controller's admitted CPU trial did not verify");
    const auto measured=owner->view();
    require(measured.amd_cpu_trial_attempts==1 && measured.amd_cpu_trial->demand.shape.input_bytes==bytes.size() &&
        measured.amd_cpu_trial->input_bytes==bytes.size()*(grouped?2:1),"Controller measured a different size");
    sample=measured.amd_cpu_trial->sample;
    if(!grouped) {
      require(sample.input_digest==detail::record_digest(bytes),"Controller measured a different range");
      write(bytes);
      await([&]{const auto demand=owner->view().amd_write_demand;return demand &&
          demand->sequence>measured.amd_cpu_trial->demand.sequence;},3000,"Repeated write demand did not reach the owner");
      require(owner->view().amd_cpu_trial_attempts==1,"An already recorded shape relaunched its CPU trial");
    }
    if(mode!=AmdWriteReplayMode::cpu) {
      await([&]{return owner->view().amd_write_trial_attempts!=0;},5000,"Write trial did not acquire its owner slot");
      const auto publish_foreground=[&] {
        wchar_t during[]=L"\\foreground";
        const auto writer=create_peer(during);
        foreground=data(521,915);
        write_to(writer,foreground);
        callback_cleanup_close(p,writer,true);
      };
      const bool interrupt=mode==AmdWriteReplayMode::interrupt;
      if(interrupt)publish_foreground();
      await([&]{return owner->view().amd_write_trial.has_value();},15000,"Write trial did not settle");
      const auto paired=owner->view();
      const auto& trial=*paired.amd_write_trial;
      std::cout<<"ordinary_amd_write_trial status="<<static_cast<unsigned>(trial.status)
          <<" attempts="<<paired.amd_write_trial_attempts<<" exchange_code="<<static_cast<unsigned>(trial.device.exchange)
          <<" worker_exit="<<trial.device.worker_exit<<" discovery_ns="<<trial.discovery_ns
          <<" device_wall_ns="<<trial.device_wall_ns<<" preparation_ns="<<trial.device.preparation_ns
          <<" supervised_ns="<<trial.device.supervised_ns<<" ready_ns="<<trial.device.ready_ns
          <<" verification_ns="<<trial.device.verification_ns<<" replay_ns="<<trial.device.replay_ns
          <<" cpu_wall_ns="<<trial.reference.wall_ns<<" bytes="<<trial.reference.input_bytes
          <<" admitted_requests="<<trial.device.admitted_requests<<'\n';
      require(paired.amd_write_trial_attempts==1 && !paired.foreground_activation &&
          trial.reference.sample.input_digest==sample.input_digest && trial.reference.sample.transactions==2,
          "Write trial changed its reference or offered an unqualified route");
      if(interrupt) {
        require(trial.status==HipDiscoveryStatus::cancelled && !trial.device.sample,
            "Foreground did not cancel and discard the private write result");
      } else {
        const auto& device=trial.device;
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
        const auto profile=installed_uniform_write_trial_profile();
        require(profile && device.admitted_requests==(profile->separate_requests?sample.transactions:0),
            "Owner did not use the qualified request admission mode");
#endif
        require(trial.status==HipDiscoveryStatus::ready && device.sample && device.worker_retired &&
            device.sample->device_executed && device.sample->stored_lz4_candidates>0 &&
            device.sample->cpu_readback_verified && device.sample->retirement_confirmed &&
            device.sample->input_digest==sample.input_digest && device.sample->transactions==sample.transactions &&
            device.ready_ns && device.supervised_ns>=device.ready_ns && device.verification_ns && device.replay_ns,
            "Normal owner did not complete and independently recover the exact Radeon replay");
        std::cout<<"ordinary_amd_pair device_extents="<<device.sample->stored_lz4_candidates
            <<" cpu_occupied="<<sample.occupied_bytes<<" gpu_occupied="<<device.sample->occupied_bytes
            <<" cpu_verified=1 retired=1\n";
        publish_foreground();
      }
    }
  } else if(grouped) {
    const auto demand=adapter.take_amd_write_demand();
    require(demand && demand->grouped_candidate,"Closed files did not produce group demand");
    const auto now=GetTickCount64();
    const auto memory=observe_memory_volume_headroom();
    const NativeMaintenanceObservation observation{1,now,memory.physical_total,memory.physical_available,
        memory.commit_total,memory.commit_available,1000,0,1000,true,false};
    std::atomic<bool> stopped{false};
    require(adapter.capture_amd_write_group(*demand,observation,now,adapter.foreground_activity(),stopped,
        true,bytes.size()-1).empty(),"A capture crossed its finite trial grant");
    const auto capture=adapter.capture_amd_write_group(*demand,observation,now,adapter.foreground_activity(),stopped,
        true,2*bytes.size());
    require(capture.size()==2 && capture[0].sequence==1 && capture[0].bytes==peer &&
        capture[1].sequence==demand->source_sequence && capture[1].bytes==bytes,
        "Closed current ranges were lost or a superseded range was captured");
    require(adapter.capture_amd_write_group(*demand,observation,now,adapter.foreground_activity(),stopped).empty(),
        "Consumed grouped receipt was reused");
    const std::array inputs{UniformLz4BatchInput{capture[0].sequence,capture[0].bytes},
        UniformLz4BatchInput{capture[1].sequence,capture[1].bytes}};
    const auto replay=record_native_amd_cpu_batch({demand->shape.isolation_unit,demand->shape.foreground},
        inputs,allowance,&domain,[]{return false;});
    require(replay.has_value(),"Captured private batch could not be reopened");
    sample=*replay;
  } else {
    const auto demand=adapter.take_amd_write_demand();
    require(demand && demand->shape.input_bytes==bytes.size(),"Ordinary demand was missing");
    std::atomic<bool> stopped{true};
    const auto capture=[&](const auto& observed,bool funded) {
      const auto now=GetTickCount64();
      const auto headroom=observe_memory_volume_headroom();
      const auto limit=funded?headroom.physical_available:std::uint64_t{1};
      const NativeMaintenanceObservation resources{1,now,headroom.physical_total,limit,
          headroom.commit_total,headroom.commit_available,1000,0,1000,true,false};
      return adapter.capture_amd_write_source(observed,resources,now,adapter.foreground_activity(),stopped);
    };
    require(capture(*demand,true).empty(),"A stopped caller acquired a source");
    stopped=false;
    require(capture(*demand,false).empty(),"An unfunded caller acquired a source");
    auto wrong=*demand;
    ++wrong.sequence;
    require(capture(wrong,true).empty(),"Different sequence acquired a source");
    const auto captured=capture(*demand,true);
    require(captured==bytes && capture(*demand,true).empty(),"CPU source differed or was consumed twice");
    const auto before=domain.usage();
    const auto cancelled=[] {return true;};
    const auto running=[] {return false;};
    require(!record_native_amd_cpu_trial(demand->shape,captured,1,&domain,running) &&
        !record_native_amd_cpu_trial(demand->shape,captured,allowance,&domain,cancelled),
        "Unfunded or cancelled replay accepted");
    unsigned cancellation_checks=0;
    const auto cancel_after_reservation=[&] {return ++cancellation_checks==2;};
    require(!record_native_amd_cpu_trial(demand->shape,captured,allowance,&domain,cancel_after_reservation) &&
        cancellation_checks>=2 && domain.usage().reserved==before.reserved,
        "Cancellation after reservation retained calibration scratch");
    const auto measured=record_native_amd_cpu_trial(demand->shape,captured,allowance,&domain,running);
    require(measured && measured->input_digest==detail::record_digest(bytes),"Detached CPU trial failed independent readback");
    sample=*measured;
    const auto after=domain.usage();
    require(before.stores==after.stores && before.reserved==after.reserved && before.resident==after.resident,
        "Replay leaked a private store or locked pages");
    write(bytes);
    const auto superseded=adapter.take_amd_write_demand();
    require(superseded.has_value(),"Second ordinary demand missing");
    const auto changed=bytes.front()^std::byte{0xff};
    write(std::span{&changed,1}); // Ineligible size, with a new revision of the same inode.
    require(capture(*superseded,true).empty(),"Superseded write range was captured");
    write(std::span(bytes).first(1));
  }
  require(sample.elapsed_ns && sample.occupied_bytes && sample.cpu_readback_verified &&
      sample.retirement_confirmed && !sample.device_executed && !sample.stored_lz4_candidates,
      "Detached CPU trial returned unproved device or storage facts");
  require(sample.transactions==(grouped?2:1),"Private CPU replay returned an unexpected source count");
  if(handle)callback_cleanup_close(p,handle);
  if(owner) {
    owner->stop_and_join();
    require(owner->view().retired,"CPU trial owner did not retire");
  }else source.reset();
  Provider::active=nullptr;
  const auto recovered=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,image_capacity});
  require(recovered.read("replay",0,static_cast<std::uint32_t>(bytes.size()))==bytes,"Trial changed the ordinary volume");
  if(!foreground.empty())require(recovered.read("foreground",0,static_cast<std::uint32_t>(foreground.size()))==foreground,
      "Bytes acknowledged during or after the write trial did not recover");
  if(grouped) {
    require(recovered.read("peer",0,static_cast<std::uint32_t>(peer.size()))==peer &&
        recovered.read("stale",0,static_cast<std::uint32_t>(stale.size()))==stale,
        "Private trial changed associated live storage");
    const std::array expected{UniformLz4BatchInput{1,peer},UniformLz4BatchInput{3,bytes}};
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto replay=record_native_amd_cpu_batch({system.dwPageSize,{StoreCompressionMode::fast,1,false}},
        expected,allowance,&domain,[]{return false;});
    require(replay && replay->input_digest==sample.input_digest,"Grouped CPU trial used different acknowledged bytes");
    std::cout<<"ordinary_amd_cpu_group controller="<<controller_owned<<" closed=2 stale_refused=1"
        <<" bytes="<<bytes.size()+peer.size()<<" retired=1 gpu_trial="<<(mode!=AmdWriteReplayMode::cpu)<<'\n';
  } else {
    std::cout<<"ordinary_amd_cpu_replay controller="<<controller_owned<<" verified=1 retired=1 gpu_submitted=0"
        <<" bytes="<<bytes.size()<<" private_image_bytes="<<sample.occupied_bytes<<" cpu_ns="<<sample.elapsed_ns<<'\n';
  }
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
    using Operation=detail::MaintenanceBackingOperation;
    const auto& backing_use=maintenance.maintenance_backing;
    const auto read_bytes=backing_use[Operation::read].completed_bytes;
    const auto writ=backing_use[Operation::write];
    const auto barriers=backing_use[Operation::flush].completed;
    require(!maintenance.reclamation_quanta || (read_bytes && writ.completed_bytes && barriers &&
        writ.attempted_bytes>=writ.completed_bytes && backing_use.elapsed_ns()>0),
        "mounted controller reclamation bypassed backing measurement");
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
        <<" max_quantum_ns="<<maintenance.maximum_quantum_ns
        <<" backing_read_bytes="<<read_bytes<<" backing_written_bytes="<<writ.completed_bytes
        <<" backing_flushes="<<barriers<<" backed_ns="<<backing_use.elapsed_ns()
        <<" backing_limited="<<maintenance.backing_limited_continuations<<" closed_session_recovered=1\n";
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

void indexed_group_callback_contract(bool scrub_due=false) {
  Provider provider;Provider::active=&provider;
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>();
  head->failure_unit=image->failure_unit=4096;
  NativeMaintenancePolicy group_policy;
  if(scrub_due)group_policy.maximum_cpu_busy_ppm=500000;
  auto adapter=WinFspNativeWritable::create_managed_indexed(head,id(90),image,id(1),
      {kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false},{},group_policy);
  WinFspNativeSession session(std::move(adapter),provider.api());
  require(session.start(plan())==STATUS_SUCCESS && !session.background_maintenance_pending(),
      "empty indexed adapter requested a group");
  const auto first=data(kNativeExtentBytes,739);auto second=first;second[101]^=std::byte{13};
  callback_file(provider,0,first);callback_file(provider,1,second);
  NativeMaintenanceObservation observed;
  observed.valid=true;observed.physical_total=16ULL<<30;observed.physical_available=12ULL<<30;
  observed.commit_limit=32ULL<<30;observed.commit_available=24ULL<<30;
  observed.cpu_total_ticks=1000;observed.cpu_busy_ticks=scrub_due?501:126;
  observed.cpu_window_ms=1000;observed.sequence=1;observed.captured_ms=1000;
  const auto reads=image->read_calls,calls=image->calls;
  const auto deferred=session.advance_background_maintenance(observed,observed.captured_ms,{},
      WinFspMaintenanceContinuation::none,scrub_due);
  require(deferred.progress.deferral==NativeMaintenanceDeferral::cpu &&
      image->read_calls==reads && image->calls==calls,"busy indexed adapter did backing work");
  observed.cpu_busy_ticks=scrub_due?200:0;
  WinFspMaintenanceResult state;
  for(unsigned i=0;session.background_maintenance_pending() && i<1000;++i) {
    ++observed.sequence;observed.captured_ms+=observed.cpu_window_ms;
    state=session.advance_background_maintenance(observed,observed.captured_ms,{},
        WinFspMaintenanceContinuation::none,scrub_due);
    require(state.progress.phase!=NativeMaintenancePhase::failed,"indexed adapter grouping failed");
    require(!scrub_due || state.payload_scrub.state==PagedPayloadScrubState::idle,
        "group admission ran scrub above its CPU limit");
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
      <<" lower_bound_savings="<<state.progress.minimum_compacted_bytes_saved
      <<" scrub_due="<<scrub_due<<'\n';
}
}
int main(int argc,char** argv){try {
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if(argc==2 && std::string_view(argv[1])=="--test-anchor-launch-budget") {
    anchor_launch_budget();
    return 0;
  }
#endif
  const bool profile_installed_read=argc==2 && std::string_view(argv[1])=="--profile-installed-native-read";
  const bool profile_existing_prefix=argc==7 && std::string_view(argv[1])=="--live-anchor-owner" &&
      std::string_view(argv[6])=="prefix-extension";
  const bool profile_automatic_sealed_read=argc==11 &&
      (std::string_view(argv[1])=="--live-amd-automatic-sealed-read" ||
       std::string_view(argv[1])=="--live-amd-automatic-sealed-refusal");
  const bool profile_paired_sealed_read=argc==11 &&
      (std::string_view(argv[1])=="--live-amd-paired-sealed-read" ||
       std::string_view(argv[1])=="--live-amd-paired-sealed-refusal" ||
       std::string_view(argv[1])=="--live-amd-dense-sealed-read" ||
       std::string_view(argv[1])=="--live-amd-dense-sealed-refusal");
  app_fixture::Budget budget(true,profile_installed_read || profile_existing_prefix ||
      profile_automatic_sealed_read || profile_paired_sealed_read);DWORD before=0,after=0;
  if(argc==2 && std::string_view(argv[1])=="--paired-seal-workspaces") {
    for(const auto unit:{4096U,65536U}) {
      const auto file=volume_file_sealed_callbacks(PairedWorkspaceFixture::file,unit);
      const auto memory=volume_file_sealed_callbacks(PairedWorkspaceFixture::locked_memory,unit);
      const auto denied=volume_file_sealed_callbacks(PairedWorkspaceFixture::denied_memory,unit);
      require(file.allocated==memory.allocated && file.allocated==denied.allocated &&
          memory.written<file.written && file.written==denied.written,
          "scratch placement changed allocation or did not reduce backing writes");
    }
    return 0;
  }
  if(argc==11) {
    const std::string_view command(argv[1]);
    if(command=="--live-amd-read-ahead" || command=="--live-amd-read-refusal")
      return live_amd_read_ahead(argv,command=="--live-amd-read-refusal");
    if(command=="--live-amd-sealed-read-ahead" || command=="--live-amd-sealed-read-refusal")
      return live_amd_read_ahead(argv,command=="--live-amd-sealed-read-refusal",AmdReadFixtureLayout::prepared);
    if(command=="--live-amd-automatic-sealed-read" || command=="--live-amd-automatic-sealed-refusal")
      return live_amd_read_ahead(argv,command=="--live-amd-automatic-sealed-refusal",AmdReadFixtureLayout::automatic_sealed);
    if(command=="--live-amd-dense-sealed-read" || command=="--live-amd-dense-sealed-refusal")
      return live_amd_read_ahead(argv,command=="--live-amd-dense-sealed-refusal",AmdReadFixtureLayout::reopened_dense);
    if(profile_paired_sealed_read)
      return live_amd_read_ahead(argv,command=="--live-amd-paired-sealed-refusal",AmdReadFixtureLayout::reopened_pair);
  }
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
  if(profile_installed_read || (argc==2 && std::string_view(argv[1])=="--live-installed-native-read"))
    return live_installed_native_read(false);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-native-read-missing")
    return live_installed_native_read(true);
  if(argc==1)anchor_launch_budget();
  if(argc==2 && std::string_view(argv[1])=="--live-installed-retired-reply")
    return live_installed_retired_reply(BatchForegroundTrial::read_after_retirement);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-adaptive-reply")
    return live_installed_retired_reply(BatchForegroundTrial::read_after_retirement,true);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-retired-reply-or-cpu")
    return live_installed_retired_reply(BatchForegroundTrial::read_or_cpu);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-adaptive-or-cpu")
    return live_installed_retired_reply(BatchForegroundTrial::read_or_cpu,true);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-recovery-check")
    return live_installed_recovery_check();
  if(argc==2 && std::string_view(argv[1])=="--live-installed-deadline-recovery")
    return live_installed_retired_reply(BatchForegroundTrial::recover_after_deadline);
  if(argc==2 && std::string_view(argv[1])=="--live-installed-retired-edit")
    return live_installed_retired_reply(BatchForegroundTrial::edit_after_retirement);
  if((argc==7 || (argc==8 && std::string_view(argv[6])=="mixed-setup") ||
      ((argc==8 || argc==9) && multiwindow_pattern(argv[6]))) &&
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
        mode=="restored" || mode=="queued" || mode=="verified-read" || mode=="verified-edit" || mode=="device-not-ready" ||
        mode=="reference-xor" || mode=="reference-missing",
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
  // Measure native process caches on a separate PDH-only path before checking
  // that every filesystem fixture returns to the same handle baseline.
  const bool lifetime_only=argc==2 && std::string_view(argv[1])=="--owned-lifetime";
  const bool shifted_grouping=argc==2 && std::string_view(argv[1])=="--live-indexed-shifted-grouping";
  const bool checkpoint=argc==2 && std::string_view(argv[1])=="--live-indexed-checkpoint";
  initialize_pdh_handles();
  // The system-preferred RNG initializes process-wide native handles on first
  // use. Prove native-only stabilization before measuring factory ownership.
  const bool remount=argc==2 && std::string_view(argv[1])=="--memory-remount";
  const bool volume_file=argc==2 && std::string_view(argv[1])=="--volume-file";
  const bool indexed_startup=argc==2 && std::string_view(argv[1])=="--indexed-startup-profile";
  const bool read_ahead=argc==2 && std::string_view(argv[1])=="--live-read-ahead";
  const bool amd_cpu_replay=argc==2 && std::string_view(argv[1])=="--amd-write-cpu-replay";
  const bool amd_cpu_group=argc==2 && std::string_view(argv[1])=="--amd-write-cpu-group";
  const bool amd_write_pair=argc==2 && std::string_view(argv[1])=="--amd-write-pair";
  const bool amd_write_interrupt=argc==2 && std::string_view(argv[1])=="--amd-write-pair-interrupt";
  const bool amd_write_arrival=(argc==3 || argc==4) && std::string_view(argv[1])=="--amd-write-arrival";
  const bool screened_public=argc==3 && std::string_view(argv[1])=="--screened-cold-corpus";
  const bool screened_packed=argc==3 && std::string_view(argv[1])=="--screened-cold-packed";
  const bool live_repair_rotation=argc==2 && std::string_view(argv[1])=="--live-indexed-repair-rotation";
  const bool live_payload_scrub=argc==2 && std::string_view(argv[1])=="--live-indexed-scrub";
  const bool live_payload_repair=live_repair_rotation || live_payload_scrub ||
      (argc==2 && std::string_view(argv[1])=="--live-indexed-repair");
  const bool reconciled_compaction=argc==2 && std::string_view(argv[1])=="--live-indexed-reconcile";
  const bool indexed_compaction=reconciled_compaction || (argc==2 && std::string_view(argv[1])=="--live-indexed-compaction");
  if(lifetime_only || shifted_grouping || checkpoint || remount || volume_file || screened_public || screened_packed || indexed_compaction ||
      live_payload_repair || indexed_startup || read_ahead || amd_cpu_replay || amd_cpu_group || amd_write_pair ||
      amd_write_interrupt || amd_write_arrival ||
      (argc==2 && (std::string_view(argv[1])=="--memory" || std::string_view(argv[1])=="--memory-handles" ||
      std::string_view(argv[1])=="--read-only-memory" || std::string_view(argv[1])=="--indexed-memory" ||
      std::string_view(argv[1])=="--live-indexed-grouping")))
    native_rng_initialization();
  // A long native PDH-only control initializes process-wide OS handles
  // before the explicit long-volume mode measures controller ownership.
  if(lifetime_only || indexed_compaction)initialize_long_pdh_window();
  require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"initial handle count");
  if(argc==1){windows_processor_windows();backing_maintenance_window();event_ownership();lifecycle();idle_until_owner_stop(100);invalid_and_unexpected();live_opportunities();}
  else if(lifetime_only)idle_until_owner_stop(31000);
  else if(read_ahead)live_read_ahead_admission();
  else if(amd_cpu_replay || amd_cpu_group)amd_write_cpu_replay(true,amd_cpu_group);
  else if(amd_write_arrival) {
#ifdef FLITZFS_INDEXED_HIP_FIXTURE
    require(gpu_lz4_cpu_verification_available(),"Arriving Radeon trial requires independent CPU verification");
    const std::filesystem::path input=argc==4?argv[3]:"";
    amd_write_arrival_trial(argv[2],input);
    before=handle_count();
    if(std::string_view(argv[2])!="geometry")amd_write_arrival_trial(argv[2],input);
#else
    throw std::runtime_error("Arriving write trial package is unavailable");
#endif
  } else if(amd_write_pair || amd_write_interrupt) {
    require(gpu_lz4_cpu_verification_available(),"Radeon trial requires independent CPU verification");
    const auto mode=amd_write_interrupt?AmdWriteReplayMode::interrupt:AmdWriteReplayMode::paired;
    amd_write_cpu_replay(true,true,mode);
    const auto settled=handle_count();
    std::cout<<"amd_write_handles cold="<<before<<" first_retired="<<settled<<'\n';
    before=settled;
    amd_write_cpu_replay(true,true,mode);
  } else if(argc==2 && std::string_view(argv[1])=="--memory") {
    memory_planning();memory_paged_footprint();memory_hybrid_placement();automatic_memory_sealing(false);
    automatic_memory_sealing(true);automatic_memory_sealing(false,true);
    memory_compaction_retirement();memory_capacity_repair();
    memory_capacity_repair(true);
    std::cout<<"memory_handles start="<<handle_count()<<'\n';
    memory_backing_ownership();std::cout<<"memory_handles backing="<<handle_count()<<'\n';
    memory_composition();std::cout<<"memory_handles rollback="<<handle_count()<<'\n';
    live_memory_composition();std::cout<<"memory_handles live="<<handle_count()<<'\n';
  }else if(argc==2 && std::string_view(argv[1])=="--indexed-memory") {
    memory_composition(NativeImageKind::indexed_paged);
    indexed_memory_composition();
    concurrent_memory_store_accounting();
    amd_write_cpu_replay(false);
    amd_write_cpu_replay(false,true);
    if(store_lz4_available())capture_during_hybrid_tail_write();
    indexed_group_callback_contract();
    indexed_group_callback_contract(true);
    memory_coherence_lifecycle();
    const auto [before_fence,after_fence]=indexed_windows_reopen_profile();
    require(before_fence && after_fence && before_fence<=2*after_fence,
        "Windows reopen repeated the verified namespace traversal");
  }else if(indexed_startup) {
    (void)indexed_windows_reopen_profile();
  }else if(volume_file) {
    volume_file_callbacks();
    dense_file_retirement_with_due_scrub();
    volume_file_scrub_checkpoint();
    indexed_reopen_platform_admission();
    volume_file_idle_reclamation();
    volume_file_sealed_callbacks();
    if(store_zstd_available()) {
      const std::array fixture{cold_test_payload(),cold_test_payload(130011),Bytes(640*1024)};
      volume_file_screened_cold(fixture,{StoreCompressionMode::fast,1,false});
      std::array<Bytes,9> varied;
      for(unsigned i=0;i+1<varied.size();++i)varied[i]=cold_test_payload(130011*i,997);
      // A sparse write supplies acknowledged input without a second large
      // payload. The cold candidate is affordable before the scan charge.
      varied.back().resize(3*1024*1024);
      volume_file_screened_cold(varied,{StoreCompressionMode::fast,1,false},ColdFixtureMode::shared_budget);
    }
    volume_file_payload_repair(false);
    volume_file_payload_repair(false,RepairFixture::scrub);
    volume_file_payload_repair(false,RepairFixture::startup_rotation);
  }else if(screened_public) {
    auto source=ReadOnlyFile::open(argv[2]);
    require(store_zstd_available() && source.size()>0 && source.size()<=2*1024*1024,"screened corpus admission");
    std::array fixture{Bytes(static_cast<std::size_t>(source.size()))};
    source.read_exact_at(0,fixture[0].data(),fixture[0].size());
    volume_file_screened_cold(fixture,{StoreCompressionMode::automatic,3,true});
  }else if(screened_packed) {
    require(store_zstd_available(),"screened packed corpus requires Zstandard");
    const auto fixture=read_packed_corpus(argv[2],true);
    volume_file_screened_cold(fixture,{StoreCompressionMode::automatic,3,true},ColdFixtureMode::public_corpus);
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
  std::cout<<"PASS: "<<checks<<" owned controller checks; real local telemetry and SDK callbacks; injected lifecycle; "
      <<(amd_write_pair || amd_write_interrupt?"scoped Radeon trial mode":"CPU mode")<<'\n';
  return 0;
}catch(const std::exception& e){std::cerr<<"after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
