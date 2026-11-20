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
  // A new admission attempt with refreshed memory and the same CPU capture.
  // It can spend remaining idle credit on preparation only.
  bool continued=false;
};
struct NativeMaintenancePolicy {
  std::uint32_t maximum_age_ms=250,memory_share_divisor=64,maximum_cpu_window_ms=2000;
  std::uint32_t maximum_cpu_busy_ppm=125000,work_share_ppm=10000;
};
enum class NativeMaintenancePhase {idle,scanning,candidates,preparing,publishing,deferred,failed};
enum class NativeMaintenanceDeferral {none,observation,foreground,cpu,memory,duty,other_maintenance,capacity};
struct NativeMaintenanceProgress {
  NativeMaintenancePhase phase=NativeMaintenancePhase::idle;
  NativeMaintenanceDeferral deferral=NativeMaintenanceDeferral::none;
  std::uint64_t sampled_extents=0,reused_features=0,attempted_groups=0,published_groups=0,declined_groups=0;
  std::uint64_t minimum_compacted_bytes_saved=0,retained_history_bytes_added=0,elapsed_work_ns=0;
  std::uint64_t next_opportunity_delay_ms=0,owned_payload_bytes=0;
  std::uint64_t checkpoints_restored=0,checkpoints_published=0;
  std::uint64_t coherence_lookups=0,coherence_matches=0,coherence_failures=0;
  std::uint64_t coherence_catalog_bytes=0; // Footprint of the attached checked advisory checkpoint.
  std::uint64_t coherence_compactions=0,coherence_discards=0,coherence_compacted_bytes_saved=0;
  std::uint64_t advised_members_published=0;
  std::uint64_t cpu_reference_pairs=0,reselected_reference_groups=0,section_reference_groups=0;
  std::uint64_t append_shortfall_bytes=0;
  bool search_window_ready=false; // Newly staged by this opportunity.
  bool search_batch_ready=false;
};
// An optional, separately admitted adviser can stage source incrementally.
// A batch grant counts the aggregate non-base members across staged groups.
// Supply results by immutable window identity. While an adviser holds a
// submitted prefix of a staged batch, only its unsubmitted groups can advance.
// An empty result with no pending prefix uses CPU preparation.
struct NativeGroupSearchOpportunity {
  std::size_t maximum_members=0;
  const NativeSharedBaseSearchReply* reply=nullptr;
  bool collect_batch=false;
  std::span<const NativeSharedBaseSearchReply> replies{};
  std::span<const NativeSharedBaseSearchWindow> pending_prefix{};
};
// Event/opportunity-driven CPU baseline. Automatically discovers related
// content groups; no caller-supplied file list, online model or thread/timer.
// Invoke on the qualified low-priority serialized control context. Polling
// never proves OS priority, physical durability or complete host interference.
class NativeGroupMaintenance final {
 public:
  explicit NativeGroupMaintenance(NativeFileSession&,NativeMaintenancePolicy={});
  NativeGroupMaintenance(NativeGroupMaintenance&&) noexcept;
  NativeGroupMaintenance& operator=(NativeGroupMaintenance&&) noexcept;
  ~NativeGroupMaintenance();
  NativeMaintenanceProgress advance(const NativeMaintenanceObservation&,std::uint64_t now_ms,
      NativeGroupSearchOpportunity={});
  NativeSharedBaseSearchWindow search_window() const noexcept;
  std::vector<NativeSharedBaseSearchWindow> search_batch() const;
  bool has_retained_search() const noexcept;
  NativeMaintenanceProgress progress() const noexcept;
  bool pending() const noexcept;
  bool active_group() const noexcept;
  void release_staged_groups() noexcept;
  std::size_t active_group_members() const noexcept;
  void cancel() noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
