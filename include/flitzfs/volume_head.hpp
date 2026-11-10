#pragma once
#include "flitzfs/native_filesystem.hpp"
#include <functional>
namespace flitzfs {
// Experimental custom-backend control record. No path/mount/worker/deletion API.
inline constexpr std::uint64_t kVolumeHeadBytes=2*kStoreCommitUnit;
struct NativeVolumeSelection {
  StoreIdentity volume_identity{},image_identity{},parent_identity{};
  std::uint64_t epoch=0,minimum_checkpoint=0,namespace_sequence=0,parent_checkpoint=0;
  bool operator==(const NativeVolumeSelection&) const = default;
};
struct NativeVolumeHeadInfo {NativeVolumeSelection selection;bool degraded=false;};
using NativeVolumeImageResolver=std::function<std::shared_ptr<StoreByteIo>(const StoreIdentity&)>;
class NativeVolumeHeadView final {
 public:
  // Resolver must own an immutable, globally unambiguous image-ID mapping.
  // The view owns its independently verified native image; never falls back.
  static NativeVolumeHeadView open(std::shared_ptr<StoreByteIo> head,StoreIdentity expected_volume,
      const NativeVolumeImageResolver&);
  NativeVolumeHeadView(NativeVolumeHeadView&&) noexcept;
  NativeVolumeHeadView& operator=(NativeVolumeHeadView&&) noexcept;
  ~NativeVolumeHeadView();
  NativeVolumeHeadInfo info() const noexcept;
  const NativeFilesystemSnapshot& filesystem() const;
 private:
  struct Impl;explicit NativeVolumeHeadView(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
class NativeVolumeHeadWriter final {
 public:
  // NEW empty head; source image must already be durable and quiescent.
  // A per-instance head lease is NOT global volume/provider authentication.
  static NativeVolumeHeadWriter create(std::shared_ptr<StoreByteIo> empty_head,
      StoreIdentity volume_identity,const NativeFilesystemSnapshot& source);
  // Exact epoch lease, actual old owner retirement and provider-wide exclusive
  // control required. Repair only this selected head, never an older image.
  static NativeVolumeHeadWriter resume(std::shared_ptr<StoreByteIo> head,
      StoreIdentity volume_identity,std::uint64_t expected_epoch,
      const NativeFilesystemSnapshot& selected_image);
  NativeVolumeHeadWriter(NativeVolumeHeadWriter&&) noexcept;
  NativeVolumeHeadWriter& operator=(NativeVolumeHeadWriter&&) noexcept;
  ~NativeVolumeHeadWriter();
  // Independently compare a current quiescent source and NEW compacted target.
  // Flush target BEFORE two separately flushed head publications. No source
  // writes, deletion, name replacement, same-ID fork or automatic retry.
  NativeVolumeHeadInfo publish_compacted(const NativeFilesystemSnapshot& source,
      const NativeFilesystemSnapshot& target);
  NativeVolumeHeadInfo info() const noexcept;
  bool poisoned() const noexcept;
 private:
  struct Impl;explicit NativeVolumeHeadWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
