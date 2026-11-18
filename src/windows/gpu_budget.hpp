#pragma once
#include "flitzfs/gpu_batch.hpp"
#include <optional>
#include <wrl/client.h>
struct IDXGIAdapter3;
struct IDXGIFactory4;
namespace flitzfs {
enum class WindowsGpuBudgetStatus { success, invalid_identity, unavailable, wrong_adapter, query_failed };
enum class WindowsGpuBudgetPhase { none, factory, adapter, description, local, nonlocal, complete, host_memory, topology };
struct WindowsGpuBudgetResult {
  WindowsGpuBudgetStatus status=WindowsGpuBudgetStatus::unavailable;
  GpuMemorySnapshot snapshot;
  WindowsGpuBudgetPhase phase=WindowsGpuBudgetPhase::none;
  std::int32_t native_status=0;
};
// One adapter binding on one worker thread. Each sample reads live DXGI and
// host counters. Topology changes and failed collections retire the binding.
class WindowsGpuBudgetSampler final {
 public:
  WindowsGpuBudgetSampler(std::array<std::byte,8> luid,std::uint32_t node_mask) noexcept;
  ~WindowsGpuBudgetSampler();
  WindowsGpuBudgetSampler(const WindowsGpuBudgetSampler&)=delete;
  WindowsGpuBudgetSampler& operator=(const WindowsGpuBudgetSampler&)=delete;
  WindowsGpuBudgetResult sample() noexcept;

 private:
  WindowsGpuBudgetResult collect() noexcept;
  const std::array<std::byte,8> luid_;
  const std::uint32_t node_mask_;
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
  std::uint64_t dedicated_bytes_=0;
  std::optional<WindowsGpuBudgetResult> failure_;
};
// Read-only DXGI telemetry for a caller-supplied verified HIP LUID/node mask.
// No adapter-order heuristic, memory reservation, compute workload or HIP call.
// OS calls can fail or block; invoke in the future supervised worker, not a
// filesystem callback. GetTickCount64 is the associated monotonic clock.
WindowsGpuBudgetResult query_windows_gpu_budget(std::array<std::byte,8> luid,
    std::uint32_t node_mask) noexcept;
} // namespace flitzfs
