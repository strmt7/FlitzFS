#pragma once
#include "../store_read_scope.hpp"
#include <windows.h>
#include <cstdint>
#include <stdexcept>

namespace flitzfs::detail {

struct PositionedFileIoResult {
  DWORD error=ERROR_SUCCESS,bytes=0;
};

// One reusable manual-reset event per serialized caller. Each native request
// retains its own stack state and buffers until Windows confirms completion.
class PositionedFileIo final {
 public:
  PositionedFileIo():event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!event_)throw std::runtime_error("FlitzFS store file: cannot allocate an I/O completion event");
  }
  ~PositionedFileIo(){CloseHandle(event_);}
  PositionedFileIo(const PositionedFileIo&)=delete;
  PositionedFileIo& operator=(const PositionedFileIo&)=delete;

  PositionedFileIoResult read(HANDLE file,std::uint64_t offset,void* buffer,DWORD bytes,
      const StoreCaptureReadScope* capture=nullptr) const noexcept {
    if(capture && capture->interrupted())return {ERROR_OPERATION_ABORTED,0};
    auto operation=at(offset);
    const auto issued=ReadFile(file,buffer,bytes,nullptr,&operation);
    const auto result=complete(file,operation,issued?ERROR_SUCCESS:GetLastError(),0,capture);
    if(capture && capture->interrupted())return {ERROR_OPERATION_ABORTED,0};
    return result;
  }

  PositionedFileIoResult write(HANDLE file,std::uint64_t offset,const void* buffer,DWORD bytes) const noexcept {
    auto operation=at(offset);
    const auto issued=WriteFile(file,buffer,bytes,nullptr,&operation);
    return complete(file,operation,issued?ERROR_SUCCESS:GetLastError());
  }

  PositionedFileIoResult control(HANDLE file,DWORD command,void* input,DWORD input_bytes,
      void* output=nullptr,DWORD output_bytes=0) const noexcept {
    auto operation=at(0);
    DWORD immediate=0;
    const auto issued=DeviceIoControl(file,command,input,input_bytes,output,output_bytes,&immediate,&operation);
    return complete(file,operation,issued?ERROR_SUCCESS:GetLastError(),immediate);
  }

 private:
  HANDLE event_;

  OVERLAPPED at(std::uint64_t offset) const noexcept {
    OVERLAPPED operation{};
    operation.Offset=static_cast<DWORD>(offset);
    operation.OffsetHigh=static_cast<DWORD>(offset>>32);
    operation.hEvent=event_;
    return operation;
  }

  static PositionedFileIoResult complete(HANDLE file,OVERLAPPED& operation,DWORD submission,
      DWORD immediate=0,const StoreCaptureReadScope* capture=nullptr) noexcept {
    if(submission!=ERROR_SUCCESS && submission!=ERROR_IO_PENDING)return {submission,immediate};
    bool cancelled=false;
    for(;;) {
      if(capture && !cancelled && capture->interrupted()) {
        cancelled=true;
        // The operation can complete before cancellation reaches it.
        // Always observe its final status with the original request intact.
        (void)CancelIoEx(file,&operation);
      }
      DWORD transferred=0;
      constexpr DWORD capture_observation_ms=10;
      const auto wait=capture && !cancelled?capture_observation_ms:INFINITE;
      const auto done=GetOverlappedResultEx(file,&operation,&transferred,wait,FALSE);
      const auto error=done?ERROR_SUCCESS:GetLastError();
      if(!done && (error==WAIT_TIMEOUT || error==ERROR_IO_INCOMPLETE))continue;
      if(!HasOverlappedIoCompleted(&operation)) {
        if(!cancelled) {
          cancelled=true;
          (void)CancelIoEx(file,&operation);
        }
        continue;
      }
      return cancelled?PositionedFileIoResult{ERROR_OPERATION_ABORTED,0}:
          PositionedFileIoResult{error,transferred};
    }
  }
};

}
