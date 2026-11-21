#pragma once
#include "native_anchor_search.hpp"
#include "winfsp_session.hpp"
#include "maintenance_host.hpp"
namespace flitzfs {
class MemoryVolumeDomain;
struct WinFspControllerView {
  SessionState state=SessionState::ready;
  NTSTATUS start_status=STATUS_PENDING;
  MaintenanceHostStatus host=MaintenanceHostStatus::warming;
  NativeMaintenanceProgress maintenance;
  std::uint64_t maintenance_quanta=0,reclamation_quanta=0,compactions=0,compaction_failures=0;
  std::uint64_t continued_preparation_steps=0,continued_compaction_steps=0,payload_repairs=0;
  std::uint64_t payload_scrub_checks=0,payload_scrub_passes=0,payload_scrub_faults=0;
  PagedPayloadScrubResult last_payload_scrub_fault;
  std::uint64_t maintenance_elapsed_ns=0,maximum_quantum_ns=0;
  std::uint64_t gpu_budget_queries=0,gpu_budget_refusals=0;
  NativeAnchorSearchStats anchor_search;
  std::optional<NativeDeferredActivation> foreground_activation;
  std::optional<HipDiscoveryStatus> installed_anchor_status;
  std::uint32_t installed_anchor_attempts=0,installed_anchor_queries=0;
  bool maintenance_disabled=false,retired=false,offline=false,controller_failed=false,write_restart_ready=false;
};
// Explicit provider and plan only. One owned thread performs start, cooperative
// maintenance and stop; callbacks still run on the provider's dispatcher.
// The volume lives until owner stop, a fenced session or dispatcher termination.
// Destruction stops and joins the owner thread.
// External owner calls are serialized. stop_and_join must not run on a
// callback/dispatcher thread. A real provider's
// stop may block in native code: live trials still require a process supervisor.
class WinFspVolumeController final {
 public:
  WinFspVolumeController(std::unique_ptr<WinFspNativeWritable>,WinFspSessionApi,NativeWriteMountPlan,
      std::optional<NativeAnchorSearchOptions> anchor={},HANDLE state_changed=nullptr);
  // Qualified memory composition: domain must outlive the controller's joined
  // worker. Allocation and retirement share the callback's control lock.
  WinFspVolumeController(std::unique_ptr<WinFspNativeWritable>,WinFspSessionApi,NativeWriteMountPlan,MemoryVolumeDomain&,
      std::optional<NativeAnchorSearchOptions> anchor={},HANDLE state_changed=nullptr);
  ~WinFspVolumeController();
  WinFspVolumeController(const WinFspVolumeController&)=delete;
  WinFspVolumeController& operator=(const WinFspVolumeController&)=delete;
  // An independent control-plane owner supplies the measured, acquired route.
  // The volume owner applies it at a quiescent foreground boundary.
  bool offer_qualified_foreground_route(NativeDeferredWriteOptions);
  void request_stop() noexcept;
  void stop_and_join() noexcept;
  WinFspControllerView view() const;
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
