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
      std::shared_ptr<NativeVolumeFile::Region> spare,bool recover_spare);
  bool pending(const NativeFileSession&) const;
  void bind_wake(const std::shared_ptr<NativeVolumeRangeWake>&) noexcept;
  NativeVolumeReclaimProgress advance(NativeFileSession&,const std::function<StoreIdentity()>&);
  void cancel(NativeFileSession&) noexcept;
  NativeStartupRepairImage startup_repair_image(const NativeVolumeHeadInfo&,const std::function<StoreIdentity()>&);
  void adopt_startup_repair(const NativeVolumeSelection& original,const NativeVolumeSelection& repaired);

 private:
  enum class Phase {idle,copying,ready,retiring,uncertain,failed};
  NativeVolumeReclaimProgress retire(const NativeVolumeHeadInfo&);
  std::shared_ptr<StoreByteIo> head_;
  std::shared_ptr<NativeVolumeFile::Region> active_,spare_;
  Phase phase_;
  std::uint64_t attempted_obsolete_=0,abandoned_bytes_=0,attempted_repair_=0;
};

} // namespace flitzfs::detail
