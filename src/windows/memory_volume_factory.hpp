#pragma once
#include "memory_volume.hpp"
#include "flitzfs/projection.hpp"
namespace flitzfs {
class NativeFileSession;
enum class MemoryCompactionStep {idle,copying,ready,published,retiring,pinned,failed};
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
  // Called with the same volume lock as foreground I/O. The scheduler must
  // admit each opportunity with live resource and contention observations.
  bool compaction_pending(const NativeFileSession&) const;
  MemoryCompactionStep advance_compaction(NativeFileSession&);
  void cancel_compaction(NativeFileSession&) noexcept;
  // Owning controller and all of its callbacks must already have joined.
  void release_unselected_after_join() noexcept;
 private:
  friend class WinFspMemoryVolume;
  std::shared_ptr<StoreByteIo> resolve_image(const StoreIdentity&) const;
  NativeVolumeHeadView open_owned_head(bool indexed_recovery=false) const;
  void settle_retained_selection(const StoreIdentity&) noexcept;
  ReadOnlyProjection seal_checked(NativeVolumeHeadView);
  enum class CompactionPhase {idle,copying,ready,uncertain,retiring};
  CompactionPhase phase_=CompactionPhase::idle;
  std::shared_ptr<StoreByteIo> alternate_;
  StoreIdentity alternate_identity_{};
  std::uint64_t attempted_obsolete_=0,abandoned_bytes_=0;
  void retire_alternate(bool charge_attempt);
};
}
