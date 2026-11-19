#pragma once
#include "flitzfs/native_session.hpp"
#include <filesystem>

namespace flitzfs {

// The existing indexed head declares the identity and capacity of the data
// behind the two fixed metadata regions.
inline constexpr std::uint64_t kNativeVolumeFileImageOffset=kVolumeHeadBytes+kOptimizationAreaBytes;

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
      NativeDispositionPolicy=NativeDispositionPolicy::aggregate);
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
  struct Owner;
  struct Region;
  struct Regions {
    std::shared_ptr<StoreByteIo> head,image,advice;
    NativeVolumeSelection selection;
    NativeVolumeImageResolver resolver() const;
    std::uint64_t advice_sequence() const;
  };
  static Regions new_regions(std::shared_ptr<StoreByteIo>,PagedStoreLimits);
  static Regions open_regions(std::shared_ptr<StoreByteIo>,bool writable,std::optional<StoreIdentity> expected);
#ifdef _WIN32
  static std::shared_ptr<StoreByteIo> create_backing(const std::filesystem::path&,StoreIoContract,PagedStoreLimits);
  static std::shared_ptr<StoreByteIo> reopen_backing(const std::filesystem::path&,StoreIoContract);
#endif
};

} // namespace flitzfs
