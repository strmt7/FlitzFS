#pragma once
#include "hip_worker_exit.hpp"
#include <hip/hip_runtime_api.h>
#include <windows.h>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <limits>
#include <string_view>

namespace flitzfs::hip_detail {
// Expand the selected SDK's versioned API macros before stringizing. The
// function pointer type and dynamic-loader name must describe the same ABI.
#define FLITZFS_HIP_STRING_INNER(value) #value
#define FLITZFS_HIP_STRING(value) FLITZFS_HIP_STRING_INNER(value)
#define FLITZFS_HIP_FUNCTIONS(X) \
  X(init, hipInit) \
  X(properties, hipGetDeviceProperties) \
  X(set_device, hipSetDevice) \
  X(priority_range, hipDeviceGetStreamPriorityRange) \
  X(create_stream, hipStreamCreateWithPriority) \
  X(destroy_stream, hipStreamDestroy) \
  X(synchronize, hipStreamSynchronize) \
  X(allocate, hipMalloc) \
  X(release, hipFree) \
  X(copy, hipMemcpy) \
  X(launch, hipLaunchKernel)

struct HipApi {
#define FLITZFS_HIP_FIELD(field, symbol) decltype(&symbol) field;
  FLITZFS_HIP_FUNCTIONS(FLITZFS_HIP_FIELD)
#undef FLITZFS_HIP_FIELD
};
inline constexpr auto runtime_symbols = std::to_array<std::string_view>({
#define FLITZFS_HIP_NAME(field, symbol) FLITZFS_HIP_STRING(symbol),
  FLITZFS_HIP_FUNCTIONS(FLITZFS_HIP_NAME)
#undef FLITZFS_HIP_NAME
});

template<class Lookup>
HipApi bind_api(Lookup&& lookup) {
  HipApi api{};
#define FLITZFS_HIP_BIND(field, symbol) \
  { const auto address = lookup(FLITZFS_HIP_STRING(symbol)); \
    if (!address) throw HipWorkerFailure(HipWorkerExit::runtime_unavailable, FLITZFS_HIP_STRING(symbol)); \
    api.field = std::bit_cast<decltype(&symbol)>(address); }
  FLITZFS_HIP_FUNCTIONS(FLITZFS_HIP_BIND)
#undef FLITZFS_HIP_BIND
  return api;
}
inline HipApi bind_api(HMODULE module) {
  return bind_api([module](const char* name) { return GetProcAddress(module, name); });
}
#undef FLITZFS_HIP_FUNCTIONS
#undef FLITZFS_HIP_STRING
#undef FLITZFS_HIP_STRING_INNER

inline void check_hip(hipError_t status, const char* operation) {
  if (status == hipSuccess) return;
  auto code = HipWorkerExit::device_fault;
  if (status == hipErrorOutOfMemory) code = HipWorkerExit::temporary_pressure;
  if (status == hipErrorInsufficientDriver) code = HipWorkerExit::runtime_unavailable;
  if (status == hipErrorNoDevice || status == hipErrorInvalidDevice || status == hipErrorNotSupported)
    code = HipWorkerExit::unsupported_configuration;
  throw HipWorkerFailure(code, operation);
}

inline int initialize_device(const HipApi& api, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask) {
  check_hip(api.init(0), "initialize");
  hipDeviceProp_t properties{};
  check_hip(api.properties(&properties, ordinal), "query device properties");
  static_assert(sizeof(properties.luid) == sizeof(luid));
  const auto actual_luid = std::bit_cast<std::array<std::byte,8>>(std::to_array(properties.luid));
  if (actual_luid != luid || properties.luidDeviceNodeMask != node_mask)
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration, "HIP/DXGI adapter identity mismatch");
  check_hip(api.set_device(ordinal), "select device");
  int least = (std::numeric_limits<int>::min)();
  int greatest = (std::numeric_limits<int>::max)();
  check_hip(api.priority_range(&least, &greatest), "query stream priorities");
  if (least < greatest) throw std::runtime_error("HIP stream priority range unavailable");
  return least;
}
}
