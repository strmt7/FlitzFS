#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
namespace flitzfs {
enum class WindowsGpuTopologyStatus : std::uint32_t {
  success,invalid_identity,unavailable,wrong_adapter,device_failed,invalid_topology,budget_unavailable,fixture_forbidden
};
enum class WindowsGpuTopologyPhase : std::uint32_t {
  none,factory,adapter,description,budget,loader,device,device_identity,device_status,retire,complete
};
struct WindowsGpuTopologyResult {
  WindowsGpuTopologyStatus status=WindowsGpuTopologyStatus::unavailable;
  WindowsGpuTopologyPhase phase=WindowsGpuTopologyPhase::none;
  std::int32_t native_status=0;
  std::array<std::byte,8> luid{};
  std::uint32_t node_count=0;
  bool amd_hardware=false;
};
inline bool verified_single_gpu_node(const WindowsGpuTopologyResult& result,
    std::array<std::byte,8> expected) noexcept {
  return result.status==WindowsGpuTopologyStatus::success &&
      result.phase==WindowsGpuTopologyPhase::complete && result.native_status==0 &&
      result.amd_hardware && result.node_count==1 && result.luid==expected &&
      std::any_of(expected.begin(),expected.end(),[](auto b){return b!=std::byte{0};});
}
// Explicit supervised metadata only. No queues/resources, HIP or kernel calls.
// Callers must enforce process budgets; this function can block in OS/driver code.
WindowsGpuTopologyResult query_windows_gpu_topology(std::array<std::byte,8>) noexcept;
// Disposable-hosted CPU/WARP fixture only; its result NEVER grants GPU consent.
WindowsGpuTopologyResult query_windows_software_topology_fixture(std::array<std::byte,8>) noexcept;
}
