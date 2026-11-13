#pragma once
#include "hip_worker_exit.hpp"
#include <hip/hip_runtime_api.h>
#include <windows.h>
#include <algorithm>
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
    if (!address) throw HipWorkerFailure(HipWorkerExit::runtime_unavailable, FLITZFS_HIP_STRING(symbol), HipWorkerStage::runtime_api); \
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

inline void check_hip(hipError_t status, const char* operation,
    HipWorkerStage stage = HipWorkerStage::none) {
  if (status == hipSuccess) return;
  auto code = HipWorkerExit::device_fault;
  if (status == hipErrorOutOfMemory) code = HipWorkerExit::temporary_pressure;
  if (status == hipErrorInsufficientDriver) code = HipWorkerExit::runtime_unavailable;
  if (status == hipErrorNoDevice || status == hipErrorInvalidDevice || status == hipErrorNotSupported)
    code = HipWorkerExit::unsupported_configuration;
  throw HipWorkerFailure(code, operation, stage);
}

struct HipDeviceIdentity {
  std::array<std::byte,8> luid{};
  std::uint32_t node_mask=0;
};
inline HipDeviceIdentity query_device_identity(const HipApi& api,int ordinal) {
  check_hip(api.init(0), "initialize", HipWorkerStage::runtime_initialize);
  hipDeviceProp_t properties{};
  check_hip(api.properties(&properties, ordinal), "query device properties", HipWorkerStage::device_properties);
  static_assert(sizeof(properties.luid) == 8);
  return {std::bit_cast<std::array<std::byte,8>>(std::to_array(properties.luid)),
      properties.luidDeviceNodeMask};
}
template<class SingleNodeProof>
int initialize_device(const HipApi& api, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask, SingleNodeProof&& single_node_proof) {
  if (ordinal<0 || ordinal>=64 || node_mask==0 || !std::has_single_bit(node_mask) ||
      std::all_of(luid.begin(),luid.end(),[](auto value){return value==std::byte{0};}))
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration, "invalid adapter selector",
        HipWorkerStage::adapter_node);
  const auto identity=query_device_identity(api,ordinal);
  const auto actual_luid=identity.luid;
  if (actual_luid != luid)
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration, "HIP/DXGI adapter LUID mismatch",
        HipWorkerStage::adapter_luid);
  // Some installed Windows HIP runtimes expose a valid LUID with no node mask.
  // Resolve ONLY that omission against the same OS adapter's single physical
  // node. The trusted caller supplies fresh native proof; no ordinal inference.
  if (identity.node_mask != node_mask &&
      !(identity.node_mask==0 && node_mask==1 && single_node_proof(luid)))
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration, "HIP/DXGI adapter node mismatch",
        HipWorkerStage::adapter_node);
  check_hip(api.set_device(ordinal), "select device", HipWorkerStage::device_select);
  int least = (std::numeric_limits<int>::min)();
  int greatest = (std::numeric_limits<int>::max)();
  check_hip(api.priority_range(&least, &greatest), "query stream priorities", HipWorkerStage::stream_priorities);
  // Some supported runtime versions return success without supplying this
  // optional capability. Corroborate both outputs with opposite initial values;
  // one bounded second query detects omitted/partial/inconsistent reports.
  int corroborated_least = (std::numeric_limits<int>::max)();
  int corroborated_greatest = (std::numeric_limits<int>::min)();
  check_hip(api.priority_range(&corroborated_least, &corroborated_greatest),
      "corroborate stream priorities", HipWorkerStage::stream_priorities);
  if (least != corroborated_least || greatest != corroborated_greatest)
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration,
        "HIP stream priority range unreported or inconsistent", HipWorkerStage::stream_priorities);
  if (least < greatest) throw HipWorkerFailure(HipWorkerExit::device_fault,
      "HIP stream priority range invalid", HipWorkerStage::stream_priorities);
  return least;
}
inline int initialize_device(const HipApi& api,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask) {
  return initialize_device(api,ordinal,luid,node_mask,[](auto){return false;});
}
}
