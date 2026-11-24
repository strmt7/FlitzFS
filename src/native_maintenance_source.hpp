#pragma once
#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/optimization_state.hpp"
#include <functional>
namespace flitzfs::detail {
struct MaintenanceResources {
  std::uint64_t memory_allowance=0;
  bool cpu_busy=false;
};
// Sequence/capture ordering belongs to the serialized opportunity consumer.
std::optional<MaintenanceResources> maintenance_resources(const NativeMaintenanceObservation&,
    std::uint64_t now_ms,const NativeMaintenancePolicy&) noexcept;
std::size_t maintenance_group_member_limit(const MaintenanceResources&,bool indexed) noexcept;
std::optional<std::uint32_t> coherence_primary_band(
    const std::array<std::uint64_t,kOptimizationGroupBands>&);
struct CoherenceScanKeys {
  std::uint32_t representative;
  OptimizationXorSignatures masked;
  std::array<std::uint64_t,kCoherenceModularWidths> modular;
};
// Borrowed only under the session's existing serialized control contract.
// The session owns this token; controllers retain weak references, never the
// session or its writer. Impl addresses survive outer-session moves.
struct NativeMaintenanceSource {
  NativeFilesystemWriter* writer=nullptr;
  const bool* terminal=nullptr;
  bool automatic_compression=false;
  std::weak_ptr<const void> active;
  // Set only by a verified indexed copy that keeps record IDs and revisions.
  struct VerifiedRelocation {
    StoreIdentity from{},to{};
  } relocation;
  struct GroupAppendRefusal {
    StoreIdentity image{};
    std::uint64_t generation=0,shortfall_bytes=0;
  } group_append_refusal;
  std::uint64_t group_append_shortfall(StoreIdentity image,std::uint64_t generation) const noexcept {
    return group_append_refusal.image==image && group_append_refusal.generation==generation?
        group_append_refusal.shortfall_bytes:0;
  }
  std::optional<IndexedCoherenceSnapshot> coherence;
  std::shared_ptr<IndexedCoherenceWriter> coherence_writer;
  std::shared_ptr<IndexedCoherenceStorage> coherence_storage;
  std::optional<PagedObjectStoreWriter::Rewrite> coherence_rewrite;
  std::optional<std::uint64_t> coherence_retry_input;
  std::uint64_t coherence_binding=0,coherence_offer_after=0;
  std::uint64_t coherence_compactions=0,coherence_discards=0,coherence_compacted_bytes_saved=0;
  bool coherence_full=false;
  void cancel_coherence_work() noexcept {
    if(!coherence_rewrite)return;
    coherence_rewrite.reset();
    coherence_retry_input=writer->workload_info().committed_input_bytes;
    ++coherence_discards;
  }
  void retire_coherence() noexcept {
    coherence_rewrite.reset();
    coherence_retry_input.reset();
    coherence_full=false;
    coherence.reset();
    if(coherence_storage && coherence_storage->retained_==coherence_writer)
      coherence_storage->retained_.reset();
    coherence_writer.reset();
    coherence_offer_after=0;
  }
  std::function<std::optional<OptimizationGroupScan>()> restore_scan;
  std::function<bool()> scan_funded;
  std::function<bool(OptimizationGroupScan,std::span<const CoherenceScanKeys>)> publish_scan;
  std::function<bool()> coherence_pending;
  std::function<void()> advance_coherence;
};
}
