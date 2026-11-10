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
// Already-authorized full descriptor access, not an ACL evaluator or a claim
// that WRITE_DAC alone authorizes owner/SACL replacement.
struct NativeSecurityAccess {bool read=false,replace=false;};
// A separately checked create/overwrite operation grant. WinFsp may remove
// implicit WRITE_DATA/DELETE from returned GrantedAccess after checking it;
// data/delete/WRITE_ATTRIBUTES bits alone must not infer this authorization.
struct NativeOverwriteAccess {bool overwrite=false,supersede=false;};
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
// Grants have already been authorized by the Windows/FSD caller. Creation
// authority on the parent and descriptor inheritance are NOT evaluated here.
struct NativeSessionCreateOptions {
  NativeFileAccess access;
  NativeBasicAccess basic_access;
  NativeSecurityAccess security_access;
  bool delete_on_close=false;
};
struct NativeSessionCreateCommit {
  NativeFsCommit transaction;
  NativeInodeCommitInfo info;
  NativeFileHandle handle;
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
  NativeFileHandle open(std::string_view path,NativeFileAccess access,
      NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access);
  NativeFileHandle open(std::string_view path,NativeFileAccess access,
      NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
      NativeOverwriteAccess overwrite_access);
  // Preflight an owned context/token before any durable creation. NEW name,
  // complete metadata and optional delete-on-close intent publish atomically;
  // successful return/slot installation needs no post-commit allocation/query.
  NativeSessionCreateCommit create_open(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      NativeSessionCreateOptions options={});
  std::optional<NativeSecurityDescriptor> security_descriptor(const NativeFileHandle&) const;
  NativeFsCommit set_security_descriptor(const NativeFileHandle&,std::span<const std::byte>);
  NativeInodeState info(const NativeFileHandle&) const;
  std::optional<NativeBasicMetadata> basic_metadata(const NativeFileHandle&) const;
  NativeFsCommit set_basic_metadata(const NativeFileHandle&,const NativeBasicMetadata&);
  std::vector<NativeFileInfo> list(const NativeFileHandle&) const;
  std::vector<std::byte> read(const NativeFileHandle&,std::uint64_t offset,std::uint32_t bytes) const;
  NativeInodeWriteCommit write(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={});
  NativeFsCommit truncate(const NativeFileHandle&,std::uint64_t bytes);
  // Trusted automatic FSD effects share the durable data transaction and
  // require data-write access, independently of explicit attribute grants.
  // Context ownership remains valid through late paging until Close.
  NativeInodeWriteCommit write_with_metadata(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions,const NativeAutomaticMetadata&);
  NativeFsCommit truncate_with_metadata(const NativeFileHandle&,std::uint64_t bytes,
      const NativeAutomaticMetadata&);
  // Owned commit-bound facts, with no post-commit query/tuple lookup.
  NativeInodeMutationCommit write_result(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={},
      const NativeAutomaticMetadata& effect={});
  NativeInodeMutationCommit truncate_result(const NativeFileHandle&,std::uint64_t bytes,
      const NativeAutomaticMetadata& effect={});
  // Already-authorized Overwrite/Supersede event before Cleanup.
  // Preserve its ownership and descriptor; no post-commit result query.
  NativeInodeMutationCommit overwrite_result(const NativeFileHandle&,const NativeOverwriteMetadata&);
  NativeFsCommit create_entry(std::string_view path,bool directory=false);
  // Trusted creation path: data/namespace and explicit full basic metadata
  // publish atomically. Access checking remains the future OS adapter's job.
  NativeFsCommit create_entry(std::string_view path,bool directory,const NativeBasicMetadata&);
  NativeFsCommit create_entry(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security);
  NativeFsCommit rename(const NativeFileHandle&,std::string_view target,bool replace=false);

  // Aggregate per-open disposition; canceling one cannot cancel another's
  // request or a request already latched at Cleanup.
  void set_delete(const NativeFileHandle&,bool requested);
  // delete_file is the trusted future FSD Cleanup flag, not a user permission.
  // Preserve context for later paging I/O; only Close releases it.
  void cleanup(const NativeFileHandle&,bool delete_file);
  // Already-authorized FSD Cleanup effects do not require explicit attribute-
  // write access. First success keeps its timestamp; repeated identical action
  // requests are no-ops even if the caller sampled a later valid clock.
  void cleanup(const NativeFileHandle&,bool delete_file,const NativeCleanupMetadata&);
  // No I/O or context release. Required when a void OS callback cannot report
  // an internal failure; the adapter must separately signal its supervisor.
  void fence() noexcept;
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
