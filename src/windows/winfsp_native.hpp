#pragma once
#include "native_file_info.hpp"
#include "native_deferred_write.hpp"
#include "winfsp_origin.hpp"
#include "flitzfs/native_maintenance.hpp"
#include <filesystem>
#include <memory>
namespace flitzfs {
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
enum class WinFspGroupMaintenanceState {busy,unavailable,unchanged,advanced};
struct WinFspGroupMaintenanceResult {
  WinFspGroupMaintenanceState state=WinFspGroupMaintenanceState::unavailable;
  NativeMaintenanceProgress progress;
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
  // Qualified exact head epoch/resolver after actual previous-owner retirement.
  // Retains supported flat/paged representations and stored root metadata/ACLs.
  static std::unique_ptr<WinFspNativeWritable> resume_managed_retired(
      std::shared_ptr<StoreByteIo> head,StoreIdentity volume_identity,std::uint64_t expected_epoch,
      const NativeVolumeImageResolver&,StoreCompressionOptions foreground={},
      std::shared_ptr<StoreByteIo> advice={},std::uint64_t expected_advice_sequence=0);
  // Trusted low-priority controller opportunity, not a filesystem callback.
  // Try-lock/refuse foreground or deferred publication before backing access.
  // Caller supplies fresh observations and owns serialization with stop.
  WinFspGroupMaintenanceResult advance_group_maintenance(const NativeMaintenanceObservation&,std::uint64_t now_ms);
  bool group_maintenance_pending() const;
  void cancel_group_maintenance();
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
  void publish_deferred_health_hosted(const GpuHealthEvidence&);
  NativeDeferredWriteStats deferred_write_stats() const;
  NativeDeferredStop stop_deferred_writes_hosted();
  void bind_callbacks(FSP_FILE_SYSTEM&,WinFspOriginQuery) noexcept;
  bool self_origin_rejected() const noexcept;
  FSP_FSCTL_VOLUME_PARAMS parameters() const noexcept;
  static const FSP_FILE_SYSTEM_INTERFACE& callbacks() noexcept;
  bool requires_offline() const noexcept;
  bool dispatcher_stopped() const noexcept;
  bool dispatcher_stopped_unexpectedly() const noexcept;
  // Controller only, never concurrently with destruction. These acquire the
  // same control lock as callbacks; no mutable writer escapes.
  NativeSessionStats stats() const;
  NativeFilesystemSnapshot snapshot() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit WinFspNativeWritable(std::unique_ptr<Impl>) noexcept;
};
}
