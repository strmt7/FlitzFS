#pragma once
#include "hip_route_binding.hpp"
#include "native_lz4_read_worker.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include "flitzfs/process_budget.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
namespace flitzfs {
enum class HipDiscoveryStatus {
  ready, invalid_request, runtime_unavailable, worker_untrusted,
  no_amd_adapter, ambiguous_adapter, topology_unavailable, resource_deferred,
  transport_busy, identity_unavailable, identity_mismatch, retirement_unconfirmed,
  package_unavailable, profile_mismatch, cancelled, configuration_quarantined, device_not_ready
};
struct HipQualifiedDeviceBinding {
  HipAdapterHardware hardware;
  std::array<std::byte,32> runtime_sha256{};
};
// The executable hash and runtime name come from an already trusted package
// manifest. The latter specifies the ABI that was used to compile the helper.
struct HipRouteDiscoveryRequest {
  std::filesystem::path helper;
  std::array<std::byte,32> helper_sha256{};
  std::wstring runtime_basename;
  ProcessBudget worker;
  std::uint32_t maximum_identity_queries=0;
  std::array<std::byte,32> uniform_module_sha256{}; // Set by the installed package.
  std::array<std::byte,32> anchor_module_sha256{}; // Shared-base worker package, exclusive of uniform.
  std::optional<HipQualifiedDeviceBinding> qualified_device;
  std::array<std::byte,32> native_read_module_sha256{};
};
struct HipDiscoveredRoute {
  GpuConfigurationId identity{};
  std::array<std::byte,32> runtime_sha256{};
  GpuMemorySnapshot memory;
  std::uint32_t ordinal=0;
  // Retain the matched helper and installed module while planning a launch.
  std::shared_ptr<const HipRoutePins> pins;
  std::filesystem::path helper;
  std::wstring runtime_basename;
  HipAdapterHardware hardware;
};
struct HipRouteDiscoveryResult {
  HipDiscoveryStatus status=HipDiscoveryStatus::invalid_request;
  std::uint32_t worker_exit=0,identity_queries=0;
  std::optional<HipDiscoveredRoute> route;
  std::optional<GpuMemorySnapshot> memory_pressure;
};
// Require a new admissible budget for the exact adapter and node that
// previously reported insufficient headroom.
bool recovered_hip_discovery_headroom(const GpuMemorySnapshot& denied,
    const GpuMemorySnapshot& observed, std::uint64_t now_ms) noexcept;
// Explicit supervised control-plane operation. The parent does not load HIP.
// A returned binding establishes a current handoff identity, not device
// health across boots, compression benefit or permission to launch a kernel.
HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest&,std::stop_token={});
// The application supplies a resource budget. The configured package supplies
// immutable digests and its ABI. Resolve relative to the running executable
// and complete the same supervised discovery used by explicit callers.
// The build carries uniform write digests. This performs no installed-file
// inspection, runtime discovery or hardware queries.
bool has_configured_uniform_write_package() noexcept;
// A metadata-only gate before a detached CPU reference. Actual GPU discovery
// still pins and verifies the complete installed package and runtime.
bool installed_uniform_write_files_present() noexcept;
HipRouteDiscoveryResult discover_installed_hip_route(ProcessBudget,std::uint32_t maximum_identity_queries,
    std::stop_token={});
// A device-bound development trial grant. This profile cannot enable
// deferred foreground writes. The live controller still funds its entire group.
struct InstalledUniformWriteTrialProfile {
  ProcessBudget worker;
  std::uint32_t maximum_batch_chunks=0,maximum_launch_chunks=0;
};
std::optional<InstalledUniformWriteTrialProfile> installed_uniform_write_trial_profile() noexcept;
HipRouteDiscoveryResult discover_installed_hip_write_trial_route(std::stop_token={});
struct InstalledAnchorProfile {
  ProcessBudget worker;
  std::uint32_t minimum_members=0,maximum_members=0,duty_ppm=0;
  std::uint32_t dispatches_per_wait=1,maximum_dispatch_members=0;
  anchor_match::BatchOutput output=anchor_match::BatchOutput::anchors;
  bool prefer_reference=false;
};
// Pure access to the approved profile embedded by the verifier at build time.
std::optional<InstalledAnchorProfile> installed_anchor_profile() noexcept;
HipRouteDiscoveryResult discover_installed_hip_anchor_route(std::stop_token={});

struct InstalledNativeReadProfile {
  ProcessBudget worker;
  std::uint32_t minimum_frames=0,maximum_frames=0,minimum_lead_ms=0;
  std::uint64_t maximum_slab_bytes=0;
};
std::optional<InstalledNativeReadProfile> installed_native_read_profile() noexcept;
HipRouteDiscoveryResult discover_installed_hip_native_read_route(std::stop_token={});
std::optional<NativeLz4ReadWorkerProfile> bind_installed_native_read(const HipDiscoveredRoute&);

// A demand-sampled observation from immediately before an installed route
// attempt. A change permits verification of the inputs, not GPU execution.
class InstalledHipInputWitness final {
 public:
  explicit InstalledHipInputWitness(HipRouteRole=HipRouteRole::anchor);
  ~InstalledHipInputWitness();
  InstalledHipInputWitness(const InstalledHipInputWitness&)=delete;
  InstalledHipInputWitness& operator=(const InstalledHipInputWitness&)=delete;
  void note_device_unavailable() noexcept;
  void note_memory_pressure(const GpuMemorySnapshot&) noexcept;
  bool changed() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
