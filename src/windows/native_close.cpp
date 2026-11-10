#include "native_close.hpp"
namespace flitzfs {
NativeCloseOutcome dispatch_native_close(NativeFileSession& session,const NativeFileHandle& handle) noexcept {
  try {session.close(handle);return NativeCloseOutcome::retired;}
  catch(...) {session.fence();return NativeCloseOutcome::fenced_engine;}
}
}
