#pragma once
#include "native_file_info.hpp"
#include <filesystem>
#include <memory>
namespace flitzfs {
// Experimental real writable adapter. Factory is restricted to disposable
// hosted VMs until its kernel/lifecycle/backing qualification is complete.
// Default factories create NEW images. The explicit hosted resume experiment
// requires independently confirmed old-owner retirement; never selects a driver.
struct HostedVolumeMaintenanceResult {
  NativeVolumeHeadInfo head;
  bool recovered=false; // A failed transition was independently resolved.
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
  HostedVolumeMaintenanceResult recompress_managed_hosted(
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_image_identity,
      StoreCompressionOptions candidate={});
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
  void bind_callbacks(FSP_FILE_SYSTEM&) noexcept;
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
