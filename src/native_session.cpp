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
  struct Slot {
    std::uint64_t token=0,inode=0;
    NativeFileAccess access;
    bool cleaned=false,cleanup_delete=false,requested=false,basic_write=false;
    NativeCleanupMetadata cleanup_metadata;
    bool security_read=false,security_replace=false;
    bool overwrite=false,supersede=false;
  };
  static_assert(std::is_nothrow_copy_assignable_v<Slot>);
  NativeFilesystemWriter writer;
  std::shared_ptr<const void> owner=std::make_shared<const int>(0);
  std::array<Slot,max_contexts> slots{};
  std::array<std::uint64_t,kNativeMaxNodes> latched{};
  std::uint64_t next_token=1;
  bool terminal=false;
  explicit Impl(NativeFilesystemWriter source):writer(std::move(source)) {
    if(writer.poisoned())fail(E::terminal,"cannot own a terminal native writer");
    auto snapshot=writer.snapshot();
    if(snapshot.needs_session_recovery())fail(E::delete_pending,"old file contexts need independent closed-session recovery");
  }
  bool latched_inode(std::uint64_t inode) const noexcept {
    for(const auto id:latched)if(id==inode)return true;
    return false;
  }
  bool intent(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    if(latched_inode(inode))return true;
    for(const auto& slot:slots)
      if(slot.token && slot.token!=excluded && slot.inode==inode && slot.requested)return true;
    return false;
  }
  std::size_t contexts(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    std::size_t count=0;
    for(const auto& slot:slots)if(slot.token && slot.token!=excluded && slot.inode==inode)++count;
    return count;
  }
  void clear_latch(std::uint64_t inode) noexcept {
    for(auto& id:latched)if(id==inode)id=0;
  }
  NativeInodeState state(const Slot& slot) {
    try {
      auto result=writer.inspect_inode(slot.inode);
      if(!result.unlinked && result.delete_pending!=intent(slot.inode)) {
        terminal=true;fail(E::terminal,"native context/delete-intent ownership diverged");
      }
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
  static_assert(sizeof(Impl)<=16*1024);
  static_assert(std::is_nothrow_move_constructible_v<NativeFileHandle>);
  return NativeFileSession(std::make_unique<Impl>(std::move(writer)));
}
NativeFileSession::Impl& NativeFileSession::ready() const {
  if(!impl_ || impl_->terminal || impl_->writer.poisoned())fail(E::terminal,"native file session is fenced");
  return *impl_;
}
std::size_t NativeFileSession::slot_for(const NativeFileHandle& handle) const {
  auto& self=ready();const auto owner=handle.owner_.lock();
  if(!handle.token_ || !owner || owner!=self.owner)fail(E::invalid_handle,"foreign/expired native file context");
  for(std::size_t i=0;i<self.slots.size();++i)
    if(self.slots[i].token==handle.token_)return i;
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
  auto& self=ready();
  if(kind!=NativeOpenKind::any && kind!=NativeOpenKind::file && kind!=NativeOpenKind::directory)
    fail(E::invalid_options,"unknown native open kind");
  std::size_t available=self.slots.size();
  for(std::size_t i=0;i<self.slots.size();++i)if(!self.slots[i].token){available=i;break;}
  if(available==self.slots.size() || self.next_token==(std::numeric_limits<std::uint64_t>::max)())
    fail(E::limit,"native file context budget exhausted");
  admit_path(path,true);
  const auto state=self.writer.inspect_path(path);
  if(state.delete_pending || state.unlinked)fail(E::delete_pending,"native name is delete-pending");
  if(kind==NativeOpenKind::file && state.info.directory)
    throw NativeFsError(NativeFsErrorCode::is_directory,"native open requires regular file");
  if(kind==NativeOpenKind::directory && !state.info.directory)
    throw NativeFsError(NativeFsErrorCode::not_directory,"native open requires directory");
  NativeFileHandle result;result.owner_=self.owner;result.token_=self.next_token;
  self.slots[available]={self.next_token,state.info.inode,access,false,false,false,basic_access.write,{},security_access.read,security_access.replace,
      overwrite_access.overwrite,overwrite_access.supersede};
  ++self.next_token;return result;
}
NativeSessionCreateCommit NativeFileSession::create_open(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,
    NativeSessionCreateOptions options) {
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionCreateCommit>);
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
  admit_path(path);
  // The session is single-control-thread. The local slot/token reservation
  // cannot race another request; do not expose/install it before publication.
  NativeSessionCreateCommit result;
  result.handle.owner_=self.owner;result.handle.token_=self.next_token;
  auto created=self.writer.create_inode_result(path,directory,basic,descriptor,options.delete_on_close);
  // ONLY statically nonthrowing scalar/optional/weak-owner/move operations
  // remain after durable success. Never call open(), inspect(), or snapshot().
  result.transaction=std::move(created.transaction);result.info=created.info;
  self.slots[available]={self.next_token,result.info.inode,options.access,false,false,
      options.delete_on_close,options.basic_access.write,{},
      options.security_access.read,options.security_access.replace};
  ++self.next_token;
  return result;
}
NativeInodeState NativeFileSession::info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();return self.state(self.slots[index]);
}
std::optional<NativeBasicMetadata> NativeFileSession::basic_metadata(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  (void)self.state(slot);return self.writer.inspect_basic_metadata(slot.inode);
}
NativeFsCommit NativeFileSession::set_basic_metadata(const NativeFileHandle& handle,
    const NativeBasicMetadata& value) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.basic_write)fail(E::access_denied,"native context has no basic metadata write access");
  (void)self.state(slot);return self.writer.set_basic_metadata(slot.inode,value);
}
std::optional<NativeSecurityDescriptor> NativeFileSession::security_descriptor(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
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
NativeFsCommit NativeFileSession::truncate(const NativeFileHandle& handle,std::uint64_t bytes) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode(slot.inode,bytes);
}
NativeInodeMutationCommit NativeFileSession::overwrite_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(effect.replace_attributes?!slot.supersede:!slot.overwrite)
    fail(E::access_denied,"native context has no checked overwrite operation grant");
  if(slot.cleaned)fail(E::invalid_options,"cannot overwrite a cleaned native context");
  if(!native_overwrite_metadata_valid(effect))
    fail(E::invalid_options,"invalid canonical native overwrite metadata");
  const auto state=self.state(slot);
  if(state.delete_pending || state.unlinked)
    fail(E::delete_pending,"cannot overwrite deleting native inode");
  return self.writer.overwrite_inode_result(slot.inode,effect);
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory) {
  auto& self=ready();admit_path(path);NativeFileMutation op;
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  op.path=path;return self.writer.apply({&op,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& value) {
  auto& self=ready();admit_path(path);
  NativeFileMutation op;op.path=std::string(path);
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  const NativeBasicMutation basic{0,1,value};
  return self.writer.apply_with_basic({&op,1},{&basic,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor) {
  auto& self=ready();admit_path(path);
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
  admit_path(target);
  const auto source=self.state(slot);
  if(source.delete_pending || source.unlinked)fail(E::delete_pending,"cannot rename deleting native inode");
  try {
    const auto destination=self.writer.inspect_path(target);
    if(replace && destination.info.inode!=slot.inode && self.contexts(destination.info.inode))
      fail(E::sharing_violation,"replacement inode still has owned file contexts");
  }catch(const NativeFsError& error){if(error.code()!=NativeFsErrorCode::not_found)throw;}
  NativeFileMutation op;op.operation=NativeFileOperation::rename;op.path=source.info.path;
  op.target=target;op.replace=replace;return self.writer.apply({&op,1});
}
void NativeFileSession::set_delete(const NativeFileHandle& handle,bool requested) {
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!slot.access.erase)fail(E::access_denied,"native context has no delete access");
  if(slot.cleaned)fail(E::invalid_options,"cannot change disposition after Cleanup");
  const auto state=self.state(slot);
  if(state.unlinked)fail(E::delete_pending,"detached inode cannot be resurrected");
  const auto desired=requested || self.intent(slot.inode,slot.token);
  self.writer.set_delete_pending(slot.inode,desired);
  slot.requested=requested; // Only after durable success; assignment cannot throw.
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file) {
  cleanup(handle,delete_file,{});
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file,
    const NativeCleanupMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!native_cleanup_metadata_valid(effect))
    fail(E::invalid_options,"invalid automatic Cleanup metadata clock");
  if(slot.cleaned) {
    if(slot.cleanup_delete!=delete_file || !slot.cleanup_metadata.same_requests(effect))
      fail(E::invalid_options,"conflicting duplicate native Cleanup");
    return;
  }
  const auto state=self.state(slot);
  std::uint64_t* latch=nullptr;
  if(delete_file) {
    if(!state.delete_pending && !state.unlinked) {
      self.terminal=true;fail(E::terminal,"Cleanup deletion has no persisted authorized intent");
    }
  } else if(slot.requested && !state.unlinked && !self.latched_inode(slot.inode)) {
    for(auto& id:self.latched)if(!id){latch=&id;break;}
    if(!latch){self.terminal=true;fail(E::terminal,"native delete latch invariant exhausted");}
  }
  // Reserve any latch before fallible publication; never acknowledge the
  // context or release latches between timestamp changes and unlink.
  if(effect.any())self.writer.cleanup_inode(slot.inode,delete_file,effect);
  else if(delete_file)self.writer.unlink_inode(slot.inode);
  if(delete_file)self.clear_latch(slot.inode);
  if(latch)*latch=slot.inode;
  slot.cleanup_metadata=effect;
  slot.cleaned=true;slot.cleanup_delete=delete_file; // Retain paging context until Close.
}
void NativeFileSession::fence() noexcept {
  if(impl_)impl_->terminal=true;
}
void NativeFileSession::close(const NativeFileHandle& handle) {
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
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
