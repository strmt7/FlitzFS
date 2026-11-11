#pragma once
#include "flitzfs/native_session.hpp"
#include <winfsp/winfsp.h>

namespace flitzfs {
// User-mode engine bridge only; no filesystem/dispatcher/mount/driver creation.
enum class NativeCleanupOutcome {applied,fenced_unsupported,fenced_clock,fenced_engine};
using NativeCleanupClock=std::uint64_t(*)() noexcept;
std::uint64_t native_cleanup_utc() noexcept;
// Already-trusted FSD flags/context. Every failure fences without releasing
// contexts or retrying I/O. The future adapter must signal its supervisor;
// Cleanup's VOID ABI cannot report a failure and this function never stops or
// joins a dispatcher from inside its callback.
NativeCleanupOutcome dispatch_native_cleanup(NativeFileSession&,const NativeFileHandle&,
    ULONG flags,NativeCleanupClock clock=native_cleanup_utc) noexcept;
// Explicit unwired bridge for an engine with tracked native reservations.
// Regular trim requires prior tracking. No physical-space/callback-table
// qualification follows; the original entry point keeps rejecting allocation.
NativeCleanupOutcome dispatch_native_allocated_cleanup(NativeFileSession&,const NativeFileHandle&,
    ULONG flags,NativeCleanupClock clock=native_cleanup_utc) noexcept;
}
