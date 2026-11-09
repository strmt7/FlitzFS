#pragma once
#include "flitzfs/object_store.hpp"
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {
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
  invalid_operation,limit,corrupt,storage_failure
};
class NativeFsError final:public std::runtime_error {
 public:
  NativeFsError(NativeFsErrorCode code,const char* message):std::runtime_error(message),code_(code){}
  NativeFsErrorCode code() const noexcept{return code_;}
 private:
  NativeFsErrorCode code_;
};
struct NativeFileInfo {
  std::uint64_t inode=0,parent=0,logical_bytes=0,created_sequence=0,modified_sequence=0;
  bool directory=false;
  std::string path;
};
enum class NativeFileOperation {create_file,create_directory,write,truncate,rename,remove};
struct NativeFileMutation {
  NativeFileOperation operation=NativeFileOperation::create_file;
  std::string path,target;
  std::uint64_t offset=0,size=0;
  std::vector<std::byte> data;
  bool replace=false;
};
struct NativeInodeWriteOptions {
  bool append=false;       // Choose the latest EOF inside the single-control-thread call.
  bool constrained=false;  // Never extend logical EOF; a range beyond EOF writes zero bytes.
};
struct NativeFsCommit {
  StoreCommitInfo storage;
  std::uint64_t sequence=0; // Namespace transaction sequence, distinct from root repair generation.
  std::vector<std::uint64_t> created_inodes;
};

struct NativeInodeWriteCommit {
  NativeFsCommit transaction;
  std::uint64_t inode=0,offset=0; // Actual selected offset, including append EOF.
  std::uint32_t bytes=0;         // Durable transferred count; no result on failure.
};

class NativeFilesystemSnapshot final {
 public:
  // Consumes a fully verified object snapshot; validates native namespace and all references.
  static NativeFilesystemSnapshot open(ObjectStoreSnapshot objects);
  NativeFilesystemSnapshot(NativeFilesystemSnapshot&&) noexcept;
  NativeFilesystemSnapshot& operator=(NativeFilesystemSnapshot&&) noexcept;
  NativeFilesystemSnapshot(const NativeFilesystemSnapshot&)=delete;
  NativeFilesystemSnapshot& operator=(const NativeFilesystemSnapshot&)=delete;
  ~NativeFilesystemSnapshot();
  StoreIdentity identity() const noexcept;
  StoreCommitInfo storage_info() const noexcept;
  std::uint64_t sequence() const noexcept;
  NativeFileInfo stat(std::string_view path) const;
  // IDs belong to this volume/snapshot. Missing/deleted IDs return not_found;
  // a later file at the old path is never selected instead.
  NativeFileInfo stat_inode(std::uint64_t inode) const;
  std::vector<NativeFileInfo> list(std::string_view directory="") const;
  std::vector<NativeFileInfo> list_inode(std::uint64_t directory) const;
  // Root-relative '/' paths, conservative ASCII/case-insensitive components.
  // Short reads at EOF; holes are zeros; full extent integrity precedes exposure.
  std::vector<std::byte> read(std::string_view path,std::uint64_t offset,std::uint32_t bytes) const;
  std::vector<std::byte> read_inode(std::uint64_t inode,
      std::uint64_t offset,std::uint32_t bytes) const;
  void verify_all() const;
 private:
  friend class NativeFilesystemWriter;
  struct Impl;
  explicit NativeFilesystemSnapshot(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

class NativeFilesystemWriter final {
 public:
  // A NEW empty generation-zero object store only. Never reinterprets existing objects.
  static NativeFilesystemWriter initialize(ObjectStoreWriter objects);
  // Object writer must already hold its qualified backend lease/recovery fence.
  static NativeFilesystemWriter resume(ObjectStoreWriter objects);
  // Copy a quiescent latest snapshot into an empty NEW volume identity.
  // Preserve inode IDs/sequences/contents, rebind namespace identity, and
  // retain the original source. No in-place reclaim, filename swap or deletion.
  static NativeFilesystemWriter compact(const NativeFilesystemSnapshot& source,
      std::shared_ptr<StoreByteIo> empty_target,StoreIdentity new_identity,
      StoreCompressionOptions compression={});
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
  // Resolve a stable LIVE inode against the last acknowledged namespace, then
  // use the same atomic staging/replicated publication as apply(). One control
  // thread; no path lookup by the caller and no intermediate callback.
  // Full supplied input <=1 MiB even if constrained; accepted bytes may be
  // shorter. Append selects current EOF, ignoring the supplied offset.
  // Zero accepted bytes publish nothing. A removed inode is never redirected.
  // These APIs do not implement Windows open/unlinked-inode lifetime or a mount.
  NativeInodeWriteCommit write_inode(std::uint64_t inode,std::uint64_t offset,
      std::span<const std::byte> bytes,NativeInodeWriteOptions options={});
  NativeFsCommit truncate_inode(std::uint64_t inode,std::uint64_t bytes);
  NativeFilesystemSnapshot snapshot() const;
  bool poisoned() const noexcept;
 private:
  struct Impl;
  explicit NativeFilesystemWriter(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
}
