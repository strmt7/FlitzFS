#pragma once
#include "flitzfs/gpu_batch.hpp"
#include "hip_worker_exit.hpp"
#include <array>
namespace flitzfs {
enum class HipMemoryPoint { pre_initialize,initialized,completed };
// One shared admission/transition policy for worker, decoder result gate and
// match controller. Budget pressure is not evidence of a broken GPU context.
// Unknown/invalid identity or telemetry remains a conservative device fault.
inline void require_hip_memory(const GpuMemorySnapshot& snapshot,std::array<std::byte,8> luid,
    std::uint32_t mask,std::uint64_t now,std::uint64_t device_bytes,HipMemoryPoint point,
    const GpuMemorySnapshot* baseline=nullptr) {
  using S=HipWorkerStage;
  if((point!=HipMemoryPoint::pre_initialize && point!=HipMemoryPoint::initialized &&
      point!=HipMemoryPoint::completed) || (point!=HipMemoryPoint::pre_initialize && !baseline))
    throw HipWorkerFailure(HipWorkerExit::device_fault,"invalid HIP memory point/baseline",S::worker_request);
  auto code=check_gpu_memory_budget(snapshot,luid,mask,now,
      point==HipMemoryPoint::completed?0:device_bytes);
  if(code==GpuGateCode::admitted && point!=HipMemoryPoint::pre_initialize)
    code=check_gpu_memory_transition(*baseline,snapshot,luid,mask,now,device_bytes);
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
    case GpuGateCode::admitted:return;
    case GpuGateCode::invalid_budget:
      stage=select(S::pre_init_invalid_budget,S::initialized_invalid_budget,S::completed_invalid_budget);break;
    case GpuGateCode::stale_budget:
      stage=select(S::pre_init_stale_budget,S::initialized_stale_budget,S::completed_stale_budget);break;
    case GpuGateCode::wrong_adapter:
      stage=select(S::pre_init_wrong_adapter,S::initialized_wrong_adapter,S::completed_wrong_adapter);break;
    case GpuGateCode::insufficient_headroom:
      stage=select(S::pre_init_headroom,S::initialized_headroom,S::completed_headroom);break;
    case GpuGateCode::nonlocal_growth:
      stage=select(S::worker_request,S::initialized_nonlocal_growth,S::completed_nonlocal_growth);break;
    default:stage=S::worker_request;break;
  }
  const auto category=code==GpuGateCode::insufficient_headroom ||
      code==GpuGateCode::nonlocal_growth || code==GpuGateCode::stale_budget ?
      HipWorkerExit::temporary_pressure:HipWorkerExit::device_fault;
  throw HipWorkerFailure(category,"HIP memory envelope refused",stage);
}
}
