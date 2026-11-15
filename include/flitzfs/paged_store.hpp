#pragma once
#include "flitzfs/object_store.hpp"

namespace flitzfs {
// Experimental scalable object-storage path. Explicitly opted in; the flat
// native filesystem can explicitly use this backend; namespace metadata is
// still flat/bounded. No file/driver/GPU I/O
// factories. Caller supplies an exclusively owned, resource-admitted backend.
// One control thread; do not race writers, snapshots or backing mutations.
struct PagedStoreLimits {
  std::uint64_t maximum_physical_bytes=0; // Required; capacity, not current RAM use.
};
struct PagedStoreInfo {
  std::uint64_t generation=0,checkpoint_generation=0,physical_end=0;
  std::uint64_t objects=0,logical_bytes=0,stored_payload_bytes=0;
  std::uint32_t height=0;
  bool degraded_roots=false,degraded_metadata=false;
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
  PagedStoreInfo info() const noexcept;
  std::optional<StoreObjectInfo> stat(std::uint64_t id) const;
  std::vector<std::byte> read(std::uint64_t id) const;
  // Stable keyset pagination: strictly after id, at most limit (1..4096).
  // Uses this immutable snapshot root; no whole-volume list or keyset in RAM.
  std::vector<StoreObjectInfo> list_page(std::uint64_t after,std::size_t limit) const;
  void verify_all() const; // Streaming CPU scrub; potentially long, explicitly invoked.
 private:
  friend class PagedObjectStoreWriter;
  struct Impl;
  explicit PagedObjectStoreSnapshot(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
class PagedObjectStoreWriter final {
 public:
  static PagedObjectStoreWriter create(std::shared_ptr<StoreByteIo> empty,
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
  PagedObjectStoreWriter(PagedObjectStoreWriter&&) noexcept;
  PagedObjectStoreWriter& operator=(PagedObjectStoreWriter&&) noexcept;
  ~PagedObjectStoreWriter();
  // Same <=256 ops / 4 MiB input / 1 MiB object limits as the flat store;
  // inventory is bounded by backing capacity, not kStoreMaxObjects.
  // Three barriers; any I/O/publication failure terminally fences this writer.
  PagedStoreInfo commit(std::span<const StoreMutation>);
  // Actual backend barrier; no new records, root publication or generation.
  // Check current roots/contract/extent before and after. Any failure after
  // barrier entry fences the writer; preflight allocation pressure does not.
  PagedStoreInfo flush_checkpoint();
  PagedObjectStoreSnapshot snapshot() const;
  bool poisoned() const noexcept;
 private:
  friend class detail::NativeObjectWriter;
  static PagedObjectStoreWriter rebuild_replacing(const PagedObjectStoreSnapshot&,
      std::shared_ptr<StoreByteIo>,StoreIdentity,PagedStoreLimits,StoreCompressionOptions,
      std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode);
  PagedObjectStoreSnapshot snapshot_for_staging() const;
  std::uint64_t generation_for_staging() const noexcept;
  // Native inode records consume their own metadata entries. Keep public
  // transactions at 256 operations. The native adapter admits 512 operations
  // with 4 MiB input, or 768 operations and 64 KiB extra directory-index input
  // for the explicit indexed-name layout. Public admission is unchanged.
  PagedStoreInfo commit_native(std::span<const StoreMutation>,bool directory_indexes=false);
  PagedStoreInfo commit_impl(std::span<const StoreMutation>,std::size_t maximum_operations,
      std::uint64_t maximum_input=kStoreMaxTransactionBytes);
  struct Impl;
  explicit PagedObjectStoreWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
