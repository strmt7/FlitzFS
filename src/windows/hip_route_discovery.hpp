#pragma once
#include "flitzfs/gpu_recovery.hpp"
#include "flitzfs/process_budget.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
namespace flitzfs {
class HipRoutePins;
enum class HipDiscoveryStatus {
  ready, invalid_request, runtime_unavailable, worker_untrusted,
  no_amd_adapter, ambiguous_adapter, topology_unavailable, resource_deferred,
  transport_busy, identity_unavailable, identity_mismatch, retirement_unconfirmed
};
// The executable hash and runtime name come from an already trusted package
// manifest. The latter specifies the ABI that was used to compile the helper.
struct HipRouteDiscoveryRequest {
  std::filesystem::path helper;
  std::array<std::byte,32> helper_sha256{};
  std::wstring runtime_basename;
  ProcessBudget worker;
  std::uint32_t maximum_identity_queries=0;
};
struct HipDiscoveredRoute {
  GpuConfigurationId identity{};
  std::array<std::byte,32> runtime_sha256{};
  GpuMemorySnapshot memory;
  std::uint32_t ordinal=0;
  // Retain the matched helper and installed module while planning a launch.
  std::shared_ptr<const HipRoutePins> pins;
};
struct HipRouteDiscoveryResult {
  HipDiscoveryStatus status=HipDiscoveryStatus::invalid_request;
  std::uint32_t worker_exit=0,identity_queries=0;
  std::optional<HipDiscoveredRoute> route;
};
// Explicit supervised control-plane operation. The parent does not load HIP.
// A returned binding establishes a current handoff identity, not device
// health across boots, compression benefit or permission to launch a kernel.
HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest&);
}
