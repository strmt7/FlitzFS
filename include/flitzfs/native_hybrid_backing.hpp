#pragma once

#include "flitzfs/native_filesystem.hpp"

namespace flitzfs {
class NativeFileSession;
class NativeVolumeHeadView;
class NativeVolumeFile;
struct NativePairedSealedRange;
namespace detail {class NativeVolumeFileReclaimer;}

// The legacy prefix spaces all four control records at 64 KiB.
inline constexpr auto kNativeHybridTailPrefixBytes=4*kStoreCommitUnit;
enum class NativeHybridControls { legacy, compact };

// One ordinary mirrored indexed address space over a packed immutable base
// and a separately qualified writable tail. The caller owns both backings
// and resolves this pair as one image. The backing contract is serialized.
class NativeHybridBacking final : public StoreByteIo {
 public:
  static std::shared_ptr<NativeHybridBacking> create(const NativeFilesystemSnapshot& closed_source,
      std::shared_ptr<StoreByteIo> empty_workspace,std::shared_ptr<StoreByteIo> empty_base,
      std::shared_ptr<StoreByteIo> empty_tail,StoreIdentity new_image,std::uint64_t physical_limit,
      StoreCompressionOptions={},NativeHybridControls=NativeHybridControls::legacy);
  enum class BuildPhase { workspace, sealing, binding, ready, stopped, awaiting_base, awaiting_tail };
  class Build;
  // Resources cover the new candidate and its workspace. The caller retains
  // the selected image and admits each advance on its serialized opportunity.
  static Build prepare(const NativeFileSession& source,std::shared_ptr<StoreByteIo> empty_workspace,
      std::shared_ptr<StoreByteIo> empty_base,std::shared_ptr<StoreByteIo> empty_tail,StoreIdentity new_image,
      std::uint64_t candidate_limit,std::uint64_t staging_limit,StoreCompressionOptions={},
      NativeHybridControls=NativeHybridControls::legacy);
  // Allow an allocator to reserve an exact base and release the workspace
  // before reserving the writable tail. The source stays pinned throughout.
  static Build prepare(const NativeFileSession& source,std::shared_ptr<StoreByteIo> empty_workspace,
      StoreIoContract base_contract,StoreIdentity new_image,std::uint64_t candidate_limit,
      std::uint64_t staging_limit,StoreCompressionOptions={},NativeHybridControls=NativeHybridControls::legacy);
  static std::shared_ptr<NativeHybridBacking> open(std::shared_ptr<StoreByteIo> base,
      std::shared_ptr<StoreByteIo> tail,StoreIdentity expected_image,std::uint64_t virtual_limit,
      std::uint64_t physical_limit);
  ~NativeHybridBacking() override;

  std::uint64_t occupied() const;
  std::uint64_t extent() const override;
  void read_exact(std::uint64_t,std::span<std::byte>) const override;
  void write_exact(std::uint64_t,std::span<const std::byte>) override;
  void admit_write(std::uint64_t) override;
  void flush() override;
  StoreIoContract contract() const noexcept override;
  bool concurrent_reads_supported() const noexcept override;
  bool backing_degraded() const noexcept override;
  void verify_backing() const override;

 private:
  friend class NativeFileSession;
  friend class NativeVolumeHeadView;
  friend class NativeVolumeFile;
  friend class detail::NativeVolumeFileReclaimer;
  friend class MemoryVolumeDomain;
  friend struct MemoryVolumeBacking;
  static Build prepare_impl(const NativeFileSession&,std::shared_ptr<StoreByteIo>,StoreIoContract,
      StoreIdentity,std::uint64_t candidate_limit,std::uint64_t staging_limit,StoreCompressionOptions,
      bool owned_file_target,NativeHybridControls);
  static std::uint64_t workspace_capacity(StoreIdentity,StoreIoContract,
      std::uint64_t packed_end_bound,std::uint64_t staging_bytes,std::uint64_t private_workspace_bytes);
  NativePairedSealedRange sealed_range() const noexcept;
  std::array<std::shared_ptr<StoreByteIo>,2> components() const noexcept;
  void verify_prepared_binding() const;
  struct Impl;
  explicit NativeHybridBacking(std::unique_ptr<Impl>) noexcept;
  bool allows_writer() const noexcept override;
  std::unique_ptr<Impl> impl_;
};

// Partial output is retained for the caller on cancellation. The source
// identity and checkpoint must still match when the completed backing leaves.
class NativeHybridBacking::Build final {
 public:
  Build(Build&&) noexcept;
  Build& operator=(Build&&) noexcept;
  ~Build();
  BuildPhase phase() const noexcept;
  bool source_current(const NativeFileSession&) const noexcept;
  bool can_continue_preparation() const noexcept;
  bool advance(const NativeFileSession&);
  // Returns zero unless a store is requested. Reserve the reported bytes for
  // the exact base or the full admissible writable tail.
  std::uint64_t required_store_bytes() const noexcept;
  void provide_store(const NativeFileSession&,std::shared_ptr<StoreByteIo>);
  std::shared_ptr<NativeHybridBacking> finish(const NativeFileSession&);
  // Select a smaller verified image on the same serialized session. The
  // caller retains the source and its physical mapping, and owns the new
  // base/tail mapping before calling. This sets an empty replacement slot
  // before selector mutation. An uncertain handoff keeps both images for
  // recovery. A declined candidate leaves the slot empty.
  bool publish(NativeFileSession&,std::shared_ptr<NativeHybridBacking>& replacement);
  void cancel() noexcept;
 private:
  friend class NativeHybridBacking;
  struct Impl;
  explicit Build(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace flitzfs
