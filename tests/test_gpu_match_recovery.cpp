#include "flitzfs/gpu_recovery.hpp"
#include <iostream>
#include <stdexcept>
namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
flitzfs::PreparedGpuBatch batch() {
  const flitzfs::GpuBlockView block{1,42,8193,{}};
  return flitzfs::PreparedGpuBatch::prepare({&block,1});
}
flitzfs::GpuHealthEvidence evidence() {
  flitzfs::GpuHealthEvidence e;
  e.configuration[0]=std::byte{1};e.sequence=1;e.recovery_generation=1;
  e.compatible=e.supervisor_quiescent=true;
  e.memory.luid[0]=std::byte{7};e.memory.node_mask=1;e.memory.captured_ms=100;
  e.memory.dedicated_bytes=e.memory.local_budget=1024ULL*1024*1024;
  e.memory.local_usage=8ULL*1024*1024;e.memory.valid=e.memory.amd_hardware=true;
  e.memory.nonlocal_budget=8ULL<<30;
  e.memory.physical_total=16ULL<<30;e.memory.physical_available=8ULL<<30;
  e.memory.commit_limit=32ULL<<30;e.memory.commit_available=16ULL<<30;
  return e;
}
void advance(flitzfs::GpuHealthEvidence& e){++e.sequence;++e.memory.captured_ms;}

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

}
int main(){try {
  match_recovery();
  std::cout<<"Match recovery policy: checks="<<checks<<" gpu_executed=false\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
