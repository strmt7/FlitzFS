#pragma once
#include "flitzfs/object_store.hpp"
#include <memory>

namespace flitzfs {
// Process-lifetime memory provider. No file/device/driver API.
// One domain per process; its lease remains alive until the last store retires.
struct MemoryVolumeObservation {
  std::uint64_t physical_total=0,physical_available=0,commit_total=0,commit_available=0;
};
MemoryVolumeObservation observe_memory_volume_headroom();
std::uint64_t usable_memory_volume_headroom(const MemoryVolumeObservation&);

struct MemoryVolumeUsage {
  std::uint64_t capacity=0,reserved=0,resident=0,stores=0;
};
class MemoryVolumeDomain final {
 public:
  static MemoryVolumeDomain create(std::uint64_t capacity);
  std::shared_ptr<StoreByteIo> create_store(std::uint64_t capacity);
  MemoryVolumeUsage usage() const noexcept;
  // Safe while the serialized writer grows the working-set allowance.
  void verify_working_set() const;
  // Full accounting and page checks require the owning control context.
  void verify_resident() const;
  bool owns(const StoreByteIo&) const noexcept;
  // One control thread, after all writers and mutation work have retired.
  // Success permanently denies writes and new stores across every alias.
  void seal_read_only();
  static bool cleanup_healthy() noexcept;
  ~MemoryVolumeDomain();
  MemoryVolumeDomain(MemoryVolumeDomain&&) noexcept;
  MemoryVolumeDomain(const MemoryVolumeDomain&)=delete;
  MemoryVolumeDomain& operator=(MemoryVolumeDomain&&)=delete;
 private:
  struct State;
  class Store;
  std::shared_ptr<State> state_;
  explicit MemoryVolumeDomain(std::shared_ptr<State>) noexcept;
};
} // namespace flitzfs
