#include "native_file_info.hpp"
#include <limits>
#include <cstddef>
#include <new>
#include <type_traits>
namespace flitzfs {
namespace {
static_assert(sizeof(FSP_FSCTL_FILE_INFO)==72);
static_assert(offsetof(FSP_FSCTL_FILE_INFO,FileAttributes)==0 && offsetof(FSP_FSCTL_FILE_INFO,ReparseTag)==4 &&
    offsetof(FSP_FSCTL_FILE_INFO,AllocationSize)==8 && offsetof(FSP_FSCTL_FILE_INFO,FileSize)==16 &&
    offsetof(FSP_FSCTL_FILE_INFO,CreationTime)==24 && offsetof(FSP_FSCTL_FILE_INFO,LastAccessTime)==32 &&
    offsetof(FSP_FSCTL_FILE_INFO,LastWriteTime)==40 && offsetof(FSP_FSCTL_FILE_INFO,ChangeTime)==48 &&
    offsetof(FSP_FSCTL_FILE_INFO,IndexNumber)==56 && offsetof(FSP_FSCTL_FILE_INFO,HardLinks)==64 &&
    offsetof(FSP_FSCTL_FILE_INFO,EaSize)==68);
static_assert(std::is_trivially_copy_assignable_v<FSP_FSCTL_FILE_INFO>);
static_assert(kNativeAllocationUnit==4096);
bool policy(NativeFileInfoAllocation value) noexcept {
  return value==NativeFileInfoAllocation::native_reserved_cache;
}
NativeWindowsIoOutcome failure(NativeFileSession& session,NTSTATUS status,bool fence=false) noexcept {
  if(fence)session.fence();
  return {status,session.poisoned()};
}
template<class Function>NativeWindowsIoOutcome boundary(NativeFileSession& session,Function&& fn) noexcept {
  try{return fn();}
  catch(const NativeSessionError& e) {
    switch(e.code()) {
      case NativeSessionErrorCode::invalid_handle:return failure(session,STATUS_INVALID_HANDLE);
      case NativeSessionErrorCode::access_denied:return failure(session,STATUS_ACCESS_DENIED);
      case NativeSessionErrorCode::delete_pending:return failure(session,STATUS_DELETE_PENDING);
      case NativeSessionErrorCode::sharing_violation:return failure(session,STATUS_SHARING_VIOLATION);
      case NativeSessionErrorCode::limit:return failure(session,STATUS_INSUFFICIENT_RESOURCES);
      case NativeSessionErrorCode::invalid_options:return failure(session,STATUS_INVALID_DEVICE_STATE);
      case NativeSessionErrorCode::terminal:return failure(session,STATUS_IO_DEVICE_ERROR,true);
    }
  }catch(const NativeFsError& e) {
    switch(e.code()) {
      case NativeFsErrorCode::invalid_path:return failure(session,STATUS_OBJECT_NAME_INVALID);
      case NativeFsErrorCode::not_found:return failure(session,STATUS_OBJECT_NAME_NOT_FOUND);
      case NativeFsErrorCode::already_exists:return failure(session,STATUS_OBJECT_NAME_COLLISION);
      case NativeFsErrorCode::not_directory:return failure(session,STATUS_NOT_A_DIRECTORY);
      case NativeFsErrorCode::is_directory:return failure(session,STATUS_FILE_IS_A_DIRECTORY);
      case NativeFsErrorCode::not_empty:return failure(session,STATUS_DIRECTORY_NOT_EMPTY);
      case NativeFsErrorCode::invalid_operation:return failure(session,STATUS_INVALID_PARAMETER);
      case NativeFsErrorCode::limit:return failure(session,STATUS_INSUFFICIENT_RESOURCES);
      case NativeFsErrorCode::corrupt:return failure(session,STATUS_FILE_CORRUPT_ERROR,true);
      case NativeFsErrorCode::storage_failure:return failure(session,STATUS_IO_DEVICE_ERROR,true);
    }
  }catch(const std::bad_alloc&){return failure(session,STATUS_INSUFFICIENT_RESOURCES);}
  catch(...){return failure(session,STATUS_IO_DEVICE_ERROR,true);}
  return failure(session,STATUS_IO_DEVICE_ERROR,true);
}
bool preflight(const NativeInodeAllocationInfo& state,NativeFileInfoAllocation mode) noexcept {
  FSP_FSCTL_FILE_INFO ignored;return encode_native_file_info(state,mode,ignored);
}
}
bool encode_native_file_info(const NativeInodeAllocationInfo& state,NativeFileInfoAllocation mode,
    FSP_FSCTL_FILE_INFO& output) noexcept {
  output={};const auto& n=state.info;
  constexpr auto maximum=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  if(!policy(mode) || !n.inode || !n.modified_sequence || !n.basic ||
      n.logical_bytes>kNativeMaxFileBytes || (n.unlinked && n.delete_pending))return false;
  const auto& b=*n.basic;
  if(b.creation_time>maximum || b.last_access_time>maximum || b.last_write_time>maximum ||
      b.change_time>maximum || (b.attributes&~kNativeBasicAttributeMask) ||
      bool(b.attributes&FILE_ATTRIBUTE_DIRECTORY)!=n.directory ||
      (n.directory && (b.attributes&FILE_ATTRIBUTE_TEMPORARY)))return false;
  UINT64 allocation=0;
  if(n.directory) {
    if(n.logical_bytes || state.reserved_bytes)return false;
  }else {
    if(!state.reserved_bytes || *state.reserved_bytes%kNativeAllocationUnit ||
        *state.reserved_bytes<n.logical_bytes || *state.reserved_bytes>kNativeAllocationCapacity)return false;
    allocation=*state.reserved_bytes;
  }
  FSP_FSCTL_FILE_INFO result{};
  result.FileAttributes=b.attributes;result.AllocationSize=allocation;result.FileSize=n.logical_bytes;
  result.CreationTime=b.creation_time;result.LastAccessTime=b.last_access_time;
  result.LastWriteTime=b.last_write_time;result.ChangeTime=b.change_time;result.IndexNumber=n.inode;
  // Current ABI requires HardLinks=0 (unimplemented). ReparseTag/EaSize stay
  // zero: this engine admits neither feature. Do not invent normal/archive.
  output=result;return true;
}
NativeWindowsIoOutcome dispatch_native_file_info(NativeFileSession& session,const NativeFileHandle& handle,
    NativeFileInfoAllocation mode,FSP_FSCTL_FILE_INFO* output) noexcept {
  if(output)*output={};
  if(!output || !policy(mode))return failure(session,STATUS_INVALID_PARAMETER);
  return boundary(session,[&] {
    const auto state=session.allocation_info(handle);
    if(!encode_native_file_info(state,mode,*output))return failure(session,STATUS_INVALID_DEVICE_STATE);
    return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
}
NativeWindowsIoOutcome dispatch_native_write(NativeFileSession& session,const NativeFileHandle& handle,
    const void* buffer,UINT64 offset,ULONG length,bool append,bool constrained,
    const NativeAutomaticMetadata& effect,NativeFileInfoAllocation mode,PULONG transferred,
    FSP_FSCTL_FILE_INFO* output) noexcept {
  if(transferred)*transferred=0;if(output)*output={};
  if(!transferred || !output || !policy(mode) || (length && !buffer) ||
      length>kStoreMaxObjectBytes || append!=(offset==UINT64_MAX) || !native_cleanup_metadata_valid(effect))
    return failure(session,STATUS_INVALID_PARAMETER);
  return boundary(session,[&] {
    const auto before=session.mutation_info(handle);
    if(!preflight(before,mode))return failure(session,STATUS_INVALID_DEVICE_STATE);
    const auto bytes=std::span<const std::byte>(static_cast<const std::byte*>(buffer),length);
    auto committed=session.write_storage_result(handle,offset,bytes,{append,constrained},effect);
    // Only bounded scalar validation/copy remains. Never query storage/path,
    // sample a clock, allocate, release a context or retry after publication.
    if(!encode_native_file_info(committed.state,mode,*output))
      return failure(session,STATUS_IO_DEVICE_ERROR,true);
    *transferred=committed.bytes;return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
}
NativeWindowsIoOutcome dispatch_native_set_size(NativeFileSession& session,const NativeFileHandle& handle,
    UINT64 bytes,bool allocation,const NativeAutomaticMetadata& effect,NativeFileInfoAllocation mode,
    FSP_FSCTL_FILE_INFO* output) noexcept {
  if(output)*output={};
  if(!output || !policy(mode) || !native_cleanup_metadata_valid(effect) ||
      (allocation && (bytes%kNativeAllocationUnit || bytes>kNativeAllocationCapacity)) ||
      (!allocation && bytes>kNativeMaxFileBytes))return failure(session,STATUS_INVALID_PARAMETER);
  return boundary(session,[&] {
    const auto before=session.mutation_info(handle);
    if(!preflight(before,mode))return failure(session,STATUS_INVALID_DEVICE_STATE);
    if(allocation) {
      auto committed=session.reserve_result(handle,bytes,effect);
      if(!encode_native_file_info(committed.state,mode,*output))
        return failure(session,STATUS_IO_DEVICE_ERROR,true);
    }else {
      auto committed=session.truncate_storage_result(handle,bytes,effect);
      if(!encode_native_file_info(committed.state,mode,*output))
        return failure(session,STATUS_IO_DEVICE_ERROR,true);
    }
    return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
}
NativeWindowsIoOutcome dispatch_native_flush(NativeFileSession& session,const NativeFileHandle* handle,
    NativeFileInfoAllocation mode,FSP_FSCTL_FILE_INFO* output) noexcept {
  if(output)*output={};
  if(!output || !policy(mode))return failure(session,STATUS_INVALID_PARAMETER);
  return boundary(session,[&] {
    FSP_FSCTL_FILE_INFO prepared{};
    if(handle) {
      const auto state=session.allocation_info(*handle);
      if(!encode_native_file_info(state,mode,prepared))return failure(session,STATUS_INVALID_DEVICE_STATE);
      session.flush(*handle);
    }else session.flush();
    // Flush verified the same acknowledged namespace before/after its one real
    // barrier. Return preflight facts, not a fallible post-barrier query.
    *output=prepared;return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
}
}
