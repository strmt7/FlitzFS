#include "winfsp_native.hpp"
#include "../store_file.hpp"
#include "../store_codec.hpp"
#include "native_cleanup.hpp"
#include "native_close.hpp"
#include "native_windows_boundary.hpp"
#include "flitzfs/directory_order.hpp"
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
constexpr auto cache_policy=NativeFileInfoAllocation::native_reserved_cache;
struct Handle {HANDLE value=nullptr;~Handle(){if(value)CloseHandle(value);}};
struct Local {void* value=nullptr;~Local(){if(value)LocalFree(value);}};
void hosted() {
  const auto* a=std::getenv("GITHUB_ACTIONS");const auto* e=std::getenv("RUNNER_ENVIRONMENT");
  if(!a||std::string_view(a)!="true"||!e||std::string_view(e)!="github-hosted")
    throw std::runtime_error("writable adapter factory is hosted-only pending qualification");
}
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
  NativeFileSession session;
  std::array<Slot,NativeFileSession::max_contexts> slots{};
  std::uintptr_t next_token=1;
  mutable std::mutex control;
  std::atomic<bool> offline{false};
  std::atomic<unsigned> stopped{0};
  StoreIdentity identity{};UINT64 created=0;UINT32 serial=0;
  Impl(NativeFileSession s,StoreIdentity id,UINT64 stamp):session(std::move(s)),identity(id),created(stamp) {
    static_assert(std::is_nothrow_copy_assignable_v<NativeFileHandle>);
    for(std::size_t i=0;i<id.size();++i)serial^=UINT32(std::to_integer<unsigned>(id[i]))<<((i%4)*8);
    if(!serial)serial=1;
  }
  template<class F>NTSTATUS invoke(F&& fn,bool void_call=false) noexcept {
    NativeWindowsIoOutcome result{STATUS_IO_DEVICE_ERROR,true};
    try {
      std::lock_guard guard(control);
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
      const auto usage=s.session.allocation_usage();
      if(usage.charged_extents>kNativeMaxExtents)return win_detail::failure(s.session,STATUS_FILE_CORRUPT_ERROR,true);
      info->TotalSize=kNativeAllocationCapacity;
      info->FreeSize=(kNativeMaxExtents-usage.charged_extents)*std::uint64_t(kNativeExtentBytes);
      constexpr wchar_t label[]=L"FlitzFS native quota";
      info->VolumeLabelLength=sizeof(label)-sizeof(wchar_t);std::memcpy(info->VolumeLabel,label,sizeof(label));
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
  static NTSTATUS create(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 access,UINT32 attrs,
      PSECURITY_DESCRIPTOR security,UINT64 allocation,PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(context)*context=nullptr;if(info)*info={};if(!context||!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.available();const bool directory=(options&FILE_DIRECTORY_FILE)!=0;
      if((options&FILE_DIRECTORY_FILE)&&(options&FILE_NON_DIRECTORY_FILE))return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
      const auto stamp=native_cleanup_utc();const auto data=descriptor(security);
      const NativeBasicMetadata basic{stamp,stamp,stamp,stamp,attributes(attrs,directory)|(directory?0U:FILE_ATTRIBUTE_ARCHIVE)};
      NativePreparedCreateOptions initial;initial.access=grants(access);initial.basic_access=basic_grant(access);
      initial.security_access=security_grant(access);initial.initial_delete={(options&FILE_DELETE_ON_CLOSE)!=0};
      auto made=s.session.create_prepared_storage(path(name),directory,basic,data,
          directory?std::optional<std::uint64_t>{}:std::optional<std::uint64_t>{allocation},initial);
      s.install(slot,made.handle,false,access);
      if(!encode_native_file_info(made.state,cache_policy,*info))return win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
      *context=reinterpret_cast<PVOID>(slot.token);return success();
    });
  }
  static NTSTATUS open(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 access,PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(context)*context=nullptr;if(info)*info={};if(!context||!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.available();const auto relative=path(name);const auto found=s.session.lookup(relative);
      if((options&FILE_DIRECTORY_FILE)&&!found.state.info.directory)return win_detail::failure(s.session,STATUS_NOT_A_DIRECTORY);
      if((options&FILE_NON_DIRECTORY_FILE)&&found.state.info.directory)return win_detail::failure(s.session,STATUS_FILE_IS_A_DIRECTORY);
      FSP_FSCTL_FILE_INFO prepared{};
      if(!encode_native_file_info(found.state,cache_policy,prepared)||!found.security)
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
      const NativeOverwriteMetadata effect{native_cleanup_utc(),attributes(attrs,false),replace!=FALSE};
      auto made=s.session.overwrite_prepared_storage_result(slot.handle,effect,allocation);
      slot.awaiting_overwrite=false;
      if(!encode_native_file_info(made.state,cache_policy,*info))return win_detail::failure(s.session,STATUS_IO_DEVICE_ERROR,true);
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
    return run(fs,[&](Impl& s){return dispatch_native_file_info(s.session,s.resolve(context).handle,cache_policy,info);});
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
    if(!count||!info||(!buffer&&length)||length>kStoreMaxObjectBytes)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);
      return dispatch_native_write(s.session,slot.handle,buffer,offset,length,append!=FALSE,constrained!=FALSE,{},cache_policy,count,info);
    });
  }
  static NTSTATUS size(FSP_FILE_SYSTEM* fs,PVOID context,UINT64 bytes,BOOLEAN allocation,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      auto& slot=s.adopt(context);return dispatch_native_set_size(s.session,slot.handle,bytes,allocation!=FALSE,{},cache_policy,info);
    });
  }
  static NTSTATUS flush(FSP_FILE_SYSTEM* fs,PVOID context,FSP_FSCTL_FILE_INFO* info) noexcept {
    if(info)*info={};if(!info)return STATUS_INVALID_PARAMETER;
    return run(fs,[&](Impl& s) {
      if(!context)return dispatch_native_flush(s.session,nullptr,cache_policy,info);
      auto& slot=s.adopt(context);return dispatch_native_flush(s.session,&slot.handle,cache_policy,info);
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
      if(!encode_native_file_info(state,cache_policy,prepared))return win_detail::failure(s.session,STATUS_INVALID_PARAMETER);
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
        if(marker&&!directory_name_after(name,after))return true;
        alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,sizeof(FSP_FSCTL_DIR_INFO)+200> raw{};
        auto* item=reinterpret_cast<FSP_FSCTL_DIR_INFO*>(raw.data());
        item->Size=static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO)+name.size()*sizeof(wchar_t));
        if(!encode_native_file_info(state,cache_policy,item->FileInfo))
          throw NativeFsError(NativeFsErrorCode::corrupt,"directory FileInfo prerequisites");
        for(std::size_t i=0;i<name.size();++i)item->FileNameBuf[i]=static_cast<wchar_t>(name[i]);
        return FspFileSystemAddDirInfo(item,buffer,length,count)!=FALSE;
      };
      const auto full=[&]{return win_detail::failure(s.session,*count?STATUS_SUCCESS:STATUS_BUFFER_OVERFLOW);};
      if(current.info.inode!=1) {
        const auto self=s.session.allocation_info(slot.handle);
        const auto slash=current.info.path.rfind('/');
        const auto parent=s.session.lookup(slash==std::string::npos?std::string_view{}:std::string_view(current.info.path).substr(0,slash));
        if(!append(self,".")||!append(parent.state,".."))return full();
      }
      for(const auto& entry:s.session.list(slot.handle)) {
        const auto found=s.session.lookup(entry.path);const auto slash=entry.path.rfind('/');
        const auto name=std::string_view(entry.path).substr(slash==std::string::npos?0:slash+1);
        if(!append(found.state,name))return full();
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
  hosted();
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
  hosted();
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
    if(view.needs_session_recovery())
      throw std::runtime_error("hosted restart requires resolved old-session state");
    // Admit the complete Windows profile BEFORE resume can repair a root.
    std::vector<std::uint64_t> pending{1};
    for(std::size_t at=0;at<pending.size();++at) {
      if(pending.size()>kNativeMaxNodes)throw std::runtime_error("restart namespace bound");
      const auto inode=pending[at];const auto state=view.allocation_info(inode);
      FSP_FSCTL_FILE_INFO info{};
      if(!encode_native_file_info(state,cache_policy,info))
        throw std::runtime_error("unqualified restart FileInfo/reservation");
      const auto security=view.security_descriptor(inode);
      if(!security)throw std::runtime_error("missing restart descriptor");
      (void)descriptor(const_cast<std::byte*>(security->data()));
      if(inode==1) {
        created=state.info.basic->creation_time;
        if(!created)throw std::runtime_error("missing original volume creation time");
      }
      if(state.info.directory)for(const auto& child:view.list_inode(inode))pending.push_back(child.inode);
    }
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
  hosted();
  const auto security=owner_security();const auto stamp=native_cleanup_utc();
  const NativeVolumeRootMetadata root{{stamp,stamp,stamp,stamp,FILE_ATTRIBUTE_DIRECTORY},security};
  auto session=NativeFileSession::create_volume(std::move(head),volume,std::move(image),image_id,
      foreground,{},NativeDispositionPolicy::windows_global,root);
  return std::unique_ptr<WinFspNativeWritable>(new WinFspNativeWritable(
      std::make_unique<Impl>(std::move(session),volume,stamp)));
}
HostedVolumeMaintenanceResult WinFspNativeWritable::recompress_managed_hosted(
    std::shared_ptr<StoreByteIo> target,StoreIdentity next,StoreCompressionOptions candidate) {
  hosted();
  std::lock_guard guard(impl_->control);
  if(requires_offline() || dispatcher_stopped())
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
void WinFspNativeWritable::bind_callbacks(FSP_FILE_SYSTEM& fs) noexcept{fs.UserContext=impl_.get();}
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
      if(fs&&fs->UserContext){unsigned expected=0;static_cast<Impl*>(fs->UserContext)->stopped.compare_exchange_strong(
          expected,normal?1U:2U,std::memory_order_acq_rel);}
    };
    t.SetVolumeLabel=[](FSP_FILE_SYSTEM*,PWSTR,FSP_FSCTL_VOLUME_INFO*)->NTSTATUS{return STATUS_INVALID_DEVICE_REQUEST;};
    t.SetSecurity=Impl::set_security;
    return t;
  }();return table;
}
}
