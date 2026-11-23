#pragma once
#include "hip_route_discovery.hpp"
#include "native_deferred_write.hpp"
#include "flitzfs/uniform_lz4_batch.hpp"

namespace flitzfs {

class MemoryVolumeDomain;
struct NativeAmdWriteSample {
  std::uint64_t elapsed_ns=0,occupied_bytes=0;
  std::array<std::byte,32> input_digest{};
  bool cpu_readback_verified=false,retirement_confirmed=false,device_executed=false;
  std::uint32_t stored_lz4_candidates=0;
  std::size_t transactions=1;
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

struct NativeAmdStoragePolicy {
  std::uint32_t isolation_unit=0;
  StoreCompressionOptions foreground;
};

// Minimum memory for source, CPU scratch and a mirrored trial image.
bool native_amd_cpu_replay_fits(std::uint64_t bytes,std::size_t writes,std::uint64_t allowance) noexcept;

// Write each complete source to its own inode in a private volume. Batch
// samples include the ordered request identities and boundaries in the digest.
std::optional<NativeAmdWriteSample> record_native_amd_cpu_batch(const NativeAmdStoragePolicy&,
    std::span<const UniformLz4BatchInput>,std::uint64_t memory_allowance,MemoryVolumeDomain*,
    const std::function<bool()>& interrupted) noexcept;
// Replay independently verified advice from the exact prepared group. This
// validates storage and recovery. The owner attributes hardware execution.
std::optional<NativeAmdWriteSample> record_native_amd_verified_batch(const NativeAmdStoragePolicy&,
    const PreparedUniformLz4Batch&,const VerifiedUniformLz4Batch&,std::uint64_t memory_allowance,
    MemoryVolumeDomain*,const std::function<bool()>& interrupted) noexcept;

enum class NativeAmdTrialLaunch {immediate,prepared,prepared_requests};
struct NativeAmdDeviceTrial {
  std::optional<NativeAmdWriteSample> sample;
  std::uint64_t ready_ns=0,exchange_ns=0;
  std::uint64_t preparation_ns=0,supervised_ns=0,verification_ns=0,replay_ns=0;
  WorkerExchangeCode exchange=WorkerExchangeCode::invalid_response;
  std::uint32_t worker_exit=0,admitted_requests=0;
  bool worker_retired=false;
};
// One detached device arm. Use an independently admitted finite grant. The
// complete measured cost includes preparation, transport, verification and the
// private native write. Cancellation never reaches the source filesystem.
NativeAmdDeviceTrial record_native_amd_device_trial(const NativeAmdWriteShape&,
    std::span<const std::byte> source,const HipDiscoveredRoute&,NativeUniformSweepGrant,
    ProcessBudget,std::uint64_t memory_allowance,MemoryVolumeDomain*,NativeAmdTrialLaunch,
    std::stop_token) noexcept;

// Share one admitted worker exchange for a complete group of native writes.
NativeAmdDeviceTrial record_native_amd_device_batch(const NativeAmdStoragePolicy&,
    std::span<const UniformLz4BatchInput>,const HipDiscoveredRoute&,NativeUniformSweepGrant,
    ProcessBudget,std::uint64_t memory_allowance,MemoryVolumeDomain*,NativeAmdTrialLaunch,
    std::stop_token,const std::function<bool()>& may_submit={}) noexcept;

// Called off the filesystem path with independently acquired launch costs.
// Package admission, a matched recommendation and an exact route are needed.
std::optional<NativeDeferredWriteOptions> qualify_native_amd_writes(const HipDiscoveredRoute&,
    const NativeAmdWriteEvidence&,const GpuLz4ResidentCost&,GpuLz4WindowPolicy,
    ProcessBudget,std::uint32_t deadline_ms,std::optional<NativeUniformSweepGrant> sweep={});

}
