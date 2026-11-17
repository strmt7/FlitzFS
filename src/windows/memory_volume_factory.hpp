#pragma once
#include "memory_volume.hpp"
namespace flitzfs {
struct MemoryVolumeObservation {
  std::uint64_t physical_total=0,physical_available=0,commit_total=0,commit_available=0;
};
struct MemoryVolumeLayout {
  std::uint64_t domain_capacity=0,image_capacity=0;
  static constexpr std::uint64_t area_capacity=131072;
};
// Pure admission calculation for trusted observations. It does not allocate or
// authorize a provider. The actual factory always collects fresh OS headroom.
MemoryVolumeLayout plan_memory_volume(const MemoryVolumeObservation&);
StoreIdentity fresh_volume_identity();
// Move-only new backing; declaration order retains the domain until all store
// references retire. Each store independently retains the provider lease too.
struct MemoryVolumeBacking {
  std::unique_ptr<MemoryVolumeDomain> domain;
  std::shared_ptr<StoreByteIo> head,image,advice;
  StoreIdentity volume_identity{},image_identity{};
  MemoryVolumeLayout layout;
  static MemoryVolumeBacking create();
};
}
