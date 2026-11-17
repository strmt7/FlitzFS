#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "native_name_index.hpp"

namespace flitzfs::detail {

struct IndexedVolumeRoot {
  StoreIdentity identity{};
  std::uint64_t sequence=0,next_inode=2,next_extent=1;
  std::uint64_t inodes=0,extents=0,charged_extents=0;
  NativeWorkloadInfo workload;
};
struct IndexedInodeInput {
  std::uint64_t inode=0,parent=0,size=0,born=0,modified=0;
  bool directory=false;
  std::string name;
  std::optional<NativeBasicMetadata> basic;
  std::optional<NativeSecurityDescriptor> security;
  std::optional<std::uint64_t> allocation;
};
// The indexed format uses the existing canonical inode frame with zero
// inline extents. Extent locations live in the ordered index.
std::vector<std::byte> encode_indexed_inode(const IndexedInodeInput&,const IndexedVolumeRoot&);
IndexedInodeInput decode_indexed_inode(std::span<const std::byte>,std::uint64_t,
    const IndexedVolumeRoot&);
bool indexed_volume_root(std::span<const std::byte>) noexcept;
struct IndexedDirectoryPage {
  std::vector<NativeFileInfo> entries;
  std::optional<std::string> after;
};
class IndexedVolumeView final {
 public:
  IndexedVolumeView(const PagedObjectStoreSnapshot&,std::span<const std::byte> root);
  IndexedVolumeView(PagedObjectStoreSnapshot&&,std::span<const std::byte>)=delete;
  const IndexedVolumeRoot& root() const noexcept {return root_;}
  IndexedInodeInput inode(std::uint64_t id) const;
  IndexedInodeInput resolve(std::string_view path,std::string* spelling=nullptr) const;
  NativeFileInfo stat_inode(std::uint64_t id) const;
  IndexedDirectoryPage list(std::uint64_t directory,std::string_view after,std::size_t maximum) const;
  std::vector<std::byte> read(const IndexedInodeInput&,std::uint64_t offset,std::uint32_t bytes) const;
  void verify_all() const;

 private:
  friend class IndexedVolumeBuilder;
  IndexedVolumeView(const PagedObjectStoreSnapshot&,IndexedVolumeRoot root);
  IndexedInodeInput referenced_inode(std::uint64_t id) const;
  void verify_inventory(bool published) const;
  std::string path(const IndexedInodeInput&) const;
  const PagedObjectStoreSnapshot& store_;
  IndexedVolumeRoot root_;
  NativeNameIndex index_;
};
struct IndexedExtentInput {
  std::uint64_t inode=0,block=0;
  std::span<const std::byte> bytes;
};

// Build on a new, exclusive mirrored store. Until finish commits the
// namespace root, the staging image cannot open as a native filesystem.
// One owning control thread. Each append copies only the supplied batch.
class IndexedVolumeBuilder final {
 public:
  IndexedVolumeBuilder(PagedObjectStoreWriter,std::uint64_t sequence,
      std::uint64_t next_inode,NativeWorkloadInfo);
  IndexedVolumeBuilder(const IndexedVolumeBuilder&)=delete;
  IndexedVolumeBuilder& operator=(const IndexedVolumeBuilder&)=delete;
  void append(std::span<const IndexedInodeInput> inodes,std::span<const IndexedExtentInput> extents={});
  NativeFilesystemSnapshot finish();

 private:
  std::optional<PagedObjectStoreWriter> writer_;
  IndexedVolumeRoot root_;
};
} // namespace flitzfs::detail
