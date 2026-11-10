#pragma once
#include <cstdint>
#include <stdexcept>

namespace flitzfs {
// Exit statuses from the configured HIP executable after exact process exit.
enum class HipWorkerExit : std::uint32_t {
  invalid_request = 0x46530001,
  runtime_unavailable,
  unsupported_configuration,
  temporary_pressure,
  device_fault,
  integrity_failure,
};
class HipWorkerFailure final : public std::runtime_error {
 public:
  HipWorkerFailure(HipWorkerExit result, const char* operation)
      : std::runtime_error(operation), code(result) {}
  const HipWorkerExit code;
};
}
