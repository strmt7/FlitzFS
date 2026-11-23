#pragma once
#include "flitzfs/object_store.hpp"
#include <optional>
#include <string>
#include <string_view>
namespace flitzfs::detail {
// Only the dense owner decides that a suffix has no durable mappings or
// in-flight readers. The provider independently checks its lease and EOF.
class DenseFileTailIo {
 public:
  virtual ~DenseFileTailIo()=default;
  virtual std::optional<std::uint64_t> allocated_file_bytes(std::uint64_t expected_end) const=0;
  virtual void shorten_file(std::uint64_t expected_end,std::uint64_t retained_end)=0;
};
std::shared_ptr<StoreByteIo> new_store_file(const std::filesystem::path&,StoreIoContract);
std::shared_ptr<StoreByteIo> read_store_file(const std::filesystem::path&,
    std::uint64_t maximum_physical_bytes=kStoreMaxPhysicalBytes);
#ifdef _WIN32
// Persistent volume files have a caller-qualified contract and independent
// extent limits. Opening never truncates, follows a reparse path or shares a writer.
struct VolumeFileGeometry {
  std::uint64_t maximum_extent;
  bool isolated_sparse_ranges;
  std::uint64_t allocation_unit_bytes=0;
};
VolumeFileGeometry volume_file_geometry(std::wstring_view filesystem,bool supports_sparse_files,
    std::uint64_t cluster_bytes) noexcept;

struct VolumeFileDestination {
  std::wstring volume_root;
  std::uint64_t available_growth;
  VolumeFileGeometry geometry;
};
VolumeFileDestination volume_file_destination(const std::filesystem::path&);
// Snapshot from the qualified open file and its pinned volume. Zero allocation
// or performance units mean the provider supplied no admitted value.
struct VolumeFileProfile {
  std::uint64_t maximum_extent=0,allocation_unit_bytes=0;
  std::uint32_t logical_sector_bytes=0,physical_atomicity_bytes=0,physical_performance_bytes=0;
  std::uint32_t effective_atomicity_bytes=0,alignment_flags=0,qualified_failure_unit_bytes=0;
  bool removable_reported=false,isolated_sparse_ranges=false;
  bool operator==(const VolumeFileProfile&) const noexcept=default;
};
std::optional<VolumeFileProfile> volume_file_profile(const StoreByteIo*) noexcept;
std::shared_ptr<StoreByteIo> new_volume_file(const std::filesystem::path&,StoreIoContract,std::uint64_t limit,
    std::wstring_view expected_volume={});
std::shared_ptr<StoreByteIo> reopen_volume_file(const std::filesystem::path&,StoreIoContract,std::uint64_t limit,
    std::wstring_view expected_volume={});
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
// The disposable mounted probe observes one fully qualified backing. Callbacks
// run inside its I/O lock and cannot perform additional I/O through that backing.
class VolumeFileObserver {
 public:
  virtual ~VolumeFileObserver()=default;
  virtual void before_write(std::uint64_t offset,std::size_t bytes)=0;
  virtual void before_flush() {}
  virtual void after_flush()=0;
  virtual void before_reclaim(std::uint64_t offset,std::uint64_t bytes)=0;
};
std::shared_ptr<StoreByteIo> new_observed_volume_file(const std::filesystem::path&,StoreIoContract,
    std::uint64_t limit,std::shared_ptr<VolumeFileObserver>);
#endif
#endif
// Test-only owned-fixture adapter; caller must qualify backing and containment.
// No production factory, CLI route, or permission to modify user files.
std::shared_ptr<StoreByteIo> reopen_store_file_for_hosted_test(const std::filesystem::path&,StoreIoContract);
}
