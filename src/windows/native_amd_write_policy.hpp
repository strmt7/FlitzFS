#pragma once
#include "hip_route_discovery.hpp"
#include "native_deferred_write.hpp"

namespace flitzfs {

class MemoryVolumeDomain;
struct NativeAmdWriteSample {
  std::uint64_t elapsed_ns=0,occupied_bytes=0;
  std::array<std::byte,32> input_digest{};
  bool cpu_readback_verified=false,retirement_confirmed=false,device_executed=false;
  std::uint32_t stored_lz4_candidates=0;
};

struct NativeAmdWriteComparison {
  NativeAmdWriteSample cpu,amd;
  bool amd_first=false;
};
enum class NativeAmdWritePreference {unqualified,cpu,amd};

// Complete, matched native write observations. The recorder owns validation
// of the actual source, write scope, response and backend used by each trial.
struct NativeAmdWriteEvidence {
  GpuConfigurationId configuration{};
  NativeAmdWriteShape shape;
  std::span<const NativeAmdWriteComparison> comparisons;
};
NativeAmdWritePreference evaluate_native_amd_writes(const NativeAmdWriteEvidence&) noexcept;

// Replay a detached CPU arm. The admitted allowance covers the captured
// source, decoder scratch and the complete private mirrored image.
std::optional<NativeAmdWriteSample> record_native_amd_cpu_trial(const NativeAmdWriteShape&,
    std::span<const std::byte> source,std::uint64_t memory_allowance,MemoryVolumeDomain*,
    const std::function<bool()>& interrupted) noexcept;

// Called off the filesystem path with independently acquired launch costs.
// Package admission, a matched recommendation and an exact route are needed.
std::optional<NativeDeferredWriteOptions> qualify_native_amd_writes(const HipDiscoveredRoute&,
    const NativeAmdWriteEvidence&,const GpuLz4ResidentCost&,GpuLz4WindowPolicy,
    ProcessBudget,std::uint32_t deadline_ms,std::optional<NativeUniformSweepGrant> sweep={});

}
