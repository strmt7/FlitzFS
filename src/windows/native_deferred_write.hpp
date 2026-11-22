#pragma once
#include "native_uniform_write.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace flitzfs {
class HipRoutePins;
// Explicit resource-qualified adapter configuration. GPU driver calls belong
// to the separate volume controller, outside the filesystem control lock.
// SDK functions are injected by the actual adapter. The control target uses
// ABI headers without linking the WinFsp runtime.
using NativeWriteHintQuery=bool(*)(UINT64&) noexcept;
using NativeWriteResponseSink=void(*)(FSP_FILE_SYSTEM*,FSP_FSCTL_TRANSACT_RSP&) noexcept;
struct NativeAmdWriteShape {
  std::uint32_t input_bytes=0,isolation_unit=0;
  StoreCompressionOptions foreground;
  bool matches_storage(StoreCompressionOptions selected,std::uint32_t unit) const noexcept {
    return isolation_unit==unit && foreground.mode==selected.mode &&
        foreground.zstd_level==selected.zstd_level && foreground.require_zstd==selected.require_zstd;
  }
};
struct NativeDeferredWriteOptions {
  NativeUniformWorkerBinding binding;
  GpuLz4ResidentCost cost;
  GpuLz4WindowPolicy window;
  ProcessBudget budget;
  std::uint32_t deadline_ms=0;
  WorkerExecution expected=WorkerExecution::hip;
  bool allow_gpu=false,workload_qualified=false,helper_independent=false;
  // Automatic observations use only this qualified, already acquired adapter.
  // The installed helper and runtime pins must outlive the mounted service.
  std::optional<GpuHealthEvidence> controller_health;
  std::shared_ptr<const HipRoutePins> controller_route_pins;
  std::optional<NativeAmdWriteShape> measured_shape;
  NativeWriteHintQuery hint_query=nullptr; // required by the control service
  NativeWriteResponseSink response_sink=nullptr; // required; the adapter supplies SDK defaults
  bool matches_controller_route() const noexcept;
};
struct NativeDeferredWriteStats {
  std::uint64_t queued=0,completed=0,cpu_forced=0,worker_submitted=0,advised=0,response_attempts=0;
  bool native_pending=false,worker_busy=false,stopping=false,thread_exited=false;
  bool retirement_confirmed=true,disabled=false;
};
struct NativeGpuBudgetTask {
  std::uint64_t sequence=0,after_ms=0;
  std::array<std::byte,8> luid{};
  std::uint32_t node_mask=0;
  std::shared_ptr<const HipRoutePins> pins;
};
enum class NativeDeferredStop {running,quiescent,retirement_unconfirmed};
enum class NativeDeferredActivation {busy,refused,active};
// Caller owns the session/control lock for this entire service lifetime.
// Enqueue/drain/stats/publication require that lock. The single owned thread
// alone accesses the GPU coordinator. No callback waits for GPU teardown.
class NativeDeferredWriteService final {
 public:
  NativeDeferredWriteService(NativeFileSession&,std::mutex&,std::atomic<bool>& offline,
      NativeDeferredWriteOptions,NativeFileInfoAllocation=NativeFileInfoAllocation::native_reserved_cache);
  ~NativeDeferredWriteService();
  NativeDeferredWriteService(const NativeDeferredWriteService&)=delete;
  NativeDeferredWriteService& operator=(const NativeDeferredWriteService&)=delete;
  // This allocation-free check binds the whole measured operation to current resources.
  static bool measured_write_funded(const NativeDeferredWriteOptions&,const GpuHealthEvidence&,
      std::uint64_t now) noexcept;
  bool enqueue_locked(FSP_FILE_SYSTEM*,const NativeFileHandle&,std::span<const std::byte>,
      UINT64 offset,bool append,bool constrained);
  void drain_locked(std::unique_lock<std::mutex>&);
  bool publication_pending_locked() const noexcept{return stats_.native_pending;}
  NativeDeferredWriteStats stats_locked() const noexcept{return stats_;}
  // Explicit control-plane observation only; no callback buffer is retained.
  // A new accepted request clears the preceding completion.
  std::optional<NativeUniformWriteResult> completion_receipt_locked() const noexcept{return completion_;}
  void publish_locked(const GpuHealthEvidence&);
  bool controller_collects_budget() const noexcept{return options_.controller_health.has_value();}
  bool budget_demand_pending() const noexcept{return budget_work_.load(std::memory_order_acquire);}
  // The adapter owns this noninheritable signal handle until this service joins.
  void bind_activity_event_locked(HANDLE event) noexcept{activity_event_=event;}
  std::optional<NativeGpuBudgetTask> budget_task_locked() const noexcept;
  bool accept_budget_locked(const NativeGpuBudgetTask&,const GpuMemorySnapshot&,std::uint64_t now) noexcept;
  NativeDeferredStop stop();
 private:
  void request_budget_locked(std::uint64_t after_ms,bool completion) noexcept;
  void disable_gpu_budget_locked() noexcept;
  void signal_activity_locked() noexcept;
  struct Request {
    FSP_FILE_SYSTEM* filesystem=nullptr;UINT64 hint=0;
    NativeFileHandle handle;std::vector<std::byte> bytes;
    UINT64 offset=0;bool append=false,constrained=false;
  };
  static GpuLz4ResidentCost charge_request(GpuLz4ResidentCost) noexcept;
  void run() noexcept;
  NativeFileSession& session_;std::mutex& control_;std::atomic<bool>& offline_;
  const NativeDeferredWriteOptions options_;
  const NativeFileInfoAllocation info_policy_;
  GpuHealthEvidence health_{};
  HANDLE activity_event_=nullptr;
  std::optional<NativeGpuBudgetTask> budget_task_;
  std::atomic<bool> budget_work_{false};
  std::uint64_t budget_sequence_=0,last_budget_probe_=0;
  bool budget_for_completion_=false,budget_disabled_=false;
  std::condition_variable changed_;
  std::unique_ptr<Request> request_;
  NativeDeferredWriteStats stats_;
  std::optional<NativeUniformWriteResult> completion_;
  std::atomic_bool force_cpu_{false};
  std::thread thread_;
};
}
