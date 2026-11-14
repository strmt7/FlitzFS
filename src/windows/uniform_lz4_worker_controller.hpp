#pragma once
#include "gpu_worker_common.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"
#include "flitzfs/gpu_lz4_window.hpp"
#include <optional>
namespace flitzfs {
struct UniformLz4WorkerStart {
  GpuWorkerStart decision;
  bool source_adopted=false;
};
// Single control thread. Persistent policy, one retiring worker and at most
// one replacement CPU source of no greater size. Caller admits preparation
// before allocating source. No normal operation starts this controller yet.
class UniformLz4WorkerController final {
 public:
  explicit UniformLz4WorkerController(WorkerExecution expected=WorkerExecution::hip);
  UniformLz4WorkerController(const UniformLz4WorkerController&)=delete;
  UniformLz4WorkerController& operator=(const UniformLz4WorkerController&)=delete;
  UniformLz4WorkerController(UniformLz4WorkerController&&)=delete;
  UniformLz4WorkerController& operator=(UniformLz4WorkerController&&)=delete;
  UniformLz4WorkerStart begin(PreparedUniformLz4&&,const std::filesystem::path&,
      const std::vector<std::wstring>&,bool consent,bool worthwhile,
      const GpuHealthEvidence&,std::uint64_t now_ms,const GpuLz4ResidentCost&,
      GpuLz4WindowPolicy,ProcessBudget,std::uint32_t deadline_ms);
  // Verification and acceptance are separate polls. The acceptance poll MUST
  // bring a newer time/snapshot after potentially substantial CPU validation.
  GpuWorkerState poll(const GpuMemorySnapshot&,std::uint64_t now_ms) noexcept;
  // Constant work: select only. No allocation, deallocation, OS query, runtime
  // call, cancellation or waiting. Candidate buffers retire separately.
  std::span<const std::byte> use_cpu_fallback() noexcept;
  bool request_cancel() const noexcept;
  // Explicit maintenance, not called by begin/CPU selection: may validate a
  // late response with one-block scratch. Never replaces terminal CPU bytes.
  bool try_retire() noexcept;
  std::span<const std::byte> source() const noexcept;
  std::span<const std::byte> candidate(std::uint32_t index) const noexcept;
  std::uint32_t count() const noexcept;
  std::uint32_t process_id() const noexcept;
  GpuWorkerState state() const noexcept{return state_;}
  bool awaiting_fresh_observation() const noexcept{return state_==GpuWorkerState::waiting && verified_;}
  GpuGateCode last_gate_code() const noexcept{return last_gate_;}
  unsigned consecutive_failures() const noexcept{return policy_.consecutive_failures();}
 private:
  void report(GpuRecoveryOutcome) noexcept;
  void select_cpu(GpuRecoveryOutcome,GpuGateCode,bool finalize=true) noexcept;
  GpuRecoveryPolicy policy_;
  WorkerExecution expected_;
  GpuWorkerState state_=GpuWorkerState::idle;
  GpuGateCode last_gate_=GpuGateCode::wrong_state;
  std::optional<PreparedUniformLz4> pending_,current_;
  std::optional<WorkerExchangeSession> session_;
  UniformLz4Candidates selected_;
  std::vector<std::uint64_t> offsets_;
  GpuMemorySnapshot baseline_{};
  std::uint64_t ticket_=0,generation_=0,started_=0,deadline_=0,verified_poll_=0;
  bool verified_=false,reported_=false,disabled_=false;
};
}
