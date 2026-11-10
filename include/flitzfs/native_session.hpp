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
// Explicit runtime policy; stored bytes are identical. The default preserves
// historical per-open aggregation. Windows cancellation clears global intent.
enum class NativeDispositionPolicy {aggregate,windows_global};
struct NativePathLookup {
  NativeInodeAllocationInfo state;
  std::optional<NativeSecurityDescriptor> security;
};
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
// The initial FILE_DELETE_ON_CLOSE option was checked by the FSD. It is NOT
// explicit DELETE access and publishes no pending intent before real Cleanup.
struct NativeInitialDeleteAccess {bool on_cleanup=false;};
// A value alias for ONE open instance, not another open/reference count.
// Only owning open/create/prepare APIs create contexts; Close invalidates aliases.
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
  bool delete_on_close=false; // Historical durable intent, NOT the FSD initial option.
};
// Driver-facing creation options, distinct from the historical explicit
// durable initial-intent option in NativeSessionCreateOptions.
struct NativePreparedCreateOptions {
  NativeFileAccess access;
  NativeBasicAccess basic_access;
  NativeSecurityAccess security_access;
  NativeInitialDeleteAccess initial_delete;
};
struct NativeSessionCreateCommit {
  NativeFsCommit transaction;
  NativeInodeCommitInfo info;
  NativeFileHandle handle;
};
struct NativeSessionStorageCreateCommit {
  NativeFsCommit transaction;
  NativeInodeAllocationInfo state;
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
  static NativeFileSession create(NativeFilesystemWriter writer,NativeDispositionPolicy);
  // Trusted pre-open adapter lookup for namespace/security admission. No new
  // context or authority is created; caller must perform OS access checking.
  NativePathLookup lookup(std::string_view path) const;
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
  // Existing regular inode for the two-stage FSD Open -> Overwrite protocol.
  // Exactly one prechecked operation grant is required. No data/delete intent
  // is published by preparation. Queries may build Open metadata; ordinary I/O
  // remains blocked until successful overwrite AND explicit trusted adoption.
  NativeFileHandle prepare_overwrite(std::string_view path,NativeFileAccess access,
      NativeBasicAccess basic_access,NativeSecurityAccess security_access,
      NativeOverwriteAccess overwrite_access);
  NativeFileHandle prepare_overwrite(std::string_view path,NativeFileAccess access,
      NativeBasicAccess basic_access,NativeSecurityAccess security_access,
      NativeOverwriteAccess overwrite_access,NativeInitialDeleteAccess initial_delete);
  // General FSD Open has no overwrite stage. Cancellation before adoption
  // preserves the inode; initial delete becomes effective only at Cleanup.
  NativeFileHandle prepare_open(std::string_view path,NativeFileAccess access,
      NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
      NativeInitialDeleteAccess initial_delete={});
  NativeInodeMutationCommit overwrite_prepared_result(const NativeFileHandle&,
      const NativeOverwriteMetadata&);
  // Trusted adapter transition, not kernel adoption attestation. No I/O.
  void adopt_prepared(const NativeFileHandle&);
  // Preflight an owned context/token before any durable creation. NEW name,
  // complete metadata and optional delete-on-close intent publish atomically;
  // successful return/slot installation needs no post-commit allocation/query.
  NativeSessionCreateCommit create_open(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      NativeSessionCreateOptions options={});
  NativeSessionCreateCommit create_prepared(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      NativePreparedCreateOptions options={});
  NativeSessionStorageCreateCommit create_prepared_storage(std::string_view path,bool directory,
      const NativeBasicMetadata&,std::span<const std::byte> security,
      std::optional<std::uint64_t> reservation,NativePreparedCreateOptions options={});
  NativeInodeAllocationCommit overwrite_storage_result(const NativeFileHandle&,
      const NativeOverwriteMetadata&,std::optional<std::uint64_t> reservation={});
  NativeInodeAllocationCommit overwrite_prepared_storage_result(const NativeFileHandle&,
      const NativeOverwriteMetadata&,std::optional<std::uint64_t> reservation={});
  NativeInodeStorageMutationCommit write_storage_result(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={},
      const NativeAutomaticMetadata& effect={});
  NativeInodeStorageMutationCommit truncate_storage_result(const NativeFileHandle&,std::uint64_t bytes,
      const NativeAutomaticMetadata& effect={});
  std::optional<NativeSecurityDescriptor> security_descriptor(const NativeFileHandle&) const;
  // Trusted adapter merge input; active FULL replacement grant is required.
  // This does not grant the caller permission to expose any descriptor part.
  std::optional<NativeSecurityDescriptor> security_descriptor_for_update(const NativeFileHandle&) const;
  NativeFsCommit set_security_descriptor(const NativeFileHandle&,std::span<const std::byte>);
  NativeInodeState info(const NativeFileHandle&) const;
  std::optional<NativeBasicMetadata> basic_metadata(const NativeFileHandle&) const;
  NativeFsCommit set_basic_metadata(const NativeFileHandle&,const NativeBasicMetadata&);
  std::vector<NativeFileInfo> list(const NativeFileHandle&) const;
  std::vector<std::byte> read(const NativeFileHandle&,std::uint64_t offset,std::uint32_t bytes) const;
  NativeInodeWriteCommit write(const NativeFileHandle&,std::uint64_t offset,
      std::span<const std::byte>,NativeInodeWriteOptions options={});
  NativeFsCommit truncate(const NativeFileHandle&,std::uint64_t bytes);
  NativeInodeAllocationInfo allocation_info(const NativeFileHandle&) const;
  // Active owned data-write preflight; denied/provisional calls reject before
  // backing I/O. Future Windows bridges must preflight output before mutation.
  NativeInodeAllocationInfo mutation_info(const NativeFileHandle&) const;
  NativeAllocationUsage allocation_usage() const;
  // Already-authorized data-write ownership; unchanged context lifetime and
  // terminal fencing. This reserves native live slots, not host disk space.
  NativeInodeAllocationCommit reserve_result(const NativeFileHandle&,std::uint64_t aligned_bytes,
      const NativeAutomaticMetadata& effect={});
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

  // Default aggregate policy preserves other opens' explicit requests/latches.
  // Explicit windows_global policy clears the inode-wide pending intent on
  // cancel; still-unprocessed per-open initial options re-arm at Cleanup.
  void set_delete(const NativeFileHandle&,bool requested);
  // delete_file is the trusted future FSD Cleanup flag, not a user permission.
  // Preserve context for later paging I/O; only Close releases it.
  void cleanup(const NativeFileHandle&,bool delete_file);
  // Already-authorized FSD Cleanup effects do not require explicit attribute-
  // write access. First success keeps its timestamp; repeated identical action
  // requests are no-ops even if the caller sampled a later valid clock.
  void cleanup(const NativeFileHandle&,bool delete_file,const NativeCleanupMetadata&);
  // Separate name avoids ambiguity for historical cleanup(...,{}). Optional
  // tracked reservation trim shares the SAME metadata/unlink publication.
  void cleanup_event(const NativeFileHandle&,bool delete_file,const NativeCleanupEvent&);
  // No I/O or context release. Required when a void OS callback cannot report
  // an internal failure; the adapter must separately signal its supervisor.
  void fence() noexcept;
  // Active contexts require Cleanup. Healthy provisional contexts retire
  // without Cleanup/storage mutation; poisoned contexts remain retained.
  // Retire detached data only after the last owned context.
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
  std::size_t slot_for(const NativeFileHandle&,bool allow_provisional=false) const;
  NativeFileHandle open_impl(std::string_view,NativeFileAccess,NativeOpenKind,
      NativeBasicAccess,NativeSecurityAccess,NativeOverwriteAccess,bool provisional,
      bool awaiting_overwrite,NativeInitialDeleteAccess);
  NativeSessionStorageCreateCommit create_impl(std::string_view,bool,const NativeBasicMetadata&,
      std::span<const std::byte>,NativeSessionCreateOptions,bool,NativeInitialDeleteAccess,
      std::optional<std::uint64_t> reservation={});
  NativeInodeAllocationCommit overwrite_impl(const NativeFileHandle&,
      const NativeOverwriteMetadata&,bool provisional,std::optional<std::uint64_t> reservation={});
};
}
