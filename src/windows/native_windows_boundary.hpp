#pragma once
#include "native_file_info.hpp"
#include <new>
namespace flitzfs::win_detail {
inline NativeWindowsIoOutcome failure(NativeFileSession& session,NTSTATUS status,bool fence=false) noexcept {
  if(fence)session.fence();return {status,session.poisoned()};
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
}
