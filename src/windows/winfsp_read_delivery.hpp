#pragma once
#include <cstdint>

namespace flitzfs {
// Logical grouped bytes admitted to read-ahead, returned by callbacks,
// and still available for demand at the time of the snapshot.
struct WinFspGroupReadUsage {
  std::uint64_t published_bytes=0,delivered_bytes=0,pending_bytes=0;
};
struct WinFspGroupReadDelivery {
  WinFspGroupReadUsage cpu,amd;
};
}
