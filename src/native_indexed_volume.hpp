#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "native_group_codec.hpp"
#include "native_name_index.hpp"

namespace flitzfs::detail {
class NativeGroupReadBatch;
struct NativeLz4CapturePrefix;

inline constexpr std::size_t kIndexedVolumeIdentityOffset=72;
inline constexpr std::size_t kIndexedVolumeRootMaximumBytes=136;
inline constexpr std::size_t kIndexedExtentHeaderBytes=24;
inline constexpr std::size_t kIndexedInlineFileBytes=512;
inline constexpr std::uint64_t kIndexedInodePageObject=6ULL<<61;
struct IndexedVolumeRoot {
  StoreIdentity identity{};
  std::uint64_t sequence=0,next_inode=2,next_extent=1;
  std::uint64_t inodes=0,extents=0,charged_extents=0;
  NativeWorkloadInfo workload;
  std::uint32_t format=1;
  std::uint32_t name_profile=0;
  bool directory_shapes=false;
  bool directory_backfill_complete=false;
  bool inline_files=false;
  bool packed_inodes=false;
  std::uint64_t active_lifetimes=0,detached_inodes=0,retiring_inodes=0;
  std::uint64_t shared_bases=0,shared_members=0;
  bool operator==(const IndexedVolumeRoot&) const = default;
};
enum class IndexedLifetime : std::uint8_t { linked, pending, detached, retiring };
constexpr bool indexed_detached(IndexedLifetime state) noexcept {
  return state==IndexedLifetime::detached || state==IndexedLifetime::retiring;
}
struct IndexedPathBound {
  std::uint16_t units=0,components=0;
  bool operator==(const IndexedPathBound&) const = default;
};
inline constexpr std::size_t kIndexedShapeHeaderBytes=4,kIndexedShapeBucketBytes=10;
struct IndexedDirectoryShape {
  struct Bucket {
    std::uint16_t bound;
    std::uint64_t children;
    bool operator==(const Bucket&) const = default;
  };
  std::vector<Bucket> units,components;
  IndexedPathBound bound() const noexcept;
  void replace(std::optional<IndexedPathBound> before,std::optional<IndexedPathBound> after);
  bool operator==(const IndexedDirectoryShape&) const = default;
};
struct IndexedDirectoryBackfill {
  struct Frame {
    std::uint64_t inode,revision=0;
    std::string after;
    IndexedDirectoryShape shape;
    bool ready=false;
  };
  std::vector<Frame> stack;
  std::uint64_t capacity_sequence=0;
};
struct IndexedInodeInput {
  std::uint64_t inode=0,parent=0,size=0,born=0,modified=0;
  bool directory=false;
  std::string name;
  std::optional<NativeBasicMetadata> basic;
  std::optional<NativeSecurityDescriptor> security;
  std::optional<std::uint64_t> allocation;
  IndexedLifetime lifetime=IndexedLifetime::linked;
  std::optional<IndexedDirectoryShape> shape;
  std::vector<std::byte> inline_data;
};
// Indexed inodes use the canonical inode frame without legacy extent
// entries. A capable root permits a short data suffix on regular inodes.
std::vector<std::byte> encode_indexed_inode(const IndexedInodeInput&,const IndexedVolumeRoot&);
IndexedInodeInput decode_indexed_inode(std::span<const std::byte>,std::uint64_t,
    const IndexedVolumeRoot&);
bool indexed_volume_root(std::span<const std::byte>) noexcept;
std::vector<std::byte> rebind_indexed_volume_root(IndexedVolumeRoot,StoreIdentity);
NativeInodeAllocationInfo indexed_inode_allocation(const IndexedInodeInput&) noexcept;
std::optional<NativeBasicMetadata> updated_automatic_basic(const std::optional<NativeBasicMetadata>&,
    bool directory,const NativeAutomaticMetadata&);
IndexedVolumeRoot initialize_indexed_volume(PagedObjectStoreWriter&,NativeIndexedDataLayout);
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
      NativeExtentSelection,std::span<const NativeExtentSelection>,std::span<const NativeContentToken>,
      bool allow_subset,bool packed_only,bool compare_packed_pair=false);
  bool watches_extent(std::uint64_t inode,std::uint64_t block) const noexcept;
  bool watches_inode(std::uint64_t inode) const noexcept;
  void invalidate() noexcept{invalidated_=true;}
  bool invalidated() const noexcept{return invalidated_;}
  NativeSharedBaseProgress progress() const noexcept;
  bool stage_search(std::size_t maximum_members,std::weak_ptr<const void>& owner_window,NativeSharedBaseSearchPurpose);
  NativeSharedBaseSearchWindow search_window() const noexcept;
  bool price_reference();
  void admit_variants(bool packing,bool staged_reference,bool automatic,bool assembled_prefix) noexcept;
  bool advance(std::span<const NativeSharedBaseCopyRange>,const NativeSharedBaseSearchReply*);
  NativeSharedBaseCommit finish(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,IndexedVolumeRoot&);

 private:
  struct Member {
    NativeExtentSelection selected;
    std::uint64_t object=0,generation=0,previous_base=0;
    std::vector<std::byte> frame;
    std::size_t payload_bytes=0;
    bool skipped=false,reuse_bound_base=false,advised=false;
  };
  struct SearchWindow {
    std::shared_ptr<NativeSharedBaseSearchWindow::Payload> bytes;
    std::size_t first,member_count;
    NativeSharedBaseSearchPurpose purpose;
  };
  struct ReferencePricing {
    std::uint32_t candidate=0,peer=0,selected=0;
    std::uint64_t total=0,best=0;
  };
  struct SectionReference {
    NativeGroupSections sections;
    std::vector<std::byte> base;
    std::optional<NativeGroupAnchorIndex> index;
    std::vector<std::vector<std::byte>> frames;
    std::size_t sampled=0,frame_bytes=0;
  };
  struct PrefixedReference {
    std::vector<std::vector<std::byte>> frames;
    std::size_t frame_bytes=0;
  };
  struct PackedReference {
    std::vector<std::byte> base;
    std::vector<std::vector<std::byte>> frames;
    std::size_t sampled=0,slice_offset=0,frame_bytes=0;
  };
  std::vector<std::byte> read_member(Member&,bool find_reusable_anchor);
  std::size_t search_slot(std::size_t position) const noexcept;
  bool can_change_reference() const noexcept;
  void change_reference(std::uint32_t member);
  bool stage_section_reference();
  bool stage_prefixed_reference(std::optional<PrefixedReference>&,const SectionReference* sections=nullptr);
  bool stage_packed_reference();
  bool advance_packed_group();
  bool complete_member();
  const PagedObjectStoreSnapshot& source_;
  IndexedVolumeRoot original_;
  std::optional<NativeNameIndex> index_;
  std::vector<Member> members_;
  std::vector<std::byte> base_,nominated_base_;
  std::optional<NativeGroupAnchorIndex> anchor_index_;
  std::optional<SearchWindow> search_;
  std::optional<SectionReference> section_reference_;
  std::optional<PrefixedReference> prefixed_reference_,prefixed_sections_;
  std::optional<PackedReference> packed_reference_;
  ReferencePricing pricing_;
  std::size_t base_index_=0;
  std::uint64_t reusable_anchor_=0,reusable_generation_=0;
  NativeSharedBaseProgress progress_{NativeSharedBasePhase::reading_base};
  std::uint32_t group_capability_=3;
  std::uint32_t reference_member_=0;
  bool allow_subset_=false,invalidated_=false,search_storage_unavailable_=false;
  bool packing_admitted_=false,staged_reference_admitted_=false,staged_reference_comparison_=false;
  bool compare_sections_=false,compare_prefix_=false,compare_section_prefix_=false;
  bool automatic_compression_=false,assembled_prefix_admitted_=false;
  bool packed_only_=false,compare_packed_pair_=false;
};
struct IndexedPreparationWatch {
  std::weak_ptr<IndexedSharedBasePreparation> preparation;
  bool changed=false;
};
// Advisory history owned by the serialized writer. The transaction geometry
// bounds this memory. Losing coverage only requires an ordinary scan.
class IndexedContentChanges final {
 public:
  void record(NativeExtentSelection,std::uint64_t sequence) noexcept;
  void require_scan(std::uint64_t sequence) noexcept;
  std::optional<std::size_t> after(std::uint64_t sequence,std::span<NativeExtentSelection>) const noexcept;

 private:
  struct Change {
    NativeExtentSelection extent;
    std::uint64_t sequence=0;
  };
  std::array<Change,kNativeMaxDirtyExtents> entries_{};
  std::size_t used_=0;
  std::uint64_t lost_through_=0;
};
struct IndexedContentWatch {
  std::uint64_t& sequence;
  std::span<IndexedPreparationWatch> preparations;
  IndexedContentChanges& changes;
};
NativeFsCommit commit_indexed_volume(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,const IndexedVolumeEdit&,NativeInodeCommitInfo*,NativeInodeAllocationInfo*,
    IndexedContentWatch);
void advance_indexed_retirement(PagedObjectStoreWriter&,const PagedObjectStoreSnapshot&,
    IndexedVolumeRoot&,bool closed_session,IndexedContentWatch);
PagedInodeProjection project_indexed_inode_pages(
    PagedObjectStoreSnapshot source,const IndexedVolumeRoot&,StoreIdentity);
NativePackedInodeMigrationStep advance_indexed_packed_inode_migration(PagedObjectStoreWriter&,
    const PagedObjectStoreSnapshot&,IndexedVolumeRoot&,std::uint64_t after,
    std::uint64_t maximum_write_bytes);
NativeDirectoryBackfillStep advance_indexed_directory_backfill(PagedObjectStoreWriter&,
    const PagedObjectStoreSnapshot&,IndexedVolumeRoot&,IndexedDirectoryBackfill&,IndexedContentWatch);
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
struct IndexedSharedBaseRead {
  std::uint64_t object=0,generation=0;
  std::optional<NativeGroupBase> base;
};

// A serialized writer can retain verified bases and a partially read tiled
// member at its current checkpoint. Inventory audits check stored copies
// independently and use a request-local cache for base decoding.
class IndexedSharedBaseCache final {
 public:
  bool bind(const PagedObjectStoreSnapshot&) noexcept;
  const IndexedSharedBaseRead* find(std::uint64_t object,std::uint64_t generation) noexcept;
  void remember(IndexedSharedBaseRead&) noexcept;
  const std::vector<std::byte>* find_tiled(std::uint64_t inode,std::uint64_t block,
      std::uint64_t object) const noexcept;
  void remember_tiled(std::uint64_t inode,std::uint64_t block,std::uint64_t object,
      std::vector<std::byte>&&) noexcept;
  void clear() noexcept;
 private:
  struct TiledMember {
    std::uint64_t inode=0,block=0,object=0;
    std::vector<std::byte> bytes;
  };
  TiledMember member_;
  StoreIdentity image_{};
  std::uint64_t checkpoint_=0;
  std::vector<IndexedSharedBaseRead> frames_;
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
  NativeInodeAllocationInfo allocation_inode(std::uint64_t id,bool verify_ancestry) const;
  std::optional<NativeDirectoryRecordPage> directory_records(std::uint64_t directory,std::string_view after,
      std::size_t maximum,const std::function<bool(const NativeInodeState&)>& admit) const;
  IndexedDirectoryPage list(std::uint64_t directory,std::string_view after,std::size_t maximum) const;
  std::vector<NativeFileInfo> list_all(std::uint64_t directory) const;
  std::vector<std::byte> read(const IndexedInodeInput&,std::uint64_t offset,std::uint32_t bytes,
      IndexedSharedBaseCache* retained=nullptr,NativeLz4ReadRoute* route=nullptr,
      NativeGroupReadRoute* groups=nullptr) const;
  bool capture_native_lz4(const IndexedInodeInput&,std::uint64_t offset,std::size_t maximum_encoded,
      NativeLz4CapturedFrame&) const;
  std::vector<NativeLz4CapturedFrame> capture_native_lz4_range(const IndexedInodeInput&,
      std::uint64_t offset,std::uint32_t bytes,std::size_t maximum_encoded,
      std::size_t remaining_frame_bytes,const std::function<bool()>& interrupted,
      NativeLz4CapturePrefix* prefix) const;
  std::unique_ptr<NativeGroupReadBatch> capture_group_range(const IndexedInodeInput&,
      std::uint64_t offset,std::uint32_t bytes,std::size_t remaining_frame_bytes,
      const std::function<bool()>& interrupted) const;
  std::optional<NativeContentToken> content_at(NativeExtentSelection) const;
  std::optional<IndexedContentSample> sample_content(NativeExtentSelection,IndexedContentLookup,
      const std::function<bool(const NativeContentToken&)>& read_payload) const;
  void verify_all(const IndexedReopenAdmission& admission={}) const;
  void verify_equivalent(const IndexedVolumeView& target) const;

 private:
  friend class IndexedVolumeBuilder;
  IndexedVolumeView(const PagedObjectStoreSnapshot&,IndexedVolumeRoot root);
  enum class InodeReference {live,directory,allow_retiring};
  IndexedInodeInput referenced_inode(std::uint64_t id,InodeReference=InodeReference::live) const;
  struct InodePageCache;
  std::uint64_t verify_namespace(const IndexedInodeInput&,std::size_t path_units,unsigned components,
      PagedObjectStoreSnapshot::AuditMetadataReads&,InodePageCache&,
      const std::function<void(const IndexedInodeInput&,bool)>& visit) const;
  struct CheckedContent {
    NativeContentToken token;
    IndexedInodeInput inode;
  };
  std::optional<CheckedContent> locate_content(NativeExtentSelection,IndexedContentLookup) const;
  std::vector<std::byte> materialize_content(NativeContentToken&,const IndexedInodeInput&,
      bool published_reference) const;
  void verify_inventory(bool published,const IndexedReopenAdmission& admission={}) const;
  std::string path(const IndexedInodeInput&,bool include_spelling=true) const;
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
