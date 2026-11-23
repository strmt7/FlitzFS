#include "native_amd_write_policy.hpp"
#include <algorithm>
#include <limits>

namespace flitzfs {
namespace {
bool verified(const NativeAmdWriteSample& sample,bool amd) noexcept {
  return sample.elapsed_ns && sample.occupied_bytes && sample.input_digest!=decltype(sample.input_digest){} &&
      sample.cpu_readback_verified && sample.retirement_confirmed && sample.device_executed==amd &&
      (amd || !sample.stored_lz4_candidates);
}
}

NativeAmdWritePreference evaluate_native_amd_writes(const NativeAmdWriteEvidence& evidence) noexcept {
  using Result=NativeAmdWritePreference;
  const auto& shape=evidence.shape;
  if(evidence.configuration==GpuConfigurationId{} ||
      shape.input_bytes<kNativeExtentBytes || shape.input_bytes%kNativeExtentBytes ||
      shape.input_bytes>native_windows_write_limit(NativeFileInfoAllocation::indexed_reserved_cache) ||
      !valid_store_io_contract({shape.isolation_unit,true}) ||
      (shape.foreground.mode!=StoreCompressionMode::fast && shape.foreground.mode!=StoreCompressionMode::automatic) ||
      shape.foreground.zstd_level<1 || shape.foreground.zstd_level>9 || evidence.comparisons.size()<3)
    return Result::unqualified;
  auto cpu_min=(std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t cpu_max=0,amd_max=0;
  bool smaller=true,nonexpanding=true;
  bool device_output_stored=true;
  auto previous_order=!evidence.comparisons.front().amd_first;
  for(const auto& trial:evidence.comparisons) {
    if(trial.amd_first==previous_order || trial.cpu.input_digest!=trial.amd.input_digest ||
        !verified(trial.cpu,false) || !verified(trial.amd,true) ||
        trial.amd.stored_lz4_candidates>shape.input_bytes/kNativeExtentBytes)
      return Result::unqualified;
    previous_order=trial.amd_first;
    cpu_min=(std::min)(cpu_min,trial.cpu.elapsed_ns);
    cpu_max=(std::max)(cpu_max,trial.cpu.elapsed_ns);
    amd_max=(std::max)(amd_max,trial.amd.elapsed_ns);
    smaller=smaller && trial.amd.occupied_bytes<trial.cpu.occupied_bytes;
    nonexpanding=nonexpanding && trial.amd.occupied_bytes<=trial.cpu.occupied_bytes;
    device_output_stored=device_output_stored && trial.amd.stored_lz4_candidates;
  }
  if(device_output_stored && ((nonexpanding && amd_max<cpu_min) || (smaller && amd_max<=cpu_max)))return Result::amd;
  return Result::cpu;
}

std::optional<NativeDeferredWriteOptions> qualify_native_amd_writes(const HipDiscoveredRoute& route,
    const NativeAmdWriteEvidence& evidence,const GpuLz4ResidentCost& cost,GpuLz4WindowPolicy window,
    ProcessBudget budget,std::uint32_t deadline,std::optional<NativeUniformSweepGrant> sweep) {
  if(evaluate_native_amd_writes(evidence)!=NativeAmdWritePreference::amd ||
      evidence.configuration!=route.identity || cost.configuration!=route.identity ||
      !cost.qualified || cost.recovery_generation || !route.pins ||
      !route.pins->uniform_module_pinned() || route.ordinal>=64 ||
      !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) ||
      !valid_process_budget(budget) || !deadline || deadline>budget.timeout_ms)
    return std::nullopt;
  NativeDeferredWriteOptions result;
  result.binding.helper=route.helper;
  result.binding.configuration=route.identity;
  result.binding.ordinal=static_cast<int>(route.ordinal);
  result.binding.runtime_sha256=route.runtime_sha256;
  result.binding.sweep=sweep;
  result.controller_health=GpuHealthEvidence{route.identity,1,0,true,true,route.memory};
  result.controller_route_pins=route.pins;
  result.measured_shape=evidence.shape;
  result.cost=cost;
  result.window=window;
  result.budget=budget;
  result.deadline_ms=deadline;
  result.allow_gpu=result.workload_qualified=result.helper_independent=true;
  if(!NativeDeferredWriteService::measured_write_funded(result,*result.controller_health,route.memory.captured_ms))
    return std::nullopt;
  return result;
}
}
