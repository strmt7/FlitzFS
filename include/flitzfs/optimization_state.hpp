#pragma once
#include "flitzfs/object_store.hpp"
namespace flitzfs {
// Experimental advisory state only. Never a decoding dependency or data authority.
inline constexpr std::uint64_t kOptimizationAreaBytes=2*kStoreCommitUnit;
inline constexpr std::size_t kOptimizationMaxProfiles=32,kOptimizationMaxBenchmarks=16;
struct OptimizationProfile {
  std::uint64_t workload_key=0,logical_bytes=0,stored_bytes=0,cpu_ns=0,gpu_ns=0;
  std::uint32_t strategy=0,observations=0,confidence_millionths=0;
  bool operator==(const OptimizationProfile&) const = default;
};
struct OptimizationBenchmark {
  std::uint64_t workload_key=0,elapsed_ns=0;
  std::uint32_t strategy=0,logical_bytes=0,stored_bytes=0,backend=0;
  bool operator==(const OptimizationBenchmark&) const = default;
};
struct OptimizationState {
  std::vector<OptimizationProfile> profiles; // Strict (workload_key,strategy) order.
  std::vector<OptimizationBenchmark> benchmarks; // Bounded chronological history.
  bool operator==(const OptimizationState&) const = default;
};
enum class OptimizationReadCode { available,absent,invalid,unsupported,foreign_volume,io_failure };
struct OptimizationSnapshot {
  OptimizationReadCode code=OptimizationReadCode::absent;
  StoreIdentity volume{};
  std::uint64_t sequence=0;
  bool degraded=false;
  OptimizationState state;
};
// Bounded CPU reader. Never repairs, writes, discovers devices or opens paths.
// Invalid/unsupported/foreign state returns NO policy, including when a peer
// could otherwise supply an older policy. User-data readers do not call this.
OptimizationSnapshot read_optimization_state(const StoreByteIo&,StoreIdentity expected_volume);
class OptimizationStateWriter final {
 public:
  // Provider must qualify a separately reserved, nonaliasing area and serialize
  // global ownership. A per-instance lease cannot attest to those properties.
  // Only a NEW empty backend; no file factory, truncation, deletion or reset.
  static OptimizationStateWriter create(std::shared_ptr<StoreByteIo>,StoreIdentity volume,
      unsigned publication_limit=1);
  // Exact independently observed sequence and actual prior-owner retirement.
  // Invalid/unsupported/both-lost areas require a NEW reserved area, never reset.
  // Repair, if needed, consumes one publication from the supplied allowance.
  static OptimizationStateWriter resume(std::shared_ptr<StoreByteIo>,StoreIdentity volume,
      std::uint64_t expected_sequence,unsigned publication_limit=1);
  OptimizationStateWriter(OptimizationStateWriter&&) noexcept;
  OptimizationStateWriter& operator=(OptimizationStateWriter&&) noexcept;
  ~OptimizationStateWriter();
  // <=4 publications per lease, counting initial creation/repair. This is NOT
  // a lifetime wear governor. Automatic scheduling/resource policy is absent.
  void publish(const OptimizationState&);
  OptimizationSnapshot snapshot() const;
  std::uint64_t sequence() const noexcept; // Zero after failure/move; no I/O/allocation.
  unsigned remaining_publications() const noexcept;
  bool poisoned() const noexcept;
 private:
  struct Impl;
  explicit OptimizationStateWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
