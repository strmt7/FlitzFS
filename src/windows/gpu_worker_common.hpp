#pragma once
#include "worker_exchange.hpp"
#include "flitzfs/gpu_recovery.hpp"
namespace flitzfs {
enum class GpuWorkerState { idle,waiting,cpu_selected,worker_selected };
struct GpuWorkerStart {
  GpuRouteDecision route;
  GpuGateCode gate=GpuGateCode::wrong_state;
  bool submitted=false;
};

GpuRecoveryOutcome worker_transport_failure(const WorkerExchangeSession&,
    const WorkerExchangeResult&,WorkerExecution) noexcept;
GpuRecoveryOutcome worker_gate_failure(GpuGateCode) noexcept;
}
