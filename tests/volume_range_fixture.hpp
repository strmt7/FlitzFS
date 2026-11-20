#pragma once
#include "flitzfs/native_volume_file.hpp"
#include "native_fs_fixture.hpp"
#include <bitset>

namespace native_fixture {
using flitzfs::kStoreCommitUnit;

// Retain allocations separately from bytes. A written zero block consumes a
// unit until range retirement, and replay retains only durable allocations.
class SparseVolumeRam final:public flitzfs::NativeVolumeRangeIo {
  static constexpr std::size_t unit_count=flitzfs::kStoreMaxPhysicalBytes/kStoreCommitUnit;
  std::bitset<unit_count> live_,stable_;
  void mark(std::uint64_t offset,std::uint64_t bytes,bool allocated) {
    for(auto slot=offset/kStoreCommitUnit;slot<(offset+bytes+kStoreCommitUnit-1)/kStoreCommitUnit;++slot)
      live_.set(static_cast<std::size_t>(slot),allocated);
  }
 public:
  Ram io;
  unsigned range_reclaims=0;
  std::uint64_t admitted_end=UINT64_MAX;
  explicit SparseVolumeRam(std::uint32_t unit){io.failure_unit=unit;}
  std::shared_ptr<SparseVolumeRam> replay() const {
    auto recovered=std::make_shared<SparseVolumeRam>(io.failure_unit);
    recovered->io.live=recovered->io.stable=io.stable;
    recovered->live_=recovered->stable_=stable_;
    return recovered;
  }
  std::uint64_t extent() const override {return io.extent();}
  void read_exact(std::uint64_t offset,std::span<std::byte> bytes) const override {io.read_exact(offset,bytes);}
  void write_exact(std::uint64_t offset,std::span<const std::byte> bytes) override {
    if(!bytes.empty())mark(offset,bytes.size(),true);
    io.write_exact(offset,bytes);
  }
  void flush() override {
    if(io.calls!=io.fail_at || io.failure!=0)stable_=live_;
    io.flush();
  }
  flitzfs::StoreIoContract contract() const noexcept override {return {io.failure_unit,true,true};}
  bool supports_reclaimable_ranges() const noexcept override {return true;}
  void admit_write(std::uint64_t) override {throw std::runtime_error("sparse model requires range admission");}
  void admit_range_write(std::uint64_t projected,std::uint64_t) override {
    if(projected>admitted_end)throw std::runtime_error("modeled backing cannot extend further");
    io.admit_write(projected);
  }
  std::uint64_t allocated_extent(std::uint64_t offset,std::uint64_t bytes) const override {
    const auto start=offset/kStoreCommitUnit;
    for(auto pos=start+bytes/kStoreCommitUnit;pos>start;--pos)
      if(live_.test(static_cast<std::size_t>(pos-1)))return (pos-start)*kStoreCommitUnit;
    return 0;
  }
  void reclaim_range(std::uint64_t offset,std::uint64_t bytes) override {
    if(!has_writer() || offset<flitzfs::kNativeVolumeFileImageOffset ||
        offset%kStoreCommitUnit || bytes%kStoreCommitUnit || offset>=io.extent())
      throw std::runtime_error("sparse model reclamation outside owned range");
    ++range_reclaims;
    const auto length=(std::min)(bytes,io.extent()-offset);
    io.write_exact(offset,Bytes(static_cast<std::size_t>(length)));
    mark(offset,bytes,false);
  }
  std::uint64_t occupied() const noexcept {return live_.count()*kStoreCommitUnit;}
};
}
