#include "native_cleanup.hpp"

namespace flitzfs {
std::uint64_t native_cleanup_utc() noexcept {
  FILETIME value{};GetSystemTimeAsFileTime(&value);
  return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
NativeCleanupOutcome dispatch_native_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock) noexcept {
  constexpr ULONG supported=FspCleanupDelete|FspCleanupSetArchiveBit|
      FspCleanupSetLastAccessTime|FspCleanupSetLastWriteTime|FspCleanupSetChangeTime;
  // SetAllocationSize is deliberately unsupported until real allocation
  // accounting is qualified; never silently acknowledge an ignored request.
  if(flags&~supported) {
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
    session.cleanup(handle,(flags&FspCleanupDelete)!=0,effect);
    return NativeCleanupOutcome::applied;
  }catch(...) {
    session.fence();return NativeCleanupOutcome::fenced_engine;
  }
}
}
