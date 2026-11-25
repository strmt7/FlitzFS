#pragma once
#include "../native_group_read.hpp"
#include "../native_lz4_read.hpp"
#include "../store_file.hpp"
#include "native_file_info.hpp"
#include "native_deferred_write.hpp"
#include "winfsp_origin.hpp"
#include "flitzfs/native_maintenance.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
namespace flitzfs {
namespace detail {struct NativeCapturedWrite;}
namespace detail {class NativeLz4ReadyReads;}
class WinFspNativeWritable;
struct WinFspGroupReadWindow {
  std::uint32_t maximum_frames=0;
  std::uint64_t required_lead_ms=0;
};
struct WinFspReadAheadWork {
  const WinFspNativeWritable* owner=nullptr;
  std::uintptr_t token=0;
  std::uint64_t offset=0,foreground_epoch=0;
  std::uint32_t bytes=0;
  std::vector<detail::NativeLz4CapturedFrame> frames;
  std::shared_ptr<detail::PagedReadMetadata> metadata;
  std::unique_ptr<detail::NativeGroupReadBatch> groups;
  std::uint64_t hint_created_ms=0,observed_lead_ms=0;
  std::uint64_t stream_revision=0;
  std::pair<StoreIdentity,std::uint64_t> checkpoint{};
  bool expanded_group_window=false;
  bool has_read_lead(std::uint64_t required_ms,std::uint64_t now_ms) const noexcept {
    if(!required_ms)return true;
    if(!hint_created_ms || now_ms<hint_created_ms)return false;
    const auto spent=now_ms-hint_created_ms;
    return spent<observed_lead_ms && required_ms<=observed_lead_ms-spent;
  }
};
enum class WinFspReadAheadState {current,busy,superseded};
struct MemoryVolumeBacking;
// Experimental real writable adapter; qualify backing, runtime and containment.
// Historical _hosted names remain for ABI/report compatibility, not location.
// Default factories create NEW images. The explicit resume experiment
// requires independently confirmed old-owner retirement; never selects a driver.
struct HostedVolumeMaintenanceResult {
  NativeVolumeHeadInfo head;
  bool recovered=false; // A failed transition was independently resolved.
};
enum class HostedIdleMaintenanceAction {copy,publish};
enum class HostedIdleMaintenanceState {busy,suppressed,begun,copying,ready,stale,unchanged,published,recovered};
struct HostedIdleMaintenanceResult {
  HostedIdleMaintenanceState state;
  NativeVolumeHeadInfo head{};
};
enum class WinFspMaintenanceState {busy,unavailable,unchanged,advanced};
enum class WinFspMaintenanceContinuation {none,group,compaction,range_retirement,packed_tail_retirement,payload_scrub};
struct WinFspMaintenanceResult {
  WinFspMaintenanceState state=WinFspMaintenanceState::unavailable;
  NativeMaintenanceProgress progress;
  bool reclaimed=false,compacted=false,compaction_failed=false;
  NativeSharedBaseSearchWindow search_window;
  std::size_t group_members=0;
  std::vector<NativeSharedBaseSearchWindow> search_batch;
  bool retained_search=false;
  WinFspMaintenanceContinuation continuation=WinFspMaintenanceContinuation::none;
  bool payload_repaired=false;
  PagedPayloadScrubResult payload_scrub;
  bool sealed_image=false;
  NativeVolumeReclaimProgress file_reclamation=NativeVolumeReclaimProgress::idle;
  // Candidate and progress observed before advancing the reclaimer.
  std::optional<PagedRewriteProgress> file_rebuild_entry;
};
class WinFspNativeWritable final {
 public:
  static std::unique_ptr<WinFspNativeWritable> create_new_hosted(
      const std::filesystem::path&,StoreIdentity,StoreCompressionOptions={});
  // Dedicated hosted custom-backend trial; no path or driver selection.
  static std::unique_ptr<WinFspNativeWritable> create_managed_hosted(
      std::shared_ptr<StoreByteIo> empty_head,StoreIdentity volume_identity,
      std::shared_ptr<StoreByteIo> empty_image,StoreIdentity image_identity,
      StoreCompressionOptions foreground={});
  // New empty mirrored paged volume with the SAME writable SDK callbacks,
  // Windows-global disposition, protected owner ACLs and input provenance.
  // Optional advice is a distinct qualified reserved backend, never a path.
  static std::unique_ptr<WinFspNativeWritable> create_managed_paged(
      std::shared_ptr<StoreByteIo> empty_head,StoreIdentity volume_identity,
      std::shared_ptr<StoreByteIo> empty_image,StoreIdentity image_identity,
      PagedStoreLimits,StoreCompressionOptions foreground={},
      std::shared_ptr<StoreByteIo> empty_advice={});
  // Capability-3 indexed head with ordinary Windows SDK callbacks.
  static std::unique_ptr<WinFspNativeWritable> create_managed_indexed(
      std::shared_ptr<StoreByteIo> empty_head,StoreIdentity volume_identity,
      std::shared_ptr<StoreByteIo> empty_image,StoreIdentity image_identity,
      PagedStoreLimits,StoreCompressionOptions foreground={},
      std::shared_ptr<StoreByteIo> empty_advice={},NativeMaintenancePolicy group_policy={});
  // Start the same indexed callbacks with a qualified memory allocation owner.
  static std::unique_ptr<WinFspNativeWritable> create_indexed_memory(
      MemoryVolumeBacking&,StoreCompressionOptions foreground={},NativeMaintenancePolicy group_policy={});
  // The same indexed callback profile over one qualified volume file.
  static std::unique_ptr<WinFspNativeWritable> create_file_volume(std::shared_ptr<StoreByteIo> empty,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits,StoreCompressionOptions foreground={},
      std::optional<std::uint64_t> dense_physical_limit={});
  static std::unique_ptr<WinFspNativeWritable> resume_file_volume(std::shared_ptr<StoreByteIo> existing,
      std::optional<StoreIdentity> expected_volume=std::nullopt,StoreCompressionOptions foreground={},
      const std::function<void(StoreIdentity)>& retain_verified={});
  static std::unique_ptr<WinFspNativeWritable> create_file_volume(const std::filesystem::path&,StoreIoContract,
      StoreIdentity volume,StoreIdentity image,PagedStoreLimits,StoreCompressionOptions foreground={},
      std::optional<std::uint64_t> dense_physical_limit={});
  static std::unique_ptr<WinFspNativeWritable> resume_file_volume(const std::filesystem::path&,StoreIoContract,
      std::optional<StoreIdentity> expected_volume=std::nullopt,StoreCompressionOptions foreground={});
  static std::unique_ptr<WinFspNativeWritable> resume_indexed_retired(
      std::shared_ptr<StoreByteIo> head,StoreIdentity volume_identity,std::uint64_t expected_epoch,
      const NativeVolumeImageResolver&,StoreCompressionOptions foreground={},
      std::shared_ptr<StoreByteIo> advice={},std::uint64_t expected_advice_sequence=0,
      NativeMaintenancePolicy group_policy={});
  // Qualified exact head epoch/resolver after actual previous-owner retirement.
  // Retains supported flat/paged representations and stored root metadata/ACLs.
  static std::unique_ptr<WinFspNativeWritable> resume_managed_retired(
      std::shared_ptr<StoreByteIo> head,StoreIdentity volume_identity,std::uint64_t expected_epoch,
      const NativeVolumeImageResolver&,StoreCompressionOptions foreground={},
      std::shared_ptr<StoreByteIo> advice={},std::uint64_t expected_advice_sequence=0);
  // Reopen a privately owned memory backing after every previous callback joined.
  static std::unique_ptr<WinFspNativeWritable> resume_memory_volume(
      MemoryVolumeBacking&,NativeImageKind,StoreCompressionOptions foreground={});
  // Trusted low-priority controller opportunity, not a filesystem callback.
  // Try-lock/refuse foreground or deferred publication before backing access.
  // The owner may continue only the preparation kind offered by the preceding step.
  WinFspMaintenanceResult advance_background_maintenance(const NativeMaintenanceObservation&,std::uint64_t now_ms,
      NativeGroupSearchOpportunity={},WinFspMaintenanceContinuation=WinFspMaintenanceContinuation::none,
      bool scrub_due=false);
  bool background_maintenance_pending(bool scrub_due=false) const;
  void cancel_background_maintenance();
  // Before callback binding only. Duplicate a same-process event with signal
  // rights, without inheritance. The caller retains its own wait handle.
  void set_maintenance_event(HANDLE);
  bool maintenance_event_failed() const noexcept;
  std::uint64_t foreground_activity() const noexcept;
  bool foreground_pending() const noexcept;
  void signal_foreground_for_anchor(bool) noexcept;
  NativeMaintenancePolicy group_maintenance_policy() const noexcept;
  bool supports_anchor_search() const noexcept;
  // Before binding callbacks. Observe only successful eligible CPU writes.
  bool enable_amd_write_demand() noexcept;
  // A parked, already funded exchange can use the latest committed write
  // before its size has accumulated a full calibration history.
  std::optional<NativeAmdWriteDemand> take_amd_write_demand(bool from_arrival=false) noexcept;
  // Copy one or more admitted recent ranges from a checked immutable root.
  // Reads run outside the callback mutex and yield at native extent boundaries.
  std::vector<detail::NativeCapturedWrite> capture_amd_write_group(const NativeAmdWriteDemand&,
      const NativeMaintenanceObservation&,std::uint64_t now,std::uint64_t epoch,
      const std::atomic<bool>& stop,bool grouped=true,std::uint64_t maximum_input_bytes=UINT64_MAX,
      bool from_arrival=false) noexcept;
  std::vector<std::byte> capture_amd_write_source(const NativeAmdWriteDemand&,
      const NativeMaintenanceObservation&,std::uint64_t now,std::uint64_t epoch,
      const std::atomic<bool>& stop) noexcept;
  // One controller reads the demand, observes resources outside the callback
  // lock and returns a sample for exactly that still-current demand.
  std::optional<NativeGpuBudgetTask> gpu_budget_task() const;
  bool complete_gpu_budget_task(const NativeGpuBudgetTask&,const GpuMemorySnapshot&,std::uint64_t now);
  HostedVolumeMaintenanceResult recompress_managed_hosted(
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_image_identity,
      StoreCompressionOptions candidate={});
  HostedIdleMaintenanceResult begin_idle_recompression_hosted(
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_image_identity,
      StoreCompressionOptions candidate={});
  HostedIdleMaintenanceResult idle_recompression_hosted(HostedIdleMaintenanceAction);
  // Explicit per-image history cap; aligned 4..64 MiB. It is checked before
  // every projected append/write; not physical reservation/preallocation.
  static std::unique_ptr<WinFspNativeWritable> create_new_hosted(
      const std::filesystem::path&,StoreIdentity,StoreCompressionOptions,std::uint64_t max_image_bytes);
  // Internal hosted restart experiment, NOT a production existing-file API.
  // CALLER must first establish dispatcher/process retirement independently.
  // This declaration is not proof. Reject unresolved native session state.
  static std::unique_ptr<WinFspNativeWritable> resume_retired_hosted(
      const std::filesystem::path&,StoreIdentity,std::uint64_t expected_generation,
      StoreCompressionOptions,std::uint64_t max_image_bytes);
  ~WinFspNativeWritable();
  WinFspNativeWritable(const WinFspNativeWritable&)=delete;
  WinFspNativeWritable& operator=(const WinFspNativeWritable&)=delete;
  // Explicit hosted opt-in, before binding/mounting. The helper must be
  // independently qualified outside this volume; default factories stay CPU.
  void enable_deferred_writes_hosted(NativeDeferredWriteOptions);
  // The running volume owner may adopt a qualified route after mount.
  // Runtime discovery and cost qualification finish before this call.
  NativeDeferredActivation activate_qualified_deferred_writes(NativeDeferredWriteOptions&) noexcept;
  void publish_deferred_health_hosted(const GpuHealthEvidence&);
  NativeDeferredWriteStats deferred_write_stats() const;
  std::optional<NativeUniformWriteResult> deferred_write_receipt() const;
  NativeDeferredStop stop_deferred_writes_hosted();
  // A controller may capture one record from a live adopted context when
  // the callback lock is available and foreground work has yielded.
  bool capture_native_lz4_read(PVOID,std::uint64_t offset,std::size_t maximum_encoded,
      detail::NativeLz4CapturedFrame&) noexcept;
  // The owner publishes only complete, verified worker results after retirement.
  // Read callbacks consume exact-key results and otherwise use the CPU decoder.
  bool publish_native_lz4_reads(std::unique_ptr<detail::NativeLz4ReadyReads>&) noexcept;
  std::size_t native_lz4_read_hits() const;
  // A qualified controller can opt in before callbacks bind. One future extent
  // is prepared at an admitted idle opportunity after consecutive demand reads.
  bool enable_native_lz4_read_ahead();
  // The controller enables this on a file-backed session before callbacks bind.
  void enable_paired_seal_memory() noexcept;
  bool native_lz4_read_ahead_pending() const noexcept;
  // A current forecast yields one measured demand cost, or zero without a sample.
  std::optional<std::uint64_t> take_native_lz4_read_ahead_sample(std::uint64_t foreground_epoch) noexcept;
  bool advance_native_lz4_read_ahead(const NativeMaintenanceObservation&,std::uint64_t now_ms,
      std::uint64_t foreground_epoch,const std::atomic<bool>* stop_requested=nullptr) noexcept;
  std::optional<WinFspReadAheadWork> capture_native_lz4_read_ahead(const NativeMaintenanceObservation&,
      std::uint64_t now_ms,std::uint64_t foreground_epoch,const std::atomic<bool>& stop_requested,
      const std::function<void(const WinFspReadAheadWork&)>& before_group_capture={},
      WinFspGroupReadWindow group_window={}) noexcept;
  WinFspReadAheadState read_ahead_state(const WinFspReadAheadWork&) const noexcept;
  bool publish_read_ahead(WinFspReadAheadWork&,std::unique_ptr<detail::NativeLz4ReadyReads>,
      std::unique_ptr<detail::NativeGroupReadyReads>,
      const std::atomic<bool>& stop_requested) noexcept;
  bool prepare_captured_native_lz4_read(WinFspReadAheadWork&,const std::atomic<bool>& stop_requested) noexcept;
  // After a measured backing refusal, the matching CPU demand may refresh cost.
  void discard_native_lz4_read_ahead(std::uint64_t foreground_epoch,bool measure_demand=false) noexcept;
  std::uint64_t native_lz4_read_ahead_hits() const;
  void bind_callbacks(FSP_FILE_SYSTEM&,WinFspOriginQuery) noexcept;
  bool self_origin_rejected() const noexcept;
  bool mount_names_supported() const;
  FSP_FSCTL_VOLUME_PARAMS parameters() const noexcept;
  static const FSP_FILE_SYSTEM_INTERFACE& callbacks() noexcept;
  bool requires_offline() const noexcept;
  bool dispatcher_stopped() const noexcept;
  bool dispatcher_stopped_unexpectedly() const noexcept;
  // Controller only, never concurrently with destruction. These acquire the
  // same control lock as callbacks; no mutable writer escapes.
  NativeSessionStats stats() const;
  NativeFilesystemSnapshot snapshot() const;
  std::optional<detail::VolumeFileProfile> attached_file_profile() const noexcept;
 private:
  friend class WinFspMemoryVolume;
  void configure_memory_maintenance(MemoryVolumeBacking&) noexcept;
  static std::unique_ptr<WinFspNativeWritable> from_indexed_session(
      NativeFileSession,StoreIdentity volume,std::uint64_t created,NativeMaintenancePolicy,
      std::optional<detail::VolumeFileProfile> profile={});
  static std::unique_ptr<WinFspNativeWritable> resume_managed_impl(std::shared_ptr<StoreByteIo>,
      StoreIdentity,std::optional<std::uint64_t>,const NativeVolumeImageResolver&,StoreCompressionOptions,
      std::shared_ptr<StoreByteIo>,std::uint64_t,bool closed_indexed,MemoryVolumeBacking* memory=nullptr,
      NativeMaintenancePolicy group_policy={});
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit WinFspNativeWritable(std::unique_ptr<Impl>) noexcept;
};
}
