#pragma once
#include "flitzfs/native_filesystem.hpp"
namespace flitzfs {
class NativeFileSession;
// Trusted host collector input, not authentication or a memory reservation.
// CPU fields describe one real observation window in the same counter domain.
struct NativeMaintenanceObservation {
  std::uint64_t sequence=0,captured_ms=0;
  std::uint64_t physical_total=0,physical_available=0,commit_limit=0,commit_available=0;
  std::uint64_t cpu_total_ticks=0,cpu_busy_ticks=0,cpu_window_ms=0;
  bool valid=false,foreground_pending=false;
};
struct NativeMaintenancePolicy {
  std::uint32_t maximum_age_ms=250,memory_share_divisor=64,maximum_cpu_window_ms=2000;
  std::uint32_t maximum_cpu_busy_ppm=125000,work_share_ppm=10000;
};
enum class NativeMaintenancePhase {idle,scanning,candidates,preparing,publishing,deferred,failed};
enum class NativeMaintenanceDeferral {none,observation,foreground,cpu,memory,duty,other_maintenance};
struct NativeMaintenanceProgress {
  NativeMaintenancePhase phase=NativeMaintenancePhase::idle;
  NativeMaintenanceDeferral deferral=NativeMaintenanceDeferral::none;
  std::uint64_t sampled_extents=0,reused_features=0,attempted_groups=0,published_groups=0,declined_groups=0;
  std::uint64_t minimum_compacted_bytes_saved=0,retained_history_bytes_added=0,elapsed_work_ns=0;
  std::uint64_t next_opportunity_delay_ms=0,owned_payload_bytes=0;
};
// Event/opportunity-driven CPU baseline. Automatically discovers aligned
// content groups; no caller-supplied file list, online model or thread/timer.
// Invoke on the qualified low-priority serialized control context. Polling
// never proves OS priority, physical durability or complete host interference.
class NativeGroupMaintenance final {
 public:
  explicit NativeGroupMaintenance(NativeFileSession&,NativeMaintenancePolicy={});
  NativeGroupMaintenance(NativeGroupMaintenance&&) noexcept;
  NativeGroupMaintenance& operator=(NativeGroupMaintenance&&) noexcept;
  ~NativeGroupMaintenance();
  NativeMaintenanceProgress advance(const NativeMaintenanceObservation&,std::uint64_t now_ms);
  NativeMaintenanceProgress progress() const noexcept;
  void cancel() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
