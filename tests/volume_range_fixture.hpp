#pragma once
#include "flitzfs/native_volume_file.hpp"
#include "native_fs_fixture.hpp"
#include <mutex>
#include <vector>

namespace native_fixture {
using flitzfs::kStoreCommitUnit;

// Retain allocations separately from bytes. A written zero block consumes a
// unit until range retirement, and replay retains only durable allocations.
class SparseVolumeRam final:public flitzfs::NativeVolumeRangeIo {
  mutable std::mutex access_;
  const bool capture_;
  std::vector<bool> live_,stable_;
  void mark(std::uint64_t offset,std::uint64_t bytes,bool allocated) {
    if(offset>io.capacity || bytes>io.capacity-offset)
      throw std::runtime_error("sparse model allocation outside fixture");
    for(auto slot=offset/kStoreCommitUnit;slot<(offset+bytes+kStoreCommitUnit-1)/kStoreCommitUnit;++slot)
      live_[static_cast<std::size_t>(slot)]=allocated;
  }
 public:
  Ram io;
  unsigned range_reclaims=0;
  std::uint64_t reclaimed_model_write_bytes=0;
  std::uint64_t admitted_end=UINT64_MAX;
  std::uint64_t admitted_growth=UINT64_MAX;
  explicit SparseVolumeRam(std::uint32_t unit,std::uint64_t capacity=flitzfs::kStoreMaxPhysicalBytes,
      bool concurrent_capture=false):capture_(concurrent_capture) {
    if(!capacity || capacity%kStoreCommitUnit)throw std::invalid_argument("sparse model capacity geometry");
    live_.resize(static_cast<std::size_t>(capacity/kStoreCommitUnit));
    stable_=live_;
    io.failure_unit=unit;
    io.capacity=capacity;
  }
  std::shared_ptr<SparseVolumeRam> replay() const {
    std::lock_guard lock(access_);
    auto recovered=std::make_shared<SparseVolumeRam>(io.failure_unit,io.capacity,capture_);
    recovered->io.live=recovered->io.stable=io.stable;
    recovered->live_=recovered->stable_=stable_;
    return recovered;
  }
  std::uint64_t extent() const override {std::lock_guard lock(access_);return io.extent();}
  bool concurrent_reads_supported() const noexcept override {return capture_;}
  void read_exact(std::uint64_t offset,std::span<std::byte> bytes) const override {
    std::lock_guard lock(access_);io.read_exact(offset,bytes);
  }
  void write_exact(std::uint64_t offset,std::span<const std::byte> bytes) override {
    std::lock_guard lock(access_);
    if(!bytes.empty())mark(offset,bytes.size(),true);
    io.write_exact(offset,bytes);
  }
  void flush() override {
    std::lock_guard lock(access_);
    if(io.calls!=io.fail_at || io.failure!=0)stable_=live_;
    io.flush();
  }
  flitzfs::StoreIoContract contract() const noexcept override {return {io.failure_unit,true,true};}
  bool supports_reclaimable_ranges() const noexcept override {return true;}
  void admit_write(std::uint64_t) override {throw std::runtime_error("sparse model requires range admission");}
  void admit_range_write(std::uint64_t projected,std::uint64_t additional) override {
    std::lock_guard lock(access_);
    if(projected>admitted_end || additional>admitted_growth)
      throw std::runtime_error("modeled backing cannot extend further");
    io.admit_write(projected);
  }
  std::uint64_t allocated_extent(std::uint64_t offset,std::uint64_t bytes) const override {
    std::lock_guard lock(access_);
    const auto start=offset/kStoreCommitUnit;
    for(auto pos=start+bytes/kStoreCommitUnit;pos>start;--pos)
      if(live_.at(static_cast<std::size_t>(pos-1)))return (pos-start)*kStoreCommitUnit;
    return 0;
  }
  void reclaim_range(std::uint64_t offset,std::uint64_t bytes) override {
    std::lock_guard lock(access_);
    if(!has_writer() || offset<flitzfs::kNativeVolumeFileImageOffset ||
        offset%kStoreCommitUnit || bytes%kStoreCommitUnit || offset>=io.extent())
      throw std::runtime_error("sparse model reclamation outside owned range");
    ++range_reclaims;
    const auto length=(std::min)(bytes,io.extent()-offset);
    io.write_exact(offset,Bytes(static_cast<std::size_t>(length)));
    reclaimed_model_write_bytes+=length;
    mark(offset,bytes,false);
  }
  std::uint64_t occupied() const {std::lock_guard lock(access_);return std::ranges::count(live_,true)*kStoreCommitUnit;}
};
}
