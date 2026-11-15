#pragma once
#include "flitzfs/native_filesystem.hpp"
namespace flitzfs::detail {
// Borrowed only under the session's existing serialized control contract.
// The session owns this token; controllers retain weak references, never the
// session or its writer. Impl addresses survive outer-session moves.
struct NativeMaintenanceSource {
  NativeFilesystemWriter* writer=nullptr;
  const bool* terminal=nullptr;
  std::weak_ptr<const void> active;
};
}
