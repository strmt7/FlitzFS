#pragma once
#include "flitzfs/gpu_recovery.hpp"
#include "flitzfs/gpu_lz4_uniform.hpp"
namespace flitzfs {
// A measured/configuration-qualified bound for fixed runtime costs plus the
// actual resident buffers of the selected implementation. These are demand
// accounting inputs, never assumed installed system specifications.
struct GpuLz4ResidentCost {
  std::uint64_t fixed_bytes=0;
  std::uint32_t input_copies=0,slab_copies=0,output_copies=0;
  std::uint64_t per_chunk_bytes=0;
  GpuConfigurationId configuration{};
  std::uint64_t recovery_generation=0;
  std::uint32_t profiled_chunk_bytes=0,maximum_grid_x=0;
  std::uint64_t kernel_ns_per_chunk=0,qualified_kernel_quantum_ns=0;
  bool qualified=false;
};
struct GpuLz4WindowPolicy {
  std::uint32_t memory_share_divisor=0;
  std::uint64_t kernel_quantum_ns=0;
};
struct GpuLz4WindowPlan {
  GpuGateCode code=GpuGateCode::invalid_budget;
  uniform_lz4::Layout layout{};
  std::uint64_t host_peak_bytes=0,remaining_input_bytes=0,estimated_kernel_ns=0;
};
// Derive the largest window from CURRENT GPU, physical and process-constrained
// commit headroom. No fixed KiB/MiB allocation cap or hardware model lookup.
// share_divisor expresses foreground/background interference policy, not RAM
// size. Larger values yield a smaller work share. A qualified caller controls
// it and kernel quanta from workload/contended-resource observations and
// configuration-qualified measured costs. At least 16 preserves a
// conservative fraction of remaining memory; this scales with actual capacity.
// Unknown or differently keyed cost/configuration evidence fails to CPU.
GpuLz4WindowPlan plan_gpu_lz4_window(std::uint64_t input_bytes,std::uint32_t chunk_bytes,
    const GpuHealthEvidence&,std::uint64_t now_ms,const GpuLz4ResidentCost&,GpuLz4WindowPolicy) noexcept;
}
