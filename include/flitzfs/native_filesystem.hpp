#pragma once
#include "flitzfs/object_store.hpp"
#include "flitzfs/paged_store.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {
class PagedObjectStoreSnapshot;
class PagedObjectStoreWriter;
enum class NativeImageKind:std::uint32_t {flat=0,paged=1,indexed_paged=2,sealed_indexed=3};
struct NativeImageStorage {
  NativeImageKind kind=NativeImageKind::flat;
  std::uint64_t maximum_physical_bytes=0; // Required only for paged images.
  bool operator==(const NativeImageStorage&) const=default;
};
namespace detail {class NativeObjectSnapshot;class NativeObjectWriter;class IndexedSharedBasePreparation;class NativeLz4CaptureLease;class NativeWriteCaptureLease;struct NativeGroupReadRoute;enum class NativeInodeAction;}
// Native file/extent layer, experimental and CPU-only. Explicit Windows
// read-only projection is available; mounted writes and live GPU remain absent.
inline constexpr std::uint32_t kNativeExtentBytes=65536;
inline constexpr std::uint64_t kNativeMaxFileBytes=1ULL<<40; // Sparse logical size.
inline constexpr std::uint32_t kNativeMaxReadBytes=4U*1024*1024;
inline constexpr std::size_t kNativeMaxNodes=256; // Including persistent root inode 1.
inline constexpr std::size_t kNativeMaxExtents=kStoreMaxObjects-1;
// Reserve one full metadata object inside the 4 MiB object-transaction budget.
inline constexpr std::size_t kNativeMaxDirtyExtents=(kStoreMaxTransactionBytes-kNativeExtentBytes)/kNativeExtentBytes;
enum class NativeFsErrorCode {
  invalid_path,not_found,already_exists,not_directory,is_directory,not_empty,
  invalid_operation,limit,corrupt,storage_failure,replica_repair_pending
};
enum class NativeDirectoryBackfillStep {idle,scanning,published,capacity};
class NativeFsError final:public std::runtime_error {
 public:
  enum class MissingPath {name,parent};
  NativeFsError(NativeFsErrorCode code,const char* message,std::uint64_t append_shortfall=0)
      :std::runtime_error(message),code_(code),append_shortfall_(append_shortfall){}
  NativeFsError(MissingPath missing,const char* message)
      :std::runtime_error(message),code_(NativeFsErrorCode::not_found),missing_parent_(missing==MissingPath::parent),
       append_shortfall_(0){}
  NativeFsErrorCode code() const noexcept{return code_;}
  bool missing_parent() const noexcept{return missing_parent_;}
  std::uint64_t append_shortfall_bytes() const noexcept{return append_shortfall_;}
 private:
  NativeFsErrorCode code_;
  bool missing_parent_=false;
  std::uint64_t append_shortfall_;
};
// Borrowed complete LZ4 block for one final native extent. The call must remain
// serialized and both this span and its bytes immutable until return.
struct NativeLz4ExtentCandidate {
  std::uint64_t extent_index=0;
  std::span<const std::byte> payload;
};
// Pure bounded descriptor admission, before backing I/O; throws limit on an
// excessive count, per-payload size or aggregate size. Never reads payload data.
void validate_native_lz4_extent_candidates(std::span<const NativeLz4ExtentCandidate>);

struct NativeFileInfo {
  std::uint64_t inode=0,parent=0,logical_bytes=0,created_sequence=0,modified_sequence=0;
  bool directory=false;
  std::string path;
};

// Canonical persisted Windows basic metadata. Times are explicit nonnegative
// 100-ns UTC values, never Win32 request sentinels or transaction counters.
// Attributes admit READONLY/HIDDEN/SYSTEM/DIRECTORY/ARCHIVE/TEMPORARY/
// NOT_CONTENT_INDEXED only. NORMAL is normalized to zero at the OS boundary.
inline constexpr std::uint32_t kNativeBasicAttributeMask=0x2137;
struct NativeBasicMetadata {
  std::uint64_t creation_time=0,last_access_time=0,last_write_time=0,change_time=0;
  std::uint32_t attributes=0;
  bool operator==(const NativeBasicMetadata&) const = default;
};
// Opaque self-relative Windows descriptor bytes. The CPU core checks bounded
// framing only; Windows admission/access checks remain the adapter's job.
inline constexpr std::size_t kNativeMaxSecurityBytes=16384;
using NativeSecurityDescriptor=std::vector<std::byte>;
bool native_security_descriptor_framed(std::span<const std::byte>) noexcept;
struct NativeSecurityMutation {
  std::uint64_t inode=0;
  std::uint32_t created_ordinal=0; // Exactly one selector, as for basic metadata.
  NativeSecurityDescriptor value;
};
struct NativeBasicMutation {
  std::uint64_t inode=0;
  // Exactly one selector: existing inode, or 1-based creation in THIS batch.
  std::uint32_t created_ordinal=0;
  NativeBasicMetadata value;
};


// Trusted dispatcher Cleanup side effects, not an explicit user SetBasicInfo.
// One UTC value is sampled for requested times; no hidden clock/timer exists
// in the persistence core. Archive is automatic only for regular files.
struct NativeCleanupMetadata {
  std::uint64_t utc_time=0;
  bool set_archive=false,set_access_time=false,set_write_time=false,set_change_time=false;
  bool any() const noexcept {
    return set_archive || set_access_time || set_write_time || set_change_time;
  }
  bool same_requests(const NativeCleanupMetadata& other) const noexcept {
    return set_archive==other.set_archive && set_access_time==other.set_access_time &&
        set_write_time==other.set_write_time && set_change_time==other.set_change_time;
  }
};
bool native_cleanup_metadata_valid(const NativeCleanupMetadata&) noexcept;
// The same bounded trusted effects apply to data mutations. Retain the
// historical Cleanup type/layout/symbols; this alias adds no encoding or ABI.
using NativeAutomaticMetadata=NativeCleanupMetadata;
// Separate trusted event so the historical metadata layout and overloads stay
// intact. Trim releases a tracked native reservation to aligned EOF only.
struct NativeCleanupEvent {
  NativeCleanupMetadata metadata;
  bool trim_allocation=false;
  // Minimum change for an automatic access-only timestamp update, in UTC
  // 100-nanosecond ticks. Zero requests the original exact-time behavior.
  std::uint64_t minimum_access_interval=0;
};

// Trusted overwrite event policy: explicit canonical request attributes and
// one sampled nonnegative UTC value. Allocation/EA/stream policy is separate.
struct NativeOverwriteMetadata {
  std::uint64_t utc_time=0;
  std::uint32_t attributes=0;
  bool replace_attributes=false;
};
bool native_overwrite_metadata_valid(const NativeOverwriteMetadata&) noexcept;

enum class NativeFileOperation {create_file,create_directory,write,truncate,rename,remove};
struct NativeFileMutation {
  NativeFileOperation operation=NativeFileOperation::create_file;
  std::string path,target;
  std::uint64_t offset=0,size=0;
  std::vector<std::byte> data;
  bool replace=false;
};
struct NativeInodeState {
  NativeFileInfo info;
  bool delete_pending=false;
  bool unlinked=false; // No name/parent; payloads remain live until explicit retirement.
};
struct NativeInodeWriteOptions {
  bool append=false;       // Choose the latest EOF inside the single-control-thread call.
  bool constrained=false;  // Never extend logical EOF; a range beyond EOF writes zero bytes.
};
// Trusted monotonic workload provenance, not mutable learned policy or credit.
// Disabled legacy images have no historical count. Saturation stops further
// accrual; metadata work, truncate, recovery and image rewrites grant no input.
struct NativeWorkloadInfo {
  bool enabled=false;
  std::uint64_t committed_input_bytes=0;
  bool operator==(const NativeWorkloadInfo&) const = default;
};
struct NativeSharedBaseInfo {bool enabled=false;std::size_t members=0,bases=0;};
struct NativeExtentSelection {std::uint64_t inode=0,extent_index=0;};
struct NativeSharedBaseCopyRange {
  std::uint32_t member_offset=0,base_offset=0,length=0;
};
// A verified window owns only CPU-decoded bytes. It can outlive the source
// writer, but its identity is useful for advice only while that work is active.
enum class NativeSharedBaseSearchPurpose {advice,section_comparison};
class NativeSharedBaseSearchWindow final {
 public:
  NativeSharedBaseSearchWindow() noexcept=default;
  explicit operator bool() const noexcept{return bool(payload_);}
  bool operator==(const NativeSharedBaseSearchWindow&) const noexcept=default;
  bool permits_reference_change() const noexcept {return payload_ && payload_->whole_new_group;}
  std::span<const std::byte> base() const & noexcept {
    return payload_?std::span<const std::byte>(payload_->base):std::span<const std::byte>{};
  }
  std::span<const std::vector<std::byte>> members() const & noexcept {
    return payload_?std::span<const std::vector<std::byte>>(payload_->members):std::span<const std::vector<std::byte>>{};
  }
  std::span<const std::byte> base() const && = delete;
  std::span<const std::vector<std::byte>> members() const && = delete;
 private:
  friend class detail::IndexedSharedBasePreparation;
  struct Payload {
    std::vector<std::byte> base;
    std::vector<std::vector<std::byte>> members;
    bool whole_new_group=false;
  };
  std::shared_ptr<const Payload> payload_;
};
enum class NativePackedGroupCheck : std::uint8_t {pending,verified,invalid};
using NativePackedXorCheck=NativePackedGroupCheck;
enum class NativePackedGroupMethod : std::uint8_t {xor_planes,modular_delta,sparse_xor,collective};
struct NativeSharedBaseSearchReply {
  NativeSharedBaseSearchWindow window;
  // The caller retains these spans for the synchronous preparation step.
  std::span<const std::vector<NativeSharedBaseCopyRange>> copies{};
  std::span<const std::vector<std::byte>> packed_bodies{};
  // Optional CPU verdicts, indexed by the exact staged member.
  std::span<NativePackedGroupCheck> packed_checks{};
  NativePackedGroupMethod body_method=NativePackedGroupMethod::xor_planes;
  // Zero retains the nominated base. Other values are one-based staged members.
  std::uint32_t reference_member=0;
};
struct NativeContentToken {
  NativeExtentSelection extent;
  // An unlinked or pending inode advances the indexed cursor with object zero.
  std::uint64_t object=0,revision=0,content_revision=0;
  bool grouped=false,exact_base=false; // A sampled extent can nominate its verified shared base.
  bool packed_slice=false;
  std::uint32_t standalone_bytes=0;
  std::uint32_t stored_payload_bytes=0; // Checked active record size for candidate pricing.
};
struct NativeFsCommit {
  StoreCommitInfo storage;
  std::uint64_t sequence=0; // Namespace transaction sequence, distinct from root repair generation.
  std::vector<std::uint64_t> created_inodes;
};
struct NativeSharedBaseCommit {
  NativeFsCommit transaction;
  bool applied=false;
  std::uint64_t before_compacted_lower_bound=0,after_compacted_upper_bound=0;
  std::size_t members=0;
  NativeExtentSelection reference{}; // Linked extent that bears the published shared-base reference.
  bool section_reference=false;
  bool packed_reference=false;
  std::size_t prefixed_zstd_members=0,advised_members=0;
  std::vector<std::uint64_t> skipped_objects; // Sorted, unmodified automatic candidates.
};
enum class NativeSharedBasePhase {reading_base,encoding_members,ready,declined,completed,cancelled,failed};
struct NativeSharedBaseProgress {
  NativeSharedBasePhase phase=NativeSharedBasePhase::failed;
  std::size_t completed_members=0,total_members=0;
  std::uint64_t owned_payload_bytes=0;
  std::size_t selected_advice_members=0;
  bool reference_changed=false,section_comparison=false;
};

struct NativeInodeWriteCommit {
  NativeFsCommit transaction;
  std::uint64_t inode=0,offset=0; // Actual selected offset, including append EOF.
  std::uint32_t bytes=0;         // Durable transferred count; no result on failure.
};

// String-free facts copied from the validated private staged inode before
// publication, exposed only after durable success or a verified no-op.
// These are not allocation reservations or physical compressed-byte counts.
struct NativeInodeCommitInfo {
  std::uint64_t inode=0,logical_bytes=0,modified_sequence=0;
  std::optional<NativeBasicMetadata> basic;
  bool directory=false,delete_pending=false,unlinked=false;
};
// One newly created inode, with all facts captured before durable publication.
struct NativeInodeCreateCommit {
  NativeFsCommit transaction;
  NativeInodeCommitInfo info;
};
struct NativeInodeMutationCommit {
  NativeFsCommit transaction;
  NativeInodeCommitInfo info;
  std::uint64_t offset=0;
  std::uint32_t bytes=0; // Durable write count; truncate returns zero.
};

// Native live-object capacity reservation, NOT host clusters or compressed
// physical bytes. Absence retains the historical sparse/unreserved contract.
inline constexpr std::uint64_t kNativeAllocationUnit=4096;
inline constexpr std::uint64_t kNativeAllocationCapacity=kNativeMaxExtents*std::uint64_t(kNativeExtentBytes);
struct NativeInodeAllocationInfo {
  NativeInodeCommitInfo info;
  std::optional<std::uint64_t> reserved_bytes;
};
namespace detail {
// Startup can validate platform metadata from each checked, reachable inode.
// The decoded fields are borrowed only for the duration of the call.
using IndexedReopenAdmission=std::function<void(const NativeInodeAllocationInfo&,
    const std::optional<NativeSecurityDescriptor>&)>;
}
struct NativePathLookup {
  NativeInodeAllocationInfo state;
  std::optional<NativeSecurityDescriptor> security;
};
struct NativeInodeAllocationCommit {
  NativeFsCommit transaction;
  NativeInodeAllocationInfo state;
};
struct NativeInodeStorageMutationCommit {
  NativeFsCommit transaction;
  NativeInodeAllocationInfo state;
  std::uint64_t offset=0;
  std::uint32_t bytes=0;
};
struct NativeAllocationUsage {
  std::size_t charged_extents=0,live_extents=0;
};

class NativeDirectoryCursor final {
 public:
  NativeDirectoryCursor() noexcept=default;
 private:
  friend class NativeFilesystemSnapshot;
  std::weak_ptr<const void> owner_;
  std::uint64_t directory_=0;
  std::string after_;
  bool initialized_=false;
};
struct NativeDirectoryPage {
  std::vector<NativeFileInfo> entries;
  std::optional<NativeDirectoryCursor> next;
};
struct NativeDirectoryRecord {
  std::string name;
  NativeInodeAllocationInfo state;
};
struct NativeDirectoryRecordPage {
  std::vector<NativeDirectoryRecord> entries;
  bool more=false;
};
class NativeFilesystemSnapshot final {
 public:
  // Consumes a fully verified object snapshot; validates native namespace and all references.
  static NativeFilesystemSnapshot open(ObjectStoreSnapshot objects);
  // Legacy paged layouts verify their full bounded namespace on open.
  // Indexed layout open verifies the roots and root inode. Its reads verify
  // touched records, and verify_all streams the complete inventory.
  static NativeFilesystemSnapshot open(PagedObjectStoreSnapshot objects);
  // Explicit persisted/provider format descriptor; never guesses or retries a
  // different reader after corruption. Existing open overloads are unchanged.
  static NativeFilesystemSnapshot open_image(std::shared_ptr<StoreByteIo>,NativeImageStorage);
  // Build a sealed, read-only indexed image from a closed mirrored source.
  // Both workspace and output must be new. The transient workspace holds an
  // ordinary single-stream image and has no authority to publish a volume.
  NativeFilesystemSnapshot create_sealed_indexed_copy(std::shared_ptr<StoreByteIo> empty_workspace,
      std::shared_ptr<StoreByteIo> empty_output,StoreIdentity new_identity,
      std::uint64_t maximum_physical_bytes,StoreCompressionOptions={}) const;
  // Export a legacy snapshot with resolved lifetimes to a new, independently
  // checked indexed image. The source remains selected by any volume head.
  NativeFilesystemSnapshot create_indexed_copy(std::shared_ptr<StoreByteIo> empty_target,
      StoreIdentity,PagedStoreLimits,StoreCompressionOptions={}) const;
  NativeFilesystemSnapshot(NativeFilesystemSnapshot&&) noexcept;
  NativeFilesystemSnapshot& operator=(NativeFilesystemSnapshot&&) noexcept;
  NativeFilesystemSnapshot(const NativeFilesystemSnapshot&)=delete;
  NativeFilesystemSnapshot& operator=(const NativeFilesystemSnapshot&)=delete;
  ~NativeFilesystemSnapshot();
  StoreIdentity identity() const noexcept;
  NativeImageStorage image_storage() const noexcept;
  StoreCommitInfo storage_info() const noexcept;
  std::uint64_t sequence() const noexcept;
  NativeWorkloadInfo workload_info() const noexcept; // Validated cached scalar; moved-from is disabled.
  NativeSharedBaseInfo shared_base_info() const noexcept;
  NativeFileInfo stat(std::string_view path) const;
  // IDs belong to this volume/snapshot. Missing/deleted IDs return not_found;
  // a later file at the old path is never selected instead.
  NativeFileInfo stat_inode(std::uint64_t inode) const;
  NativeInodeState inode_state(std::uint64_t inode) const;
  // Absent for legacy/unstamped inodes; never invent original timestamps.
  std::optional<NativeBasicMetadata> basic_metadata(std::uint64_t inode) const;
  std::optional<NativeSecurityDescriptor> security_descriptor(std::uint64_t inode) const;
  NativeInodeAllocationInfo allocation_info(std::uint64_t inode) const;
  NativeAllocationUsage allocation_usage() const;
  bool needs_session_recovery() const noexcept;
  std::vector<NativeFileInfo> list(std::string_view directory="") const;
  std::vector<NativeFileInfo> list_inode(std::uint64_t directory) const;
  // Canonical-name keyset order, 1..1024 results. Continuation belongs to this
  // exact immutable snapshot/directory; move preserves it, reopening does not.
  // It does not pin a mutable writer or make a live directory scan atomic.
  NativeDirectoryPage list_inode_page(std::uint64_t directory,
      const NativeDirectoryCursor& after={},std::size_t maximum=128) const;
  // Root-relative '/' paths, conservative ASCII/case-insensitive components.
  // Short reads at EOF; holes are zeros; full extent integrity precedes exposure.
  std::vector<std::byte> read(std::string_view path,std::uint64_t offset,std::uint32_t bytes) const;
  std::vector<std::byte> read_inode(std::uint64_t inode,
      std::uint64_t offset,std::uint32_t bytes) const;
  void verify_all() const;
 private:
  friend class NativeFilesystemWriter;
  friend class NativeVolumeHeadView;
  friend class NativeVolumeHeadWriter;
  friend class NativeFileSession;
  friend class NativeHybridBacking;
  detail::FoldedCheckpoint build_folded_workspace(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions) const;
  void verify_all(const detail::IndexedReopenAdmission&) const;
  void recheck_verified_indexed_mirrors(const NativeFilesystemSnapshot& audited) const;
  std::shared_ptr<StoreByteIo> backing_for_volume_head() const;
  void verify_compacted_equivalent_for_volume_head(const NativeFilesystemSnapshot&,bool closed_indexed=false) const;
  bool has_degraded_indexed_payloads() const noexcept;
  bool matches_indexed_checkpoint(const NativeFilesystemSnapshot&) const noexcept;
  std::optional<NativeContentToken> indexed_extent_token(NativeExtentSelection) const;
  struct Impl;
  static NativeFilesystemSnapshot open_storage(detail::NativeObjectSnapshot);
  explicit NativeFilesystemSnapshot(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

class NativeFilesystemWriter final {
 public:
  class SharedBaseWork;
  // A NEW empty generation-zero object store only. Never reinterprets existing objects.
  static NativeFilesystemWriter initialize(ObjectStoreWriter objects);
  // Explicitly opt into a NEW empty paged store (initial publication generation
  // 2). Uses the same mutation/session engine and namespace bytes. Flat volume-
  // head/recompression facilities require their existing flat backend.
  static NativeFilesystemWriter initialize(PagedObjectStoreWriter objects);
  // Explicit inode-record namespace layout on a NEW empty paged backend.
  // Publishes a fixed root and individual inode records atomically; no format
  // conversion of existing images. Current in-memory namespace limits remain.
  // Workload accounting and format-7 shared references are enabled from start.
  static NativeFilesystemWriter initialize_inode_records(PagedObjectStoreWriter objects);
  // New empty layout with separately indexed canonical directory names.
  // Existing layouts are never converted in place. Current frontend limits remain.
  static NativeFilesystemWriter initialize_indexed_names(PagedObjectStoreWriter objects);
  // New mirrored volume with on-demand indexed metadata and path transactions.
  // Existing images keep their original layout. Handle lifetime and volume-head
  // adoption require the indexed Windows session implementation.
  static NativeFilesystemWriter initialize_indexed(PagedObjectStoreWriter objects);
  NativeImageKind image_kind() const;
  // Capability-3 namespace on a NEW empty mirrored paged store. Ordinary
  // writes stay independent; explicit shared-base work is separately selected.
  static NativeFilesystemWriter initialize_shared_bases(PagedObjectStoreWriter objects);
  // Explicit CPU primitive, not an idle scheduler/GPU route. Atomically install
  // position-aligned shared-base patches only when conservative COMPLETE
  // compacted-image bounds prove a reduction. Existing frontend limits remain.
  NativeSharedBaseCommit compress_shared_base(NativeExtentSelection base,
      std::span<const NativeExtentSelection> members);
  // Cooperative CPU preparation for both shared-base namespace layouts.
  // Prepare performs no backing I/O. Advance checks one extent and its
  // decode dependency at a time. Selected mutations invalidate the work.
  // Finish reconciles the live mappings and uses the synchronous transaction.
  SharedBaseWork prepare_shared_base(NativeExtentSelection base,
      std::span<const NativeExtentSelection> members);
  // Stage at most one member from the private indexed snapshot. The grant
  // counts non-base members. False leaves the ordinary CPU step available.
  bool stage_shared_base_search(SharedBaseWork&,std::size_t maximum_members);
  bool stage_shared_base_search(SharedBaseWork&,std::size_t maximum_members,NativeSharedBaseSearchPurpose);
  // Advisory copies describe the next indexed member. Invalid or larger hints
  // leave the independent CPU encoder eligible for that member.
  bool advance_shared_base(SharedBaseWork&,std::span<const NativeSharedBaseCopyRange> advice={},
      const NativeSharedBaseSearchReply* reply=nullptr);
  NativeSharedBaseCommit finish_shared_base(SharedBaseWork&);
  // Explicit sticky namespace capability; legacy initialization is unchanged.
  // Start at zero without crediting old files. Commit through the same
  // replicated metadata transaction. Repeated enable never resets the count.
  NativeFsCommit enable_workload_accounting();
  // Explicit bounded CPU maintenance, never invoked automatically by I/O.
  // Share exactly equal verified 64-KiB extents across files/offsets in one
  // replicated transaction. Format 7 is emitted only if new sharing occurs;
  // legacy formats remain readable and unchanged. Subsequent data mutations
  // detach privately. Old records remain until a new-image compaction, so a
  // live-payload saving is not an immediate total-image saving. No GPU work.
  NativeFsCommit coalesce_equal_extents();
  // Cached diagnostic only; disabled after move/fencing. No I/O/allocation.
  NativeWorkloadInfo workload_info() const noexcept;
  // Object writer must already hold its qualified backend lease/recovery fence.
  // Ordinary admission refuses pending/unlinked state. A live old file context
  // must never be silently treated as closed.
  static NativeFilesystemWriter resume(ObjectStoreWriter objects);
  static NativeFilesystemWriter resume(PagedObjectStoreWriter objects);
  // ONLY after the caller has independently established retirement of ALL old
  // file contexts and exclusive backend ownership. Finish persisted deletions
  // and detached inode retirement atomically before returning. This declaration
  // is not itself driver/context attestation; the future helper must prove it.
  static NativeFilesystemWriter resume_closed_session(ObjectStoreWriter objects);
  static NativeFilesystemWriter resume_closed_session(PagedObjectStoreWriter objects);
  // Copy a quiescent latest snapshot into an empty NEW volume identity.
  // Preserve inode IDs/sequences/contents, rebind namespace identity, and
  // retain the original source. No in-place reclaim, filename swap or deletion.
  // Paged sources preserve their layout and use the source's configured capacity
  // bound for the target. compact_metadata is a flat-only option and rejects.
  static NativeFilesystemWriter compact(const NativeFilesystemSnapshot& source,
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_identity,
      StoreCompressionOptions compression={});
  static NativeFilesystemWriter compact(const NativeFilesystemSnapshot& source,
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_identity,
      StoreCompressionOptions compression,StoreCheckpointOptions checkpoints);
  // Explicit post-compression primitive: preserve native state while decoding/
  // re-encoding payloads into a NEW image. No in-place writes or idle scheduler.
  static NativeFilesystemWriter recompress(const NativeFilesystemSnapshot& source,
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_identity,
      StoreCompressionOptions compression={},StoreCheckpointOptions checkpoints={});
  // Explicit staged candidate with bounded copy and verification. Exact
  // checkpoint mode invalidates on a foreground commit. Indexed compaction
  // can opt into reconciling committed edits before handoff.
  PagedObjectStoreWriter::Rewrite prepare_paged_recompression(std::shared_ptr<StoreByteIo>,
      StoreIdentity,StoreCompressionOptions={},bool reencode=true,PagedRewriteMode=PagedRewriteMode::exact_checkpoint) const;
  bool advance_paged_recompression(PagedObjectStoreWriter::Rewrite&) const;
  bool paged_recompression_ready(const PagedObjectStoreWriter::Rewrite&) const noexcept;
  NativeFilesystemWriter finish_paged_recompression(PagedObjectStoreWriter::Rewrite&) const;
  NativeFilesystemWriter(NativeFilesystemWriter&&) noexcept;
  NativeFilesystemWriter& operator=(NativeFilesystemWriter&&) noexcept;
  NativeFilesystemWriter(const NativeFilesystemWriter&)=delete;
  NativeFilesystemWriter& operator=(const NativeFilesystemWriter&)=delete;
  ~NativeFilesystemWriter();
  // 1..16 ordered operations, <=4 MiB input, <=1 MiB per write.
  // Commit metadata + changed extents atomically. Admission errors precede backing writes.
  // At most 63 dirty 64 KiB extents; input size/alignment may hit that cap first.
  // All-zero extents are omitted; exact unchanged bytes reuse immutable records.
  // Nonempty identical writes still publish metadata and the normal barriers.
  // Old snapshots keep their original records.
  NativeFsCommit apply(std::span<const NativeFileMutation> mutations);
  // <=16 combined ordered file operations + final full metadata replacements.
  // File operations run first; metadata selectors address stable IDs or this
  // batch's creations. One atomic publication, no unbounded clock/query.
  // Generic file operations preserve basic fields; the Windows adapter must
  // supply correctly authorized/stamped values in this SAME transaction.
  NativeFsCommit apply_with_basic(std::span<const NativeFileMutation>,
      std::span<const NativeBasicMutation>);
  // <=16 TOTAL file/basic/security operations in one publication. Descriptors
  // are full replacements, not SECURITY_INFORMATION patches or ACL decisions.
  NativeFsCommit apply_with_metadata(std::span<const NativeFileMutation>,
      std::span<const NativeBasicMutation>,std::span<const NativeSecurityMutation>);
  // Trusted Windows-engine creation: NEW name, canonical basic tuple and full
  // framed descriptor in ONE publication. Optional delete intent is persisted
  // in that same Stage, never by a second transaction. No ACL decision is made.
  NativeInodeCreateCommit create_inode_result(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      bool delete_pending=false);
  // Optional aligned regular-file reservation in the SAME creation commit.
  // Directories require absence. No host preallocation is implied.
  NativeInodeAllocationCommit create_inode_storage_result(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      std::optional<std::uint64_t> reservation={},bool delete_pending=false);
  NativeFsCommit set_security_descriptor(std::uint64_t inode,std::span<const std::byte>);
  std::optional<NativeSecurityDescriptor> inspect_security_descriptor(std::uint64_t inode) const;
  NativeFsCommit set_basic_metadata(std::uint64_t inode,const NativeBasicMetadata&);
  std::optional<NativeBasicMetadata> inspect_basic_metadata(std::uint64_t inode) const;
  // Resolve a stable LIVE inode against the last acknowledged namespace, then
  // use the same atomic staging/replicated publication as apply(). One control
  // thread; no path lookup by the caller and no intermediate callback.
  // Full supplied input <=1 MiB even if constrained; accepted bytes may be
  // shorter. Append selects current EOF, ignoring the supplied offset.
  // Zero accepted bytes publish nothing. A removed inode is never redirected.
  // Detached inodes stay writable until explicitly retired. These APIs do not
  // track Windows open contexts or enable a mounted writable session.
  NativeInodeWriteCommit write_inode(std::uint64_t inode,std::uint64_t offset,
      std::span<const std::byte> bytes,NativeInodeWriteOptions options={});
  NativeFsCommit truncate_inode(std::uint64_t inode,std::uint64_t bytes);
  // Trusted requested effects, not explicit user SetBasicInfo. One data/basic
  // transaction; preserve original creation/untouched values and require a
  // present tuple for meaningful effects. Invalid clocks reject before I/O.
  // Zero accepted write bytes never apply effects. Truncate may explicitly
  // request effects even at unchanged EOF. No clock or policy is invented.
  NativeInodeWriteCommit write_inode_with_metadata(std::uint64_t inode,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions,const NativeAutomaticMetadata&);
  NativeFsCommit truncate_inode_with_metadata(std::uint64_t inode,std::uint64_t bytes,
      const NativeAutomaticMetadata&);
  // Commit-bound output for the future Windows callback. No post-publication
  // allocation, snapshot, pathname lookup or backing query forms these facts.
  NativeInodeMutationCommit write_inode_result(std::uint64_t inode,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={},
      const NativeAutomaticMetadata& effect={});
  NativeInodeMutationCommit truncate_inode_result(std::uint64_t inode,std::uint64_t bytes,
      const NativeAutomaticMetadata& effect={});
  NativeInodeStorageMutationCommit write_inode_storage_result(std::uint64_t inode,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={},
      const NativeAutomaticMetadata& effect={});
  // Same committed count/metadata contract as the ordinary inode write. Hints
  // apply only to this inode's changed extents, never namespace or other files.
  // Invalid, stale, duplicate and non-saving payloads keep CPU encoding.
  NativeInodeStorageMutationCommit write_inode_storage_with_lz4(std::uint64_t inode,
      std::uint64_t offset,std::span<const std::byte>,
      std::span<const NativeLz4ExtentCandidate>,NativeInodeWriteOptions options={},
      const NativeAutomaticMetadata& effect={});
  NativeInodeStorageMutationCommit truncate_inode_storage_result(std::uint64_t inode,std::uint64_t bytes,
      const NativeAutomaticMetadata& effect={});
  NativeInodeAllocationCommit overwrite_inode_storage_result(std::uint64_t inode,
      const NativeOverwriteMetadata&,std::optional<std::uint64_t> reservation={});
  // Existing regular inode: truncate to zero + merge/replace request attrs,
  // archive and access/write/change time in ONE publication. Preserve original
  // creation time, descriptor, inode and name. Requires a present basic tuple;
  // no allocation reservation or complete Windows FileInfo is implied.
  NativeInodeMutationCommit overwrite_inode_result(std::uint64_t inode,
      const NativeOverwriteMetadata&);
  // Persistent deletion primitives for the future owning Windows engine.
  // Eligibility/cancel changes intent only; it never unlinks. Unlink requires
  // pending intent, removes the name, and retains every payload. Retirement
  // requires detached state AND the caller's proof of no remaining file context.
  // No public ordinary-file reopen, open-context proof, or mount is added.
  NativeFsCommit set_delete_pending(std::uint64_t inode,bool pending);
  NativeFsCommit unlink_inode(std::uint64_t inode);
  // Trusted Cleanup event: optional unlink and requested automatic metadata
  // share ONE replicated transaction. Creation/untouched fields stay exact.
  // Any effective metadata change requires an already present basic tuple.
  NativeFsCommit cleanup_inode(std::uint64_t inode,bool delete_file,
      const NativeCleanupMetadata&);
  // Trusted initial FILE_DELETE_ON_CLOSE becomes effective only at Cleanup.
  // Arm pending + requested metadata + optional unlink in one Stage. Nonempty
  // directories are retained without creating an unsafe durable delete intent.
  // Captured facts let the owner update runtime latches without post-commit I/O.
  NativeInodeMutationCommit cleanup_initial_delete_result(std::uint64_t inode,bool delete_file,
      const NativeCleanupMetadata&);
  // Trusted metadata + optional tracked-reservation trim + optional unlink in
  // ONE Stage. initial_delete selects the existing directory-preserving policy.
  // Trim rejects untracked regular files; directories need no reservation.
  NativeInodeAllocationCommit cleanup_inode_storage_result(std::uint64_t inode,bool delete_file,
      bool initial_delete,const NativeCleanupEvent&);
  // Explicit aligned native reservation. Shrinking below EOF truncates/zeros
  // tails in the SAME transaction. Growth preserves EOF/data. Tracked writes
  // and EOF growth extend reservations; ordinary EOF shrink retains them.
  // Every live inode (including detached) shares the unchanged extent budget.
  // This does NOT preallocate host file/disk space or guarantee future history.
  NativeInodeAllocationCommit reserve_inode_result(std::uint64_t inode,
      std::uint64_t aligned_bytes,const NativeAutomaticMetadata& effect={});
  NativeInodeAllocationInfo inspect_allocation(std::uint64_t inode) const;
  NativeAllocationUsage inspect_allocation_usage() const;
  NativeFsCommit retire_inode(std::uint64_t inode);
  // Advance one indexed reclamation transaction for an already retired inode.
  // Returns false when there is no queued work. Old snapshots remain readable.
  bool retirement_pending() const noexcept; // Cached, with no backing I/O.
  bool reclaim_retired_inode();
  bool directory_backfill_pending() const noexcept; // Cached, with no backing I/O.
  NativeDirectoryBackfillStep advance_directory_backfill();
  void cancel_directory_backfill() noexcept;
  // Single-control-thread committed view for the owning Windows engine.
  // Verify namespace against the last acknowledged model; reads privately
  // verify every touched record. These do not expose an unchecked snapshot.
  NativeInodeState inspect_path(std::string_view path) const;
  NativeInodeState inspect_inode(std::uint64_t inode) const;
  std::vector<NativeFileInfo> inspect_directory(std::uint64_t inode) const;
  std::vector<std::byte> read_inode(std::uint64_t inode,
      std::uint64_t offset,std::uint32_t bytes,detail::NativeLz4ReadRoute* route=nullptr,
      detail::NativeGroupReadRoute* groups=nullptr) const;
  bool capture_native_lz4(std::uint64_t inode,std::uint64_t offset,std::size_t maximum_encoded,
      detail::NativeLz4CapturedFrame&) const;
  // Real shared-image barrier; pre/post namespace verification, no publication.
  NativeFsCommit flush();
  NativeFilesystemSnapshot snapshot() const;
  bool poisoned() const noexcept;
 private:
  friend class NativeFileSession;
  friend class NativeGroupMaintenance;
  friend class NativeHybridBacking;
  friend class NativeVolumeHeadWriter;
  bool supports_leased_lz4_capture() const noexcept;
  std::unique_ptr<detail::NativeLz4CaptureLease> lease_native_lz4_capture(
      std::uint64_t inode,std::uint64_t offset,std::size_t maximum_encoded) const;
  std::unique_ptr<detail::NativeGroupCaptureLease> lease_native_group_capture(
      std::uint64_t inode,std::uint64_t offset) const;
  std::unique_ptr<detail::NativeWriteCaptureLease> lease_committed_write_capture() const;
  NativeInodeAllocationInfo inspect_allocation(std::uint64_t inode,bool verify_ancestry) const;
  PagedObjectStoreWriter::Rewrite prepare_folded_recompression(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,std::function<void(std::uint64_t)> admission) const;
  bool advance_folded_recompression(PagedObjectStoreWriter::Rewrite&,detail::FoldedCheckpoint&) const;
  void verify_folded_recompression_source(const PagedObjectStoreWriter::Rewrite&) const;
  PagedObjectStoreWriter::Rewrite prepare_paged_recompression_impl(std::shared_ptr<StoreByteIo>,
      StoreIdentity,StoreCompressionOptions,bool reencode,PagedRewriteMode,
      std::optional<std::uint64_t> cold_after_generation,
      std::function<bool(std::uint64_t)> reserve_writes={}) const;
  std::optional<NativeContentToken> maintenance_next(NativeExtentSelection,bool first) const;
  std::optional<NativeContentToken> maintenance_token(NativeExtentSelection);
  bool maintenance_matches(const NativeContentToken&) const noexcept;
  std::vector<std::byte> maintenance_read(NativeContentToken&,bool published_reference=false);
  std::uint64_t maintenance_content_sequence() const noexcept;
  std::optional<std::size_t> maintenance_changes_since(std::uint64_t,std::span<NativeExtentSelection>) const noexcept;
  bool maintenance_indexed() const noexcept;
  bool maintenance_has_candidates() const noexcept;
  std::uint64_t maintenance_sequence() const noexcept;
  StoreIdentity maintenance_identity() const noexcept;
  std::uint64_t maintenance_physical_end() const noexcept;
  bool maintenance_busy() const noexcept;
  bool maintenance_supported() const noexcept;
  enum class SharedBaseMode { compare,pack };
  SharedBaseWork prepare_shared_base(NativeExtentSelection,std::span<const NativeExtentSelection>,
      std::span<const NativeContentToken>,bool batch=false,bool allow_subset=false,SharedBaseMode=SharedBaseMode::compare);
  bool stage_shared_base_search(SharedBaseWork&,std::size_t maximum_members,std::size_t retained_extents,
      NativeSharedBaseSearchPurpose=NativeSharedBaseSearchPurpose::advice);
  bool price_shared_base_reference(SharedBaseWork&);
  void admit_shared_base_variants(SharedBaseWork&,bool packing,bool staged_reference,bool automatic) noexcept;

  using Recompression=ObjectStoreWriter::Rewrite;
  Recompression prepare_recompression(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,StoreCheckpointOptions) const;
  bool matches_recompression(const Recompression&) const noexcept;
  bool matches_paged_recompression(const PagedObjectStoreWriter::Rewrite&) const noexcept;
  std::uint64_t recompression_generation() const noexcept;
  static std::optional<StoreRecompressionInput> next_recompression_input(const Recompression&);
  static std::optional<StoreRecompressionFeedback> recompression_feedback(const Recompression&) noexcept;
  static bool advance_recompression(Recompression&,std::uint64_t,std::span<const StoreMatchProposal>);
  static void finish_recompression(Recompression&);
  struct Impl;
  void check_shared_base(SharedBaseWork&) const;
  bool shared_base_step(SharedBaseWork&,std::span<const NativeSharedBaseCopyRange>,
      const NativeSharedBaseSearchReply*,std::size_t search_members,std::size_t retained_extents=0,bool price_reference=false,
      NativeSharedBaseSearchPurpose=NativeSharedBaseSearchPurpose::advice);
  Impl& checked_impl() const;
  detail::NativeObjectSnapshot checked_namespace() const;
  NativePathLookup inspect_path_lookup(std::string_view path) const;
  std::optional<NativeDirectoryRecordPage> inspect_directory_page(std::uint64_t directory,
      std::string_view after,std::size_t maximum,
      const std::function<bool(const NativeInodeState&)>& admit) const;
  bool supports_file_contexts() const noexcept;
  std::uint64_t indexed_append_floor() const noexcept;
  std::uint64_t indexed_obsolete_bytes() const noexcept;
  bool indexed_payload_repair_pending() const noexcept;
  std::uint64_t indexed_capacity_repair_generation() const noexcept;
  PagedPayloadRepairProgress repair_indexed_payload();
  bool indexed_payload_scrub_eligible() const noexcept;
  PagedPayloadScrubResult scrub_indexed_payload(const PagedPayloadScrubBudget&);
  bool restore_indexed_payload_scrub(const PagedPayloadScrubPosition&) noexcept;
  using InodeAction=detail::NativeInodeAction;
  NativeFsCommit transact(std::span<const NativeFileMutation>,InodeAction,std::uint64_t,
      std::span<const NativeBasicMutation> basic={},
      const NativeAutomaticMetadata* automatic=nullptr,bool delete_requested=false,
      NativeInodeCommitInfo* published=nullptr,
      std::span<const NativeSecurityMutation> security={},
      const NativeOverwriteMetadata* overwrite=nullptr,
      const std::uint64_t* reservation=nullptr,NativeInodeAllocationInfo* allocation=nullptr,
      bool trim_allocation=false,std::span<const NativeLz4ExtentCandidate> lz4_candidates={});
  static NativeFilesystemWriter copy_image(const NativeFilesystemSnapshot&,
      std::shared_ptr<StoreByteIo>,StoreIdentity,StoreCompressionOptions,
      StoreCheckpointOptions,bool reencode);
  static PagedObjectStoreWriter copy_indexed_image(const NativeFilesystemSnapshot&,
      std::shared_ptr<StoreByteIo>,StoreIdentity,StoreCompressionOptions,bool reencode);
  static NativeFilesystemWriter initialize_storage(detail::NativeObjectWriter,bool inode_records=false,
      bool indexed_names=false,bool shared_bases=false);
  static NativeFilesystemWriter resume_impl(detail::NativeObjectWriter,bool closed_session);
  // The head owner supplies a verified closed-session view and checks both
  // leases and barriers before admitting the image writer to the session.
  static NativeFilesystemWriter lease_verified_indexed_reopen(const NativeFilesystemSnapshot&,StoreCompressionOptions);
  explicit NativeFilesystemWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
class NativeFilesystemWriter::SharedBaseWork final {
 public:
  SharedBaseWork(SharedBaseWork&&) noexcept;
  SharedBaseWork& operator=(SharedBaseWork&&) noexcept;
  ~SharedBaseWork();
  NativeSharedBaseProgress progress() const noexcept;
  NativeSharedBaseSearchWindow search_window() const noexcept;
  void cancel() noexcept;
 private:
  friend class NativeFilesystemWriter;
  struct Impl;
  explicit SharedBaseWork(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
}
