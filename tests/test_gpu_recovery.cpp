#include "flitzfs/gpu_recovery.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void require(bool value, const char* message) {
  ++checks;
  if(!value) throw std::runtime_error(message);
}
flitzfs::PreparedGpuBatch batch() {
  const flitzfs::GpuBlockView block{1,42,8193,{}};
  return flitzfs::PreparedGpuBatch::prepare({&block,1});
}
flitzfs::GpuHealthEvidence evidence() {
  flitzfs::GpuHealthEvidence e;
  e.configuration[0] = std::byte{1};
  e.sequence = 1; e.recovery_generation = 1;
  e.compatible = e.supervisor_quiescent = true;
  e.memory.luid[0] = std::byte{7}; e.memory.node_mask = 1;
  e.memory.captured_ms = 100;
  e.memory.dedicated_bytes = e.memory.local_budget = 1024ULL*1024*1024;
  e.memory.local_usage = 8ULL*1024*1024;
  e.memory.valid = e.memory.amd_hardware = true;
  return e;
}
void advance(flitzfs::GpuHealthEvidence& e) { ++e.sequence; ++e.memory.captured_ms; }
auto choose(flitzfs::GpuRecoveryPolicy& policy, const flitzfs::GpuHealthEvidence& e) {
  return policy.consider(true,true,e,batch(),e.memory.captured_ms);
}
}

void match_recovery() {
  using namespace flitzfs;
  const auto search=PreparedGpuMatchSearch::prepare(std::vector<std::byte>(65536,std::byte{41}));
  auto e=evidence();
  GpuRecoveryPolicy policy;
  require(!policy.consider(false,true,e,search,100).try_gpu(),"match consent bypass");
  const auto tiny=PreparedGpuMatchSearch::prepare(std::vector<std::byte>(3));
  require(policy.consider(true,true,e,tiny,100).reason==GpuRouteReason::unqualified,"empty match work admitted");
  auto pressure=e;
  pressure.memory.local_budget=pressure.memory.local_usage+kGpuBudgetReserveBytes+search.device_buffer_bytes()-1;
  require(policy.consider(true,true,pressure,search,100).reason==GpuRouteReason::memory_pressure,
      "match-specific device allocation was not admitted");
  const auto started=policy.consider(true,true,e,search,100);
  require(started.try_gpu(),"qualified match attempt rejected");
  require(policy.consider(true,true,e,batch(),100).reason==GpuRouteReason::worker_busy,
      "decode workload bypassed an outstanding match ticket");
  require(policy.finish(started.ticket,GpuRecoveryOutcome::device_fault,false),"match fault not recorded");
  advance(e);
  require(policy.consider(true,true,e,search,101).reason==GpuRouteReason::worker_busy,
      "match fault fabricated retirement");
  require(policy.confirm_quiescent(started.ticket),"match ticket did not retire");
  require(policy.consider(true,true,e,search,101).reason==GpuRouteReason::device_quarantined,
      "fresh match telemetry cleared device quarantine");
  ++e.recovery_generation;
  require(policy.consider(true,true,e,search,101).try_gpu(),"qualified match recovery did not admit work");
}

int main() {
  try {
    using namespace flitzfs;
    match_recovery();
    auto e = evidence();
    GpuRecoveryPolicy policy;
    require(!policy.consider(false,true,e,batch(),100).try_gpu(), "consent bypass");
    require(!policy.consider(true,false,e,batch(),100).try_gpu(), "workload qualification bypass");
    auto bad = e; bad.compatible = false;
    require(!choose(policy,bad).try_gpu(), "incompatible environment");
    bad = e; bad.configuration = {};
    require(!choose(policy,bad).try_gpu(), "unqualified configuration");
    bad = e; bad.sequence = 0;
    require(!choose(policy,bad).try_gpu(), "missing sequence");
    bad = e; bad.supervisor_quiescent = false;
    require(!choose(policy,bad).try_gpu(), "worker supervision bypass");
    bad = e; bad.memory.local_usage = bad.memory.local_budget;
    require(choose(policy,bad).reason == GpuRouteReason::memory_pressure, "memory reserve bypass");
    for(auto member: {&GpuMemorySnapshot::valid,&GpuMemorySnapshot::amd_hardware}) {
      bad = e; bad.memory.*member = false;
      require(!choose(policy,bad).try_gpu(), "invalid hardware budget");
    }
    auto decision = choose(policy,e);
    require(decision.try_gpu() && decision.ticket, "qualified initial candidate");
    require(!choose(policy,e).try_gpu(), "second outstanding candidate");
    require(!policy.finish(decision.ticket+1,GpuRecoveryOutcome::accepted,true), "foreign completion");
    require(!policy.finish(decision.ticket,static_cast<GpuRecoveryOutcome>(99),true), "unknown outcome");
    require(policy.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,true), "temporary failure report");
    require(!policy.finish(decision.ticket,GpuRecoveryOutcome::accepted,true), "late success erased failure");
    require(!choose(policy,e).try_gpu(), "same evidence retried");
    advance(e);
    require(choose(policy,e).try_gpu(), "first temporary failure did not retry next healthy round");

    // Exponential evidence hysteresis; repeated calls/ticks cannot satisfy it.
    GpuRecoveryPolicy flapping;
    e = evidence();
    decision = choose(flapping,e);
    for(unsigned failure = 1; failure <= 9; ++failure) {
      require(flapping.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,true), "flapping report");
      const auto required = 1u << (std::min)(failure-1,5u);
      require(flapping.required_healthy_observations() == required, "unbounded/wrong health hysteresis");
      for(unsigned i = 1; i <= required; ++i) {
        require(!choose(flapping,e).try_gpu(), "duplicate evidence counted toward recovery");
        auto forged = e; ++forged.sequence;
        require(!choose(flapping,forged).try_gpu(), "new sequence with old timestamp counted");
        advance(e);
        decision = choose(flapping,e);
        require(decision.try_gpu() == (i == required), "health streak admitted too soon/late");
      }
    }
    require(flapping.finish(decision.ticket,GpuRecoveryOutcome::accepted,true), "first success");
    require(flapping.consecutive_failures() != 0, "one success erased flapping history");
    for(unsigned i = 0; i < flapping.required_healthy_observations(); ++i) { advance(e); decision = choose(flapping,e); }
    require(decision.try_gpu() && flapping.finish(decision.ticket,GpuRecoveryOutcome::accepted,true), "second recovery success");
    require(flapping.consecutive_failures() == 0, "stable recovery did not clear transient history");

    // A partial healthy streak must not survive an observed loss of
    // compatibility, identity, supervision, sequence or monotonic time.
    for(unsigned interruption = 0; interruption < 5; ++interruption) {
      GpuRecoveryPolicy interrupted;
      e = evidence(); decision = choose(interrupted,e);
      require(decision.try_gpu() && interrupted.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,true),
              "first interruption setup failure");
      advance(e); decision = choose(interrupted,e);
      require(decision.try_gpu() && interrupted.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,true),
              "second interruption setup failure");
      advance(e);
      require(choose(interrupted,e).reason == GpuRouteReason::recovering, "partial recovery streak setup");
      bad = e; advance(bad);
      if(interruption == 0) bad.compatible = false;
      if(interruption == 1) bad.configuration = {};
      if(interruption == 2) bad.sequence = 0;
      if(interruption == 3) bad.supervisor_quiescent = false;
      const auto denied = interruption == 4 ?
          interrupted.consider(true,true,bad,batch(),e.memory.captured_ms-1) : choose(interrupted,bad);
      require(!denied.try_gpu(), "invalid interruption evidence admitted");
      advance(e);
      require(choose(interrupted,e).reason == GpuRouteReason::recovering,
              "interrupted incompatible evidence retained health streak");
      advance(e);
      require(choose(interrupted,e).try_gpu(), "fresh recovery streak did not complete");
    }

    // Immediate CPU fallback does not wait for worker cleanup, but that worker
    // blocks any later GPU round until separate supervisor confirmation.
    GpuRecoveryPolicy teardown;
    e = evidence(); decision = choose(teardown,e);
    GpuResultGate gate(batch(),e.memory.luid,1);
    const auto submitted = gate.begin(true,e.memory,100);
    require(submitted.code == GpuGateCode::admitted, "gate admission");
    const auto cpu = gate.use_cpu_fallback();
    require(cpu.size() == 8193 && std::all_of(cpu.begin(),cpu.end(),[](auto b){return b==std::byte{42};}), "independent CPU fallback");
    require(gate.state() == GpuGateState::poisoned && gate.accepted_bytes().empty(), "fallback did not terminally fence output");
    require(teardown.finish(decision.ticket,GpuRecoveryOutcome::device_fault,false), "pre-teardown failure report");
    advance(e); ++e.recovery_generation;
    require(!choose(teardown,e).try_gpu(), "unconfirmed worker exit admitted new work");
    auto different = e; different.configuration[0] = std::byte{2};
    require(!choose(teardown,different).try_gpu(), "config change bypassed unconfirmed worker");
    require(!teardown.confirm_quiescent(decision.ticket+1), "foreign supervisor confirmation");
    require(teardown.confirm_quiescent(decision.ticket), "supervised worker retirement");
    GpuBatchReply late; late.generation = submitted.generation;
    late.bytes.assign(cpu.begin(),cpu.end());
    late.status.assign(gate.batch().tasks().size(),gpu_detail::success);
    require(gate.complete(std::move(late),e.memory,e.memory.captured_ms) == GpuGateCode::wrong_state,
            "late GPU result revived CPU-fallback request");
    require(choose(teardown,e).try_gpu(), "separately requalified device did not recover");

    // A fresh budget or elapsed time is not recovery from a driver fault.
    GpuRecoveryPolicy device;
    e = evidence(); decision = choose(device,e);
    require(device.finish(decision.ticket,GpuRecoveryOutcome::device_fault,true), "device fault report");
    advance(e);
    require(choose(device,e).reason == GpuRouteReason::device_quarantined, "budget alone repaired device");
    auto other = e; other.configuration[0] = std::byte{2};
    auto other_decision = choose(device,other);
    require(other_decision.try_gpu() && device.finish(other_decision.ticket,GpuRecoveryOutcome::accepted,true), "different qualified config");
    advance(e);
    require(choose(device,e).reason == GpuRouteReason::device_quarantined, "A B A forgot driver fault");
    ++e.recovery_generation;
    require(choose(device,e).try_gpu(), "fresh independent device qualification");

    for(auto outcome: {GpuRecoveryOutcome::runtime_unavailable,GpuRecoveryOutcome::unsupported_configuration,
                       GpuRecoveryOutcome::integrity_failure}) {
      GpuRecoveryPolicy blocked;
      e = evidence(); decision = choose(blocked,e);
      require(blocked.finish(decision.ticket,outcome,true), "configuration failure report");
      for(unsigned i = 0; i < 128; ++i) {
        advance(e); ++e.recovery_generation;
        require(choose(blocked,e).reason == GpuRouteReason::configuration_blocked, "unproductive repeated discovery/attempt");
      }
      other = e; other.configuration[0] = std::byte{3};
      other_decision = choose(blocked,other);
      require(other_decision.try_gpu() && blocked.finish(other_decision.ticket,GpuRecoveryOutcome::accepted,true), "new qualified config blocked");
      require(choose(blocked,e).reason == GpuRouteReason::configuration_blocked, "A B A forgot bad configuration");
    }
    GpuRecoveryPolicy capacity;
    e = evidence();
    for(unsigned i = 1; i <= 9; ++i) {
      e.configuration[0] = static_cast<std::byte>(i); advance(e);
      decision = choose(capacity,e);
      require(decision.try_gpu(), "quarantine capacity shrank unexpectedly");
      require(capacity.finish(decision.ticket,GpuRecoveryOutcome::integrity_failure,true), "quarantine fill");
    }
    e.configuration[0] = std::byte{10}; advance(e);
    require(choose(capacity,e).reason == GpuRouteReason::exhausted, "unsafe quarantine eviction");

    // Clock rollback, changed adapter under same identity, sequence rollback,
    // and an invalid budget break health accumulation rather than bypassing it.
    GpuRecoveryPolicy stale;
    e = evidence(); decision = choose(stale,e);
    require(stale.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,true), "stale test report");
    require(!stale.consider(true,true,e,batch(),99).try_gpu(), "clock rollback");
    bad = e; bad.memory.luid[0] = std::byte{9}; advance(bad);
    require(!choose(stale,bad).try_gpu(), "adapter changed without configuration identity");
    require(!stale.consider(true,true,e,batch(),1000).try_gpu(), "old telemetry");
    std::cout << "PASS: " << checks << " GPU recovery/fallback policy checks (CPU only, no runtime loaded)\n";
    return 0;
  } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
