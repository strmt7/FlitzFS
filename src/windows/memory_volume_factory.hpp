#pragma once
#include "memory_volume.hpp"
#include "flitzfs/projection.hpp"
namespace flitzfs {
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
  // Independently checks the current selected image, closes writer admission,
  // and returns a CPU projection that retains the backing lease. The owner
  // keeps its original data even if a later read-only mount fails.
  ReadOnlyProjection open_read_only();
};
}
