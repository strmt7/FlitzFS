#pragma once
#include "flitzfs/native_session.hpp"
namespace flitzfs {
enum class NativeCloseOutcome {retired,fenced_engine};
// Bridge for the selected WinFsp VOID Close contract. Caller authenticates the
// context and serializes callbacks. A fenced outcome requires supervisor
// offlining outside the callback, never stop/join or storage retries here.
NativeCloseOutcome dispatch_native_close(NativeFileSession&,const NativeFileHandle&) noexcept;
}
