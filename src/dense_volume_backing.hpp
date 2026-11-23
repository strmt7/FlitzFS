#pragma once

#include "flitzfs/native_volume_file.hpp"

namespace flitzfs::detail {

// Run-mapped virtual stream over an exclusively owned ordinary file. The
// owner of the inner filesystem remains responsible for retiring logical
// ranges and preventing reads through any checkpoint that still uses them.
class DenseVolumeBacking final : public NativeVolumeRangeIo {
 public:
  static std::uint64_t payload_capacity(std::uint64_t backing_bytes) noexcept;
  static bool recognizes(const StoreByteIo&);
  static std::shared_ptr<DenseVolumeBacking> create(std::shared_ptr<StoreByteIo>,
      StoreIdentity volume, std::uint64_t logical_limit, std::uint64_t physical_limit);
  static std::shared_ptr<DenseVolumeBacking> open(std::shared_ptr<StoreByteIo>, bool writable);
  ~DenseVolumeBacking() override;

  StoreIdentity volume_identity() const noexcept;
  std::uint64_t logical_limit() const noexcept;
  std::uint64_t extent() const override;
  void read_exact(std::uint64_t, std::span<std::byte>) const override;
  void write_exact(std::uint64_t, std::span<const std::byte>) override;
  void flush() override;
  void admit_write(std::uint64_t) override;
  StoreIoContract contract() const noexcept override;
  bool concurrent_reads_supported() const noexcept override;
  bool supports_reclaimable_ranges() const noexcept override;
  std::uint64_t allocated_extent(std::uint64_t offset, std::uint64_t bytes) const override;
  void admit_range_write(std::uint64_t projected, std::uint64_t additional) override;
  void admit_contiguous_growth(std::uint64_t projected, std::uint64_t begin, std::uint64_t end) override;
  void reclaim_range(std::uint64_t offset, std::uint64_t bytes) override;
  NativeDenseFileSpace file_space() const;
  bool tail_work_pending(std::uint64_t budget) const;
  bool advance_tail(std::uint64_t budget);

 private:
  struct Impl;
  explicit DenseVolumeBacking(std::unique_ptr<Impl>);
  bool allows_writer() const noexcept override;
  std::unique_ptr<Impl> state_;
};

} // namespace flitzfs::detail
