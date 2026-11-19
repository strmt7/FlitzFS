#pragma once
#include "flitzfs/object_store.hpp"
#include <stdexcept>

namespace flitzfs {
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
};
struct PagedStoreWriteResult {
  PagedStoreInfo storage;
  std::uint64_t write_bytes_upper_bound=0;
};
struct PagedCompactedSizes {
  std::uint64_t before=0,after_upper=0;
};
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
  std::optional<StoreObjectInfo> stat(std::uint64_t id) const;
  std::vector<std::byte> read(std::uint64_t id) const;
  // Check the verified logical length before opening or decoding a typed object.
  std::vector<std::byte> read(std::uint64_t id,std::size_t maximum_decoded_bytes) const;
  // Absence is reported only for an absent verified key. Integrity or length
  // failures still throw before the caller receives decoded bytes.
  std::optional<std::vector<std::byte>> read_if_present(std::uint64_t id,std::size_t maximum_decoded_bytes) const;
  // Stable keyset pagination: strictly after id, at most limit (1..4096).
  // Uses this immutable snapshot root; no whole-volume list or keyset in RAM.
  std::vector<StoreObjectInfo> list_page(std::uint64_t after,std::size_t limit) const;
  void verify_all() const; // Streaming CPU scrub; potentially long, explicitly invoked.
 private:
  friend class PagedObjectStoreWriter;
  friend class NativeFilesystemSnapshot;
  std::shared_ptr<StoreByteIo> backing_for_volume_head() const;
  bool same_checkpoint(const PagedObjectStoreSnapshot&) const noexcept;
  struct Impl;
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
  static PagedObjectStoreWriter create_impl(std::shared_ptr<StoreByteIo>,StoreIdentity,
      PagedStoreLimits,StoreCompressionOptions,bool mirrored);
  Rewrite prepare_rebuild_replacing(std::shared_ptr<StoreByteIo>,StoreIdentity,
      StoreCompressionOptions,std::span<const std::byte> replacement,bool reencode,
      std::size_t identity_offset=40) const;
  static PagedObjectStoreWriter rebuild_replacing(const PagedObjectStoreSnapshot&,
      std::shared_ptr<StoreByteIo>,StoreIdentity,PagedStoreLimits,StoreCompressionOptions,
      std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode,
      std::size_t identity_offset=40);
  PagedObjectStoreSnapshot snapshot_for_staging(bool query_cache=false) const;
  std::uint64_t generation_for_staging() const noexcept;
  std::uint64_t physical_end_for_staging() const noexcept;
  std::uint64_t append_floor_for_staging() const noexcept;
  // Native inode records consume their own metadata entries. Keep public
  // transactions at 256 operations. The native adapter admits 512 operations
  // with 4 MiB input, or 768 operations and 64 KiB extra directory-index input
  // for the explicit indexed-name layout, or 1024 operations / 128 KiB extra
  // input for shared-base namespaces. Public admission is unchanged.
  PagedStoreInfo commit_native(std::span<const StoreMutation>,bool directory_indexes=false,bool shared_bases=false);
  PagedStoreWriteResult commit_impl(std::span<const StoreMutation>,std::size_t maximum_operations,
      std::uint64_t maximum_input,std::uint64_t maximum_write_bytes);
  struct Impl;
  explicit PagedObjectStoreWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
enum class PagedRewritePhase {verifying_source,copying,verifying_target,publishing,
    verifying_publication,ready,failed,building_index};
struct PagedRewriteProgress {
  PagedRewritePhase phase=PagedRewritePhase::failed;
  std::uint64_t objects=0,completed_in_phase=0;
};
// One control thread. Every operation takes the current writer and checks its
// owner/generation before I/O. Each advance handles one payload or up to a
// leaf of descriptors. Tree finishing and root publication are separate steps.
// Destruction cancels, releases only the target lease and retains partial bytes.
class PagedObjectStoreWriter::Rewrite final {
 public:
  Rewrite(Rewrite&&) noexcept;
  Rewrite& operator=(Rewrite&&) noexcept;
  ~Rewrite();
  bool matches(const PagedObjectStoreWriter&) const noexcept;
  StoreIdentity identity() const noexcept;
  bool replaces_native_root() const noexcept;
  PagedRewriteProgress progress() const noexcept;
  bool advance(const PagedObjectStoreWriter&);
  PagedObjectStoreWriter finish(const PagedObjectStoreWriter&);
 private:
  friend class PagedObjectStoreWriter;
  struct Impl;
  explicit Rewrite(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
