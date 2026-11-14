#pragma once
#include "flitzfs/native_session.hpp"
#include <winfsp/winfsp.h>
namespace flitzfs {
// Explicit cache/reservation presentation, NEVER measured compressed or host
// allocation. A declaration is not backing/cache/client qualification.
enum class NativeFileInfoAllocation {unavailable,native_reserved_cache};
struct NativeWindowsIoOutcome {
  NTSTATUS status=STATUS_INVALID_DEVICE_STATE;
  bool offline_required=false;
};
// Pure bounded translation of captured facts; initialize ALL 72 ABI bytes.
// No fallback timestamps/attributes/reservations, backing I/O or allocation.
bool encode_native_file_info(const NativeInodeAllocationInfo&,NativeFileInfoAllocation,
    FSP_FSCTL_FILE_INFO&) noexcept;
// Unwired synchronous engine bridges. Caller owns and serializes the session;
// exceptions never cross the ABI. Output/count stay zero on failure.
NativeWindowsIoOutcome dispatch_native_file_info(NativeFileSession&,const NativeFileHandle&,
    NativeFileInfoAllocation,FSP_FSCTL_FILE_INFO*) noexcept;
NativeWindowsIoOutcome dispatch_native_write(NativeFileSession&,const NativeFileHandle&,
    const void* buffer,UINT64 offset,ULONG length,bool append,bool constrained,
    const NativeAutomaticMetadata&,NativeFileInfoAllocation,PULONG transferred,
    FSP_FSCTL_FILE_INFO*) noexcept;
// Explicit optional advice, with pure budget rejection before metadata reads.
// The ordinary bridge retains its original API and supplies no advice.
NativeWindowsIoOutcome dispatch_native_write_with_lz4(NativeFileSession&,const NativeFileHandle&,
    const void* buffer,UINT64 offset,ULONG length,bool append,bool constrained,
    std::span<const NativeLz4ExtentCandidate>,const NativeAutomaticMetadata&,
    NativeFileInfoAllocation,PULONG transferred,FSP_FSCTL_FILE_INFO*) noexcept;
NativeWindowsIoOutcome dispatch_native_set_size(NativeFileSession&,const NativeFileHandle&,
    UINT64 bytes,bool allocation,const NativeAutomaticMetadata&,NativeFileInfoAllocation,
    FSP_FSCTL_FILE_INFO*) noexcept;
NativeWindowsIoOutcome dispatch_native_flush(NativeFileSession&,const NativeFileHandle*,
    NativeFileInfoAllocation,FSP_FSCTL_FILE_INFO*) noexcept;
}
