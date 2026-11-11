#pragma once
#include "flitzfs/gpu_batch.hpp"
namespace flitzfs {
enum class WindowsGpuBudgetStatus { success, invalid_identity, unavailable, wrong_adapter, query_failed };
enum class WindowsGpuBudgetPhase { none, factory, adapter, description, local, nonlocal, complete };
struct WindowsGpuBudgetResult {
  WindowsGpuBudgetStatus status=WindowsGpuBudgetStatus::unavailable;
  GpuMemorySnapshot snapshot;
  WindowsGpuBudgetPhase phase=WindowsGpuBudgetPhase::none;
  std::int32_t native_status=0;
};
// Read-only DXGI telemetry for a caller-supplied verified HIP LUID/node mask.
// No adapter-order heuristic, memory reservation, compute workload or HIP call.
// OS calls can fail or block; invoke in the future supervised worker, not a
// filesystem callback. GetTickCount64 is the associated monotonic clock.
WindowsGpuBudgetResult query_windows_gpu_budget(std::array<std::byte,8> luid,
    std::uint32_t node_mask) noexcept;
} // namespace flitzfs
