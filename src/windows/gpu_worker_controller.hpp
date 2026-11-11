#pragma once
#include "gpu_worker_common.hpp"
#include <optional>

namespace flitzfs {
// Explicit single-control-thread integration of policy, gate and exact worker
// ownership. Never a discovery loop or automatic filesystem/runtime route.
// Keep this controller across requests: replacing it loses advisory history.
// Helper/configuration/workload qualification is an external prerequisite.
class GpuWorkerController final {
 public:
  explicit GpuWorkerController(WorkerExecution expected=WorkerExecution::hip);
  ~GpuWorkerController()=default; // Session abandonment retains supervision.
  GpuWorkerController(const GpuWorkerController&)=delete;
  GpuWorkerController& operator=(const GpuWorkerController&)=delete;
  GpuWorkerController(GpuWorkerController&&)=delete;
  GpuWorkerController& operator=(GpuWorkerController&&)=delete;
  // Consume only after rejecting an overlapping waiting request. Previously
  // selected bytes must be consumed before begin; spans expire on next begin.
  // A CPU-only request can proceed while a previous helper is still retiring.
  GpuWorkerStart begin(PreparedGpuBatch&& batch,const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments,bool explicit_consent,
      bool workload_qualified,const GpuHealthEvidence& evidence,std::uint64_t now_ms,
      ProcessBudget budget={},std::uint32_t deadline_ms=100);
  GpuWorkerState poll(const GpuMemorySnapshot& after,std::uint64_t now_ms) noexcept;
  // Select CPU for a waiting request, without any OS/runtime call or wait.
  // Already selected results remain terminal; late replies cannot change them.
  std::span<const std::byte> use_cpu_fallback() noexcept;
  // Signals only; process teardown remains on the bounded supervisor.
  bool request_cancel() const noexcept;
  // Classify any late immutable result once, then zero-time confirm exact
  // child AND supervisor exit. Never changes selected bytes; no discovery,
  // process wait or sleep. Late verification is bounded by one batch.
  bool try_retire() noexcept;
  std::span<const std::byte> selected_bytes() const noexcept;
  GpuWorkerState state() const noexcept {return state_;}
  GpuGateCode last_gate_code() const noexcept {return last_gate_;}
  unsigned consecutive_failures() const noexcept {return policy_.consecutive_failures();}
  unsigned required_healthy_observations() const noexcept {return policy_.required_healthy_observations();}
  std::uint32_t process_id() const noexcept;
 private:
  void report(GpuRecoveryOutcome outcome) noexcept;
  void select_cpu(GpuRecoveryOutcome outcome,GpuGateCode why,bool finalize=true) noexcept;
  GpuRecoveryPolicy policy_;
  WorkerExecution expected_;
  GpuWorkerState state_=GpuWorkerState::idle;
  GpuGateCode last_gate_=GpuGateCode::wrong_state;
  std::optional<PreparedGpuBatch> cpu_;
  std::unique_ptr<GpuResultGate> gate_;
  std::optional<WorkerExchangeSession> session_;
  std::uint64_t ticket_=0,generation_=0;
  bool reported_=false,disabled_=false;
};
} // namespace flitzfs
