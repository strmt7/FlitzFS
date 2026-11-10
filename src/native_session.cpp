#include "flitzfs/native_session.hpp"
#include "flitzfs/catalog.hpp"
#include <new>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

namespace flitzfs {
namespace {
using E=NativeSessionErrorCode;
[[noreturn]] void fail(E code,const char* message){throw NativeSessionError(code,message);}
void admit_path(std::string_view path,bool root=false) {
  // Reuse the pure validator, which checks the byte cap before allocating.
  // Never copy a caller's arbitrary-length view into an owned mutation first.
  try{(void)canonical_catalog_path(path,root);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){throw NativeFsError(NativeFsErrorCode::invalid_path,"invalid native session path");}
}
}
struct NativeFileSession::Impl {
  enum class Phase:std::uint8_t {active,prepared,overwritten,opened};
  struct Slot {
    std::uint64_t token=0,inode=0;
    NativeFileAccess access;
    bool cleaned=false,cleanup_delete=false,requested=false,basic_write=false;
    NativeCleanupMetadata cleanup_metadata;
    bool security_read=false,security_replace=false;
    bool overwrite=false,supersede=false;
    Phase phase=Phase::active;
    bool initial_delete=false,cleanup_trim=false;
  };
  static_assert(std::is_nothrow_copy_assignable_v<Slot>);
  NativeFilesystemWriter writer;
  std::shared_ptr<const void> owner=std::make_shared<const int>(0);
  std::array<Slot,max_contexts> slots{};
  std::array<std::uint64_t,kNativeMaxNodes> latched{};
  // Nonempty initial-delete directories retain contents on disk. Their
  // kernel-like pending state/authority is runtime-only until final Cleanup.
  static constexpr std::uint8_t kExplicit=1,kInitial=2,kVolatile=4;
  std::array<std::uint8_t,kNativeMaxNodes> latch_flags{};
  std::uint64_t next_token=1;
  bool terminal=false;
  NativeDispositionPolicy disposition_policy=NativeDispositionPolicy::aggregate;
  explicit Impl(NativeFilesystemWriter source,NativeDispositionPolicy policy):writer(std::move(source)),disposition_policy(policy) {
    if(writer.poisoned())fail(E::terminal,"cannot own a terminal native writer");
    auto snapshot=writer.snapshot();
    if(snapshot.needs_session_recovery())fail(E::delete_pending,"old file contexts need independent closed-session recovery");
  }
  bool volatile_initial(std::uint64_t inode) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode && (latch_flags[i]&kVolatile))return true;
    return false;
  }
  void admit_destination(std::string_view path) {
    bool any=false;for(auto value:latch_flags)any=any || (value&kVolatile)!=0;
    if(!any)return; // Preserve historical traffic when no volatile intent exists.
    const auto slash=path.rfind('/');
    const auto parent=writer.inspect_path(slash==std::string_view::npos?std::string_view{}:path.substr(0,slash));
    if(volatile_initial(parent.info.inode))fail(E::delete_pending,"destination parent has initial delete pending");
  }
  bool intent(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)
      if(latched[i]==inode && !(latch_flags[i]&kVolatile))return true;
    for(const auto& slot:slots)
      if(slot.token && slot.token!=excluded && slot.inode==inode && slot.requested)return true;
    return false;
  }
  bool explicit_intent(std::uint64_t inode,std::uint64_t excluded) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)
      if(latched[i]==inode && (latch_flags[i]&kExplicit))return true;
    for(const auto& slot:slots)
      if(slot.token && slot.token!=excluded && slot.inode==inode && slot.requested)return true;
    return false;
  }
  void cancel_initial_latch(std::uint64_t inode) noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode) {
      latch_flags[i]=static_cast<std::uint8_t>(latch_flags[i]&kExplicit);
      if(!latch_flags[i])latched[i]=0;
    }
  }
  std::size_t contexts(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    std::size_t count=0;
    for(const auto& slot:slots)if(slot.token && slot.token!=excluded && slot.inode==inode)++count;
    return count;
  }
  bool provisional_inode(std::uint64_t inode) const noexcept {
    for(const auto& slot:slots)
      if(slot.token && slot.inode==inode && slot.phase!=Phase::active)return true;
    return false;
  }
  void clear_latch(std::uint64_t inode) noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode){latched[i]=0;latch_flags[i]=0;}
  }
  NativeInodeState state(const Slot& slot) {
    try {
      auto result=writer.inspect_inode(slot.inode);
      if(!result.unlinked && result.delete_pending!=intent(slot.inode)) {
        terminal=true;fail(E::terminal,"native context/delete-intent ownership diverged");
      }
      if(!result.unlinked && volatile_initial(slot.inode))result.delete_pending=true;
      return result;
    }catch(const NativeFsError& error) {
      if(error.code()==NativeFsErrorCode::not_found) {
        terminal=true;fail(E::terminal,"owned native inode disappeared");
      }
      throw;
    }
  }
};
NativeFileSession::NativeFileSession(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)){}
NativeFileSession::~NativeFileSession()=default;
NativeFileSession::NativeFileSession(NativeFileSession&&) noexcept=default;
NativeFileSession NativeFileSession::create(NativeFilesystemWriter writer) {
  return create(std::move(writer),NativeDispositionPolicy::aggregate);
}
NativeFileSession NativeFileSession::create(NativeFilesystemWriter writer,NativeDispositionPolicy policy) {
  if(policy!=NativeDispositionPolicy::aggregate && policy!=NativeDispositionPolicy::windows_global)
    fail(E::invalid_options,"unknown native disposition policy");
  static_assert(sizeof(Impl)<=16*1024);
  static_assert(std::is_nothrow_move_constructible_v<NativeFileHandle>);
  return NativeFileSession(std::make_unique<Impl>(std::move(writer),policy));
}
NativeFileSession::Impl& NativeFileSession::ready() const {
  if(!impl_ || impl_->terminal || impl_->writer.poisoned())fail(E::terminal,"native file session is fenced");
  return *impl_;
}
std::size_t NativeFileSession::slot_for(const NativeFileHandle& handle,bool allow_provisional) const {
  auto& self=ready();const auto owner=handle.owner_.lock();
  if(!handle.token_ || !owner || owner!=self.owner)fail(E::invalid_handle,"foreign/expired native file context");
  for(std::size_t i=0;i<self.slots.size();++i)
    if(self.slots[i].token==handle.token_) {
      if(!allow_provisional && self.slots[i].phase!=Impl::Phase::active)
        fail(E::invalid_options,"native context is not adopted");
      return i;
    }
  fail(E::invalid_handle,"closed native file context");
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,NativeOpenKind kind) {
  return open(path,access,kind,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access) {
  return open(path,access,kind,basic_access,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access) {
  return open(path,access,kind,basic_access,security_access,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeOverwriteAccess overwrite_access) {
  return open_impl(path,access,kind,basic_access,security_access,overwrite_access,false,false,{});
}
NativeFileHandle NativeFileSession::prepare_overwrite(std::string_view path,NativeFileAccess access,
    NativeBasicAccess basic_access,NativeSecurityAccess security_access,NativeOverwriteAccess overwrite_access) {
  return prepare_overwrite(path,access,basic_access,security_access,overwrite_access,{});
}
NativeFileHandle NativeFileSession::prepare_overwrite(std::string_view path,NativeFileAccess access,
    NativeBasicAccess basic_access,NativeSecurityAccess security_access,NativeOverwriteAccess overwrite_access,
    NativeInitialDeleteAccess initial_delete) {
  return open_impl(path,access,NativeOpenKind::file,basic_access,security_access,overwrite_access,true,true,initial_delete);
}
NativeFileHandle NativeFileSession::prepare_open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeInitialDeleteAccess initial_delete) {
  return open_impl(path,access,kind,basic_access,security_access,{},true,false,initial_delete);
}
NativeFileHandle NativeFileSession::open_impl(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeOverwriteAccess overwrite_access,bool provisional,bool awaiting_overwrite,
    NativeInitialDeleteAccess initial_delete) {
  auto& self=ready();
  if(provisional && awaiting_overwrite && overwrite_access.overwrite==overwrite_access.supersede)
    fail(E::invalid_options,"prepared overwrite requires exactly one checked operation grant");
  if(kind!=NativeOpenKind::any && kind!=NativeOpenKind::file && kind!=NativeOpenKind::directory)
    fail(E::invalid_options,"unknown native open kind");
  std::size_t available=self.slots.size();
  for(std::size_t i=0;i<self.slots.size();++i)if(!self.slots[i].token){available=i;break;}
  if(available==self.slots.size() || self.next_token==(std::numeric_limits<std::uint64_t>::max)())
    fail(E::limit,"native file context budget exhausted");
  admit_path(path,true);
  const auto state=self.writer.inspect_path(path);
  if(state.delete_pending || state.unlinked || self.volatile_initial(state.info.inode))
    fail(E::delete_pending,"native name is delete-pending");
  if(initial_delete.on_cleanup && state.info.inode==1)
    fail(E::invalid_options,"initial delete-on-close is forbidden on root");
  if(kind==NativeOpenKind::file && state.info.directory)
    throw NativeFsError(NativeFsErrorCode::is_directory,"native open requires regular file");
  if(kind==NativeOpenKind::directory && !state.info.directory)
    throw NativeFsError(NativeFsErrorCode::not_directory,"native open requires directory");
  NativeFileHandle result;result.owner_=self.owner;result.token_=self.next_token;
  self.slots[available]={self.next_token,state.info.inode,access,false,false,false,basic_access.write,{},security_access.read,security_access.replace,
      overwrite_access.overwrite,overwrite_access.supersede,
      provisional?(awaiting_overwrite?Impl::Phase::prepared:Impl::Phase::opened):Impl::Phase::active,
      initial_delete.on_cleanup};
  ++self.next_token;return result;
}
NativeSessionCreateCommit NativeFileSession::create_open(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,
    NativeSessionCreateOptions options) {
  auto stored=create_impl(path,directory,basic,descriptor,options,false,{});
  return {std::move(stored.transaction),stored.state.info,std::move(stored.handle)};
}
NativeSessionCreateCommit NativeFileSession::create_prepared(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,NativePreparedCreateOptions options) {
  auto stored=create_impl(path,directory,basic,descriptor,
      {options.access,options.basic_access,options.security_access,false},true,options.initial_delete);
  return {std::move(stored.transaction),stored.state.info,std::move(stored.handle)};
}
NativeSessionStorageCreateCommit NativeFileSession::create_prepared_storage(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,
    std::optional<std::uint64_t> reservation,NativePreparedCreateOptions options) {
  return create_impl(path,directory,basic,descriptor,
      {options.access,options.basic_access,options.security_access,false},true,options.initial_delete,reservation);
}
NativeSessionStorageCreateCommit NativeFileSession::create_impl(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,NativeSessionCreateOptions options,
    bool provisional,NativeInitialDeleteAccess initial_delete,std::optional<std::uint64_t> reservation) {
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionCreateCommit>);
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionStorageCreateCommit>);
  static_assert(std::is_nothrow_move_assignable_v<NativeFsCommit>);
  static_assert(std::is_nothrow_copy_assignable_v<NativeInodeCommitInfo>);
  static_assert(std::is_nothrow_copy_assignable_v<NativeFileHandle>);
  auto& self=ready();
  if(options.delete_on_close && !options.access.erase)
    fail(E::access_denied,"delete-on-close creation requires authorized delete access");
  std::size_t available=self.slots.size();
  for(std::size_t i=0;i<self.slots.size();++i)if(!self.slots[i].token){available=i;break;}
  if(available==self.slots.size() || self.next_token==(std::numeric_limits<std::uint64_t>::max)())
    fail(E::limit,"native file context budget exhausted");
  admit_path(path);self.admit_destination(path);
  // The session is single-control-thread. The local slot/token reservation
  // cannot race another request; do not expose/install it before publication.
  NativeSessionStorageCreateCommit result;
  result.handle.owner_=self.owner;result.handle.token_=self.next_token;
  auto created=self.writer.create_inode_storage_result(path,directory,basic,descriptor,reservation,options.delete_on_close);
  // ONLY statically nonthrowing scalar/optional/weak-owner/move operations
  // remain after durable success. Never call open(), inspect(), or snapshot().
  result.transaction=std::move(created.transaction);result.state=created.state;
  self.slots[available]={self.next_token,result.state.info.inode,options.access,false,false,
      options.delete_on_close,options.basic_access.write,{},
      options.security_access.read,options.security_access.replace,false,false,
      provisional?Impl::Phase::opened:Impl::Phase::active,initial_delete.on_cleanup};
  ++self.next_token;
  return result;
}
NativePathLookup NativeFileSession::lookup(std::string_view path) const {
  auto& self=ready();admit_path(path,true);
  const auto selected=self.writer.inspect_path(path);
  Impl::Slot selector;selector.inode=selected.info.inode;
  const auto state=self.state(selector);
  NativePathLookup result;result.state=self.writer.inspect_allocation(selected.info.inode);
  result.state.info.delete_pending=state.delete_pending;
  result.security=self.writer.inspect_security_descriptor(selected.info.inode);
  return result;
}
NativeInodeState NativeFileSession::info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();return self.state(self.slots[index]);
}
std::optional<NativeBasicMetadata> NativeFileSession::basic_metadata(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  (void)self.state(slot);return self.writer.inspect_basic_metadata(slot.inode);
}
NativeFsCommit NativeFileSession::set_basic_metadata(const NativeFileHandle& handle,
    const NativeBasicMetadata& value) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.basic_write)fail(E::access_denied,"native context has no basic metadata write access");
  (void)self.state(slot);return self.writer.set_basic_metadata(slot.inode,value);
}
std::optional<NativeSecurityDescriptor> NativeFileSession::security_descriptor(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.security_read)fail(E::access_denied,"native context has no descriptor read grant");
  (void)self.state(slot);return self.writer.inspect_security_descriptor(slot.inode);
}
NativeFsCommit NativeFileSession::set_security_descriptor(const NativeFileHandle& handle,std::span<const std::byte> value) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.security_replace)fail(E::access_denied,"native context has no full descriptor replacement grant");
  if(!native_security_descriptor_framed(value))
    throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid native security descriptor framing");
  (void)self.state(slot);return self.writer.set_security_descriptor(slot.inode,value);
}
std::vector<NativeFileInfo> NativeFileSession::list(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.read)fail(E::access_denied,"native context has no list access");
  (void)self.state(slot);return self.writer.inspect_directory(slot.inode);
}
std::vector<std::byte> NativeFileSession::read(const NativeFileHandle& handle,
    std::uint64_t offset,std::uint32_t bytes) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.read)fail(E::access_denied,"native context has no read access");
  return self.writer.read_inode(slot.inode,offset,bytes);
}
NativeInodeWriteCommit NativeFileSession::write(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode(slot.inode,offset,bytes,options);
}
NativeInodeWriteCommit NativeFileSession::write_with_metadata(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_with_metadata(slot.inode,offset,bytes,options,effect);
}
NativeFsCommit NativeFileSession::truncate_with_metadata(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_with_metadata(slot.inode,bytes,effect);
}
NativeInodeMutationCommit NativeFileSession::write_result(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_result(slot.inode,offset,bytes,options,effect);
}
NativeInodeMutationCommit NativeFileSession::truncate_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_result(slot.inode,bytes,effect);
}
NativeInodeAllocationInfo NativeFileSession::allocation_info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  const auto state=self.state(slot);auto result=self.writer.inspect_allocation(slot.inode);
  result.info.delete_pending=state.delete_pending;return result;
}
NativeInodeAllocationInfo NativeFileSession::mutation_info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no data mutation access");
  const auto state=self.state(slot);
  if(state.info.directory)throw NativeFsError(NativeFsErrorCode::is_directory,"native data mutation requires a file");
  auto result=self.writer.inspect_allocation(slot.inode);result.info.delete_pending=state.delete_pending;return result;
}
NativeAllocationUsage NativeFileSession::allocation_usage() const {return ready().writer.inspect_allocation_usage();}
NativeInodeAllocationCommit NativeFileSession::reserve_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no reservation write access");
  return self.writer.reserve_inode_result(slot.inode,bytes,effect);
}
NativeFsCommit NativeFileSession::truncate(const NativeFileHandle& handle,std::uint64_t bytes) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode(slot.inode,bytes);
}
NativeInodeMutationCommit NativeFileSession::overwrite_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect) {
  auto stored=overwrite_impl(handle,effect,false);
  return {std::move(stored.transaction),stored.state.info,0,0};
}
NativeInodeMutationCommit NativeFileSession::overwrite_prepared_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect) {
  auto stored=overwrite_impl(handle,effect,true);
  return {std::move(stored.transaction),stored.state.info,0,0};
}
NativeInodeAllocationCommit NativeFileSession::overwrite_storage_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,std::optional<std::uint64_t> reservation) {
  return overwrite_impl(handle,effect,false,reservation);
}
NativeInodeAllocationCommit NativeFileSession::overwrite_prepared_storage_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,std::optional<std::uint64_t> reservation) {
  return overwrite_impl(handle,effect,true,reservation);
}
NativeInodeStorageMutationCommit NativeFileSession::write_storage_result(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_storage_result(slot.inode,offset,bytes,options,effect);
}
NativeInodeStorageMutationCommit NativeFileSession::truncate_storage_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_storage_result(slot.inode,bytes,effect);
}
void NativeFileSession::adopt_prepared(const NativeFileHandle& handle) {
  const auto index=slot_for(handle,true);auto& slot=ready().slots[index];
  if(slot.phase!=Impl::Phase::overwritten && slot.phase!=Impl::Phase::opened)
    fail(E::invalid_options,"native prepared context is not ready for adoption");
  slot.phase=Impl::Phase::active;
}
NativeInodeAllocationCommit NativeFileSession::overwrite_impl(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,bool provisional,std::optional<std::uint64_t> reservation) {
  static_assert(std::is_nothrow_move_constructible_v<NativeInodeMutationCommit>);
  const auto index=slot_for(handle,provisional);auto& self=ready();auto& slot=self.slots[index];
  if(provisional && slot.phase!=Impl::Phase::prepared)
    fail(E::invalid_options,"native context is not awaiting overwrite");
  if(effect.replace_attributes?!slot.supersede:!slot.overwrite)
    fail(E::access_denied,"native context has no checked overwrite operation grant");
  if(slot.cleaned)fail(E::invalid_options,"cannot overwrite a cleaned native context");
  if(!native_overwrite_metadata_valid(effect))
    fail(E::invalid_options,"invalid canonical native overwrite metadata");
  const auto state=self.state(slot);
  if(state.delete_pending || state.unlinked)
    fail(E::delete_pending,"cannot overwrite deleting native inode");
  auto result=self.writer.overwrite_inode_storage_result(slot.inode,effect,reservation);
  // No I/O/allocation/fallible operation after publication. Waiting for
  // adoption permits a healthy cancellation Close without fabricated Cleanup.
  if(provisional)slot.phase=Impl::Phase::overwritten;
  return result;
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory) {
  auto& self=ready();admit_path(path);self.admit_destination(path);NativeFileMutation op;
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  op.path=path;return self.writer.apply({&op,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& value) {
  auto& self=ready();admit_path(path);self.admit_destination(path);
  NativeFileMutation op;op.path=std::string(path);
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  const NativeBasicMutation basic{0,1,value};
  return self.writer.apply_with_basic({&op,1},{&basic,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor) {
  auto& self=ready();admit_path(path);self.admit_destination(path);
  if(!native_security_descriptor_framed(descriptor))
    throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid native creation security descriptor");
  NativeFileMutation op;op.path=std::string(path);
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  const NativeBasicMutation metadata{0,1,basic};
  const NativeSecurityMutation security{0,1,NativeSecurityDescriptor(descriptor.begin(),descriptor.end())};
  return self.writer.apply_with_metadata({&op,1},{&metadata,1},{&security,1});
}
NativeFsCommit NativeFileSession::rename(const NativeFileHandle& handle,std::string_view target,bool replace) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.erase)fail(E::access_denied,"native context has no rename access");
  if(slot.cleaned)fail(E::invalid_options,"cannot rename cleaned native context");
  admit_path(target);self.admit_destination(target);
  const auto source=self.state(slot);
  if(source.delete_pending || source.unlinked)fail(E::delete_pending,"cannot rename deleting native inode");
  try {
    const auto destination=self.writer.inspect_path(target);
    if(replace && destination.info.inode!=slot.inode &&
        (self.contexts(destination.info.inode) || self.volatile_initial(destination.info.inode)))
      fail(E::sharing_violation,"replacement inode still has owned file contexts");
  }catch(const NativeFsError& error){if(error.code()!=NativeFsErrorCode::not_found)throw;}
  NativeFileMutation op;op.operation=NativeFileOperation::rename;op.path=source.info.path;
  op.target=target;op.replace=replace;return self.writer.apply({&op,1});
}
void NativeFileSession::set_delete(const NativeFileHandle& handle,bool requested) {
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!slot.access.erase)fail(E::access_denied,"native context has no delete access");
  if(slot.cleaned)fail(E::invalid_options,"cannot change disposition after Cleanup");
  if(requested && self.provisional_inode(slot.inode))
    fail(E::sharing_violation,"native inode has an unresolved provisional open");
  const auto state=self.state(slot);
  if(state.unlinked)fail(E::delete_pending,"detached inode cannot be resurrected");
  const bool global=self.disposition_policy==NativeDispositionPolicy::windows_global;
  const auto desired=global?requested:(requested || self.explicit_intent(slot.inode,slot.token));
  self.writer.set_delete_pending(slot.inode,desired);
  // FSD SetDisposition(false) clears current initial pending state, but not
  // any descriptor's still-unprocessed initial option. Preserve independent
  // explicit requests/latches. No latch changes precede durable success.
  if(!requested) {
    if(global) {
      // Selected FSD clears the NODE's pending state, not just one open's
      // request. Preserve still-unprocessed initial per-FD options to re-arm
      // at their actual Cleanup. All updates follow durable success.
      for(auto& other:self.slots)if(other.token && other.inode==slot.inode)other.requested=false;
      self.clear_latch(slot.inode);
    }else self.cancel_initial_latch(slot.inode);
  }
  slot.requested=requested; // Only after durable success; assignment cannot throw.
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file) {
  cleanup(handle,delete_file,{});
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file,
    const NativeCleanupMetadata& effect) {
  cleanup_event(handle,delete_file,{effect,false});
}
void NativeFileSession::cleanup_event(const NativeFileHandle& handle,bool delete_file,
    const NativeCleanupEvent& event) {
  const auto& effect=event.metadata;
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!native_cleanup_metadata_valid(effect))
    fail(E::invalid_options,"invalid automatic Cleanup metadata clock");
  if(slot.cleaned) {
    if(slot.cleanup_delete!=delete_file || slot.cleanup_trim!=event.trim_allocation ||
        !slot.cleanup_metadata.same_requests(effect))
      fail(E::invalid_options,"conflicting duplicate native Cleanup");
    return;
  }
  const bool initial=slot.initial_delete || self.volatile_initial(slot.inode);
  if((delete_file || initial) && self.provisional_inode(slot.inode))
    fail(E::sharing_violation,"cannot arm deletion with a provisional open");
  const auto state=self.state(slot);
  if(delete_file && !initial && !state.delete_pending && !state.unlinked) {
    self.terminal=true;fail(E::terminal,"Cleanup deletion has no authorized intent");
  }
  std::size_t latch=self.latched.size();
  for(std::size_t i=0;i<self.latched.size();++i)
    if(self.latched[i]==slot.inode){latch=i;break;}
  if(!delete_file && !state.unlinked && (slot.requested || initial) && latch==self.latched.size()) {
    for(std::size_t i=0;i<self.latched.size();++i)if(!self.latched[i]){latch=i;break;}
    if(latch==self.latched.size()){self.terminal=true;fail(E::terminal,"native delete latch invariant exhausted");}
  }
  if(initial) {
    // All latch capacity is reserved BEFORE publication. Initial intent is
    // armed at real Cleanup, never by Open/creation/adoption. Capture the
    // effective directory policy before commit; no post-commit query.
    auto committed=self.writer.cleanup_inode_storage_result(slot.inode,delete_file,true,event);
    if(!delete_file && !committed.state.info.unlinked) {
      self.latched[latch]=slot.inode;
      self.latch_flags[latch]|=Impl::kInitial;
      if(slot.requested)self.latch_flags[latch]|=Impl::kExplicit;
      if(!committed.state.info.delete_pending)self.latch_flags[latch]|=Impl::kVolatile;
      else self.latch_flags[latch]=static_cast<std::uint8_t>(self.latch_flags[latch]&~Impl::kVolatile);
    } else self.clear_latch(slot.inode);
  } else {
    if(effect.any() || event.trim_allocation)
      self.writer.cleanup_inode_storage_result(slot.inode,delete_file,false,event);
    else if(delete_file)self.writer.unlink_inode(slot.inode);
    if(delete_file)self.clear_latch(slot.inode);
    else if(latch!=self.latched.size()) {
      self.latched[latch]=slot.inode;
      if(slot.requested)self.latch_flags[latch]|=Impl::kExplicit;
    }
  }
  slot.cleanup_metadata=effect;slot.cleanup_trim=event.trim_allocation;
  slot.cleaned=true;slot.cleanup_delete=delete_file; // Paging stays owned until Close.
}
void NativeFileSession::fence() noexcept {
  if(impl_)impl_->terminal=true;
}
void NativeFileSession::close(const NativeFileHandle& handle) {
  const auto index=slot_for(handle,true);auto& self=ready();auto& slot=self.slots[index];
  if(slot.phase==Impl::Phase::prepared || slot.phase==Impl::Phase::overwritten || slot.phase==Impl::Phase::opened) {
    // Preparation owns no persisted delete intent, and peer deletion was
    // excluded while provisional. Release ONLY this healthy context; do not
    // query storage, synthesize Cleanup, undo overwrite or remove its inode.
    if(slot.cleaned || slot.requested || slot.cleanup_delete || self.intent(slot.inode,slot.token) ||
        self.volatile_initial(slot.inode)) {
      self.terminal=true;fail(E::terminal,"provisional native context lifetime diverged");
    }
    slot=Impl::Slot{};return;
  }
  if(slot.phase!=Impl::Phase::active) {
    self.terminal=true;fail(E::terminal,"invalid native open phase");
  }
  if(!slot.cleaned)fail(E::invalid_options,"native Close requires prior Cleanup");
  const auto state=self.state(slot);
  const auto last=self.contexts(slot.inode,slot.token)==0;
  if(last && state.delete_pending && !state.unlinked) {
    self.terminal=true;fail(E::terminal,"last Close arrived without required deletion Cleanup");
  }
  if(last && state.unlinked)self.writer.retire_inode(slot.inode);
  // All fallible work is complete. Never release a context before retirement
  // succeeds, or allocate/throw after a durable retirement.
  if(last)self.clear_latch(slot.inode);
  slot=Impl::Slot{};
}
NativeFsCommit NativeFileSession::flush() {
  return ready().writer.flush();
}
NativeFsCommit NativeFileSession::flush(const NativeFileHandle& handle) {
  const auto index=slot_for(handle);auto& self=ready();
  (void)self.state(self.slots[index]);
  return self.writer.flush();
}
NativeFilesystemSnapshot NativeFileSession::snapshot() const {
  return ready().writer.snapshot();
}
NativeSessionStats NativeFileSession::stats() const noexcept {
  NativeSessionStats result;
  if(!impl_)return result;
  result.fenced=impl_->terminal || impl_->writer.poisoned();
  for(const auto& slot:impl_->slots)if(slot.token) {
    ++result.contexts;if(slot.cleaned)++result.cleaned;if(slot.requested)++result.delete_requests;
  }
  for(const auto id:impl_->latched)if(id)++result.latched_inodes;
  return result;
}
bool NativeFileSession::poisoned() const noexcept{return !impl_ || impl_->terminal || impl_->writer.poisoned();}
}
