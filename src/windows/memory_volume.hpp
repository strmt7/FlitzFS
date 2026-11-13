#pragma once
#include "flitzfs/object_store.hpp"
#include <memory>

namespace flitzfs {
// Experimental process-lifetime memory provider. No file/device/driver API.
// One domain per process; its lease remains alive until the last store retires.
struct MemoryVolumeUsage {
  std::uint64_t capacity=0,allocated=0,peak_allocated=0,stores=0;
};
class MemoryVolumeDomain final {
 public:
  static constexpr std::uint64_t maximum_capacity=40ULL*1024*1024;
  static MemoryVolumeDomain create(std::uint64_t capacity);
  std::shared_ptr<StoreByteIo> create_store(std::uint64_t capacity);
  MemoryVolumeUsage usage() const noexcept;
  void verify_resident() const; // Verify the lease and every currently owned store.
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
