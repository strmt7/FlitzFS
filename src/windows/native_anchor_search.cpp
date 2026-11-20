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
bool valid_anchor_route(const HipDiscoveredRoute& route) noexcept {
  return route.ordinal<64 && route.pins && route.pins->anchor_module_pinned() &&
      route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory);
}
}
std::optional<NativeAnchorLaunchPlan> plan_native_anchor_launch(
    std::span<const std::uint32_t> members, const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed, std::uint64_t now, bool permits_reference_change) {
  const auto total = std::accumulate(members.begin(), members.end(), std::uint64_t{0});
  if (!options.minimum_members || total < options.minimum_members ||
      total > options.maximum_members || total > detail::anchor_match_batch_member_limit() ||
      std::ranges::find(members, 0U) != members.end()) return {};
  const auto largest = *std::ranges::max_element(members);
  (void)detail::anchor_match_layout(largest, options.output);

  const auto& identity = options.route.memory;
  std::uint32_t fit = 0, limit = largest;
  while (fit < limit) {
    const auto trial = fit + (limit - fit + 1) / 2;
    if (check_gpu_memory_budget(observed, identity.luid, identity.node_mask, now,
        anchor_match::device_bytes(trial, options.output)) == GpuGateCode::admitted) fit = trial;
    else limit = trial - 1;
  }
  if (!fit) return {};

  using Output = anchor_match::BatchOutput;
  const bool reference = options.output == Output::reference_xor;
  const auto baseline = reference ? Output::sparse_xor : options.output;
  const auto dispatch_limit = anchor_match::dispatch_member_limit(options.output);
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
      const auto launch_grant = std::accumulate(prefix.begin(), prefix.end(),
          static_cast<std::uint32_t>(prefix.size()));
      auto batch = detail::plan_anchor_match_batch(prefix, quantum,
          anchor_match::device_bytes(quantum, baseline), launch_grant, baseline);
      if (reference && fit == largest && permits_reference_change &&
          launch_grant - batch.launches >= 2 * detail::anchor_reference_pricing_launches(batch))
        batch = detail::plan_anchor_match_batch(prefix, quantum,
            anchor_match::device_bytes(quantum, options.output), launch_grant, options.output);
      const auto transfer = detail::plan_anchor_match_transfers(batch, anchor_match::device_bytes(fit, batch.output));
      WorkerChannelCapacity capacity{batch.request_bytes, batch.reply_bytes};
      auto budget = budget_for(transfer, capacity);
      if (!valid_process_budget(budget)) return {};
      std::uint32_t queued = 1;
      const auto admitted_dispatches = 1 + (transfer.members_per_transfer - 1) / batch.quantum_members;
      if (options.dispatches_per_wait > 1 && admitted_dispatches >= options.dispatches_per_wait &&
          options.worker.timeout_ms <= kAnchorSweepProfileLimitNs / million) {
        auto profiled = capacity;
        profiled.reply_bytes += kAnchorBatchProfileBytes;
        if (const auto planned = budget_for(transfer, profiled); valid_process_budget(planned)) {
          capacity = profiled;
          budget = planned;
          queued = options.dispatches_per_wait;
        }
      }
      return NativeAnchorLaunchPlan{std::move(batch), transfer, capacity, budget, queued};
    } catch (const std::invalid_argument&) {
      // A prefix may fit the checked wire capacity when the staged batch does not.
      return {};
    }
  };

  auto accepted = candidate(members);
  if (accepted || reference) return accepted;
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

struct NativeAnchorSearchController::Impl {
  enum class Disposition {pending,yielded,expired};
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
  std::uint64_t activity=0,next_launch=0,sequence=0,unobserved_ticket=0,packed_ticket=0;
  bool enabled=false,configuration_blocked=false,device_interrupted=false;
  explicit Impl(NativeAnchorSearchOptions configured):options(std::move(configured)) {
    enabled=valid_anchor_route(options.route) && valid_profile();
  }
  bool valid_profile() const noexcept {
    return options.minimum_members && options.minimum_members<=options.maximum_members &&
        options.minimum_members<=detail::anchor_match_batch_member_limit() &&
        (!anchor_match::compact_output(options.output) || options.maximum_dispatch_members) &&
        options.dispatches_per_wait && options.dispatches_per_wait<=kAnchorProfileMaxQueuedDispatches &&
        (options.dispatches_per_wait==1 || options.output==anchor_match::BatchOutput::anchors) &&
        options.duty_ppm && options.duty_ppm<=100000 &&
        options.worker.memory_share_divisor && valid_process_budget(options.worker);
  }
  void discard() noexcept {
    if(packed_ticket)(void)recovery.abandon(std::exchange(packed_ticket,0),true);
    replies.clear();
    windows.clear();
    std::vector<detail::AnchorMatchAdvice>{}.swap(advice);
    std::vector<detail::AnchorPackedAdvice>{}.swap(packed_advice);
    packed_checks.clear();
  }
  void yield(Disposition disposition) noexcept {
    discard();
    if(!flight || flight->disposition!=Disposition::pending)return;
    flight->disposition=disposition;
    (void)flight->session.request_cancel();
    if(disposition==Disposition::yielded)++stats.yielded;
    else ++stats.declined;
  }
  void pace(std::uint64_t now,std::uint64_t started) noexcept {
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    const auto used=(std::max)(std::uint64_t{1},now>=started?now-started:std::uint64_t{0});
    const auto delay=(std::min)(used,maximum/million)*million/options.duty_ppm;
    next_launch=delay<=maximum-now?now+delay:maximum;
  }
  void suspend(bool blocked,bool retired=true) noexcept {
    enabled=false;
    configuration_blocked=blocked;
    if(retired)options.route.pins.reset();
  }
  bool check_device(std::uint64_t ticket=0) noexcept {
    if(options.route.pins->device_ready())return true;
    if(ticket)(void)recovery.abandon(ticket,true);
    device_interrupted=true;
    suspend(false);
    ++stats.declined;
    return false;
  }
  void fail(std::uint64_t ticket,GpuRecoveryOutcome outcome,bool retired) noexcept {
    (void)recovery.finish(ticket,outcome,retired);
    if(outcome==GpuRecoveryOutcome::integrity_failure)++stats.integrity_failures;
    if(outcome!=GpuRecoveryOutcome::temporary_pressure)suspend(true,retired);
  }
};
NativeAnchorSearchController::NativeAnchorSearchController(NativeAnchorSearchOptions options)
    :impl_(std::make_unique<Impl>(std::move(options))) {}
NativeAnchorSearchController::~NativeAnchorSearchController(){impl_->yield(Impl::Disposition::yielded);}
std::size_t NativeAnchorSearchController::stage_grant(std::uint64_t now,std::size_t funded_extents) const noexcept {
  const auto& self=*impl_;
  if(!self.enabled || self.flight || !self.windows.empty() || self.unobserved_ticket ||
      now<self.next_launch || funded_extents<2)return 0;
  const auto grant=(std::min)({funded_extents-1,static_cast<std::size_t>(self.options.maximum_members),
      static_cast<std::size_t>(detail::anchor_match_batch_member_limit())});
  return grant>=self.options.minimum_members?grant:0;
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
    for(const auto& window:windows) {
      const auto members=window.members().size();
      if(!members || members>grant-total)return;
      const auto count=static_cast<std::uint32_t>(members);
      total+=count;counts.push_back(count);
    }
    if(total<options.minimum_members || !self.check_device())return;
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
        self.suspend(false);
      return;
    }
    const auto launch=plan_native_anchor_launch(counts,options,observed.snapshot,now,
        options.output==anchor_match::BatchOutput::reference_xor &&
        std::ranges::all_of(windows,&NativeSharedBaseSearchWindow::permits_reference_change));
    if(!launch){defer();return;}
    if(self.sequence==(std::numeric_limits<std::uint64_t>::max)()){self.enabled=false;return;}
    const auto& [plan,transfer,capacity,budget,dispatches_per_wait]=*launch;
    windows=windows.first(plan.groups.size());
    const GpuHealthEvidence health{route.identity,++self.sequence,0,true,true,observed.snapshot};
    const auto route_decision=self.recovery.consider_anchor(true,health,transfer.device_bytes,GetTickCount64());
    if(!route_decision.try_gpu()) {
      if(route_decision.reason==GpuRouteReason::configuration_blocked ||
          route_decision.reason==GpuRouteReason::device_quarantined || route_decision.reason==GpuRouteReason::exhausted)
        self.suspend(true);
      ++self.stats.declined;
      return;
    }
    ticket=route_decision.ticket;
    detail::AnchorMatchBatchBuilder builder(ticket,plan);
    for(std::size_t i=0;i<windows.size();++i)builder.append(i+1,windows[i]);
    auto request=builder.finish();
    std::optional<AnchorBatchProfileBinding> profile;
    if(dispatches_per_wait>1)profile=anchor_batch_profile_binding(request,transfer,route.ordinal,
        route.memory.luid,route.memory.node_mask,dispatches_per_wait);
    std::vector<NativeSharedBaseSearchWindow> retained(windows.begin(),windows.end());
    const auto begun=GetTickCount64();
    using Output=anchor_match::BatchOutput;
    const auto command=plan.output==Output::reference_xor?L"--driver-anchor-reference-batch":
        plan.output==Output::collective?L"--driver-anchor-collective-batch":
        plan.output==Output::sparse_xor?L"--driver-anchor-sparse-xor-batch":
        plan.output==Output::modular_delta?L"--driver-anchor-modular-delta-batch":
        plan.output==Output::wide_packed_xor?L"--driver-anchor-xor-wide-batch":
        plan.output==Output::packed_xor?L"--driver-anchor-xor-batch":
        profile?L"--driver-anchor-transfer-batch-profile":L"--driver-anchor-transfer-batch";
    std::vector<std::wstring> arguments{command,std::to_wstring(route.ordinal),hex(route.memory.luid),
        std::to_wstring(route.memory.node_mask),hex(route.runtime_sha256),std::to_wstring(capacity.request_bytes),
        std::to_wstring(capacity.reply_bytes),std::to_wstring(plan.quantum_members),std::to_wstring(plan.device_bytes),
        std::to_wstring(plan.launches),std::to_wstring(transfer.device_bytes)};
    if(profile)arguments.push_back(std::to_wstring(dispatches_per_wait));
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
  }catch(...) {
    ++self.stats.declined;
    if(ticket) {
      const auto idle=WorkerExchangeSession::available();
      self.fail(ticket,GpuRecoveryOutcome::temporary_pressure,idle);
      if(!idle)self.unobserved_ticket=ticket;
    }
  }
}
void NativeAnchorSearchController::observe(std::uint64_t activity,std::uint64_t now,bool collect) noexcept {
  auto& self=*impl_;
  if(self.unobserved_ticket && WorkerExchangeSession::available()) {
    (void)self.recovery.confirm_quiescent(self.unobserved_ticket);
    self.unobserved_ticket=0;
  }
  if(activity!=self.activity) {
    self.activity=activity;
    yield_to_foreground();
  }
  if(!self.flight)return;
  auto& flight=*self.flight;
  if(!flight.supervisor_exited) {
    const auto state=WaitForSingleObject(flight.session.supervisor_wait_handle(),0);
    if(state==WAIT_TIMEOUT) {
      if(now>=flight.deadline)self.yield(Impl::Disposition::expired);
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
  }
  if(flight.disposition==Impl::Disposition::yielded) {
    (void)self.recovery.abandon(flight.ticket,true);
  } else if(flight.disposition==Impl::Disposition::expired) {
    self.fail(flight.ticket,GpuRecoveryOutcome::device_fault,true);
  } else {
    const auto* result=flight.session.try_result();
    if(!result || result->code!=WorkerExchangeCode::completed) {
      self.fail(flight.ticket,result?worker_transport_failure(flight.session,*result,WorkerExecution::hip):
          GpuRecoveryOutcome::device_fault,true);
      self.discard();
      ++self.stats.declined;
    } else {
      if(!collect)return;
      try {
        self.replies.reserve(self.windows.size());
        const auto output=flight.request.plan().output;
        if(output!=anchor_match::BatchOutput::anchors) {
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
          (void)self.recovery.finish(flight.ticket,GpuRecoveryOutcome::accepted,true);
          ++self.stats.verified;
          self.stats.verified_groups+=self.windows.size();
          self.stats.copy_suggested_ranges=verification.suggested_ranges;
          self.stats.copy_compared_ranges=verification.compared_ranges;
          self.stats.copy_suggested_bytes=verification.suggested_bytes;
          self.stats.copy_compared_bytes=verification.compared_bytes;
          if(phase) {
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
  if(check.status==detail::AnchorPackedBatchStatus::verified &&
      self.recovery.finish(ticket,GpuRecoveryOutcome::accepted,true)) {
    ++self.stats.verified;
    self.stats.verified_groups+=check.verified_groups;
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
bool NativeAnchorSearchController::rebind(HipDiscoveredRoute route) noexcept {
  auto& self=*impl_;
  if(!can_reacquire() || !self.valid_profile() || !valid_anchor_route(route) ||
      (self.configuration_blocked && route.identity==self.options.route.identity))return false;
  self.options.route=std::move(route);
  self.configuration_blocked=false;
  self.enabled=true;
  return true;
}
void NativeAnchorSearchController::discard_reply() noexcept {impl_->discard();}
void NativeAnchorSearchController::yield_to_foreground() noexcept {
  auto& self=*impl_;
  if(self.flight || self.packed_ticket)self.yield(Impl::Disposition::yielded);
}
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
  return state;
}
}
