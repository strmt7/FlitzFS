#pragma once
#include "flitzfs/gpu_batch.hpp"
#include "hip_worker_exit.hpp"
#include <array>
namespace flitzfs {
enum class HipMemoryPoint { pre_initialize,initialized,completed };
// Diagnostic-only factoring of the existing strict budget/residency predicates.
// Every refusal retains the legacy device_fault recovery category. No telemetry
// is normalized, no reserve relaxed, no nonlocal growth accepted.
inline void require_hip_memory(const GpuMemorySnapshot& snapshot,std::array<std::byte,8> luid,
    std::uint32_t mask,std::uint64_t now,std::uint64_t additional,HipMemoryPoint point,
    std::uint64_t baseline_nonlocal=0) {
  using S=HipWorkerStage;
  const auto code=check_gpu_memory_budget(snapshot,luid,mask,now,additional);
  const auto select=[&](S pre,S initialized,S completed) {
    switch(point) {
      case HipMemoryPoint::pre_initialize:return pre;
      case HipMemoryPoint::initialized:return initialized;
      case HipMemoryPoint::completed:return completed;
    }
    return S::worker_request;
  };
  auto stage=S::none;
  switch(code) {
    case GpuGateCode::admitted:break;
    case GpuGateCode::invalid_budget:
      stage=select(S::pre_init_invalid_budget,S::initialized_invalid_budget,S::completed_invalid_budget);break;
    case GpuGateCode::stale_budget:
      stage=select(S::pre_init_stale_budget,S::initialized_stale_budget,S::completed_stale_budget);break;
    case GpuGateCode::wrong_adapter:
      stage=select(S::pre_init_wrong_adapter,S::initialized_wrong_adapter,S::completed_wrong_adapter);break;
    case GpuGateCode::insufficient_headroom:
      stage=select(S::pre_init_headroom,S::initialized_headroom,S::completed_headroom);break;
    default:stage=S::worker_request;break;
  }
  if(stage!=S::none)
    throw HipWorkerFailure(HipWorkerExit::device_fault,"HIP memory budget refused",stage);
  switch(point) {
    case HipMemoryPoint::pre_initialize:return;
    case HipMemoryPoint::initialized:stage=S::initialized_nonlocal_growth;break;
    case HipMemoryPoint::completed:stage=S::completed_nonlocal_growth;break;
    default:throw HipWorkerFailure(HipWorkerExit::device_fault,"invalid HIP memory point",S::worker_request);
  }
  if(snapshot.nonlocal_usage>baseline_nonlocal)
    throw HipWorkerFailure(HipWorkerExit::device_fault,"HIP nonlocal residency grew",stage);
}
}
