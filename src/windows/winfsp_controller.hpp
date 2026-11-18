#pragma once
#include "winfsp_session.hpp"
#include "maintenance_host.hpp"
namespace flitzfs {
class MemoryVolumeDomain;
struct WinFspControllerView {
  SessionState state=SessionState::ready;
  NTSTATUS start_status=STATUS_PENDING;
  MaintenanceHostStatus host=MaintenanceHostStatus::warming;
  NativeMaintenanceProgress maintenance;
  std::uint64_t maintenance_quanta=0,reclamation_quanta=0,maintenance_elapsed_ns=0,maximum_quantum_ns=0;
  bool maintenance_disabled=false,retired=false,expired=false,offline=false,controller_failed=false;
};
// Explicit provider and plan only. One owned thread performs start, cooperative
// maintenance and stop; callbacks still run on the provider's dispatcher.
// The existing experimental plan lifetime is enforced. No thread is detached.
// External owner calls are serialized. stop_and_join must not run on a
// callback/dispatcher thread. A real provider's
// stop may block in native code: live trials still require a process supervisor.
class WinFspVolumeController final {
 public:
  WinFspVolumeController(std::unique_ptr<WinFspNativeWritable>,WinFspSessionApi,NativeWriteMountPlan);
  // Qualified memory composition: domain must outlive the controller's joined
  // worker. Its store registry remains fixed while callbacks run.
  WinFspVolumeController(std::unique_ptr<WinFspNativeWritable>,WinFspSessionApi,NativeWriteMountPlan,MemoryVolumeDomain&);
  ~WinFspVolumeController();
  WinFspVolumeController(const WinFspVolumeController&)=delete;
  WinFspVolumeController& operator=(const WinFspVolumeController&)=delete;
  void request_stop() noexcept;
  void stop_and_join() noexcept;
  WinFspControllerView view() const;
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
}
