#pragma once
#include "flitzfs/object_store.hpp"
#include <functional>
#include <optional>
#include <stdexcept>

namespace flitzfs {
namespace detail {
class IndexedSharedBasePreparation;
class IndexedVolumeView;
class NativeNameIndex;
inline constexpr std::uint64_t kPagedStoreCheckpointWriteBytes=8192;
}
// Scalable object storage for native volumes. The caller supplies an
// exclusively owned, resource-admitted backend.
// One control thread; do not race writers, snapshots or backing mutations.
struct PagedStoreLimits {
  std::uint64_t maximum_physical_bytes=0; // Required; capacity, not current RAM use.
};
// Resource or capacity rejection during nonmutating transaction preparation.
class PagedStoreCapacityError final:public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
  PagedStoreCapacityError(const char* message,bool physical_space,std::uint64_t append_shortfall=0)
      :std::runtime_error(message),physical_space_(physical_space),append_shortfall_(append_shortfall){}
  bool physical_space_exhausted() const noexcept {return physical_space_;}
  // Planning can stop at the first refused record. This is then a lower bound.
  std::uint64_t append_shortfall_bytes() const noexcept {return append_shortfall_;}
 private:
  bool physical_space_=false;
  std::uint64_t append_shortfall_=0;
};
struct PagedStoreInfo {
  // Stored payload total includes both copies in capability 2. Individual
  // StoreObjectInfo lengths still describe one encoded stream; headers/pages
  // and padding are charged by physical_end, not this payload-only counter.
  std::uint64_t generation=0,checkpoint_generation=0,physical_end=0;
  std::uint64_t objects=0,logical_bytes=0,stored_payload_bytes=0;
  std::uint32_t height=0;
  bool degraded_roots=false,degraded_metadata=false;
  bool mirrored_payloads=false,degraded_payloads=false;
  std::uint32_t metadata_page_bytes=0;
};
struct PagedStoreWriteResult {
  PagedStoreInfo storage;
  std::uint64_t write_bytes_upper_bound=0;
};
struct PagedCompactedSizes {
  std::uint64_t before=0,after_upper=0;
};
enum class PagedPayloadRepairProgress { idle, checked, repaired, capacity };
enum class PagedPayloadScrubState { idle, checked, repair_queued, unverified_payload, unverified_index, complete };
struct PagedPayloadScrubPosition {
  StoreIdentity image{};
  std::uint64_t generation=0,after=0,through=0;
  bool operator==(const PagedPayloadScrubPosition&) const = default;
};
struct PagedPayloadScrubResult {
  PagedPayloadScrubState state=PagedPayloadScrubState::idle;
  std::uint64_t object_id=0,verified_record_bytes=0;
  std::optional<PagedPayloadScrubPosition> resumable;
};
enum class PagedRewriteMode { exact_checkpoint, reconcile_foreground, cold_compression };
class IndexedCoherenceWriter;
class PagedObjectStoreWriter;
class PagedObjectStoreSnapshot final {
 public:
  // Checks roots and the root page. Descendants/payloads are checked on access;
  // this is deliberately NOT the legacy eager full-inventory open contract.
  static PagedObjectStoreSnapshot open(std::shared_ptr<StoreByteIo>,PagedStoreLimits);
  PagedObjectStoreSnapshot(PagedObjectStoreSnapshot&&) noexcept;
  PagedObjectStoreSnapshot& operator=(PagedObjectStoreSnapshot&&) noexcept;
  ~PagedObjectStoreSnapshot();
  StoreIdentity identity() const noexcept;
  PagedStoreLimits limits() const noexcept;
  std::uint32_t isolation_unit() const noexcept;
  // Packed layout accounting for sorted hypothetical encoded records, using this
  // image's capability/isolation. No backend I/O, root publication or encoding.
  std::uint64_t rebuilt_size(std::span<const StoreObjectInfo> records) const;
  // Compare packed compaction at this image's isolation unit, preserving the
  // encoding of untouched records. Use checked totals and changed lookup paths.
  // Charge replacements at their raw upper bound. A rebound object's payload
  // ranges from zero to its decoded length when the copy re-encodes it.
  PagedCompactedSizes rebuilt_size_after(std::span<const StoreMutation> changes,
      std::uint64_t rebound_object=0) const;
  PagedStoreInfo info() const noexcept;
  // True for directly mirrored objects or a bound, immutable sealed reader.
  bool payloads_protected() const noexcept;
  std::optional<StoreObjectInfo> stat(std::uint64_t id) const;
  std::vector<std::byte> read(std::uint64_t id) const;
  // Check the verified logical length before opening or decoding a typed object.
  std::vector<std::byte> read(std::uint64_t id,std::size_t maximum_decoded_bytes) const;
  // Absence is reported only for an absent verified key. Integrity or length
  // failures still throw before the caller receives decoded bytes.
  std::optional<std::vector<std::byte>> read_if_present(std::uint64_t id,std::size_t maximum_decoded_bytes) const;
  // Check the expected generation in the same lookup, before reading any payload.
  std::optional<std::vector<std::byte>> read_if_generation(std::uint64_t id,std::uint64_t generation,
      std::size_t maximum_decoded_bytes) const;
  // Stable keyset pagination: strictly after id, at most limit (1..4096).
  // Uses this immutable snapshot root; no whole-volume list or keyset in RAM.
  std::vector<StoreObjectInfo> list_page(std::uint64_t after,std::size_t limit) const;
  void verify_all() const; // Streaming CPU scrub; potentially long, explicitly invoked.
 private:
  friend class PagedObjectStoreWriter;
  friend class NativeFilesystemSnapshot;
  friend class NativeFilesystemWriter;
  friend class IndexedCoherenceSnapshot;
  friend class detail::IndexedVolumeView;
  friend class detail::NativeNameIndex;
  static PagedObjectStoreSnapshot open_sealed(std::shared_ptr<detail::ProtectedStripeImage>);
  // Stream decoded objects after checking all of their payload copies.
  void visit_verified_objects(const std::function<void(std::uint64_t,std::vector<std::byte>)>&) const;
  // For an already decoded and audited checkpoint under the same backing
  // lease. Both stored replicas still receive fresh reads and digest checks.
  void recheck_verified_mirrors(const PagedObjectStoreSnapshot& audited) const;
  struct Impl;
  class QueryScope {
   public:
    QueryScope(const QueryScope&)=delete;
    QueryScope& operator=(const QueryScope&)=delete;
    ~QueryScope();
   private:
    friend class PagedObjectStoreSnapshot;
    explicit QueryScope(Impl&) noexcept;
    Impl* owner_=nullptr;
    bool owns_lookup_pages_=false;
  };
  // A serialized namespace query may revisit verified metadata. Discard the
  // scratch before another query can run, including on an exception.
  QueryScope scoped_query() const noexcept;
  // Consume the span before requesting another metadata object.
  std::span<const std::byte> read_query_metadata(std::uint64_t,std::size_t,
      std::vector<std::byte>& uncached) const;
  std::shared_ptr<StoreByteIo> backing_for_volume_head() const;
  bool same_checkpoint(const PagedObjectStoreSnapshot&) const noexcept;
  std::optional<std::vector<std::byte>> read_matching(std::uint64_t id,std::size_t maximum_decoded_bytes,
      std::optional<std::uint64_t> expected_generation) const;
  std::optional<std::pair<std::uint64_t,std::vector<std::byte>>> read_first_after(std::uint64_t after,
      std::uint64_t last,std::size_t maximum_decoded_bytes) const;
  static PagedObjectStoreSnapshot open_impl(std::shared_ptr<StoreByteIo>,PagedStoreLimits,bool retain_tree_root);
  explicit PagedObjectStoreSnapshot(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
class PagedObjectStoreWriter final {
 public:
  class Rewrite;
  static PagedObjectStoreWriter create(std::shared_ptr<StoreByteIo> empty,
      StoreIdentity,PagedStoreLimits,StoreCompressionOptions={});
  // Explicit capability-2 layout: every payload has an independently checked
  // copy in disjoint qualified failure units. Existing create bytes are unchanged.
  static PagedObjectStoreWriter create_mirrored(std::shared_ptr<StoreByteIo> empty,
      StoreIdentity,PagedStoreLimits,StoreCompressionOptions={});
  // Exclusive explicit recovery. Fully CPU-scrubs before further writes;
  // repairs root publication only. Damaged descendant metadata is readable via
  // its peer but requires a future streaming rebuild before writable resume.
  static PagedObjectStoreWriter resume(std::shared_ptr<StoreByteIo>,StoreIdentity,
      std::uint64_t expected_generation,PagedStoreLimits,StoreCompressionOptions={});
  // Streaming compact/recovery copy into a NEW empty backend/new identity.
  // Reads surviving metadata peers and independently checks CPU payload bytes.
  // Preserves object IDs/generations. No target root is published until the
  // entire copy is checked. Source must
  // be current and quiescent; source/pinned snapshots are never modified.
  static PagedObjectStoreWriter rebuild(const PagedObjectStoreSnapshot&,
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_identity,
      PagedStoreLimits,StoreCompressionOptions={});
  // One explicit candidate while this foreground writer remains alive. No
  // target writes during preparation or initial incremental source validation.
  Rewrite prepare_rebuild(std::shared_ptr<StoreByteIo> empty_target,StoreIdentity,
      StoreCompressionOptions={},bool reencode=true) const;
  PagedObjectStoreWriter(PagedObjectStoreWriter&&) noexcept;
  PagedObjectStoreWriter& operator=(PagedObjectStoreWriter&&) noexcept;
  ~PagedObjectStoreWriter();
  // Same <=256 ops / 4 MiB input / 1 MiB object limits as the flat store;
  // inventory is bounded by backing capacity, not kStoreMaxObjects.
  // Three barriers; any I/O/publication failure terminally fences this writer.
  PagedStoreInfo commit(std::span<const StoreMutation>);
  // Charge the full appended span and both root writes before asking the
  // backend to admit growth. A budget refusal performs no writes or flushes.
  PagedStoreWriteResult commit_with_write_budget(std::span<const StoreMutation>,std::uint64_t maximum_write_bytes);
  // Actual backend barrier; no new records, root publication or generation.
  // Check current roots/contract/extent before and after. Any failure after
  // barrier entry fences the writer; preflight allocation pressure does not.
  PagedStoreInfo flush_checkpoint();
  PagedObjectStoreSnapshot snapshot() const;
  // Lower bound within this writer's lifetime. Includes superseded pages
  // and encoded records. Readers can still pin every one of these bytes.
  std::uint64_t obsolete_bytes() const noexcept;
  bool poisoned() const noexcept;
 private:
  friend class detail::NativeObjectWriter;
  friend class IndexedCoherenceWriter;
  friend class NativeFileSession;
  friend class NativeFilesystemWriter;
  friend class detail::IndexedSharedBasePreparation;
  // The indexed session has already verified the selected native checkpoint and
  // proved retired-owner admission before enabling payload repair.
  static PagedObjectStoreWriter resume_impl(std::shared_ptr<StoreByteIo>,StoreIdentity,
      std::uint64_t expected_generation,PagedStoreLimits,StoreCompressionOptions,bool repair_payloads,
      const PagedObjectStoreSnapshot* verified_indexed_source=nullptr);
  // Advisory compact metadata only. Fence the checked root without repairing
  // it. Descendant pages and objects are checked as they are used.
  static PagedObjectStoreWriter resume_advisory(const PagedObjectStoreSnapshot&,StoreCompressionOptions);
  static PagedObjectStoreWriter create_impl(std::shared_ptr<StoreByteIo>,StoreIdentity,
      PagedStoreLimits,StoreCompressionOptions,bool mirrored,std::span<const StoreMutation> initial={},
      std::uint64_t maximum_write_bytes=UINT64_MAX,
      const std::function<bool(std::uint64_t)>& reserve_writes={},bool compact_metadata=false);
  struct RebuildColdEncoding {
    std::uint64_t after_generation,first_object,past_last_object;
    std::function<bool(std::uint64_t)> reserve_writes;
    bool eligible(std::uint64_t id) const noexcept {return id>=first_object && id<past_last_object;}
  };
  Rewrite prepare_rebuild_replacing(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,std::span<const std::byte> replacement,bool reencode,
      std::size_t identity_offset=40,PagedRewriteMode=PagedRewriteMode::exact_checkpoint,
      std::optional<RebuildColdEncoding> cold={}) const;
  struct RebuildRetention {
    std::size_t maximum_object_bytes;
    std::function<bool(std::uint64_t,std::span<const std::byte>)> keep;
    std::function<bool(std::uint64_t)> charge;
  };
  // Private filtered rotation of an advisory catalog on distinct backing.
  Rewrite prepare_rebuild_retaining(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,RebuildRetention) const;
  Rewrite prepare_rebuild_impl(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,std::span<const std::byte> replacement,bool reencode,
      std::size_t identity_offset,std::optional<RebuildRetention>,PagedRewriteMode,
      std::optional<RebuildColdEncoding> cold={}) const;
  enum class RebuildPayloads { preserve, sealed_workspace, mirrored_recovery };
  static PagedObjectStoreWriter rebuild_replacing(const PagedObjectStoreSnapshot&,
      std::shared_ptr<StoreByteIo>,StoreIdentity,PagedStoreLimits,StoreCompressionOptions,
      std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode,
      std::size_t identity_offset=40,RebuildPayloads=RebuildPayloads::preserve);
  PagedObjectStoreSnapshot snapshot_impl(bool retain_tree_root) const;
  PagedObjectStoreSnapshot snapshot_for_staging(bool query_cache=false) const;
  // Borrowed by serialized indexed queries. Independent snapshots and staged
  // workers do not schedule repairs on the selected writer.
  PagedObjectStoreSnapshot snapshot_for_indexed_read() const;
  // Validate the native namespace against the freshly checked tree on each
  // side of the barrier. The borrowed snapshot and its cache end there.
  PagedStoreInfo flush_checkpoint_verified(const std::function<void(const PagedObjectStoreSnapshot&)>&);
  bool payload_repair_pending() const noexcept;
  std::uint64_t capacity_repair_generation() const noexcept;
  PagedPayloadRepairProgress repair_payload();
  PagedPayloadScrubResult scrub_payload();
  bool restore_payload_scrub(const PagedPayloadScrubPosition&) noexcept;
  std::uint64_t generation_for_staging() const noexcept;
  std::uint64_t physical_end_for_staging() const noexcept;
  std::uint64_t append_floor_for_staging() const noexcept;
  std::uint64_t compaction_savings_lower_bound(std::uint64_t extra_stored_bytes) const noexcept;
  // Native inode records consume their own metadata entries. Keep public
  // transactions at 256 operations. The native adapter admits 512 operations
  // with 4 MiB input, or 768 operations and 64 KiB extra directory-index input
  // for the explicit indexed-name layout, or 1024 operations / 128 KiB extra
  // input for shared-base namespaces. Public admission is unchanged.
  PagedStoreInfo commit_native(std::span<const StoreMutation>,bool directory_indexes=false,bool shared_bases=false);
  std::optional<PagedStoreInfo> commit_if_compacted_smaller(std::span<const StoreMutation>,
      std::uint64_t rebound_object,PagedCompactedSizes&,std::span<const std::span<const StoreMutation>> alternatives,
      std::size_t& selected);
  struct CompactedAdmission {
    std::uint64_t rebound_object;
    PagedCompactedSizes& sizes;
    std::span<const std::span<const StoreMutation>> alternatives;
    std::size_t selected=0;
  };
  PagedStoreWriteResult commit_impl(std::span<const StoreMutation>,std::size_t maximum_operations,
      std::uint64_t maximum_input,std::uint64_t maximum_write_bytes,
      const std::function<bool(std::uint64_t)>& reserve_writes={},CompactedAdmission* compacted=nullptr);
  struct Impl;
  explicit PagedObjectStoreWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
enum class PagedRewritePhase {verifying_source,copying,verifying_target,publishing,
    verifying_publication,ready,failed,building_index,awaiting_charge,reconciling,verifying_reconciliation,
    screening_compression,declined};
struct PagedRewriteProgress {
  PagedRewritePhase phase=PagedRewritePhase::failed;
  std::uint64_t objects=0,completed_in_phase=0,required_write_bytes=0;
};
// One control thread. Validate the source owner and checkpoint before using a
// candidate. The optional indexed compactor reconciles immutable checkpoints.
// All modes require an exact current checkpoint at finish. Root publication
// stays separate from bounded payload and index preparation.
// Destruction cancels, releases only the target lease and retains partial bytes.
class PagedObjectStoreWriter::Rewrite final {
 public:
  Rewrite(Rewrite&&) noexcept;
  Rewrite& operator=(Rewrite&&) noexcept;
  ~Rewrite();
  bool matches(const PagedObjectStoreWriter&) const noexcept;
  bool ready_for(const PagedObjectStoreWriter&) const noexcept;
  std::uint64_t verified_capacity_repair_generation() const noexcept;
  StoreIdentity identity() const noexcept;
  bool replaces_native_root() const noexcept;
  PagedRewriteProgress progress() const noexcept;
  bool advance(const PagedObjectStoreWriter&);
  PagedObjectStoreWriter finish(const PagedObjectStoreWriter&);
 private:
  friend class PagedObjectStoreWriter;
  friend class NativeFileSession;
  std::uint64_t cold_encoding_through() const noexcept;
  struct Impl;
  explicit Rewrite(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
