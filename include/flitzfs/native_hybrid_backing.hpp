#pragma once

#include "flitzfs/native_filesystem.hpp"

namespace flitzfs {
class NativeFileSession;

// One ordinary mirrored indexed address space over a packed immutable base
// and a separately qualified writable tail. The caller owns both backings
// and resolves this pair as one image. The backing contract is serialized.
class NativeHybridBacking final : public StoreByteIo {
 public:
  static std::shared_ptr<NativeHybridBacking> create(const NativeFilesystemSnapshot& closed_source,
      std::shared_ptr<StoreByteIo> empty_workspace,std::shared_ptr<StoreByteIo> empty_base,
      std::shared_ptr<StoreByteIo> empty_tail,StoreIdentity new_image,std::uint64_t physical_limit,
      StoreCompressionOptions={});
  enum class BuildPhase { workspace, sealing, binding, ready, stopped };
  class Build;
  // Resources cover the new candidate and its workspace. The caller retains
  // the selected image and admits each advance on its serialized opportunity.
  static Build prepare(const NativeFileSession& source,std::shared_ptr<StoreByteIo> empty_workspace,
      std::shared_ptr<StoreByteIo> empty_base,std::shared_ptr<StoreByteIo> empty_tail,StoreIdentity new_image,
      std::uint64_t candidate_limit,std::uint64_t staging_limit,StoreCompressionOptions={});
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
  bool backing_degraded() const noexcept override;
  void verify_backing() const override;

 private:
  friend class NativeFileSession;
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
  bool advance(const NativeFileSession&);
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
