#include "flitzfs/gpu_recovery.hpp"
#include <algorithm>
#include <limits>

namespace flitzfs {
namespace {
template<std::size_t N> bool nonzero(const std::array<std::byte,N>& value) noexcept {
  return std::any_of(value.begin(),value.end(),[](auto b){ return b != std::byte{0}; });
}
bool known_outcome(GpuRecoveryOutcome outcome) noexcept {
  switch(outcome) {
    case GpuRecoveryOutcome::accepted:
    case GpuRecoveryOutcome::temporary_pressure:
    case GpuRecoveryOutcome::runtime_unavailable:
    case GpuRecoveryOutcome::unsupported_configuration:
    case GpuRecoveryOutcome::device_fault:
    case GpuRecoveryOutcome::integrity_failure: return true;
  }
  return false;
}
}

unsigned GpuRecoveryPolicy::required_healthy_observations() const noexcept {
  // First temporary failure can retry on the very next fresh observation.
  // Flapping needs 2,4,8,16,32 independent observations, then saturates.
  return failures_ ? (1u << (std::min)(failures_-1,5u)) : 1u;
}

GpuRouteDecision GpuRecoveryPolicy::consider(bool consent, bool worthwhile,
    const GpuHealthEvidence& e, const PreparedGpuBatch& batch, std::uint64_t now) noexcept {
  if(!consent) return {GpuRouteReason::disabled};
  if(active_ticket_ || !e.supervisor_quiescent) return {GpuRouteReason::worker_busy};
  if(clock_seen_ && now < last_now_) return {GpuRouteReason::bad_evidence};
  last_now_ = now; clock_seen_ = true;
  if(!worthwhile || !e.compatible || !nonzero(e.configuration) || !e.sequence || batch.tasks().empty())
    return {GpuRouteReason::unqualified};
  if(quarantine_full_) return {GpuRouteReason::exhausted};
  for(const auto& q:quarantines_) {
    if(q.configuration != e.configuration) continue;
    if(q.hard) return {GpuRouteReason::configuration_blocked};
    if(e.recovery_generation <= q.recovery_floor) return {GpuRouteReason::device_quarantined};
  }

  const bool changed = e.configuration != configuration_;
  if(!changed && (e.memory.luid != luid_ || e.memory.node_mask != node_mask_))
    { healthy_ = 0; return {GpuRouteReason::bad_evidence}; }
  const auto admission = check_gpu_memory_budget(e.memory,e.memory.luid,e.memory.node_mask,
                                                now,batch.device_buffer_bytes());
  if(admission != GpuGateCode::admitted) {
    // Consecutive health means consecutive GOOD telemetry, not merely time.
    healthy_ = 0;
    return {admission == GpuGateCode::insufficient_headroom ?
            GpuRouteReason::memory_pressure : GpuRouteReason::bad_evidence};
  }
  if(changed) {
    configuration_ = e.configuration; luid_ = e.memory.luid; node_mask_ = e.memory.node_mask;
    last_sequence_ = 0; last_capture_ = 0; recovery_floor_ = 0;
    failures_ = healthy_ = successes_ = 0;
    last_outcome_ = GpuRecoveryOutcome::accepted;
  } else {
    if(e.sequence < last_sequence_ || e.memory.captured_ms < last_capture_ ||
        e.recovery_generation < observed_recovery_) { healthy_ = 0; return {GpuRouteReason::bad_evidence}; }
    if(last_outcome_ == GpuRecoveryOutcome::runtime_unavailable ||
        last_outcome_ == GpuRecoveryOutcome::unsupported_configuration ||
        last_outcome_ == GpuRecoveryOutcome::integrity_failure)
      return {GpuRouteReason::configuration_blocked};
    if(last_outcome_ == GpuRecoveryOutcome::device_fault &&
        e.recovery_generation <= recovery_floor_) return {GpuRouteReason::device_quarantined};
  }
  // Repeated calls with the same snapshot cannot manufacture healthy samples.
  // A new sequence paired with old capture time also cannot accelerate retries.
  const bool fresh = e.sequence > last_sequence_ && (changed || e.memory.captured_ms > last_capture_);
  if(!fresh) return {GpuRouteReason::recovering};
  last_sequence_ = e.sequence; last_capture_ = e.memory.captured_ms;
  observed_recovery_ = e.recovery_generation;
  healthy_ = (std::min)(healthy_+1,32u);
  if(healthy_ < required_healthy_observations()) return {GpuRouteReason::recovering};
  if(next_ticket_ == (std::numeric_limits<std::uint64_t>::max)())
    return {GpuRouteReason::exhausted};
  active_ticket_ = next_ticket_++; reported_ = false; healthy_ = 0;
  return {GpuRouteReason::candidate,active_ticket_};
}

bool GpuRecoveryPolicy::finish(std::uint64_t ticket, GpuRecoveryOutcome outcome,
    bool quiescent) noexcept {
  if(!ticket || ticket != active_ticket_ || reported_ || !known_outcome(outcome)) return false;
  reported_ = true;
  last_outcome_ = outcome;
  if(outcome == GpuRecoveryOutcome::accepted) {
    // One lucky success does not erase a failure history during flapping.
    if(++successes_ >= 2) { failures_ = 0; successes_ = 0; }
  } else {
    failures_ = (std::min)(failures_+1,6u); successes_ = 0;
    recovery_floor_ = observed_recovery_;
    if(outcome != GpuRecoveryOutcome::temporary_pressure) {
      Quarantine* entry = nullptr;
      for(auto& q:quarantines_) if(q.configuration == configuration_) { entry = &q; break; }
      if(!entry) for(auto& q:quarantines_) if(!nonzero(q.configuration)) { entry = &q; break; }
      if(!entry) quarantine_full_ = true;
      else {
        entry->configuration = configuration_;
        entry->recovery_floor = (std::max)(entry->recovery_floor,recovery_floor_);
        entry->hard = entry->hard || outcome != GpuRecoveryOutcome::device_fault;
      }
    }
  }
  healthy_ = 0;
  if(quiescent) active_ticket_ = 0;
  return true;
}

bool GpuRecoveryPolicy::confirm_quiescent(std::uint64_t ticket) noexcept {
  if(!ticket || ticket != active_ticket_ || !reported_) return false;
  active_ticket_ = 0;
  return true;
}
} // namespace flitzfs
