#pragma once
#include "flitzfs/native_maintenance.hpp"
#include <windows.h>
#include <memory>
namespace flitzfs {
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
