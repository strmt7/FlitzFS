#include "native_cleanup.hpp"

namespace flitzfs {
std::uint64_t native_cleanup_utc() noexcept {
  FILETIME value{};GetSystemTimeAsFileTime(&value);
  return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
namespace {
NativeCleanupOutcome dispatch_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock,bool allow_trim) noexcept {
  const ULONG supported=FspCleanupDelete|FspCleanupSetArchiveBit|
      FspCleanupSetLastAccessTime|FspCleanupSetLastWriteTime|FspCleanupSetChangeTime|
      (allow_trim?FspCleanupSetAllocationSize:0);
  // The selected FSD requests trim only without delete-pending. Reject an
  // impossible combined flag before clock/I/O; old entry point still rejects
  // every allocation flag. Neither entry point is wired into a writable table.
  if((flags&~supported) || ((flags&FspCleanupDelete) && (flags&FspCleanupSetAllocationSize))) {
    session.fence();return NativeCleanupOutcome::fenced_unsupported;
  }
  NativeCleanupMetadata effect;
  effect.set_archive=(flags&FspCleanupSetArchiveBit)!=0;
  effect.set_access_time=(flags&FspCleanupSetLastAccessTime)!=0;
  effect.set_write_time=(flags&FspCleanupSetLastWriteTime)!=0;
  effect.set_change_time=(flags&FspCleanupSetChangeTime)!=0;
  if(effect.set_access_time || effect.set_write_time || effect.set_change_time) {
    if(!clock){session.fence();return NativeCleanupOutcome::fenced_clock;}
    effect.utc_time=clock();
  }
  if(!native_cleanup_metadata_valid(effect)) {
    session.fence();return NativeCleanupOutcome::fenced_clock;
  }
  try {
    session.cleanup_event(handle,(flags&FspCleanupDelete)!=0,
        {effect,(flags&FspCleanupSetAllocationSize)!=0});
    return NativeCleanupOutcome::applied;
  }catch(...) {
    session.fence();return NativeCleanupOutcome::fenced_engine;
  }
}
}
NativeCleanupOutcome dispatch_native_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock) noexcept {
  return dispatch_cleanup(session,handle,flags,clock,false);
}
NativeCleanupOutcome dispatch_native_allocated_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock) noexcept {
  return dispatch_cleanup(session,handle,flags,clock,true);
}
}
