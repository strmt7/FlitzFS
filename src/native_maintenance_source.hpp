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
// Borrowed only under the session's existing serialized control contract.
// The session owns this token; controllers retain weak references, never the
// session or its writer. Impl addresses survive outer-session moves.
struct NativeMaintenanceSource {
  NativeFilesystemWriter* writer=nullptr;
  const bool* terminal=nullptr;
  std::weak_ptr<const void> active;
  std::optional<IndexedCoherenceSnapshot> coherence;
  std::optional<IndexedCoherenceWriter> coherence_writer;
  std::uint64_t coherence_binding=0,coherence_offer_after=0;
  void retire_coherence() noexcept {
    coherence.reset();
    coherence_writer.reset();
    coherence_offer_after=0;
  }
  std::function<std::optional<OptimizationGroupScan>()> restore_scan;
  std::function<bool()> scan_funded;
  std::function<bool(OptimizationGroupScan)> publish_scan;
};
}
