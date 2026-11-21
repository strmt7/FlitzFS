#pragma once
#include "flitzfs/paged_store.hpp"
#include <optional>
namespace flitzfs {
// Experimental advisory state only. Never a decoding dependency or data authority.
inline constexpr std::uint64_t kOptimizationAreaBytes=2*kStoreCommitUnit;
inline constexpr std::size_t kOptimizationMaxProfiles=32,kOptimizationMaxBenchmarks=16;
inline constexpr std::size_t kOptimizationGroupBands=12,kOptimizationXorProjections=4;
using OptimizationXorSignatures=std::array<std::uint64_t,kOptimizationXorProjections>;
// Number of feature records that fit in an extended advisory slot.
inline constexpr std::size_t kOptimizationGroupRepresentatives=373;
inline constexpr std::uint64_t kOptimizationGroupScanInputPerWriteByte=32;
inline constexpr std::uint64_t kOptimizationGroupScanInputBytes=
    kOptimizationGroupScanInputPerWriteByte*kOptimizationAreaBytes;
struct OptimizationGroupFeature {
  std::uint64_t inode=0,extent_index=0,object=0,revision=0;
  std::array<std::byte,32> content{};
  std::array<std::uint64_t,kOptimizationGroupBands> bands{};
  bool exact_base=false;
  bool operator==(const OptimizationGroupFeature&) const = default;
};
struct OptimizationGroupProjection {
  std::uint32_t representative=0; // Row in the same checkpoint.
  OptimizationXorSignatures signatures{};
  bool operator==(const OptimizationGroupProjection&) const = default;
};
struct OptimizationGroupScan {
  StoreIdentity image{};
  std::uint64_t namespace_sequence=0,inode=0,extent_index=0;
  std::vector<OptimizationGroupFeature> representatives; // Strict object order.
  bool complete=false,revisit=false,xor_sampling=false;
  std::vector<OptimizationGroupProjection> projections; // Strict representative order.
  bool operator==(const OptimizationGroupScan&) const = default;
};
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
struct OptimizationPayloadScrub {
  PagedPayloadScrubPosition position;
  std::uint64_t pass_started_utc=0; // 100-ns FILETIME ticks.
  bool operator==(const OptimizationPayloadScrub&) const = default;
};
// Session-owned progress. A copied OptimizationState can report this position.
struct OptimizationColdCoverage {
  StoreIdentity image{};
  std::uint64_t selector_epoch=0,minimum_checkpoint=0,through_generation=0;
  std::uint32_t policy_revision=0,zstd_version=0,foreground_level=0;
  bool operator==(const OptimizationColdCoverage&) const = default;
};
struct OptimizationState {
  std::vector<OptimizationProfile> profiles; // Strict (workload_key,strategy) order.
  std::vector<OptimizationBenchmark> benchmarks; // Bounded chronological history.
  // Controlled settled logical-input position in the funded capabilities.
  std::optional<std::uint64_t> settled_input_bytes;
  std::optional<OptimizationGroupScan> group_scan;
  std::optional<OptimizationPayloadScrub> payload_scrub;
  std::optional<OptimizationColdCoverage> cold_coverage;
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
  friend class NativeFileSession;
  enum class BudgetResult { unchanged,published,deferred };
  static OptimizationStateWriter create_state(std::shared_ptr<StoreByteIo>,StoreIdentity,
      OptimizationState,unsigned limit);
  static OptimizationStateWriter create_budgeted(std::shared_ptr<StoreByteIo>,StoreIdentity,
      std::uint64_t settled_input,unsigned limit);
  static OptimizationStateWriter resume_budgeted(std::shared_ptr<StoreByteIo>,StoreIdentity,
      std::uint64_t sequence,std::uint64_t current_input,unsigned limit);
  BudgetResult publish_budgeted(const OptimizationState&,std::uint64_t current_input,
      std::optional<OptimizationColdCoverage>,bool automatic_scan=false,
      std::uint64_t additional_write_bytes=0);
  bool reserve_rewrite(std::uint64_t current_input,std::uint64_t cost,
      std::optional<OptimizationColdCoverage>);
  BudgetResult repair_budgeted(std::uint64_t current_input);
  std::optional<OptimizationPayloadScrub> payload_scrub_checkpoint() const noexcept;
  std::optional<OptimizationColdCoverage> cold_coverage_checkpoint() const noexcept;
  // The caller must have earned fresh CPU verification credit.
  bool publish_payload_scrub(const OptimizationPayloadScrub&,std::optional<OptimizationColdCoverage>);
  bool budget_repair_pending() const noexcept;
  std::optional<std::uint64_t> budget_settlement() const noexcept;
  std::optional<std::uint64_t> budget_repair_floor() const noexcept;
  struct Impl;
  explicit OptimizationStateWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
