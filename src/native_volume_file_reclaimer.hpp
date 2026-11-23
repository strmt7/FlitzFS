#pragma once
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/native_hybrid_backing.hpp"

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
  bool seal_preparation_pending(const NativeFileSession&) const noexcept;
  std::uint64_t projected_seal_writes(const NativeVolumeSelection&,std::uint64_t occupied) const noexcept;
  bool cold_target_available() const noexcept {return phase_==Phase::idle && spare_.use_count()==1;}
  void bind_wake(const std::shared_ptr<NativeVolumeRangeWake>&) noexcept;
  NativeVolumeReclaimProgress advance(NativeFileSession&,const std::function<StoreIdentity()>&,bool allow_cold);
  void cancel(NativeFileSession&) noexcept;
  NativeStartupRepairImage startup_repair_image(const NativeVolumeHeadInfo&,const std::function<StoreIdentity()>&);
  void adopt_startup_repair(const NativeVolumeSelection& original,const NativeVolumeSelection& repaired);
  void settle_recovery(std::uint64_t previous_epoch,const NativeVolumeSelection& selected);

 private:
  friend struct NativeVolumeFile::Regions;
  enum class Phase {idle,copying,ready,retiring,uncertain,failed};
  struct SealPlan {std::uint64_t workspace,staging,writes;};
  bool image_work_pending(const NativeFileSession&,bool allow_cold) const;
  std::optional<SealPlan> seal_plan(const NativeVolumeSelection&,std::uint64_t occupied) const;
  NativeVolumeReclaimProgress begin_seal(NativeFileSession&,StoreIdentity,const SealPlan&);
  NativeVolumeReclaimProgress advance_seal(NativeFileSession&);
  void check_inactive(const NativeVolumeHeadInfo&) const;
  NativeVolumeReclaimProgress retire(const NativeVolumeHeadInfo&);
  void adopt_selection() noexcept;
  void discard_seal() noexcept;
  std::shared_ptr<StoreByteIo> head_;
  std::shared_ptr<NativeVolumeFile::Region> active_,spare_;
  std::shared_ptr<NativeVolumeFile::CatalogRanges> catalog_;
  std::optional<NativeHybridBacking::Build> seal_;
  std::shared_ptr<NativeVolumeFile::Region> workspace_;
  std::uint64_t base_bytes_=0;
  Phase phase_;
  std::uint64_t attempted_obsolete_=0,abandoned_bytes_=0,attempted_repair_=0;
};

} // namespace flitzfs::detail
