#pragma once
#include "hip_api.hpp"
#include "hip_memory_wire.hpp"
#include <array>
namespace flitzfs::hip_detail {
// Shared ownership for isolated compute paths. This performs no discovery or
// launch; callers retain the authenticated runtime until exact retirement.
struct DeviceResources {
  const HipApi& api;
  hipStream_t stream = nullptr;
  std::array<void*,4> buffers{};
  HipMemoryObservation* observation=nullptr;
  bool retirement_failed=false;

  explicit DeviceResources(const HipApi& functions,HipMemoryObservation* trace=nullptr)
      : api(functions),observation(trace) {}
  DeviceResources(const DeviceResources&) = delete;
  DeviceResources& operator=(const DeviceResources&) = delete;

  hipError_t release_all() noexcept {
    hipError_t first = hipSuccess;
    if (stream) {
      first = api.destroy_stream(stream);
      stream = nullptr;
    }
    for (auto& buffer : buffers) {
      if (!buffer) continue;
      const auto status = api.release(buffer);
      if (first == hipSuccess) first = status;
      buffer = nullptr;
    }
    retirement_failed=retirement_failed || first!=hipSuccess;
    if(observation){
      observation->flags&=~static_cast<std::uint32_t>(hip_memory_resources_released);
      if(!retirement_failed)observation->flags|=hip_memory_resources_released;
    }
    return first;
  }
  ~DeviceResources() { (void)release_all(); }
};
} // namespace flitzfs::hip_detail
