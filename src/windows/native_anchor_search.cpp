#include "native_anchor_launch.hpp"
#include "anchor_batch_profile.hpp"
#include "gpu_budget.hpp"
#include "gpu_worker_common.hpp"
#include <algorithm>
#include <dxgi.h>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace flitzfs {
namespace {
template<std::size_t N>std::wstring hex(const std::array<std::byte,N>& bytes) {
  constexpr wchar_t digits[]=L"0123456789abcdef";
  std::wstring result;
  result.reserve(2*N);
  for(const auto byte:bytes) {
    const auto value=std::to_integer<unsigned>(byte);
    result.push_back(digits[value>>4]);
    result.push_back(digits[value&15]);
  }
  return result;
}
constexpr std::uint64_t mib=1024*1024,million=1000000;
constexpr DWORD retirement_grace_ms=2000;

bool valid_dispatch_grant(const NativeAnchorSearchOptions& options) noexcept {
  if(options.output!=anchor_match::BatchOutput::wide_anchors)return true;
  const auto members=options.maximum_dispatch_members;
  return members>anchor_match::max_members && members<=options.maximum_members &&
      members<=detail::anchor_match_batch_member_limit();
}
// The measured setup is useful when the same dispatch and full-group staging
// can serve every group. A narrower slab can change the number of transfers.
std::uint32_t setup_group_size(const detail::AnchorMatchBatchPlan& batch,
    const detail::AnchorMatchTransferPlan& transfer,const NativeAnchorSearchOptions& options) noexcept {
  if(batch.groups.empty() || !anchor_match::copy_output(batch.output))return 0;
  const auto members=batch.groups.front().whole.members;
  const auto codec_width=anchor_match::dispatch_member_limit(batch.output);
  const auto configured_width=options.maximum_dispatch_members?options.maximum_dispatch_members:codec_width;
  if(transfer.members_per_transfer!=members || batch.quantum_members!=(std::min)({members,codec_width,configured_width}) ||
      !std::ranges::all_of(batch.groups,[members](const auto& group){return group.whole.members==members;}))return 0;
  return members;
}
bool valid_anchor_route(const HipDiscoveredRoute& route) noexcept {
  return route.ordinal<64 && route.pins && route.pins->anchor_module_pinned() &&
      route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory);
}
std::vector<std::wstring> anchor_worker_arguments(const HipDiscoveredRoute& route,
    const NativeAnchorLaunchPlan& launch) {
  const auto& plan=launch.batch;
  const auto& capacity=launch.capacity;
  using Output=anchor_match::BatchOutput;
  const auto command=plan.output==Output::reference_xor?L"--driver-anchor-reference-batch":
      plan.output==Output::collective?L"--driver-anchor-collective-batch":
      plan.output==Output::sparse_xor?L"--driver-anchor-sparse-xor-batch":
      plan.output==Output::modular_delta?L"--driver-anchor-modular-delta-batch":
      plan.output==Output::wide_packed_xor?L"--driver-anchor-xor-wide-batch":
      plan.output==Output::packed_xor?L"--driver-anchor-xor-batch":
      plan.output==Output::wide_anchors?
          (launch.profiled?L"--driver-anchor-wide-transfer-profile":L"--driver-anchor-wide-transfer-batch"):
      launch.profiled?L"--driver-anchor-transfer-batch-profile":L"--driver-anchor-transfer-batch";
  std::vector<std::wstring> arguments{command,std::to_wstring(route.ordinal),hex(route.memory.luid),
      std::to_wstring(route.memory.node_mask),hex(route.runtime_sha256),std::to_wstring(capacity.request_bytes),
      std::to_wstring(capacity.reply_bytes),std::to_wstring(plan.quantum_members),std::to_wstring(plan.device_bytes),
      std::to_wstring(plan.launches),std::to_wstring(launch.transfer.device_bytes)};
  if(launch.profiled)arguments.push_back(std::to_wstring(launch.dispatches_per_wait));
  return arguments;
}
std::optional<NativeAnchorLaunchPlan> plan_anchor_output(
    std::span<const std::uint32_t> members, const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed, std::uint64_t now, bool permits_reference_change,
    anchor_match::BatchOutput output,std::optional<std::uint32_t> launch_limit,bool reference_required=false) {
  const auto total = std::accumulate(members.begin(), members.end(), std::uint64_t{0});
  if (!options.minimum_members || !valid_dispatch_grant(options) || total < options.minimum_members ||
      total > options.maximum_members || total > detail::anchor_match_batch_member_limit() ||
      std::ranges::find(members, 0U) != members.end()) return {};
  const auto largest = *std::ranges::max_element(members);
  (void)detail::anchor_match_layout(largest, output);

  const auto& identity = options.route.memory;
  std::uint32_t fit = 0, limit = largest;
  while (fit < limit) {
    const auto trial = fit + (limit - fit + 1) / 2;
    if (check_gpu_memory_budget(observed, identity.luid, identity.node_mask, now,
        anchor_match::device_bytes(trial, output)) == GpuGateCode::admitted) fit = trial;
    else limit = trial - 1;
  }
  if (!fit || (reference_required && fit != largest)) return {};

  using Output = anchor_match::BatchOutput;
  const bool reference = output == Output::reference_xor;
  const auto baseline = reference ? Output::sparse_xor : output;
  const auto dispatch_limit = anchor_match::dispatch_member_limit(output);
  const auto qualified_limit = options.maximum_dispatch_members ? options.maximum_dispatch_members : dispatch_limit;
  const auto quantum = (std::min)({fit, dispatch_limit, qualified_limit});
  const auto available = (std::min)(observed.physical_available, observed.commit_available);
  const auto budget_for = [&](const detail::AnchorMatchTransferPlan& transfer, WorkerChannelCapacity channel) {
    const auto variable = std::uint64_t(transfer.device_bytes) + 2ULL * channel.request_bytes +
        2ULL * channel.reply_bytes;
    constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (options.worker.memory_mib > (maximum - variable) / mib) return ProcessBudget{0};
    const auto child = (std::max)(options.worker.memory_mib * mib + variable,
        6ULL * channel.request_bytes + 4ULL * channel.reply_bytes + mib);
    const auto budget = plan_resource_process_budget(child, observed.physical_available,
        observed.commit_available, options.worker.memory_share_divisor, options.worker.cpu_rate,
        options.worker.timeout_ms);
    const auto required = required_sized_worker_headroom(channel, budget);
    return required && required <= available ? budget : ProcessBudget{0};
  };

  const auto candidate = [&](std::span<const std::uint32_t> prefix) -> std::optional<NativeAnchorLaunchPlan> {
    try {
      // Keep the learned grant constant during prefix search so adding a
      // group cannot make a previously infeasible reference plan fit.
      const auto launch_grant=launch_limit?
          (std::min)(static_cast<std::uint32_t>(total+members.size()),*launch_limit):
          std::accumulate(prefix.begin(),prefix.end(),static_cast<std::uint32_t>(prefix.size()));
      auto batch = detail::plan_anchor_match_batch(prefix, quantum,
          anchor_match::device_bytes(quantum, baseline), launch_grant, baseline);
      if (reference && fit == largest && permits_reference_change &&
          launch_grant - batch.launches >= 2 * detail::anchor_reference_pricing_launches(batch))
        batch = detail::plan_anchor_match_batch(prefix, quantum,
            anchor_match::device_bytes(quantum, output), launch_grant, output);
      else if(reference_required)return {};
      const auto transfer = detail::plan_anchor_match_transfers(batch, anchor_match::device_bytes(fit, batch.output));
      WorkerChannelCapacity capacity{batch.request_bytes, batch.reply_bytes};
      auto budget = budget_for(transfer, capacity);
      if (!valid_process_budget(budget)) return {};
      std::uint32_t queued = 1;
      bool profiled = false;
      const auto admitted_dispatches = 1 + (transfer.members_per_transfer - 1) / batch.quantum_members;
      const auto queue_limit = (std::min)(options.dispatches_per_wait, admitted_dispatches);
      const auto serial_profile=(options.adaptive_batch_sizing || options.profile_copy_batches) &&
          options.dispatches_per_wait==1 && !options.prefer_reference && anchor_match::copy_output(batch.output);
      if ((queue_limit > 1 || serial_profile) &&
          options.worker.timeout_ms <= kAnchorSweepProfileLimitNs / million) {
        auto measured_capacity = capacity;
        measured_capacity.reply_bytes += kAnchorBatchProfileBytes;
        if (const auto planned = budget_for(transfer, measured_capacity); valid_process_budget(planned)) {
          capacity = measured_capacity;
          budget = planned;
          queued = queue_limit;
          profiled = true;
        }
      }
      return NativeAnchorLaunchPlan{std::move(batch), transfer, capacity, budget, queued, profiled};
    } catch (const std::invalid_argument&) {
      // A prefix may fit the checked wire capacity when the staged batch does not.
      return {};
    }
  };

  auto accepted = candidate(members);
  if (accepted || (reference && !launch_limit)) return accepted;
  // For a fixed output and dispatch limit, adding a group cannot reduce the
  // base transport or transfer allocation. Profile trailers are optional.
  std::size_t first = 0;
  std::uint32_t eligible = 0;
  while (eligible < options.minimum_members) eligible += members[first++];
  auto last = members.size() - 1;
  while (first <= last) {
    const auto middle = first + (last - first) / 2;
    if (auto planned = candidate(members.first(middle))) {
      accepted = std::move(planned);
      first = middle + 1;
    } else last = middle - 1;
  }
  return accepted;
}
}

std::optional<NativeAnchorLaunchPlan> plan_native_anchor_launch(
    std::span<const std::uint32_t> members, const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed, std::uint64_t now, bool permits_reference_change,
    std::optional<std::uint32_t> launch_limit) {
  if(options.prefer_reference && permits_reference_change) {
    try {
      if(auto reference=plan_anchor_output(members,options,observed,now,true,anchor_match::BatchOutput::reference_xor,
          launch_limit,true))
        return reference;
    } catch(const std::invalid_argument&) {
      // The larger reference reply can exceed the format's admission limit.
    }
  }
  return plan_anchor_output(members,options,observed,now,permits_reference_change,options.output,launch_limit);
}

void NativeAnchorBatchTiming::record_verified(std::uint64_t elapsed_ms,std::uint32_t launches,
    std::uint64_t setup_ns,std::uint32_t group_members) noexcept {
  if(!elapsed_ms || !launches)return;
  // Job durations are normally bounded in seconds. Cap the conversion even
  // if the supervisor did not regain a CPU promptly after the child exited.
  const auto microseconds=(std::min)(elapsed_ms,std::uint64_t{UINT32_MAX})*1000;
  const auto retain_peak=[](std::uint64_t old,std::uint64_t observed) {
    return (std::max)(observed,old-old/8);
  };
  microseconds_per_launch_=retain_peak(microseconds_per_launch_,(microseconds+launches-1)/launches);
  const auto measured_setup=setup_ns/1000;
  if(!group_members || !measured_setup || measured_setup>=microseconds) {
    calibrated_group_=0;
    setup_us_=incremental_us_per_launch_=0;
    return;
  }
  const auto remaining=microseconds-measured_setup;
  setup_us_=retain_peak(setup_us_,measured_setup);
  incremental_us_per_launch_=retain_peak(incremental_us_per_launch_,(remaining+launches-1)/launches);
  calibrated_group_=group_members;
}
std::optional<std::uint32_t> NativeAnchorBatchTiming::launch_limit(std::uint32_t hard_timeout_ms,
    std::optional<std::uint32_t> group_members) const noexcept {
  if(!microseconds_per_launch_)return {};
  // Leave one third of the hard deadline for contention and variability.
  const auto available=std::uint64_t(hard_timeout_ms)*1000*2/3;
  if(setup_us_>=available)return 0;
  const auto calibrated=calibrated_group_ && (!group_members || *group_members==calibrated_group_);
  const auto launches=calibrated?(available-setup_us_)/incremental_us_per_launch_:
      available/microseconds_per_launch_;
  return static_cast<std::uint32_t>((std::min)(launches,std::uint64_t{UINT32_MAX}));
}

std::optional<NativeAnchorLaunchPlan> plan_timed_native_anchor_launch(
    std::span<const std::uint32_t> members,const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed,std::uint64_t now,bool permits_reference_change,
    const NativeAnchorBatchTiming& timing) {
  if(members.empty())return {};
  const auto first=members.front();
  const auto complete_limit=timing.launch_limit(options.worker.timeout_ms,0);
  const auto calibrated_limit=timing.launch_limit(options.worker.timeout_ms,first);
  if(complete_limit==calibrated_limit)
    return plan_native_anchor_launch(members,options,observed,now,permits_reference_change,complete_limit);

  const auto different=std::ranges::find_if(members,[first](auto count){return count!=first;});
  const auto matching=static_cast<std::size_t>(different-members.begin());
  if(matching<members.size()) {
    auto mixed=plan_native_anchor_launch(members,options,observed,now,permits_reference_change,complete_limit);
    if(mixed && mixed->batch.groups.size()>matching)return mixed;
  }
  auto uniform=plan_native_anchor_launch(members.first(matching),options,observed,now,permits_reference_change,calibrated_limit);
  if(uniform && !setup_group_size(uniform->batch,uniform->transfer,options))
    return plan_native_anchor_launch(members.first(matching),options,observed,now,permits_reference_change,complete_limit);
  return uniform;
}

NativeAnchorRecoveryCheck qualify_native_anchor_recovery(NativeAnchorRecoveryTarget target,
    const HipDiscoveredRoute& route,ProcessBudget worker,std::stop_token stop) noexcept {
  using Status=HipDiscoveryStatus;
  if(stop.stop_requested())return {Status::cancelled};
  if(!target.incident || target.identity!=route.identity || !valid_anchor_route(route))return {};
  if(!WorkerExchangeSession::available())return {Status::resource_deferred};
  try {
    if(!route.pins->device_ready())return {Status::device_not_ready};
    const auto observed=query_windows_gpu_budget(route.memory.luid,route.memory.node_mask);
    if(observed.status!=WindowsGpuBudgetStatus::success)return {Status::resource_deferred};
    const NativeAnchorSearchOptions options{.route=route,.worker=worker,.minimum_members=1,.maximum_members=1,
        .adaptive_batch_sizing=false};
    const std::array<std::uint32_t,1> members{1};
    const auto launch=plan_native_anchor_launch(members,options,observed.snapshot,GetTickCount64(),false);
    if(!launch)return {Status::resource_deferred};
    std::vector<std::byte> base(kNativeExtentBytes);
    std::uint32_t state=1;
    for(auto& byte:base) {
      state=state*1664525u+1013904223u;
      byte=static_cast<std::byte>(state>>24);
    }
    auto revised=base;
    revised[revised.size()/2]^=std::byte{1};
    detail::AnchorMatchBatchBuilder builder(target.incident,launch->batch);
    builder.append(1,base,revised);
    const auto request=builder.finish();
    const auto source=request.groups().front();
    const std::array records{detail::cpu_anchor_match_records(source)};
    const auto ranges=detail::verify_anchor_match_records(source,records.front());
    if(ranges.empty() || ranges.front().empty())return {};
    const auto expected=detail::encode_anchor_match_batch_reply(request,records);
    const auto arguments=anchor_worker_arguments(route,*launch);
    if(!route.pins->device_ready())return {Status::device_not_ready};
    const auto result=WorkerExchangeSession::run_sized(route.helper,arguments,request.snapshot(),
        launch->capacity,launch->worker,stop,route.pins);
    if(result.code==WorkerExchangeCode::exit_unconfirmed ||
        result.code==WorkerExchangeCode::supervisor_exit_unconfirmed)return {Status::retirement_unconfirmed};
    if(stop.stop_requested() || result.code==WorkerExchangeCode::cancelled)return {Status::cancelled};
    if(result.code!=WorkerExchangeCode::completed || !result.process.exit_confirmed || result.response!=expected)return {};
    if(!route.pins->device_ready())return {Status::device_not_ready};
    return {Status::ready,NativeAnchorRecoveryProof(target,route.pins)};
  }catch(const std::exception&) {
    return {WorkerExchangeSession::available()?Status::resource_deferred:Status::retirement_unconfirmed};
  }
}

struct NativeAnchorSearchController::Impl {
  enum class Disposition {pending,yielded,expired};
  enum class RebindBlock {none,device,hard};
  struct TimingSample {
    std::uint64_t elapsed_ms;
    std::uint32_t launches;
    std::uint64_t setup_ns=0;
    std::uint32_t group_members=0;
  };
  struct Flight {
    WorkerExchangeSession session;
    detail::AnchorMatchBatchRequest request;
    std::optional<AnchorBatchProfileBinding> profile;
    std::uint64_t ticket,started,deadline;
    Disposition disposition=Disposition::pending;
    bool supervisor_exited=false;
  };
  NativeAnchorSearchOptions options;
  GpuRecoveryPolicy recovery;
  std::optional<Flight> flight;
  std::vector<NativeSharedBaseSearchWindow> windows;
  std::vector<detail::AnchorMatchAdvice> advice;
  std::vector<detail::AnchorPackedAdvice> packed_advice;
  std::vector<std::vector<NativePackedGroupCheck>> packed_checks;
  std::vector<NativeSharedBaseSearchReply> replies;
  NativeAnchorSearchStats stats;
  NativeAnchorBatchTiming timing;
  std::optional<TimingSample> packed_timing;
  std::uint64_t activity=0,next_launch=0,sequence=0,unobserved_ticket=0,packed_ticket=0;
  std::uint64_t recovery_serial=0,route_recovery_generation=0;
  RebindBlock rebind_block=RebindBlock::none;
  bool enabled=false,device_interrupted=false,deadline_observed=false,deadline_check_spent=false;
  explicit Impl(NativeAnchorSearchOptions configured):options(std::move(configured)) {
    enabled=valid_anchor_route(options.route) && valid_profile();
  }
  bool valid_profile() const noexcept {
    return options.minimum_members && options.minimum_members<=options.maximum_members &&
        options.minimum_members<=detail::anchor_match_batch_member_limit() &&
        (!anchor_match::compact_output(options.output) || options.maximum_dispatch_members) &&
        (!options.prefer_reference || (anchor_match::copy_output(options.output) && options.maximum_dispatch_members &&
            options.dispatches_per_wait==1)) &&
        options.dispatches_per_wait && options.dispatches_per_wait<=kAnchorProfileMaxQueuedDispatches &&
        (options.dispatches_per_wait==1 || anchor_match::copy_output(options.output)) &&
        valid_dispatch_grant(options) &&
        options.duty_ppm && options.duty_ppm<=100000 &&
        options.worker.memory_share_divisor && valid_process_budget(options.worker);
  }
  void discard() noexcept {
    if(packed_ticket)(void)recovery.abandon(std::exchange(packed_ticket,0),true);
    packed_timing.reset();
    replies.clear();
    windows.clear();
    std::vector<detail::AnchorMatchAdvice>{}.swap(advice);
    std::vector<detail::AnchorPackedAdvice>{}.swap(packed_advice);
    packed_checks.clear();
  }
  void yield(Disposition disposition) noexcept {
    discard();
    if(!flight || flight->disposition!=Disposition::pending)return;
    if(disposition==Disposition::yielded)++stats.yielded;
    else ++stats.declined;
    if(flight->supervisor_exited) {
      (void)recovery.abandon(flight->ticket,true);
      flight.reset();
      return;
    }
    flight->disposition=disposition;
    (void)flight->session.request_cancel();
  }
  void pace(std::uint64_t now,std::uint64_t started) noexcept {
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    const auto used=(std::max)(std::uint64_t{1},now>=started?now-started:std::uint64_t{0});
    const auto delay=(std::min)(used,maximum/million)*million/options.duty_ppm;
    next_launch=delay<=maximum-now?now+delay:maximum;
  }
  void suspend(RebindBlock block,bool retired=true) noexcept {
    enabled=false;
    rebind_block=block;
    deadline_observed=false;
    if(retired)options.route.pins.reset();
  }
  bool check_device(std::uint64_t ticket=0) noexcept {
    if(options.route.pins->device_ready())return true;
    if(ticket)(void)recovery.abandon(ticket,true);
    device_interrupted=true;
    suspend(RebindBlock::none);
    ++stats.declined;
    return false;
  }
  void fail(std::uint64_t ticket,GpuRecoveryOutcome outcome,bool retired,bool deadline=false) noexcept {
    const bool recorded=recovery.finish(ticket,outcome,retired);
    if(outcome==GpuRecoveryOutcome::integrity_failure)++stats.integrity_failures;
    if(outcome==GpuRecoveryOutcome::device_fault && recorded) {
      suspend(RebindBlock::device,retired);
      deadline_observed=deadline && retired;
    }else if(outcome!=GpuRecoveryOutcome::temporary_pressure)suspend(RebindBlock::hard,retired);
  }
  void learn(TimingSample observed) noexcept {
    deadline_check_spent=false;
    if(!options.adaptive_batch_sizing || !observed.elapsed_ms || !observed.launches)return;
    timing.record_verified(observed.elapsed_ms,observed.launches,observed.setup_ns,observed.group_members);
    stats.last_profiled_setup_ns=observed.setup_ns;
    ++stats.verified_timing_samples;
  }
};
NativeAnchorSearchController::NativeAnchorSearchController(NativeAnchorSearchOptions options)
    :impl_(std::make_unique<Impl>(std::move(options))) {}
NativeAnchorSearchController::~NativeAnchorSearchController(){impl_->yield(Impl::Disposition::yielded);}
std::size_t NativeAnchorSearchController::stage_grant(std::uint64_t now,std::size_t funded_extents) const noexcept {
  const auto& self=*impl_;
  const auto& options=self.options;
  if(!self.enabled || self.flight || !self.windows.empty() || self.unobserved_ticket ||
      now<self.next_launch || funded_extents<2)return 0;
  if(options.adaptive_batch_sizing && self.timing.launch_limit(options.worker.timeout_ms)==0 &&
      self.timing.launch_limit(options.worker.timeout_ms,0)==0)return 0;
  // Staging needs the full group to change its shared reference. Limit
  // complete groups and launches when starting the worker.
  const auto grant=(std::min)({funded_extents-1,static_cast<std::size_t>(options.maximum_members),
      static_cast<std::size_t>(detail::anchor_match_batch_member_limit())});
  return grant>=options.minimum_members?grant:0;
}
bool NativeAnchorSearchController::fits_group_budget(std::size_t admitted_members) const noexcept {
  if(admitted_members<2)return false;
  for(const auto& window:impl_->windows) {
    if(window.members().size()>=admitted_members)return false;
    admitted_members-=window.members().size()+1;
  }
  return true;
}
void NativeAnchorSearchController::start(std::span<const NativeSharedBaseSearchWindow> windows,
    std::uint64_t activity) noexcept {
  auto& self=*impl_;
  const auto& options=self.options;
  const auto grant=stage_grant(GetTickCount64(),std::size_t{detail::anchor_match_batch_member_limit()}+1);
  if(!grant || !WorkerExchangeSession::available())return;
  std::uint64_t ticket=0;
  try {
    std::vector<std::uint32_t> counts;
    counts.reserve(windows.size());
    std::uint32_t total=0;
    const auto calibrated=options.adaptive_batch_sizing?self.timing.launch_limit(options.worker.timeout_ms):std::nullopt;
    for(const auto& window:windows) {
      const auto members=window.members().size();
      if(!members)return;
      if(members>grant-total)break;
      const auto count=static_cast<std::uint32_t>(members);
      total+=count;counts.push_back(count);
      if(options.adaptive_batch_sizing && !calibrated && total>=options.minimum_members)break;
    }
    if(total<options.minimum_members || !self.check_device())return;
    windows=windows.first(counts.size());
    const auto& route=options.route;
    const auto observed=query_windows_gpu_budget(route.memory.luid,route.memory.node_mask);
    const auto now=GetTickCount64();
    const auto defer=[&] {
      (void)self.recovery.consider_anchor(true,{},anchor_match::device_bytes(1,options.output),now);
      ++self.stats.declined;
    };
    if(observed.status!=WindowsGpuBudgetStatus::success) {
      defer();
      if(observed.status==WindowsGpuBudgetStatus::wrong_adapter ||
          (observed.phase==WindowsGpuBudgetPhase::adapter && observed.native_status==DXGI_ERROR_NOT_FOUND))
        self.suspend(Impl::RebindBlock::none);
      return;
    }
    const auto allow_reference=(options.prefer_reference || options.output==anchor_match::BatchOutput::reference_xor) &&
        std::ranges::all_of(windows,&NativeSharedBaseSearchWindow::permits_reference_change);
    const auto launch=options.adaptive_batch_sizing?
        plan_timed_native_anchor_launch(counts,options,observed.snapshot,now,allow_reference,self.timing):
        plan_native_anchor_launch(counts,options,observed.snapshot,now,allow_reference);
    if(!launch){defer();return;}
    if(self.sequence==(std::numeric_limits<std::uint64_t>::max)()){self.enabled=false;return;}
    const auto& [plan,transfer,capacity,budget,dispatches_per_wait,profiled]=*launch;
    windows=windows.first(plan.groups.size());
    const GpuHealthEvidence health{route.identity,++self.sequence,self.route_recovery_generation,true,true,observed.snapshot};
    const auto route_decision=self.recovery.consider_anchor(true,health,transfer.device_bytes,GetTickCount64());
    if(!route_decision.try_gpu()) {
      if(route_decision.reason==GpuRouteReason::device_quarantined)
        self.suspend(Impl::RebindBlock::device);
      else if(route_decision.reason==GpuRouteReason::configuration_blocked ||
          route_decision.reason==GpuRouteReason::exhausted)self.suspend(Impl::RebindBlock::hard);
      ++self.stats.declined;
      return;
    }
    ticket=route_decision.ticket;
    detail::AnchorMatchBatchBuilder builder(ticket,plan);
    for(std::size_t i=0;i<windows.size();++i)builder.append(i+1,windows[i]);
    auto request=builder.finish();
    std::optional<AnchorBatchProfileBinding> profile;
    if(profiled)profile=anchor_batch_profile_binding(request,transfer,route.ordinal,
        route.memory.luid,route.memory.node_mask,dispatches_per_wait);
    std::vector<NativeSharedBaseSearchWindow> retained(windows.begin(),windows.end());
    const auto begun=GetTickCount64();
    const auto arguments=anchor_worker_arguments(route,*launch);
    if(!self.check_device(ticket))return;
    auto session=WorkerExchangeSession::start_sized(route.helper,arguments,
        request.snapshot(),capacity,budget,route.pins);
    self.flight.emplace(Impl::Flight{std::move(session),std::move(request),std::move(profile),ticket,begun,
        begun+budget.timeout_ms});
    self.activity=activity;
    self.windows=std::move(retained);
    ++self.stats.submitted;
    if(dispatches_per_wait>1)++self.stats.submitted_queued_batches;
    self.stats.submitted_groups+=windows.size();
    self.stats.submitted_kernel_launches+=plan.launches;
    self.stats.largest_dispatch_members=(std::max)(self.stats.largest_dispatch_members,plan.quantum_members);
  }catch(...) {
    ++self.stats.declined;
    if(ticket) {
      const auto idle=WorkerExchangeSession::available();
      self.fail(ticket,GpuRecoveryOutcome::temporary_pressure,idle);
      if(!idle)self.unobserved_ticket=ticket;
    }
  }
}
void NativeAnchorSearchController::observe(std::uint64_t activity,std::uint64_t now,
    NativeAnchorOpportunity opportunity) noexcept {
  auto& self=*impl_;
  if(self.unobserved_ticket && WorkerExchangeSession::available()) {
    (void)self.recovery.confirm_quiescent(self.unobserved_ticket);
    self.unobserved_ticket=0;
  }
  const auto previous_activity=std::exchange(self.activity,activity);
  const bool foreground=activity!=previous_activity || opportunity==NativeAnchorOpportunity::foreground;
  if(!self.flight)return;
  auto& flight=*self.flight;
  if(!flight.supervisor_exited) {
    const auto state=WaitForSingleObject(flight.session.supervisor_wait_handle(),0);
    if(state==WAIT_TIMEOUT) {
      if(foreground)self.yield(Impl::Disposition::yielded);
      else if(now>=flight.deadline)self.yield(Impl::Disposition::expired);
      return;
    }
    if(state!=WAIT_OBJECT_0 || !flight.session.retirement_confirmed()) {
      self.yield(Impl::Disposition::expired);
      self.fail(flight.ticket,GpuRecoveryOutcome::device_fault,false);
      self.stats.retirement_confirmed=false;
      self.flight.reset();
      return;
    }
    self.pace(now,flight.started);
    flight.supervisor_exited=true;
    self.stats.last_retirement_elapsed_ms=now>=flight.started?now-flight.started:0;
    const auto* result=flight.session.try_result();
    self.stats.last_exchange=result?std::optional(result->code):std::nullopt;
    self.stats.last_worker_exit_code=result?result->process.exit_code:0;
    self.stats.last_worker_elapsed_ms=result?result->process.elapsed_ms:0;
    self.stats.last_publication_observed=result && result->publication_observed;
  }
  if(flight.disposition==Impl::Disposition::yielded) {
    (void)self.recovery.abandon(flight.ticket,true);
  } else if(flight.disposition==Impl::Disposition::expired) {
    self.fail(flight.ticket,GpuRecoveryOutcome::device_fault,true,true);
  } else {
    const auto* result=flight.session.try_result();
    if(!result || result->code!=WorkerExchangeCode::completed) {
      self.fail(flight.ticket,result?worker_transport_failure(flight.session,*result,WorkerExecution::hip):
          GpuRecoveryOutcome::device_fault,true,result && result->code==WorkerExchangeCode::timed_out);
      self.discard();
      ++self.stats.declined;
    } else {
      if(opportunity!=NativeAnchorOpportunity::idle || foreground)return;
      try {
        Impl::TimingSample timing{result->process.elapsed_ms,flight.request.plan().launches};
        self.replies.reserve(self.windows.size());
        const auto output=flight.request.plan().output;
        if(!anchor_match::copy_output(output)) {
          const bool reference=output==anchor_match::BatchOutput::reference_xor;
          std::vector<std::uint32_t> selected_references(reference?self.windows.size():0);
          self.packed_advice=detail::decode_anchor_packed_batch_reply(result->response,flight.request,selected_references);
          if(self.packed_advice.size()!=self.windows.size())throw std::invalid_argument("Incomplete packed batch");
          self.packed_checks.reserve(self.windows.size());
          const auto method=output==anchor_match::BatchOutput::modular_delta?
              NativePackedGroupMethod::modular_delta:(output==anchor_match::BatchOutput::sparse_xor || reference)?
              NativePackedGroupMethod::sparse_xor:output==anchor_match::BatchOutput::collective?
              NativePackedGroupMethod::collective:NativePackedGroupMethod::xor_planes;
          for(std::size_t i=0;i<self.windows.size();++i) {
            auto& checks=self.packed_checks.emplace_back(self.packed_advice[i].size(),NativePackedGroupCheck::pending);
            self.replies.push_back({self.windows[i],{},self.packed_advice[i],checks,method,
                reference?selected_references[i]:0});
          }
          if(reference) {
            ++self.stats.reference_batches;
            self.stats.reselected_groups+=static_cast<std::uint64_t>(
                std::ranges::count_if(selected_references,[](auto member){return member!=0;}));
          }
          // Retirement is proven, but the recovery ticket still awaits the
          // byte verdicts from the exact members consuming this reply.
          self.packed_ticket=flight.ticket;
          self.packed_timing=timing;
        } else {
          auto wire=std::span<const std::byte>(result->response);
          std::optional<HipPhaseTrace> phase;
          if(flight.profile) {
            if(wire.size()<=kAnchorBatchProfileBytes)throw std::invalid_argument("Missing queued anchor batch reply");
            phase=decode_anchor_batch_profile(wire.last(kAnchorBatchProfileBytes),*flight.profile);
            wire=wire.first(wire.size()-kAnchorBatchProfileBytes);
          }
          detail::AnchorMatchVerificationWork verification;
          self.advice=detail::accept_anchor_match_batch_reply(wire,flight.request,&verification);
          if(self.advice.size()!=self.windows.size())throw std::invalid_argument("Incomplete verified anchor batch");
          for(std::size_t i=0;i<self.windows.size();++i)
            self.replies.push_back({self.windows[i],self.advice[i],{}});
          if(phase && flight.profile->dispatches_per_wait==1) {
            timing.elapsed_ms=(std::max)(timing.elapsed_ms,(phase->total_wall_ns+million-1)/million);
            timing.group_members=setup_group_size(flight.request.plan(),flight.profile->transfer,self.options);
            if(timing.group_members) {
              timing.setup_ns=phase->wall_ns[static_cast<unsigned>(HipPhase::device_initialize)]+
                  phase->wall_ns[static_cast<unsigned>(HipPhase::stream_allocate)];
            }
          }
          if(self.recovery.finish(flight.ticket,GpuRecoveryOutcome::accepted,true)) {
            self.learn(timing);
            self.stats.last_verified_copy_profile=phase;
          }
          ++self.stats.verified;
          self.stats.verified_groups+=self.windows.size();
          self.stats.copy_suggested_ranges=verification.suggested_ranges;
          self.stats.copy_compared_ranges=verification.compared_ranges;
          self.stats.copy_suggested_bytes=verification.suggested_bytes;
          self.stats.copy_compared_bytes=verification.compared_bytes;
          if(phase && flight.profile->dispatches_per_wait>1) {
            ++self.stats.verified_queued_batches;
            self.stats.queued_dispatches_per_wait=flight.profile->dispatches_per_wait;
            self.stats.queued_worker_wall_ns=phase->total_wall_ns;
            self.stats.queued_kernel_wait_ns=phase->wall_ns[static_cast<unsigned>(HipPhase::kernel_wait)];
          }
        }
        self.stats.reply_bytes+=result->response.size();
      }catch(const std::bad_alloc&) {
        self.fail(flight.ticket,GpuRecoveryOutcome::temporary_pressure,true);
        self.discard();++self.stats.declined;
      }catch(...) {
        self.fail(flight.ticket,GpuRecoveryOutcome::integrity_failure,true);
        self.discard();++self.stats.declined;
      }
    }
  }
  self.flight.reset();
  settle_reply_checks();
}
bool NativeAnchorSearchController::waiting() const noexcept {
  return impl_->flight && !impl_->flight->supervisor_exited && impl_->flight->disposition==Impl::Disposition::pending;
}
std::span<const NativeSharedBaseSearchWindow> NativeAnchorSearchController::pending_prefix() const noexcept {
  if(!impl_->flight || impl_->flight->disposition!=Impl::Disposition::pending)return {};
  return impl_->windows;
}
std::uint64_t NativeAnchorSearchController::deadline() const noexcept {
  return waiting()?impl_->flight->deadline:0;
}
void* NativeAnchorSearchController::wake_handle() const noexcept {
  return impl_->flight && !impl_->flight->supervisor_exited?impl_->flight->session.supervisor_wait_handle():nullptr;
}
std::span<const NativeSharedBaseSearchReply> NativeAnchorSearchController::replies() const noexcept {
  return impl_->replies;
}
void NativeAnchorSearchController::settle_reply_checks() noexcept {
  auto& self=*impl_;
  if(!self.packed_ticket)return;
  const auto check=detail::inspect_anchor_packed_checks(self.replies);
  if(check.status==detail::AnchorPackedBatchStatus::pending)return;
  const auto ticket=std::exchange(self.packed_ticket,0);
  const auto timing=std::exchange(self.packed_timing,std::nullopt);
  if(check.status==detail::AnchorPackedBatchStatus::verified &&
      self.recovery.finish(ticket,GpuRecoveryOutcome::accepted,true)) {
    ++self.stats.verified;
    self.stats.verified_groups+=check.verified_groups;
    if(timing)self.learn(*timing);
    return;
  }
  if(check.status==detail::AnchorPackedBatchStatus::empty) {
    (void)self.recovery.abandon(ticket,true);
    ++self.stats.empty_xor_batches;
  }else self.fail(ticket,GpuRecoveryOutcome::integrity_failure,true);
  self.discard();
  ++self.stats.declined;
}
bool NativeAnchorSearchController::can_reacquire() const noexcept {
  const auto& self=*impl_;
  return !self.enabled && !self.flight && !self.unobserved_ticket && self.windows.empty() &&
      self.stats.retirement_confirmed;
}
bool NativeAnchorSearchController::take_device_interruption() noexcept {
  return std::exchange(impl_->device_interrupted,false);
}
std::optional<NativeAnchorRecoveryTarget> NativeAnchorSearchController::recovery_target() const noexcept {
  const auto& self=*impl_;
  if(!can_reacquire() || self.rebind_block!=Impl::RebindBlock::device || !self.sequence)return {};
  return NativeAnchorRecoveryTarget{self.options.route.identity,self.sequence};
}
bool NativeAnchorSearchController::take_deadline_recovery() noexcept {
  auto& self=*impl_;
  if(!self.deadline_observed || self.deadline_check_spent || !recovery_target())return false;
  self.deadline_check_spent=true;
  return true;
}
bool NativeAnchorSearchController::rebind(HipDiscoveredRoute route,const NativeAnchorRecoveryProof* proof) noexcept {
  auto& self=*impl_;
  if(!can_reacquire() || !self.valid_profile() || !valid_anchor_route(route))return false;
  if(route.identity==self.options.route.identity) {
    if(self.rebind_block==Impl::RebindBlock::hard)return false;
    if(self.rebind_block==Impl::RebindBlock::device) {
      const auto target=recovery_target();
      if(!target || !proof || proof->target_!=*target || proof->pins_!=route.pins ||
          self.recovery_serial==(std::numeric_limits<std::uint64_t>::max)())return false;
      self.route_recovery_generation=++self.recovery_serial;
    }
  }else self.route_recovery_generation=0;
  self.options.route=std::move(route);
  self.timing=NativeAnchorBatchTiming{};
  self.stats.last_profiled_setup_ns=0;
  self.stats.last_verified_copy_profile.reset();
  self.rebind_block=Impl::RebindBlock::none;
  self.deadline_observed=false;
  self.enabled=true;
  return true;
}
void NativeAnchorSearchController::discard_reply() noexcept {impl_->discard();}
void NativeAnchorSearchController::cancel() noexcept {impl_->yield(Impl::Disposition::yielded);}
bool NativeAnchorSearchController::stop() noexcept {
  auto& self=*impl_;
  self.yield(Impl::Disposition::yielded);
  if(self.flight) {
    const auto wait=WaitForSingleObject(self.flight->session.supervisor_wait_handle(),retirement_grace_ms);
    if(wait==WAIT_OBJECT_0)observe(self.activity,GetTickCount64());
    else {
      self.stats.retirement_confirmed=false;
      self.flight.reset();
    }
  }
  return self.stats.retirement_confirmed && !self.flight && !self.unobserved_ticket;
}
NativeAnchorSearchStats NativeAnchorSearchController::stats() const noexcept {
  auto state=impl_->stats;
  state.retirement_confirmed=state.retirement_confirmed && !impl_->flight && !impl_->unobserved_ticket;
  state.learned_launch_limit=impl_->timing.launch_limit(impl_->options.worker.timeout_ms);
  return state;
}
}
