#pragma once
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
// Borrowed only under the session's existing serialized control contract.
// The session owns this token; controllers retain weak references, never the
// session or its writer. Impl addresses survive outer-session moves.
struct NativeMaintenanceSource {
  NativeFilesystemWriter* writer=nullptr;
  const bool* terminal=nullptr;
  std::weak_ptr<const void> active;
  std::function<std::optional<OptimizationGroupScan>()> restore_scan;
  std::function<bool()> scan_funded;
  std::function<bool(OptimizationGroupScan)> publish_scan;
};
}
