#pragma once
#include "flitzfs/native_volume_file.hpp"

namespace flitzfs::detail {

// The last image owner may release its pin outside the storage control thread.
// Waking the controller performs no storage I/O.
class NativeVolumeRangeWake {
 public:
  virtual ~NativeVolumeRangeWake()=default;
  virtual void signal() noexcept=0;
};

// The two image aliases and the selector refer to one exclusive backing.
// Engine snapshots pin the exact Region that was active when they opened.
class NativeVolumeFileReclaimer final {
 public:
  NativeVolumeFileReclaimer(std::shared_ptr<StoreByteIo> head,
      std::shared_ptr<NativeVolumeFile::Region> active,
      std::shared_ptr<NativeVolumeFile::Region> spare,bool recover_spare,
      std::shared_ptr<NativeVolumeFile::CatalogRanges> catalog);
  bool pending(const NativeFileSession&,bool allow_cold) const;
  void bind_wake(const std::shared_ptr<NativeVolumeRangeWake>&) noexcept;
  NativeVolumeReclaimProgress advance(NativeFileSession&,const std::function<StoreIdentity()>&,bool allow_cold);
  void cancel(NativeFileSession&) noexcept;
  NativeStartupRepairImage startup_repair_image(const NativeVolumeHeadInfo&,const std::function<StoreIdentity()>&);
  void adopt_startup_repair(const NativeVolumeSelection& original,const NativeVolumeSelection& repaired);

 private:
  friend struct NativeVolumeFile::Regions;
  enum class Phase {idle,copying,ready,retiring,uncertain,failed};
  bool image_work_pending(const NativeFileSession&,bool allow_cold) const;
  NativeVolumeReclaimProgress retire(const NativeVolumeHeadInfo&);
  std::shared_ptr<StoreByteIo> head_;
  std::shared_ptr<NativeVolumeFile::Region> active_,spare_;
  std::shared_ptr<NativeVolumeFile::CatalogRanges> catalog_;
  Phase phase_;
  std::uint64_t attempted_obsolete_=0,abandoned_bytes_=0,attempted_repair_=0;
};

} // namespace flitzfs::detail
