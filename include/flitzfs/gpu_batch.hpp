#pragma once
#include "flitzfs/gpu_protocol.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace flitzfs {
inline constexpr std::size_t kGpuMaxBlocks=8;
inline constexpr std::uint64_t kGpuBudgetReserveBytes=256ULL*1024*1024;
inline constexpr std::uint64_t kGpuMaxBudgetAgeMs=50;
struct GpuBlockView {
  std::uint8_t kind=0, fill=0;
  std::uint32_t decoded_bytes=0;
  std::span<const std::byte> payload;
};
// Fully owned immutable payload/tasks/reference, validated by the independent
// CPU decoder. Preparation neither queries the OS nor initializes a GPU.
class PreparedGpuBatch final {
 public:
  static PreparedGpuBatch prepare(std::span<const GpuBlockView> blocks);
  std::span<const std::byte> payload() const noexcept { return payload_; }
  std::span<const gpu_detail::Task> tasks() const noexcept { return tasks_; }
  std::span<const std::byte> cpu_reference() const noexcept { return reference_; }
  // Explicit buffers only; excludes runtime/driver/module overhead.
  std::uint64_t device_buffer_bytes() const noexcept;
 private:
  std::vector<std::byte> payload_, reference_;
  std::vector<gpu_detail::Task> tasks_;
};
// This represents DXGI's process budget, NOT global physical free VRAM.
// A failed collector leaves valid=false; identity comes from a verified adapter.
struct GpuMemorySnapshot {
  std::array<std::byte,8> luid{};
  std::uint32_t node_mask=0;
  std::uint64_t captured_ms=0, dedicated_bytes=0;
  std::uint64_t local_budget=0, local_usage=0, nonlocal_usage=0;
  bool valid=false, amd_hardware=false;
  std::uint64_t nonlocal_budget=0;
  // GlobalMemoryStatusEx observations from the allocating process. Commit is
  // process-constrained capacity, NOT system-wide free memory or a reservation.
  // Zero/default fields are incomplete telemetry and must fail admission.
  std::uint64_t physical_total=0, physical_available=0;
  std::uint64_t commit_limit=0, commit_available=0;
};
enum class GpuGateCode {
  admitted, accepted, no_consent, invalid_budget, stale_budget, wrong_adapter,
  insufficient_headroom, nonlocal_growth, wrong_state, expired, stale_result,
  invalid_output, task_failed, mismatch, worker_failed
};
enum class GpuGateState { idle, waiting, accepted, poisoned };
// Pure admission calculation; no OS query, allocation or runtime initialization.
GpuGateCode check_gpu_memory_budget(const GpuMemorySnapshot& snapshot,
    std::array<std::byte,8> luid, std::uint32_t node_mask,
    std::uint64_t now_ms, std::uint64_t additional_bytes) noexcept;
// Pure, allocation-free resource envelope. Divisors are conservative experimental
// interference controls, not learned optimal settings or hardware assumptions.
struct GpuMemoryEnvelope {
  GpuGateCode code=GpuGateCode::invalid_budget;
  std::uint64_t local_reserve=0, physical_reserve=0, commit_reserve=0;
  std::uint64_t nonlocal_growth=0;
};
GpuMemoryEnvelope gpu_memory_envelope(const GpuMemorySnapshot&,
    std::uint64_t additional_bytes=0) noexcept;
// Compare against the ORIGINAL admitted snapshot, never a moving baseline.
// Re-evaluate both envelopes so falling headroom cannot retain an old allowance.
GpuGateCode check_gpu_memory_transition(const GpuMemorySnapshot& before,
    const GpuMemorySnapshot& after,std::array<std::byte,8> luid,
    std::uint32_t node_mask,std::uint64_t now_ms,std::uint64_t device_bytes) noexcept;
struct GpuAttempt { GpuGateCode code; std::uint64_t generation=0; };
struct GpuBatchReply {
  std::uint64_t generation=0;
  std::vector<std::byte> bytes;
  std::vector<std::uint32_t> status;
};
// One control thread, one outstanding request. This is a result-admission
// gate, not a GPU runtime or a timer thread. A supervisor must stop a worker.
// No destructor or failure path performs a driver operation.
class GpuResultGate final {
 public:
  GpuResultGate(PreparedGpuBatch batch, std::array<std::byte,8> luid,
      std::uint32_t node_mask, std::uint32_t deadline_ms=100);
  GpuResultGate(const GpuResultGate&)=delete;
  GpuResultGate& operator=(const GpuResultGate&)=delete;
  GpuResultGate(GpuResultGate&&)=delete;
  GpuResultGate& operator=(GpuResultGate&&)=delete;
  const PreparedGpuBatch& batch() const noexcept { return batch_; }
  GpuAttempt begin(bool explicit_consent, const GpuMemorySnapshot& before,
      std::uint64_t now_ms) noexcept;
  GpuGateCode complete(GpuBatchReply reply, const GpuMemorySnapshot& after,
      std::uint64_t now_ms) noexcept;
  // Either failure terminally poisons the current gate. Never call runtime
  // cleanup here: it could block. Late results are discarded, not published.
  GpuGateCode expire(std::uint64_t now_ms) noexcept;
  GpuGateCode worker_failed() noexcept;
  // Immediately select the independently decoded CPU bytes, terminally rejecting
  // every late candidate. This never waits for or cleans up a GPU/driver.
  // The caller must still supervise/reap a worker independently.
  std::span<const std::byte> use_cpu_fallback() noexcept;
  GpuGateState state() const noexcept { return state_; }
  std::span<const std::byte> accepted_bytes() const noexcept;
 private:
  GpuGateCode budget(const GpuMemorySnapshot&,std::uint64_t,std::uint64_t) const noexcept;
  GpuGateCode poison(GpuGateCode) noexcept;
  PreparedGpuBatch batch_;
  std::array<std::byte,8> luid_;
  std::uint32_t node_mask_, deadline_ms_;
  std::uint64_t generation_=0, started_=0, deadline_=0;
  GpuMemorySnapshot baseline_;
  GpuGateState state_=GpuGateState::idle;
  std::vector<std::byte> accepted_;
};
} // namespace flitzfs
