#pragma once
#include "flitzfs/native_session.hpp"
#include <filesystem>

namespace flitzfs {

// The existing indexed head declares the identity and capacity of the data
// behind the two fixed metadata regions.
inline constexpr std::uint64_t kNativeVolumeFileImageOffset=kVolumeHeadBytes+kOptimizationAreaBytes;

// Explicit range-placement provider. The allocation bound is relative to the
// requested range and aligned to kStoreCommitUnit. Failed reclamation may
// affect only that range. A successful flush persists the resulting allocation.
class NativeVolumeRangeIo : public StoreByteIo {
 public:
  virtual bool supports_reclaimable_ranges() const noexcept=0;
  virtual std::uint64_t allocated_extent(std::uint64_t offset,std::uint64_t bytes) const=0;
  virtual void admit_range_write(std::uint64_t projected_file_extent,std::uint64_t additional_bytes)=0;
  virtual void reclaim_range(std::uint64_t offset,std::uint64_t bytes)=0;
};

// Composes the indexed engine over a single qualified byte-I/O owner. The
// provider must pin the exact backing and exclude other writable handles.
// Inspection requires stable backing and performs no recovery writes.
class NativeVolumeFile final {
 public:
  static NativeFileSession create(std::shared_ptr<StoreByteIo> empty,StoreIdentity volume,
      StoreIdentity image,PagedStoreLimits,StoreCompressionOptions foreground={},
      NativeDispositionPolicy=NativeDispositionPolicy::aggregate,
      std::optional<NativeVolumeRootMetadata> root={});
  static NativeFileSession resume(std::shared_ptr<StoreByteIo> existing,
      std::optional<StoreIdentity> expected_volume=std::nullopt,StoreCompressionOptions foreground={},
      NativeDispositionPolicy=NativeDispositionPolicy::aggregate,
      const std::function<StoreIdentity()>& startup_repair_identity={});
  static NativeVolumeHeadView inspect(std::shared_ptr<StoreByteIo> existing,
      std::optional<StoreIdentity> expected_volume=std::nullopt);
#ifdef _WIN32
  // Exclusive OS creation and reopen of an explicit local file. The contract
  // comes from independent provider qualification and must fit live geometry.
  static NativeFileSession create(const std::filesystem::path&,StoreIoContract,StoreIdentity volume,
      StoreIdentity image,PagedStoreLimits,StoreCompressionOptions foreground={},
      NativeDispositionPolicy=NativeDispositionPolicy::aggregate,
      std::optional<NativeVolumeRootMetadata> root={});
  static NativeFileSession resume(const std::filesystem::path&,StoreIoContract,
      std::optional<StoreIdentity> expected_volume=std::nullopt,StoreCompressionOptions foreground={},
      NativeDispositionPolicy=NativeDispositionPolicy::aggregate);
  static NativeVolumeHeadView inspect(const std::filesystem::path&,
      std::optional<StoreIdentity> expected_volume=std::nullopt);
#endif

 private:
  friend class WinFspNativeWritable;
  friend class WinFspVolumeAttachment;
  friend class detail::NativeVolumeFileReclaimer;
  struct Owner;
  struct Region;
  struct CatalogRanges;
  struct Regions {
    std::shared_ptr<StoreByteIo> head,image,advice;
    NativeVolumeSelection selection;
    std::shared_ptr<detail::NativeVolumeFileReclaimer> reclaimer;
    NativeVolumeImageResolver resolver() const;
    std::uint64_t advice_sequence() const;
    void configure_catalog(NativeFileSession&) const noexcept;
  };
  static Regions new_regions(std::shared_ptr<StoreByteIo>,StoreIdentity,PagedStoreLimits);
  static Regions open_regions(std::shared_ptr<StoreByteIo>,bool writable,std::optional<StoreIdentity> expected);
  static NativeFileSession resume_regions(Regions,StoreCompressionOptions,NativeDispositionPolicy,
      const std::function<StoreIdentity()>& startup_repair_identity,const detail::IndexedReopenAdmission& admission={},
      const std::function<void(StoreIdentity)>& retain_verified={});
#ifdef _WIN32
  static std::shared_ptr<StoreByteIo> create_backing(const std::filesystem::path&,StoreIoContract,PagedStoreLimits);
  static std::shared_ptr<StoreByteIo> reopen_backing(const std::filesystem::path&,StoreIoContract);
#endif
};

} // namespace flitzfs
