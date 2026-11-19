#include "winfsp_native.hpp"
#include "memory_volume_factory.hpp"
#include "../native_maintenance_source.hpp"
#include "../store_file.hpp"
#include "../store_codec.hpp"
#include "native_cleanup.hpp"
#include "native_close.hpp"
#include "native_windows_boundary.hpp"
#include "flitzfs/directory_order.hpp"
#include "flitzfs/native_volume_file.hpp"
#include <sddl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
namespace flitzfs {
namespace {
constexpr auto legacy_file_info=NativeFileInfoAllocation::native_reserved_cache;
constexpr auto indexed_file_info=NativeFileInfoAllocation::indexed_reserved_cache;
NativeFileInfoAllocation file_info_policy(NativeImageKind kind) noexcept {
  return kind==NativeImageKind::indexed_paged?indexed_file_info:legacy_file_info;
}
bool sdk_hint(UINT64& hint) noexcept{
  const auto* context=FspFileSystemGetOperationContext();
  if(!context||!context->Request||context->Request->Kind!=FspFsctlTransactWriteKind)return false;
  hint=context->Request->Hint;return true; // opaque, including all 64 bits
}
void sdk_response(FSP_FILE_SYSTEM* fs,FSP_FSCTL_TRANSACT_RSP& response) noexcept{
  FspFileSystemSendResponse(fs,&response);
}
struct Handle {HANDLE value=nullptr;~Handle(){if(value)CloseHandle(value);}};
struct Local {void* value=nullptr;~Local(){if(value)LocalFree(value);}};
// Owns the SAME new-file backend and its lease/barriers. The smaller cap never
// replaces its free-space/memory/identity admission or physical-failure contract.
class CappedFile final:public StoreByteIo {
 public:
  CappedFile(std::shared_ptr<StoreByteIo> file,std::uint64_t cap):file_(std::move(file)),cap_(cap){}
  std::uint64_t extent() const override {
    const auto n=file_->extent();if(n>cap_)throw std::runtime_error("native image exceeded history cap");return n;
  }
  StoreIoContract contract() const noexcept override{return file_->contract();}
  void admit_write(std::uint64_t projected) override {
    if(projected>cap_)throw std::runtime_error("native image history cap");
    file_->admit_write(projected);
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    bounds(at,out.size());file_->read_exact(at,out);
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    bounds(at,in.size());file_->write_exact(at,in);
  }
  void flush() override{file_->flush();}
 private:
  void bounds(std::uint64_t at,std::size_t n) const {
    if(at>cap_||n>cap_-at)throw std::runtime_error("native image bounded range");
  }
  std::shared_ptr<StoreByteIo> file_;std::uint64_t cap_;
};
std::vector<std::byte> descriptor(PSECURITY_DESCRIPTOR value) {
  SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
  if(!value||!IsValidSecurityDescriptor(value)||!GetSecurityDescriptorControl(value,&control,&revision)||
      !(control&SE_SELF_RELATIVE))throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid relative Windows descriptor");
  const auto bytes=GetSecurityDescriptorLength(value);
  if(!bytes||bytes>kNativeMaxSecurityBytes||bytes>FSP_FSCTL_TRANSACT_RSP_BUFFER_SIZEMAX)
    throw NativeFsError(NativeFsErrorCode::limit,"descriptor transport budget");
  const auto* start=reinterpret_cast<const std::byte*>(value);return {start,start+bytes};
}
std::vector<std::byte> owner_security() {
  Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("owner token");
  DWORD bytes=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&bytes);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||!bytes||bytes>16384)throw std::runtime_error("owner token bounds");
  std::vector<std::byte> data(bytes);
  if(!GetTokenInformation(token.value,TokenUser,data.data(),bytes,&bytes))throw std::runtime_error("owner token read");
  LPWSTR text=nullptr;
  if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text))throw std::runtime_error("owner SID");
  Local sid{text};const std::wstring identity(text);
  const auto sddl=L"O:"+identity+L"G:"+identity+L"D:P(A;OICI;FA;;;"+identity+L")";
  Local result;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&result.value,&bytes))
    throw std::runtime_error("owner descriptor");
  return descriptor(result.value);
}
UINT64 admit_windows_snapshot(const NativeFilesystemSnapshot& view,bool closed_indexed=false) {
  if(!closed_indexed && view.needs_session_recovery())
    throw std::runtime_error("restart requires resolved old-session state");
  const auto mode=file_info_policy(view.image_storage().kind);
  UINT64 created=0;
  const auto inspect=[&](auto&& self,std::uint64_t inode,unsigned depth)->void {
    if(depth>16)throw std::runtime_error("restart namespace depth");
    const auto state=view.allocation_info(inode);
    FSP_FSCTL_FILE_INFO info{};
    if(!encode_native_file_info(state,mode,info))throw std::runtime_error("unqualified restart FileInfo/reservation");
    const auto security=view.security_descriptor(inode);
    if(!security)throw std::runtime_error("missing restart descriptor");
    (void)descriptor(const_cast<std::byte*>(security->data()));
    if(inode==1)created=info.CreationTime;
    if(!state.info.directory)return;
    std::optional<NativeDirectoryCursor> cursor;
    do {
      const auto page=view.list_inode_page(inode,cursor.value_or(NativeDirectoryCursor{}),64);
      for(const auto& child:page.entries)self(self,child.inode,depth+1);
      cursor=page.next;
    }while(cursor);
  };
  inspect(inspect,1,0);
  if(!created)throw std::runtime_error("missing original volume creation time");
  return created;
}
std::wstring_view bounded(PCWSTR value,std::size_t maximum=241) {
  if(!value)throw NativeFsError(NativeFsErrorCode::invalid_path,"null Windows name");
  for(std::size_t i=0;i<=maximum;++i)if(!value[i])return {value,i};
  throw NativeFsError(NativeFsErrorCode::invalid_path,"Windows name bounds");
}
std::string path(PCWSTR value) {
  auto text=bounded(value);if(!text.empty()) {
    if(text.front()!=L'\\')throw NativeFsError(NativeFsErrorCode::invalid_path,"root-relative Windows name required");
    text.remove_prefix(1);
  }
  std::string result;result.reserve(text.size());
  for(auto c:text) {
    if(c<0x20||c>0x7e)throw NativeFsError(NativeFsErrorCode::invalid_path,"native ASCII name profile");
    result.push_back(c==L'\\'?'/':static_cast<char>(c));
  }
  return result;
}
UINT32 attributes(UINT32 value,bool directory) {
  value&=~UINT32(FILE_ATTRIBUTE_NORMAL);
  if(value&~kNativeBasicAttributeMask)throw NativeFsError(NativeFsErrorCode::invalid_operation,"unsupported native attributes");
  return (value&~UINT32(FILE_ATTRIBUTE_DIRECTORY))|(directory?FILE_ATTRIBUTE_DIRECTORY:0);
}
NativeWindowsIoOutcome success() noexcept{return {STATUS_SUCCESS,false};}
}
struct WinFspNativeWritable::Impl {
  struct Slot {std::uintptr_t token=0;NativeFileHandle handle;bool provisional=false,awaiting_overwrite=false;UINT32 security_access=0;};
  WinFspOriginGuard origin;
  NativeFileSession session;
  std::array<Slot,NativeFileSession::max_contexts> slots{};
  std::uintptr_t next_token=1;
  mutable std::mutex control;
  std::atomic<unsigned> foreground{0};
  std::atomic<std::uint64_t> foreground_epoch{0};
  std::atomic<bool> anchor_pending{false};
  std::atomic<bool> offline{false};
  std::atomic<unsigned> stopped{0};
  Handle maintenance_event;
  // The service joins before the control lock, session and signal handle retire.
  std::unique_ptr<NativeDeferredWriteService> deferred;
  std::unique_ptr<NativeGroupMaintenance> groups;
  NativeMaintenancePolicy group_policy;
  MemoryVolumeBacking* memory_backing=nullptr;
  std::uint64_t last_retirement_observation=0,last_retirement_capture=0;
  bool maintenance_cancelled=false;
  std::atomic<bool> maintenance_signal_failed{false};
  bool callbacks_bound=false;
  StoreIdentity identity{};UINT64 created=0;UINT32 serial=0;
  const NativeFileInfoAllocation info_policy;
  Impl(NativeFileSession s,StoreIdentity id,UINT64 stamp,NativeFileInfoAllocation mode=legacy_file_info)
      :session(std::move(s)),identity(id),created(stamp),info_policy(mode) {
    static_assert(std::is_nothrow_copy_assignable_v<NativeFileHandle>);
    for(std::size_t i=0;i<id.size();++i)serial^=UINT32(std::to_integer<unsigned>(id[i]))<<((i%4)*8);
    if(!serial)serial=1;
  }
  struct ForegroundCall {
    Impl& owner;
    explicit ForegroundCall(Impl& active) noexcept:owner(active) {
      owner.foreground.fetch_add(1,std::memory_order_acq_rel);
      owner.foreground_epoch.fetch_add(1,std::memory_order_release);
      if(owner.anchor_pending.load(std::memory_order_acquire))owner.signal_maintenance();
    }
    ~ForegroundCall(){owner.foreground.fetch_sub(1,std::memory_order_acq_rel);owner.signal_maintenance();}
  };
  void signal_maintenance() noexcept {
    if(maintenance_event.value && !SetEvent(maintenance_event.value))
      maintenance_signal_failed.store(true,std::memory_order_release);
  }
  WinFspMaintenanceResult advance_groups(const NativeMaintenanceObservation& observed,std::uint64_t now,
      NativeGroupSearchOpportunity search) {
    using State=WinFspMaintenanceState;
    if(!groups)return {State::unchanged};
    if(!groups->pending())return {State::unchanged,groups->progress()};
    const auto progress=groups->advance(observed,now,search);
    WinFspMaintenanceResult result{State::advanced,progress};
    result.group_members=groups->active_group_members();
    if(progress.search_window_ready || search.reply)result.search_window=groups->search_window();
    if(progress.search_batch_ready)result.search_batch=groups->search_batch();
    if(!search.replies.empty())result.retained_search=groups->has_retained_search();
    if(progress.phase==NativeMaintenancePhase::failed) {
      if(session.poisoned())offline.store(true,std::memory_order_release);
      groups.reset();
    }
    return result;
  }
  WinFspMaintenanceResult advance_indexed_maintenance(const NativeMaintenanceObservation& observed,std::uint64_t now) {
    using State=WinFspMaintenanceState;
    using Phase=NativeMaintenancePhase;
    using Deferral=NativeMaintenanceDeferral;
    const auto defer=[](Deferral reason) {
      return WinFspMaintenanceResult{State::advanced,{Phase::deferred,reason}};
    };
    const bool reap=session.retirement_pending();
    const bool discard_catalog=memory_backing && memory_backing->coherence_retirement_pending();
    if(!reap && !discard_catalog && (!memory_backing || !memory_backing->compaction_pending(session)))return {State::unchanged};
    if(!observed.sequence || observed.sequence<=last_retirement_observation)
      return defer(Deferral::observation);
    last_retirement_observation=observed.sequence;
    const auto resources=detail::maintenance_resources(observed,now,{});
    if(!resources || observed.captured_ms<=last_retirement_capture)return defer(Deferral::observation);
    last_retirement_capture=observed.captured_ms;
    if(observed.foreground_pending)return defer(Deferral::foreground);
    if(resources->cpu_busy)return defer(Deferral::cpu);
    // Fund prefix verification, decoded pages, COW edit/encoding scratch and
    // allocation planning before entering the existing one-transaction reaper.
    constexpr auto transaction_scratch=8ULL*kStoreMaxTransactionBytes;
    if(resources->memory_allowance<transaction_scratch)return defer(Deferral::memory);
    try {
      if(reap) {
        if(!session.reclaim_retired_inode())return {State::unchanged};
        return {State::advanced,{session.retirement_pending()?Phase::publishing:Phase::idle},true};
      }
      if(discard_catalog) {
        memory_backing->advance_coherence_retirement();
        return {State::advanced,{memory_backing->coherence_retirement_pending()?Phase::preparing:Phase::idle}};
      }
      using Step=MemoryCompactionStep;
      switch(memory_backing->advance_compaction(session)) {
        case Step::idle:return {State::unchanged};
        case Step::copying:return {State::advanced,{Phase::preparing}};
        case Step::ready:return {State::advanced,{Phase::publishing}};
        case Step::published:return {State::advanced,{Phase::idle},false,true};
        case Step::retiring:return {State::advanced,{Phase::preparing}};
        case Step::pinned:return defer(Deferral::other_maintenance);
        case Step::failed:return {State::advanced,{Phase::idle},false,false,true};
      }
    }catch(...) {
      if(session.poisoned())offline.store(true,std::memory_order_release);
      throw;
    }
    return {State::unchanged};
  }
  template<class F>HostedIdleMaintenanceResult idle(F&& fn) {
    std::unique_lock guard(control,std::try_to_lock);
    if(!guard || foreground.load(std::memory_order_acquire)!=0 || (deferred&&deferred->publication_pending_locked()))
      return {HostedIdleMaintenanceState::busy};
    if(offline.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire)!=0)
      throw std::runtime_error("mounted session is unavailable for idle maintenance");
    try{return fn();}
    catch(...) {
      if(!session.poisoned())throw;
      try{return {HostedIdleMaintenanceState::recovered,session.recover_volume()};}
      catch(...){offline.store(true,std::memory_order_release);throw;}
    }
  }
  template<class F>NTSTATUS invoke(F&& fn,bool void_call=false) noexcept {
    const ForegroundCall active(*this);
    NativeWindowsIoOutcome result{STATUS_IO_DEVICE_ERROR,true};
    try {
      std::unique_lock guard(control);
      if(deferred)deferred->drain_locked(guard);
      if(offline.load(std::memory_order_acquire))result=win_detail::failure(session,STATUS_IO_DEVICE_ERROR,true);
      else {
        result=win_detail::boundary(session,std::forward<F>(fn));
        if(void_call&&!NT_SUCCESS(result.status))result=win_detail::failure(session,result.status,true);
      }
    }catch(...){result={STATUS_IO_DEVICE_ERROR,true};}
    if(result.offline_required)offline.store(true,std::memory_order_release);
    return result.status;
  }
  template<class F>static NTSTATUS run(FSP_FILE_SYSTEM* fs,F&& fn,bool void_call=false) noexcept {
    if(!fs||!fs->UserContext)return STATUS_INVALID_PARAMETER;
    auto& self=*static_cast<Impl*>(fs->UserContext);
    return self.invoke([&]{return fn(self);},void_call);
  }
  Slot& available() {
    if(next_token==(std::numeric_limits<std::uintptr_t>::max)())throw NativeSessionError(NativeSessionErrorCode::limit,"adapter token budget");
    for(auto& slot:slots)if(!slot.token)return slot;
    throw NativeSessionError(NativeSessionErrorCode::limit,"adapter context budget");
  }
  Slot& resolve(PVOID context) {
    const auto token=reinterpret_cast<std::uintptr_t>(context);
    if(token)for(auto& slot:slots)if(slot.token==token)return slot;
    throw NativeSessionError(NativeSessionErrorCode::invalid_handle,"stale adapter context");
  }
  Slot& adopt(PVOID context) {
    auto& slot=resolve(context);
    if(slot.provisional) {
      session.adopt_prepared(slot.handle);slot.provisional=false;
    }
    return slot;
  }
  void install(Slot& slot,const NativeFileHandle& handle,bool overwrite,UINT32 access) noexcept {
    slot={next_token,handle,true,overwrite,access&(READ_CONTROL|WRITE_DAC|WRITE_OWNER|ACCESS_SYSTEM_SECURITY)};++next_token;
  }
  static NTSTATUS security_copy(const std::vector<std::byte>& value,PSECURITY_DESCRIPTOR destination,SIZE_T* size) noexcept {
    if(!size)return destination?STATUS_INVALID_PARAMETER:STATUS_SUCCESS;
    const auto capacity=*size;*size=value.size();
    if(!destination||capacity<value.size())return STATUS_BUFFER_OVERFLOW;
    std::memcpy(destination,value.data(),value.size());return STATUS_SUCCESS;
  }
  static NTSTATUS volume(FSP_FILE_SYSTEM* fs,FSP_FSCTL_VOLUME_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      std::wstring_view label;
      if(s.info_policy==indexed_file_info) {
        const auto space=s.session.indexed_volume_space();
        info->TotalSize=space.total_bytes;info->FreeSize=space.free_bytes;
        label=L"FlitzFS";
      }else {
        const auto usage=s.session.allocation_usage();
        if(usage.charged_extents>kNativeMaxExtents)
          return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
        info->TotalSize=kNativeAllocationCapacity;
        info->FreeSize=(kNativeMaxExtents-usage.charged_extents)*std::uint64_t(kNativeExtentBytes);
        label=L"FlitzFS native quota";
      }
      const auto label_bytes=label.size()*sizeof(wchar_t);
      info->VolumeLabelLength=static_cast<UINT16>(label_bytes);
      std::memcpy(info->VolumeLabel,label.data(),label_bytes);
      return success();
    });
  }
  static NTSTATUS security_by_name(FSP_FILE_SYSTEM* fs,PWSTR name,PUINT32 attrs,PSECURITY_DESCRIPTOR value,SIZE_T* size) noexcept {
    if(attrs)*attrs=0;
    return run(fs,[&](Impl& s) {
      const auto found=s.session.lookup(path(name));
      if(found.state.info.delete_pending)return win_detail::failure(s.session,STATUS_DELETE_PENDING);
      if(!found.state.info.basic||!found.security)return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      if(attrs)*attrs=found.state.info.basic->attributes;
      return NativeWindowsIoOutcome{security_copy(*found.security,value,size),false};
    });
  }
  static NativeFileAccess grants(UINT32 access) noexcept {
    return {(access&FILE_READ_DATA)!=0,(access&(FILE_WRITE_DATA|FILE_APPEND_DATA))!=0,(access&DELETE)!=0};
  }
  static NativeBasicAccess basic_grant(UINT32 access) noexcept{return {(access&FILE_WRITE_ATTRIBUTES)!=0};}
  static NativeSecurityAccess security_grant(UINT32 access) noexcept{return {(access&(READ_CONTROL|ACCESS_SYSTEM_SECURITY))!=0,(access&(WRITE_DAC|ACCESS_SYSTEM_SECURITY))!=0};}
  static NTSTATUS admit_open(FSP_FILE_SYSTEM* fs) noexcept {
    if(!fs||!fs->UserContext)return STATUS_INVALID_PARAMETER;
    return static_cast<Impl*>(fs->UserContext)->origin.allows_open()?STATUS_SUCCESS:STATUS_ACCESS_DENIED;
  }
  static NTSTATUS create(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 access,UINT32 attrs,
      PSECURITY_DESCRIPTOR security,UINT64 allocation,PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(context)*context=nullptr;if(info)*info={};if(!context||!info)return STATUS_INVALID_PARAMETER;
    const auto origin=admit_open(fs);if(!NT_SUCCESS(origin))return origin;
    return run(fs,[&](Impl& s) {
      auto& slot=s.available();const bool directory=(options&FILE_DIRECTORY_FILE)!=0;
      if((options&FILE_DIRECTORY_FILE)&&(options&FILE_NON_DIRECTORY_FILE))return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      if(!directory && (allocation%kNativeAllocationUnit || allocation>native_windows_allocation_limit(s.info_policy)))
        return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      const auto stamp=native_cleanup_utc();const auto data=descriptor(security);
      const NativeBasicMetadata basic{stamp,stamp,stamp,stamp,attributes(attrs,directory)|(directory?0U:FILE_ATTRIBUTE_ARCHIVE)};
      NativePreparedCreateOptions initial;initial.access=grants(access);initial.basic_access=basic_grant(access);
      initial.security_access=security_grant(access);initial.initial_delete={(options&FILE_DELETE_ON_CLOSE)!=0};
      auto made=s.session.create_prepared_storage(path(name),directory,basic,data,
          directory?std::optional<std::uint64_t>{}:std::optional<std::uint64_t>{allocation},initial);
      s.install(slot,made.handle,false,access);
      if(!encode_native_file_info(made.state,s.info_policy,*info))return win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
      *context=reinterpret_cast<PVOID>(slot.token);return success();
    });
  }
  static NTSTATUS open(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 access,PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(context)*context=nullptr;if(info)*info={};if(!context||!info)return STATUS_INVALID_PARAMETER;
    const auto origin=admit_open(fs);if(!NT_SUCCESS(origin))return origin;
    return run(fs,[&](Impl& s) {
      auto& slot=s.available();const auto relative=path(name);const auto found=s.session.lookup(relative);
      if((options&FILE_DIRECTORY_FILE)&&!found.state.info.directory)return win_detail::failure(s.session,STATUS_NOT_A_DIRECTORY);
      if((options&FILE_NON_DIRECTORY_FILE)&&found.state.info.directory)return win_detail::failure(s.session,STATUS_FILE_IS_A_DIRECTORY);
      FSP_FSCTL_FILE_INFO prepared{};
      if(!encode_native_file_info(found.state,s.info_policy,prepared)||!found.security)
        return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      const auto disposition=options>>24;
      const bool overwrite=!found.state.info.directory&&(disposition==0||disposition==4||disposition==5);
      const NativeInitialDeleteAccess initial{(options&FILE_DELETE_ON_CLOSE)!=0};
      NativeFileHandle handle;
      if(overwrite)handle=s.session.prepare_overwrite(relative,grants(access),basic_grant(access),security_grant(access),
          {disposition!=0,disposition==0},initial);
      else handle=s.session.prepare_open(relative,grants(access),found.state.info.directory?NativeOpenKind::directory:NativeOpenKind::file,
          basic_grant(access),security_grant(access),initial);
      s.install(slot,handle,overwrite,access);*info=prepared;*context=reinterpret_cast<PVOID>(slot.token);return success();
    });
  }
  static NTSTATUS overwrite(FSP_FILE_SYSTEM* fs,PVOID context,UINT32 attrs,BOOLEAN replace,UINT64 allocation,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.resolve(context);
      if(!slot.provisional||!slot.awaiting_overwrite)return win_detail::failure(s.session,STATUS_INVALID_DEVICE_STATE);
      if(allocation%kNativeAllocationUnit || allocation>native_windows_allocation_limit(s.info_policy))
        return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      const NativeOverwriteMetadata effect{native_cleanup_utc(),attributes(attrs,false),replace!=FALSE};
      auto made=s.session.overwrite_prepared_storage_result(slot.handle,effect,allocation);
      slot.awaiting_overwrite=false;
      if(!encode_native_file_info(made.state,s.info_policy,*info))return win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
      return success();
    });
  }
  static VOID cleanup(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,ULONG flags) noexcept {
    (void)run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);
      const auto outcome=dispatch_native_allocated_cleanup(s.session,slot.handle,flags);
      return outcome==NativeCleanupOutcome::applied?success():win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
    },true);
  }
  static VOID close(FSP_FILE_SYSTEM* fs,PVOID context) noexcept {
    (void)run(fs,[&](Impl& s) {
      auto& slot=s.resolve(context);
      if(dispatch_native_close(s.session,slot.handle)!=NativeCloseOutcome::retired)
        return win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
      slot={};return success();
    },true);
  }
  static NTSTATUS get_info(FSP_FILE_SYSTEM* fs,PVOID context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s){return dispatch_native_file_info(s.session,s.resolve(context).handle,s.info_policy,info);});
  }
  static NTSTATUS read(FSP_FILE_SYSTEM* fs,PVOID context,PVOID buffer,UINT64 offset,ULONG length,PULONG count) noexcept {
    if(count)*count=0;if(!count||(!buffer&&length)||length>kNativeMaxReadBytes)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);const auto bytes=s.session.read(slot.handle,offset,length);
      if(bytes.empty()&&length)return win_detail::failure(s.session,STATUS_END_OF_FILE);
      if(!bytes.empty())std::memcpy(buffer,bytes.data(),bytes.size());*count=static_cast<ULONG>(bytes.size());return success();
    });
  }
  static NTSTATUS write(FSP_FILE_SYSTEM* fs,PVOID context,PVOID buffer,UINT64 offset,ULONG length,
      BOOLEAN append,BOOLEAN constrained,PULONG count,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(count)*count=0;if(info)*info={};
    if(!count||!info||(!buffer&&length)||length>kStoreMaxTransactionBytes)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      if(length>native_windows_write_limit(s.info_policy))return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      auto& slot=s.adopt(context);
      if(s.deferred && s.deferred->enqueue_locked(fs,slot.handle,
          {static_cast<const std::byte*>(buffer),length},offset,append!=FALSE,constrained!=FALSE))
        return NativeWindowsIoOutcome{STATUS_PENDING,false};
      return dispatch_native_write(s.session,slot.handle,buffer,offset,length,append!=FALSE,constrained!=FALSE,{},s.info_policy,count,info);
    });
  }
  static NTSTATUS size(FSP_FILE_SYSTEM* fs,PVOID context,UINT64 bytes,BOOLEAN allocation,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);return dispatch_native_set_size(s.session,slot.handle,bytes,allocation!=FALSE,{},s.info_policy,info);
    });
  }
  static NTSTATUS flush(FSP_FILE_SYSTEM* fs,PVOID context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      if(!context)return dispatch_native_flush(s.session,nullptr,s.info_policy,info);
      auto& slot=s.adopt(context);return dispatch_native_flush(s.session,&slot.handle,s.info_policy,info);
    });
  }
  static NTSTATUS set_delete(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,BOOLEAN deleting) noexcept {
    return run(fs,[&](Impl& s){auto& slot=s.adopt(context);s.session.set_delete(slot.handle,deleting!=FALSE);return success();});
  }
  static NTSTATUS basic(FSP_FILE_SYSTEM* fs,PVOID context,UINT32 attrs,UINT64 creation,UINT64 access,
      UINT64 write_time,UINT64 change,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);auto state=s.session.allocation_info(slot.handle);
      if(!state.info.basic)return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      auto value=*state.info.basic;
      if(attrs!=INVALID_FILE_ATTRIBUTES)value.attributes=attributes(attrs,state.info.directory);
      if(creation)value.creation_time=creation;if(access)value.last_access_time=access;
      if(write_time)value.last_write_time=write_time;if(change)value.change_time=change;
      state.info.basic=value;FSP_FSCTL_FILE_INFO prepared{};
      if(!encode_native_file_info(state,s.info_policy,prepared))return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      s.session.set_basic_metadata(slot.handle,value);*info=prepared;return success();
    });
  }
  static NTSTATUS rename(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,PWSTR destination,BOOLEAN replace) noexcept {
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);const auto target=path(destination);
      if(replace)try {
        const auto found=s.session.lookup(target);
        if(found.state.info.directory&&found.state.info.inode!=s.session.info(slot.handle).info.inode)
          return win_detail::failure(s.session,STATUS_ACCESS_DENIED);
      }catch(const NativeFsError& e){if(e.code()!=NativeFsErrorCode::not_found)throw;}
      s.session.rename(slot.handle,target,replace!=FALSE);return success();
    });
  }
  static NTSTATUS get_security(FSP_FILE_SYSTEM* fs,PVOID context,PSECURITY_DESCRIPTOR value,SIZE_T* size) noexcept {
    return run(fs,[&](Impl& s) {
      const auto security=s.session.security_descriptor(s.resolve(context).handle);
      if(!security)return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      return NativeWindowsIoOutcome{security_copy(*security,value,size),false};
    });
  }
  static NTSTATUS set_security(FSP_FILE_SYSTEM* fs,PVOID context,SECURITY_INFORMATION parts,
      PSECURITY_DESCRIPTOR modification) noexcept {
    return run(fs,[&](Impl& s) {
      // Kernel-originated granted access is captured at Create/Open. A partial
      // Windows right NEVER becomes an unrestricted public replacement API.
      constexpr SECURITY_INFORMATION dacl_flags=PROTECTED_DACL_SECURITY_INFORMATION|UNPROTECTED_DACL_SECURITY_INFORMATION;
      constexpr SECURITY_INFORMATION sacl_flags=PROTECTED_SACL_SECURITY_INFORMATION|UNPROTECTED_SACL_SECURITY_INFORMATION;
      constexpr SECURITY_INFORMATION supported=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|
          DACL_SECURITY_INFORMATION|SACL_SECURITY_INFORMATION|dacl_flags|sacl_flags;
      if(!parts||(parts&~supported)||(parts&dacl_flags)==dacl_flags||(parts&sacl_flags)==sacl_flags||
          ((parts&dacl_flags)&&!(parts&DACL_SECURITY_INFORMATION))||
          ((parts&sacl_flags)&&!(parts&SACL_SECURITY_INFORMATION)))
        return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      auto& selected=s.resolve(context);const auto access=selected.security_access;
      if(((parts&(DACL_SECURITY_INFORMATION|dacl_flags))&&!(access&WRITE_DAC))||
          ((parts&(OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION))&&!(access&WRITE_OWNER))||
          ((parts&(SACL_SECURITY_INFORMATION|sacl_flags))&&!(access&ACCESS_SYSTEM_SECURITY)))
        return win_detail::failure(s.session,STATUS_ACCESS_DENIED);
      // The selected WinFsp SetSecurity frame has no requesting client token.
      // WRITE_OWNER is not proof that a supplied owner SID is assignable.
      // Never rely on the token-less merge helper to enforce that policy.
      // Reject the WHOLE mixed request before adoption, descriptor merge or I/O.
      if(parts&OWNER_SECURITY_INFORMATION)
        return win_detail::failure(s.session,STATUS_INVALID_OWNER);
      if(parts&GROUP_SECURITY_INFORMATION)
        return win_detail::failure(s.session,STATUS_INVALID_PRIMARY_GROUP);
      // Reject malformed/oversized trusted transport input before any I/O.
      const auto input=descriptor(modification);
      auto& slot=s.adopt(context);
      auto original=s.session.security_descriptor_for_update(slot.handle);
      if(!original)return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      struct Merged {
        PSECURITY_DESCRIPTOR value=nullptr;
        ~Merged(){if(value)FspDeleteSecurityDescriptor(value,(NTSTATUS (*)())FspSetSecurityDescriptor);}
      } merged;
      const auto status=FspSetSecurityDescriptor(original->data(),parts,
          const_cast<std::byte*>(input.data()),&merged.value);
      if(!NT_SUCCESS(status))return win_detail::failure(s.session,status);
      const auto replacement=descriptor(merged.value);
      s.session.set_security_descriptor(slot.handle,replacement);
      return success(); // No backing read, clock or allocation after publication.
    });
  }
  static NTSTATUS directory(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,PWSTR marker,PVOID buffer,ULONG length,PULONG count) noexcept {
    if(count)*count=0;if(!count||(!buffer&&length)||length>kNativeMaxReadBytes)return STATUS_INVALID_PARAMETER;
    const auto status=run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);const auto current=s.session.info(slot.handle);
      if(!current.info.directory)return win_detail::failure(s.session,STATUS_NOT_A_DIRECTORY);
      if(current.unlinked)return win_detail::failure(s.session,STATUS_DELETE_PENDING);
      std::string after;
      if(marker)for(auto c:bounded(marker,100)) {
        if(c<0x20||c>0x7e||c==L'\\'||c==L'/')return win_detail::failure(s.session,STATUS_OBJECT_NAME_INVALID);
        after.push_back(static_cast<char>(c));
      }
      auto append=[&](const NativeInodeAllocationInfo& state,std::string_view name) {
        alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,sizeof(FSP_FSCTL_DIR_INFO)+200> raw{};
        auto* item=reinterpret_cast<FSP_FSCTL_DIR_INFO*>(raw.data());
        item->Size=static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO)+name.size()*sizeof(wchar_t));
        if(!encode_native_file_info(state,s.info_policy,item->FileInfo))
          throw NativeFsError(NativeFsErrorCode::corrupt,"directory FileInfo prerequisites");
        for(std::size_t i=0;i<name.size();++i)item->FileNameBuf[i]=static_cast<wchar_t>(name[i]);
        return FspFileSystemAddDirInfo(item,buffer,length,count)!=FALSE;
      };
      const auto full=[&]{return win_detail::failure(s.session,*count?STATUS_SUCCESS:STATUS_BUFFER_OVERFLOW);};
      if(current.info.inode!=1) {
        if((!marker || directory_name_after(".",after)) && !append(s.session.allocation_info(slot.handle),"."))return full();
        if(!marker || directory_name_after("..",after)) {
          const auto slash=current.info.path.rfind('/');
          const auto parent=s.session.lookup(slash==std::string::npos?
              std::string_view{}:std::string_view(current.info.path).substr(0,slash));
          if(!append(parent.state,".."))return full();
        }
      }
      for(;;) {
        const auto remaining=static_cast<std::size_t>(length-*count);
        const auto maximum=(std::min)(std::size_t{32},remaining/sizeof(FSP_FSCTL_DIR_INFO)+1);
        auto page=s.session.list_page(slot.handle,after,maximum);
        for(const auto& entry:page.entries)if(!append(entry.state,entry.name))return full();
        if(!page.more)break;
        after=std::move(page.entries.back().name);
      }
      if(!FspFileSystemAddDirInfo(nullptr,buffer,length,count))return full();return success();
    });
    if(!NT_SUCCESS(status))*count=0;return status;
  }
};
WinFspNativeWritable::WinFspNativeWritable(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)){}
WinFspNativeWritable::~WinFspNativeWritable()=default;
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_new_hosted(
    const std::filesystem::path& file,StoreIdentity id,StoreCompressionOptions compression) {
  return create_new_hosted(file,id,compression,kStoreMaxPhysicalBytes);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_new_hosted(
    const std::filesystem::path& file,StoreIdentity id,StoreCompressionOptions compression,std::uint64_t cap) {

  if(cap<4U*1024*1024||cap>kStoreMaxPhysicalBytes||cap%65536)
    throw std::invalid_argument("native image cap must be aligned and 4..64 MiB");
  if(std::all_of(id.begin(),id.end(),[](auto b){return b==std::byte{0};}))
    throw std::invalid_argument("native image identity must be nonzero");
  detail::validate_store_compression(compression);
  const auto security=owner_security();const auto stamp=native_cleanup_utc();
  auto io=std::make_shared<CappedFile>(detail::new_store_file(file,{65536,true}),cap);
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(std::move(io),id,compression));
  const NativeBasicMutation basic{1,0,{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY}};
  const NativeSecurityMutation descriptor{1,0,security};
  writer.apply_with_metadata({},{&basic,1},{&descriptor,1});
  auto session=NativeFileSession::create(std::move(writer),NativeDispositionPolicy::windows_global);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(std::make_unique<Impl>(std::move(session),id,stamp)));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_retired_hosted(
    const std::filesystem::path& file,StoreIdentity id,std::uint64_t generation,
    StoreCompressionOptions compression,std::uint64_t cap) {

  if(cap<4U*1024*1024||cap>kStoreMaxPhysicalBytes||cap%65536||!generation||
      std::all_of(id.begin(),id.end(),[](auto b){return b==std::byte{0};}))
    throw std::invalid_argument("invalid hosted restart lease");
  detail::validate_store_compression(compression);
  // OS-exclusive existing-file lease, single-link/regular identity and all
  // original headroom/barrier checks remain in the owned-fixture backend.
  auto io=std::make_shared<CappedFile>(detail::reopen_store_file_for_hosted_test(file,{65536,true}),cap);
  UINT64 created=0;
  {
    auto objects=ObjectStoreSnapshot::open(io);
    if(objects.identity()!=id||objects.info().generation!=generation)
      throw std::runtime_error("hosted restart identity/generation mismatch");
    auto view=NativeFilesystemSnapshot::open(std::move(objects));view.verify_all();
    // Admit the complete Windows profile BEFORE resume can repair a root.
    created=admit_windows_snapshot(view);
  }
  auto objects=ObjectStoreWriter::resume(io,id,generation,compression);
  auto writer=NativeFilesystemWriter::resume(std::move(objects)); // Never fabricate old Cleanup.
  auto session=NativeFileSession::create(std::move(writer),NativeDispositionPolicy::windows_global);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(
      std::make_unique<Impl>(std::move(session),id,created)));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_managed_hosted(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,
    std::shared_ptr<StoreByteIo> image,StoreIdentity image_id,StoreCompressionOptions foreground) {

  const auto security=owner_security();const auto stamp=native_cleanup_utc();
  const NativeVolumeRootMetadata root{{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY},security};
  auto session=NativeFileSession::create_volume(std::move(head),volume,std::move(image),image_id,
      foreground,{},NativeDispositionPolicy::windows_global,root);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(
      std::make_unique<Impl>(std::move(session),volume,stamp)));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_managed_paged(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,std::shared_ptr<StoreByteIo> image,
    StoreIdentity image_id,PagedStoreLimits limits,StoreCompressionOptions foreground,
    std::shared_ptr<StoreByteIo> advice) {
  const auto security=owner_security();const auto stamp=native_cleanup_utc();
  const NativeVolumeRootMetadata root{{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY},security};
  auto session=NativeFileSession::create_paged_volume(std::move(head),volume,std::move(image),image_id,
      limits,foreground,NativeDispositionPolicy::windows_global,root);
  session.enable_workload_accounting();
  if(advice)session.create_budgeted_optimization_area(std::move(advice),1);
  auto impl=std::make_unique<Impl>(std::move(session),volume,stamp);
  impl->groups=std::make_unique<NativeGroupMaintenance>(impl->session);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(std::move(impl)));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_managed_indexed(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,std::shared_ptr<StoreByteIo> image,
    StoreIdentity image_id,PagedStoreLimits limits,StoreCompressionOptions foreground,
    std::shared_ptr<StoreByteIo> advice,NativeMaintenancePolicy group_policy) {
  const auto security=owner_security();const auto stamp=native_cleanup_utc();
  const NativeVolumeRootMetadata root{{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY},security};
  auto session=NativeFileSession::create_indexed_volume(std::move(head),volume,std::move(image),image_id,
      limits,foreground,NativeDispositionPolicy::windows_global,root);
  if(advice)session.create_budgeted_optimization_area(std::move(advice),1);
  auto impl=std::make_unique<Impl>(std::move(session),volume,stamp,indexed_file_info);
  impl->groups=std::make_unique<NativeGroupMaintenance>(impl->session,group_policy);
  impl->group_policy=group_policy;
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(std::move(impl)));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_file_volume(std::shared_ptr<StoreByteIo> empty,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground) {
  auto regions=NativeVolumeFile::new_regions(std::move(empty),limits);
  return create_managed_indexed(std::move(regions.head),volume,std::move(regions.image),image,
      limits,foreground,std::move(regions.advice));
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_file_volume(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground) {
  auto regions=NativeVolumeFile::open_regions(std::move(existing),true,expected);
  const auto& selected=regions.selection;
  const auto sequence=regions.advice_sequence();
  return resume_indexed_retired(std::move(regions.head),selected.volume_identity,selected.epoch,
      regions.resolver(),foreground,sequence?std::move(regions.advice):nullptr,sequence);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::create_file_volume(const std::filesystem::path& path,
    StoreIoContract contract,StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground) {
  return create_file_volume(NativeVolumeFile::create_backing(path,contract,limits),volume,image,limits,foreground);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_file_volume(const std::filesystem::path& path,
    StoreIoContract contract,std::optional<StoreIdentity> expected,StoreCompressionOptions foreground) {
  return resume_file_volume(NativeVolumeFile::reopen_backing(path,contract),expected,foreground);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_indexed_retired(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,std::uint64_t epoch,
    const NativeVolumeImageResolver& resolver,StoreCompressionOptions foreground,
    std::shared_ptr<StoreByteIo> advice,std::uint64_t advice_sequence) {
  return resume_managed_impl(std::move(head),volume,epoch,resolver,foreground,std::move(advice),advice_sequence,true);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_managed_retired(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,std::uint64_t epoch,
    const NativeVolumeImageResolver& resolver,StoreCompressionOptions foreground,
    std::shared_ptr<StoreByteIo> advice,std::uint64_t advice_sequence) {
  return resume_managed_impl(std::move(head),volume,epoch,resolver,foreground,std::move(advice),advice_sequence,false);
}
std::unique_ptr<WinFspNativeWritable> WinFspNativeWritable::resume_managed_impl(
    std::shared_ptr<StoreByteIo> head,StoreIdentity volume,std::optional<std::uint64_t> expected_epoch,
    const NativeVolumeImageResolver& resolver,StoreCompressionOptions foreground,
    std::shared_ptr<StoreByteIo> advice,std::uint64_t advice_sequence,bool closed_indexed) {
  if(bool(advice)!=bool(advice_sequence))throw std::invalid_argument("exact advisory sequence required");
  std::shared_ptr<StoreByteIo> selected;StoreIdentity selected_id{};
  const NativeVolumeImageResolver pinned=[&](const StoreIdentity& id){
    if(!selected){selected=resolver(id);selected_id=id;}
    else if(id!=selected_id)throw std::invalid_argument("restart image mapping changed");
    return selected;
  };
  const auto view=closed_indexed?NativeVolumeHeadView::open_indexed_recovery(head,volume,pinned):
      NativeVolumeHeadView::open(head,volume,pinned);
  const auto epoch=view.info().selection.epoch;
  if(expected_epoch && *expected_epoch!=epoch)throw std::invalid_argument("exact head epoch required");
  const auto created=admit_windows_snapshot(view.filesystem(),closed_indexed);
  const auto kind=view.filesystem().image_storage().kind;
  const bool grouped=kind==NativeImageKind::indexed_paged || (kind==NativeImageKind::paged && view.filesystem().shared_base_info().enabled);
  constexpr auto disposition=NativeDispositionPolicy::windows_global;
  auto session=closed_indexed?
      NativeFileSession::resume_closed_indexed_volume(std::move(head),volume,epoch,pinned,foreground,disposition):
      NativeFileSession::resume_volume(std::move(head),volume,epoch,pinned,foreground,disposition);
  if(advice)session.resume_budgeted_optimization_area(std::move(advice),advice_sequence,1);
  auto impl=std::make_unique<Impl>(std::move(session),volume,created,file_info_policy(kind));
  if(grouped)
    impl->groups=std::make_unique<NativeGroupMaintenance>(impl->session);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(std::move(impl)));
}
void WinFspNativeWritable::configure_memory_maintenance(MemoryVolumeBacking& backing) noexcept {
  impl_->memory_backing=&backing;
  backing.configure_coherence(impl_->session);
}
bool WinFspNativeWritable::background_maintenance_pending() const {
  std::unique_lock guard(impl_->control,std::try_to_lock);
  // A busy control is not proof of quiescence. This cached query performs no
  // storage I/O and also sees publications by the deferred service thread.
  if(!guard)return true;
  if(impl_->maintenance_cancelled || requires_offline() || dispatcher_stopped() || impl_->session.poisoned())
    return false;
  return impl_->session.retirement_pending() ||
      (impl_->memory_backing && (impl_->memory_backing->coherence_retirement_pending() ||
      impl_->memory_backing->compaction_pending(impl_->session))) ||
      (impl_->groups && (impl_->groups->pending() ||
      (impl_->deferred && (impl_->deferred->publication_pending_locked() || impl_->deferred->stats_locked().worker_busy))));
}
void WinFspNativeWritable::set_maintenance_event(HANDLE event) {
  std::lock_guard guard(impl_->control);
  if(!event || impl_->callbacks_bound || impl_->maintenance_event.value)
    throw std::invalid_argument("Maintenance event requires an unbound adapter and one valid event");
  if(!DuplicateHandle(GetCurrentProcess(),event,GetCurrentProcess(),&impl_->maintenance_event.value,
      EVENT_MODIFY_STATE,FALSE,0))throw std::runtime_error("Maintenance event duplication failed");
  if(impl_->deferred)impl_->deferred->bind_activity_event_locked(impl_->maintenance_event.value);
}
std::optional<NativeGpuBudgetTask> WinFspNativeWritable::gpu_budget_task() const {
  if(!impl_->deferred || !impl_->deferred->budget_demand_pending())return {};
  std::lock_guard guard(impl_->control);
  return impl_->deferred->budget_task_locked();
}
bool WinFspNativeWritable::complete_gpu_budget_task(const NativeGpuBudgetTask& task,
    const GpuMemorySnapshot& snapshot,std::uint64_t now) {
  std::lock_guard guard(impl_->control);
  return impl_->deferred && impl_->deferred->accept_budget_locked(task,snapshot,now);
}
bool WinFspNativeWritable::maintenance_event_failed() const noexcept {
  return impl_->maintenance_signal_failed.load(std::memory_order_acquire);
}
std::uint64_t WinFspNativeWritable::foreground_activity() const noexcept {
  return impl_->foreground_epoch.load(std::memory_order_acquire);
}
bool WinFspNativeWritable::foreground_pending() const noexcept {
  return impl_->foreground.load(std::memory_order_acquire)!=0;
}
void WinFspNativeWritable::signal_foreground_for_anchor(bool pending) noexcept {
  impl_->anchor_pending.store(pending,std::memory_order_release);
}
NativeMaintenancePolicy WinFspNativeWritable::group_maintenance_policy() const noexcept {
  return impl_->group_policy;
}
bool WinFspNativeWritable::supports_anchor_search() const noexcept {
  return impl_->info_policy==indexed_file_info;
}
void WinFspNativeWritable::cancel_background_maintenance() {
  std::lock_guard guard(impl_->control);
  impl_->maintenance_cancelled=true;
  impl_->groups.reset();
  if(impl_->memory_backing)impl_->memory_backing->cancel_compaction(impl_->session);
}
WinFspMaintenanceResult WinFspNativeWritable::advance_background_maintenance(
    const NativeMaintenanceObservation& observed,std::uint64_t now,NativeGroupSearchOpportunity search) {
  std::unique_lock guard(impl_->control,std::try_to_lock);
  if(!guard || impl_->foreground.load(std::memory_order_acquire)!=0 ||
      (impl_->deferred&&(impl_->deferred->publication_pending_locked() || impl_->deferred->stats_locked().worker_busy)))
    return {WinFspMaintenanceState::busy};
  if((impl_->deferred && !impl_->deferred->stats_locked().retirement_confirmed) ||
      impl_->maintenance_cancelled || requires_offline() || dispatcher_stopped() || impl_->session.poisoned()) {
    impl_->maintenance_cancelled=true;
    impl_->groups.reset();
    return {WinFspMaintenanceState::unavailable};
  }
  if(impl_->info_policy==indexed_file_info) {
    // An admitted group pins its source image. Complete or retire it before
    // a compaction handoff. Detached-inode reclamation can still invalidate it.
    if(!impl_->session.retirement_pending() && impl_->groups && impl_->groups->active_group())
      return impl_->advance_groups(observed,now,search);
    auto storage=impl_->advance_indexed_maintenance(observed,now);
    if(storage.state!=WinFspMaintenanceState::unchanged &&
        (storage.progress.deferral!=NativeMaintenanceDeferral::other_maintenance || !impl_->groups || !impl_->groups->pending())) {
      if(impl_->groups) {
        auto progress=impl_->groups->progress();
        progress.phase=storage.progress.phase;progress.deferral=storage.progress.deferral;
        progress.next_opportunity_delay_ms=storage.progress.next_opportunity_delay_ms;
        storage.progress=progress;
      }
      return storage;
    }
  }
  return impl_->advance_groups(observed,now,search);
}
HostedVolumeMaintenanceResult WinFspNativeWritable::recompress_managed_hosted(
    std::shared_ptr<StoreByteIo> target,StoreIdentity next,StoreCompressionOptions candidate) {

  std::lock_guard guard(impl_->control);
  if(requires_offline() || dispatcher_stopped() || (impl_->deferred&&impl_->deferred->publication_pending_locked()))
    throw std::runtime_error("mounted session is unavailable for maintenance");
  try {
    return {impl_->session.recompress_volume(std::move(target),next,candidate),false};
  }catch(...) {
    if(!impl_->session.poisoned())throw;
    // The callback guard stays held. No callback sees an unverified image or
    // loses its open context while the one explicit CPU recovery is attempted.
    try{return {impl_->session.recover_volume(),true};}
    catch(...){impl_->offline.store(true,std::memory_order_release);throw;}
  }
}
HostedIdleMaintenanceResult WinFspNativeWritable::begin_idle_recompression_hosted(
    std::shared_ptr<StoreByteIo> target,StoreIdentity next,StoreCompressionOptions candidate) {

  return impl_->idle([&]{
    return HostedIdleMaintenanceResult{impl_->session.begin_idle_recompression(std::move(target),next,candidate)?
        HostedIdleMaintenanceState::begun:HostedIdleMaintenanceState::suppressed};
  });
}
HostedIdleMaintenanceResult WinFspNativeWritable::idle_recompression_hosted(HostedIdleMaintenanceAction action) {

  return impl_->idle([&]() -> HostedIdleMaintenanceResult {
    using State=HostedIdleMaintenanceState;
    using Progress=NativeFileSession::RecompressionProgress;
    switch(action) {
      case HostedIdleMaintenanceAction::copy:
        switch(impl_->session.advance_recompression()) {
          case Progress::copying:return {State::copying};
          case Progress::ready:return {State::ready};
          case Progress::stale:return {State::stale};
        }
        break;
      case HostedIdleMaintenanceAction::publish:
        if(auto next=impl_->session.publish_idle_recompression())return {State::published,*next};
        return {State::unchanged};
    }
    throw std::invalid_argument("unknown idle maintenance action");
  });
}
void WinFspNativeWritable::enable_deferred_writes_hosted(NativeDeferredWriteOptions options){
  std::lock_guard guard(impl_->control);
  if(impl_->callbacks_bound||impl_->deferred||impl_->foreground.load(std::memory_order_acquire))
    throw std::runtime_error("deferred writes require an unbound, unconfigured adapter");
  if(!options.hint_query)options.hint_query=sdk_hint;
  if(!options.response_sink)options.response_sink=sdk_response;
  impl_->deferred=std::make_unique<NativeDeferredWriteService>(impl_->session,impl_->control,
      impl_->offline,std::move(options),impl_->info_policy);
  impl_->deferred->bind_activity_event_locked(impl_->maintenance_event.value);
}
void WinFspNativeWritable::publish_deferred_health_hosted(const GpuHealthEvidence& health){
  std::lock_guard guard(impl_->control);
  if(!impl_->deferred)throw std::runtime_error("deferred writes are not configured");
  impl_->deferred->publish_locked(health);
}
NativeDeferredWriteStats WinFspNativeWritable::deferred_write_stats() const{
  std::lock_guard guard(impl_->control);
  return impl_->deferred?impl_->deferred->stats_locked():NativeDeferredWriteStats{};
}
std::optional<NativeUniformWriteResult> WinFspNativeWritable::deferred_write_receipt() const {
  std::lock_guard guard(impl_->control);
  return impl_->deferred?impl_->deferred->completion_receipt_locked():std::nullopt;
}
NativeDeferredStop WinFspNativeWritable::stop_deferred_writes_hosted(){
  // Owned-resource cleanup must not depend on mutable environment flags.
  return impl_->deferred?impl_->deferred->stop():NativeDeferredStop::quiescent;
}
void WinFspNativeWritable::bind_callbacks(FSP_FILE_SYSTEM& fs,WinFspOriginQuery query) noexcept{
  impl_->origin.bind(query);impl_->callbacks_bound=true;fs.UserContext=impl_.get();
}
bool WinFspNativeWritable::self_origin_rejected() const noexcept{return impl_->origin.self_rejected();}
bool WinFspNativeWritable::requires_offline() const noexcept{return impl_->offline.load(std::memory_order_acquire);}
bool WinFspNativeWritable::dispatcher_stopped() const noexcept{return impl_->stopped.load(std::memory_order_acquire)!=0;}
bool WinFspNativeWritable::dispatcher_stopped_unexpectedly() const noexcept{return impl_->stopped.load(std::memory_order_acquire)==2;}
NativeSessionStats WinFspNativeWritable::stats() const{std::lock_guard guard(impl_->control);return impl_->session.stats();}
NativeFilesystemSnapshot WinFspNativeWritable::snapshot() const{std::lock_guard guard(impl_->control);return impl_->session.snapshot();}
FSP_FSCTL_VOLUME_PARAMS WinFspNativeWritable::parameters() const noexcept {
  FSP_FSCTL_VOLUME_PARAMS p{};p.Version=sizeof(p);p.SectorSize=512;p.SectorsPerAllocationUnit=8;
  p.MaxComponentLength=100;p.VolumeCreationTime=impl_->created;p.VolumeSerialNumber=impl_->serial;p.IrpCapacity=100;
  p.FileInfoTimeout=0;p.PostCleanupWhenModifiedOnly=0;p.PostDispositionWhenNecessaryOnly=0;
  p.FlushAndPurgeOnCleanup=1;p.SupportsPosixUnlinkRename=0;p.CasePreservedNames=1;p.PersistentAcls=1;
  p.UmFileContextIsUserContext2=1;p.ReadOnlyVolume=0;
  constexpr wchar_t name[]=L"FlitzFS";std::memcpy(p.FileSystemName,name,sizeof(name));return p;
}
const FSP_FILE_SYSTEM_INTERFACE& WinFspNativeWritable::callbacks() noexcept {
  static const auto table=[] {
    FSP_FILE_SYSTEM_INTERFACE t{};t.GetVolumeInfo=Impl::volume;t.GetSecurityByName=Impl::security_by_name;
    t.Create=Impl::create;t.Open=Impl::open;t.Overwrite=Impl::overwrite;t.Cleanup=Impl::cleanup;t.Close=Impl::close;
    t.Read=Impl::read;t.Write=Impl::write;t.GetFileInfo=Impl::get_info;t.SetFileSize=Impl::size;t.Flush=Impl::flush;
    t.SetDelete=Impl::set_delete;t.SetBasicInfo=Impl::basic;t.Rename=Impl::rename;t.GetSecurity=Impl::get_security;
    t.ReadDirectory=Impl::directory;
    t.DispatcherStopped=[](FSP_FILE_SYSTEM* fs,BOOLEAN normal) {
      if(fs&&fs->UserContext){auto& self=*static_cast<Impl*>(fs->UserContext);unsigned expected=0;
        self.stopped.compare_exchange_strong(expected,normal?1U:2U,std::memory_order_acq_rel);self.signal_maintenance();}
    };
    t.SetVolumeLabel=[](FSP_FILE_SYSTEM*,PWSTR,FSP_FSCTL_VOLUME_INFO*)->NTSTATUS{return STATUS_INVALID_DEVICE_REQUEST;};
    t.SetSecurity=Impl::set_security;
    return t;
  }();return table;
}
}
