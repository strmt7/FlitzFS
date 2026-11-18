#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "native_group_codec.hpp"
#include "native_name_index.hpp"

namespace flitzfs::detail {

inline constexpr std::size_t kIndexedVolumeIdentityOffset=72;
struct IndexedVolumeRoot {
  StoreIdentity identity{};
  std::uint64_t sequence=0,next_inode=2,next_extent=1;
  std::uint64_t inodes=0,extents=0,charged_extents=0;
  NativeWorkloadInfo workload;
  std::uint32_t format=1;
  std::uint64_t active_lifetimes=0,detached_inodes=0,retiring_inodes=0;
  std::uint64_t shared_bases=0,shared_members=0;
  bool operator==(const IndexedVolumeRoot&) const = default;
};
enum class IndexedLifetime : std::uint8_t { linked, pending, detached, retiring };
constexpr bool indexed_detached(IndexedLifetime state) noexcept {
  return state==IndexedLifetime::detached || state==IndexedLifetime::retiring;
}
struct IndexedInodeInput {
  std::uint64_t inode=0,parent=0,size=0,born=0,modified=0;
  bool directory=false;
  std::string name;
  std::optional<NativeBasicMetadata> basic;
  std::optional<NativeSecurityDescriptor> security;
  std::optional<std::uint64_t> allocation;
  IndexedLifetime lifetime=IndexedLifetime::linked;
};
// The indexed format uses the existing canonical inode frame with zero
// inline extents. Extent locations live in the ordered index.
std::vector<std::byte> encode_indexed_inode(const IndexedInodeInput&,const IndexedVolumeRoot&);
IndexedInodeInput decode_indexed_inode(std::span<const std::byte>,std::uint64_t,
    const IndexedVolumeRoot&);
bool indexed_volume_root(std::span<const std::byte>) noexcept;
std::vector<std::byte> rebind_indexed_volume_root(IndexedVolumeRoot,StoreIdentity);
NativeInodeAllocationInfo indexed_inode_allocation(const IndexedInodeInput&) noexcept;
std::optional<NativeBasicMetadata> updated_automatic_basic(const std::optional<NativeBasicMetadata>&,
    bool directory,const NativeAutomaticMetadata&);
IndexedVolumeRoot initialize_indexed_volume(PagedObjectStoreWriter&);
enum class NativeInodeAction {path_mutations,create,overwrite,mutate,set_delete,clear_delete,unlink,
    retire,finish_session,cleanup,cleanup_initial,reserve,enable_workload,coalesce_extents};
struct IndexedVolumeEdit {
  NativeInodeAction action;
  std::uint64_t inode;
  std::span<const NativeFileMutation> paths;
  std::span<const NativeBasicMutation> basic;
  std::span<const NativeSecurityMutation> security;
  const NativeAutomaticMetadata* automatic;
  const NativeOverwriteMetadata* overwrite;
  const std::uint64_t* reservation;
  bool deleting,trim_allocation;
};
// One selected group on the serialized native writer. Coordinates are the
// foreground mutation guard. Decoded object generations are checked again
// against the live index before the candidate can publish.
class IndexedSharedBasePreparation final {
 public:
  IndexedSharedBasePreparation(const PagedObjectStoreSnapshot&,IndexedVolumeRoot,
      NativeExtentSelection,std::span<const NativeExtentSelection>,std::span<const NativeContentToken>);
  bool watches_extent(std::uint64_t inode,std::uint64_t block) const noexcept;
  bool watches_inode(std::uint64_t inode) const noexcept;
  void invalidate() noexcept{invalidated_=true;}
  bool invalidated() const noexcept{return invalidated_;}
  NativeSharedBaseProgress progress() const noexcept;
  bool stage_search(std::size_t maximum_members,std::weak_ptr<const void>& owner_window);
  NativeSharedBaseSearchWindow search_window() const noexcept;
  bool advance(std::span<const NativeSharedBaseCopyRange>,const NativeSharedBaseSearchReply*);
  NativeSharedBaseCommit finish(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,IndexedVolumeRoot&);

 private:
  struct Member {
    NativeExtentSelection selected;
    std::uint64_t object=0,generation=0,previous_base=0;
    std::vector<std::byte> frame;
  };
  struct SearchWindow {
    std::shared_ptr<NativeSharedBaseSearchWindow::Payload> bytes;
    std::size_t first,member_count;
  };
  std::vector<std::byte> read_member(Member&,bool find_reusable_anchor);
  std::size_t search_slot(std::size_t position) const noexcept;
  const PagedObjectStoreSnapshot& source_;
  IndexedVolumeRoot original_;
  std::optional<NativeNameIndex> index_;
  std::vector<Member> members_;
  std::vector<std::byte> base_;
  std::optional<NativeGroupAnchorIndex> anchor_index_;
  std::optional<SearchWindow> search_;
  std::size_t base_index_=0;
  std::uint64_t reusable_anchor_=0,reusable_generation_=0;
  NativeSharedBaseProgress progress_{NativeSharedBasePhase::reading_base};
  bool invalidated_=false,uses_anchor_copies_=false,search_storage_unavailable_=false;
};
struct IndexedPreparationWatch {
  std::weak_ptr<IndexedSharedBasePreparation> preparation;
  bool changed=false;
};
struct IndexedContentWatch {
  std::uint64_t& sequence;
  std::span<IndexedPreparationWatch> preparations;
};
NativeFsCommit commit_indexed_volume(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,const IndexedVolumeEdit&,NativeInodeCommitInfo*,NativeInodeAllocationInfo*,
    IndexedContentWatch);
void advance_indexed_retirement(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,bool closed_session,IndexedContentWatch);
NativeInodeStorageMutationCommit write_indexed_inode(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,std::uint64_t inode,std::uint64_t offset,std::span<const std::byte>,
    NativeInodeWriteOptions,const NativeAutomaticMetadata&,std::span<const NativeLz4ExtentCandidate>,
    IndexedContentWatch);
NativeInodeStorageMutationCommit truncate_indexed_inode(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,std::uint64_t inode,std::uint64_t bytes,const NativeAutomaticMetadata&,
    IndexedContentWatch);
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
  NativeInodeState state(std::string_view path) const;
  NativeInodeState state_inode(std::uint64_t id) const;
  NativeDirectoryRecordPage directory_records(std::uint64_t directory,std::string_view after,
      std::size_t maximum) const;
  IndexedDirectoryPage list(std::uint64_t directory,std::string_view after,std::size_t maximum) const;
  std::vector<NativeFileInfo> list_all(std::uint64_t directory) const;
  std::vector<std::byte> read(const IndexedInodeInput&,std::uint64_t offset,std::uint32_t bytes) const;
  std::optional<NativeContentToken> next_content(NativeExtentSelection after,bool first) const;
  std::optional<NativeContentToken> content_at(NativeExtentSelection) const;
  std::vector<std::byte> read_content(NativeContentToken&) const;
  void verify_all() const;
  void verify_equivalent(const IndexedVolumeView& target) const;

 private:
  friend class IndexedVolumeBuilder;
  IndexedVolumeView(const PagedObjectStoreSnapshot&,IndexedVolumeRoot root);
  IndexedInodeInput referenced_inode(std::uint64_t id,bool allow_retiring=false) const;
  NativeContentToken content_token(NativeExtentSelection,std::uint64_t object) const;
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
      std::uint64_t next_inode,NativeWorkloadInfo,std::uint32_t format=1);
  IndexedVolumeBuilder(const IndexedVolumeBuilder&)=delete;
  IndexedVolumeBuilder& operator=(const IndexedVolumeBuilder&)=delete;
  void append(std::span<const IndexedInodeInput> inodes,std::span<const IndexedExtentInput> extents={});
  NativeFilesystemSnapshot finish();

 private:
  std::optional<PagedObjectStoreWriter> writer_;
  IndexedVolumeRoot root_;
};
} // namespace flitzfs::detail
