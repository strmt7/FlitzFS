#include "native_cleanup.hpp"

namespace flitzfs {
std::uint64_t native_cleanup_utc() noexcept {
  FILETIME value{};GetSystemTimeAsFileTime(&value);
  return (std::uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime;
}
namespace {
constexpr std::uint64_t access_time_interval=60ULL*60*10'000'000;
NativeCleanupOutcome dispatch_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock,bool allow_trim,std::uint64_t minimum_access_interval) noexcept {
  const ULONG supported=FspCleanupDelete|FspCleanupSetArchiveBit|
      FspCleanupSetLastAccessTime|FspCleanupSetLastWriteTime|FspCleanupSetChangeTime|
      (allow_trim?FspCleanupSetAllocationSize:0);
  // The FSD requests trim only without deletion. Validate flags before
  // sampling a clock or touching storage.
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
        {effect,(flags&FspCleanupSetAllocationSize)!=0,minimum_access_interval});
    return NativeCleanupOutcome::applied;
  }catch(...) {
    session.fence();return NativeCleanupOutcome::fenced_engine;
  }
}
}
NativeCleanupOutcome dispatch_native_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock) noexcept {
  return dispatch_cleanup(session,handle,flags,clock,false,0);
}
NativeCleanupOutcome dispatch_native_allocated_cleanup(NativeFileSession& session,const NativeFileHandle& handle,
    ULONG flags,NativeCleanupClock clock) noexcept {
  return dispatch_cleanup(session,handle,flags,clock,true,access_time_interval);
}
}
