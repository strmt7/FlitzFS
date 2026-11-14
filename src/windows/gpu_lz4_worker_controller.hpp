#pragma once
#include "gpu_worker_common.hpp"
#include "flitzfs/lz4_worker_wire.hpp"
#include <optional>
namespace flitzfs {
// Single control thread. Retain this object across requests so qualified
// failure/recovery history survives CPU fallback. Normal I/O never launches it.
class GpuLz4WorkerController final {
 public:
  explicit GpuLz4WorkerController(WorkerExecution expected=WorkerExecution::hip);
  GpuLz4WorkerController(const GpuLz4WorkerController&)=delete;
  GpuLz4WorkerController& operator=(const GpuLz4WorkerController&)=delete;
  GpuLz4WorkerController(GpuLz4WorkerController&&)=delete;
  GpuLz4WorkerController& operator=(GpuLz4WorkerController&&)=delete;
  // Source/candidate spans expire on the next nonoverlapping begin.
  // Selected candidates are verified blocks, not proof of a storage saving.
  GpuWorkerStart begin(PreparedLz4WorkerBatch&&,const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments,bool explicit_consent,bool workload_qualified,
      const GpuHealthEvidence&,std::uint64_t now_ms,ProcessBudget budget={},
      std::uint32_t deadline_ms=100);
  GpuWorkerState poll(const GpuMemorySnapshot&,std::uint64_t now_ms) noexcept;
  // Select ordinary CPU encoding immediately. This never waits, signals a
  // process, queries the OS, invokes a runtime or changes a terminal result.
  std::span<const std::byte> use_cpu_fallback() noexcept;
  bool request_cancel() const noexcept;
  bool try_retire() noexcept;
  std::span<const std::byte> source() const noexcept;
  std::span<const std::byte> source(std::size_t index) const noexcept;
  std::span<const std::byte> candidate(std::size_t index) const noexcept;
  std::uint32_t count() const noexcept;
  GpuWorkerState state() const noexcept{return state_;}
  GpuGateCode last_gate_code() const noexcept{return last_gate_;}
  unsigned consecutive_failures() const noexcept{return policy_.consecutive_failures();}
  unsigned required_healthy_observations() const noexcept{return policy_.required_healthy_observations();}
  std::uint32_t process_id() const noexcept;
 private:
  void report(GpuRecoveryOutcome) noexcept;
  void select_cpu(GpuRecoveryOutcome,GpuGateCode,bool finalize=true) noexcept;
  GpuRecoveryPolicy policy_;
  WorkerExecution expected_;
  GpuWorkerState state_=GpuWorkerState::idle;
  GpuGateCode last_gate_=GpuGateCode::wrong_state;
  std::optional<PreparedLz4WorkerBatch> pending_,current_;
  std::optional<WorkerExchangeSession> session_;
  std::vector<std::vector<std::byte>> selected_;
  std::array<std::byte,8> luid_{};
  std::uint32_t node_mask_=0;
  std::uint64_t ticket_=0,generation_=0,started_=0,deadline_=0;
  GpuMemorySnapshot baseline_;
  bool reported_=false,disabled_=false;
};
}
