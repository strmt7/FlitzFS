#pragma once
#include "flitzfs/native_maintenance.hpp"
#include <windows.h>
#include <memory>
#include <optional>
namespace flitzfs {
namespace detail {
struct WindowsProcessorTimes {
  std::uint64_t idle,kernel,user;
};
struct WindowsProcessorDelta {
  std::uint64_t total,busy;
};
std::optional<WindowsProcessorDelta> windows_processor_delta(
    WindowsProcessorTimes before,WindowsProcessorTimes after) noexcept;
enum class MaintenanceFollowup {admit,elapsed,backing};
inline MaintenanceFollowup maintenance_followup(std::uint64_t budget,std::uint64_t used,
    std::uint64_t previous,std::uint64_t backing_prediction) noexcept {
  if(used>=budget || previous>budget-used)return MaintenanceFollowup::elapsed;
  return backing_prediction>budget-used?MaintenanceFollowup::backing:MaintenanceFollowup::admit;
}
}
inline constexpr std::uint64_t maintenance_wall_share_ppm=10000;
inline std::uint64_t maintenance_delay_ms(std::uint64_t elapsed_ns) noexcept {
  return elapsed_ns/maintenance_wall_share_ppm+(elapsed_ns%maintenance_wall_share_ppm!=0);
}
inline bool can_continue_preparation(const NativeMaintenanceProgress& progress) noexcept {
  using Phase=NativeMaintenancePhase;
  return progress.deferral==NativeMaintenanceDeferral::none && !progress.search_window_ready &&
      !progress.search_batch_ready && (progress.phase==Phase::scanning ||
          progress.phase==Phase::candidates || progress.phase==Phase::preparing);
}
// Owns only a local built-in processor counter query. No timing thread, remote
// machine, registry mutation or dependency installation. Serialized owner only.
enum class MaintenanceHostStatus {warming,ready,waiting,unavailable};
class WindowsMaintenanceHost final {
 public:
  WindowsMaintenanceHost();
  ~WindowsMaintenanceHost();
  WindowsMaintenanceHost(const WindowsMaintenanceHost&)=delete;
  WindowsMaintenanceHost& operator=(const WindowsMaintenanceHost&)=delete;
  NativeMaintenanceObservation observe(bool foreground_pending) noexcept;
  // Reuse the current CPU capture only within its fresh, measured idle slice.
  // The caller owns the foreground and stop checks around every step.
  std::optional<NativeMaintenanceObservation> continue_preparation(std::uint32_t maximum_age_ms,
      std::uint64_t backing_prediction_ns=0,bool* backing_limited=nullptr) noexcept;
  MaintenanceHostStatus status() const noexcept;
  std::uint64_t next_delay_ms() const noexcept;
 private:
  struct Impl;std::unique_ptr<Impl> impl_;
};
// Current-thread resource background mode covers CPU, I/O and memory scheduling.
// Always end before queue locks, foreground commands, notifications or waits.
class MaintenanceThreadScope final {
 public:
  MaintenanceThreadScope() noexcept;
  ~MaintenanceThreadScope();
  MaintenanceThreadScope(const MaintenanceThreadScope&)=delete;
  bool active() const noexcept {return active_;}
  bool finish() noexcept;
 private:
  bool active_=false;
};
}
