#pragma once
#include "flitzfs/native_filesystem.hpp"
#include <memory>

namespace flitzfs {
// Pure CPU ownership layer for the future Windows adapter. It never creates
// a driver, dispatcher, mount, worker or thread. One control thread is required.
enum class NativeSessionErrorCode {
  invalid_handle,access_denied,delete_pending,sharing_violation,
  invalid_options,limit,terminal
};
class NativeSessionError final:public std::runtime_error {
 public:
  NativeSessionError(NativeSessionErrorCode code,const char* message):std::runtime_error(message),code_(code){}
  NativeSessionErrorCode code() const noexcept{return code_;}
 private:
  NativeSessionErrorCode code_;
};
enum class NativeOpenKind {any,file,directory};
struct NativeFileAccess {
  bool read=true,write=false,erase=false; // Already authorized by the future OS adapter.
};
// Separate already-authorized WRITE_ATTRIBUTES-like grant; data write does
// not silently grant permission to replace persisted timestamps/attributes.
struct NativeBasicAccess {bool write=false;};
// A value alias for ONE open instance, not another open/reference count.
// Only open() creates a context; one successful Close invalidates all aliases.
// Weak ownership prevents stale tokens from binding to another session.
class NativeFileHandle final {
 public:
  NativeFileHandle() noexcept=default;
 private:
  friend class NativeFileSession;
  std::weak_ptr<const void> owner_;
  std::uint64_t token_=0;
};
struct NativeSessionStats {
  std::uint32_t contexts=0,cleaned=0,delete_requests=0,latched_inodes=0;
  bool fenced=true; // Table diagnostics, not an independent storage/driver verdict.
};
class NativeFileSession final {
 public:
  static constexpr std::size_t max_contexts=256;
  // Consumes exclusive writer ownership and fully verifies admission. No raw
  // writer accessor; unresolved old-session state must be separately recovered.
  static NativeFileSession create(NativeFilesystemWriter writer);
  ~NativeFileSession();
  NativeFileSession(NativeFileSession&&) noexcept;
  NativeFileSession& operator=(NativeFileSession&&)=delete; // Cannot replace an owner with live contexts.
  NativeFileSession(const NativeFileSession&)=delete;
  NativeFileSession& operator=(const NativeFileSession&)=delete;

  NativeFileHandle open(std::string_view path,NativeFileAccess access={},
      NativeOpenKind kind=NativeOpenKind::any);
  NativeFileHandle open(std::string_view path,NativeFileAccess access,
      NativeOpenKind kind,NativeBasicAccess basic_access);
  NativeInodeState info(const NativeFileHandle&) const;
  std::optional<NativeBasicMetadata> basic_metadata(const NativeFileHandle&) const;
  NativeFsCommit set_basic_metadata(const NativeFileHandle&,const NativeBasicMetadata&);
  std::vector<NativeFileInfo> list(const NativeFileHandle&) const;
  std::vector<std::byte> read(const NativeFileHandle&,std::uint64_t offset,std::uint32_t bytes) const;
  NativeInodeWriteCommit write(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={});
  NativeFsCommit truncate(const NativeFileHandle&,std::uint64_t bytes);
  NativeFsCommit create_entry(std::string_view path,bool directory=false);
  // Trusted creation path: data/namespace and explicit full basic metadata
  // publish atomically. Access checking remains the future OS adapter's job.
  NativeFsCommit create_entry(std::string_view path,bool directory,const NativeBasicMetadata&);
  NativeFsCommit rename(const NativeFileHandle&,std::string_view target,bool replace=false);

  // Aggregate per-open disposition; canceling one cannot cancel another's
  // request or a request already latched at Cleanup.
  void set_delete(const NativeFileHandle&,bool requested);
  // delete_file is the trusted future FSD Cleanup flag, not a user permission.
  // Preserve context for later paging I/O; only Close releases it.
  void cleanup(const NativeFileHandle&,bool delete_file);
  // Requires Cleanup. Retire detached data only after the last owned context.
  // Failed retirement keeps the context and fences further I/O.
  void close(const NativeFileHandle&);
  // All native mutations already acknowledge after publication barriers.
  // Both forms execute a real shared-image flush; a file form first validates
  // its still-owned context and remains valid after Cleanup until Close.
  NativeFsCommit flush();
  NativeFsCommit flush(const NativeFileHandle&);
  NativeFilesystemSnapshot snapshot() const;
  NativeSessionStats stats() const noexcept;
  bool poisoned() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit NativeFileSession(std::unique_ptr<Impl>) noexcept;
  Impl& ready() const;
  std::size_t slot_for(const NativeFileHandle&) const;
};
}
