#pragma once
#include "native_uniform_write.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
namespace flitzfs {
// Explicit resource-qualified adapter configuration. Evidence is supplied separately;
// neither callbacks nor this service discover devices or query GPU drivers.
// SDK functions are injected by the actual adapter. The control target uses
// ABI headers without linking the WinFsp runtime.
using NativeWriteHintQuery=bool(*)(UINT64&) noexcept;
using NativeWriteResponseSink=void(*)(FSP_FILE_SYSTEM*,FSP_FSCTL_TRANSACT_RSP&) noexcept;
struct NativeDeferredWriteOptions {
  NativeUniformWorkerBinding binding;
  GpuLz4ResidentCost cost;
  GpuLz4WindowPolicy window;
  ProcessBudget budget;
  std::uint32_t deadline_ms=0;
  WorkerExecution expected=WorkerExecution::hip;
  bool allow_gpu=false,workload_qualified=false,helper_independent=false;
  NativeWriteHintQuery hint_query=nullptr; // required by the control service
  NativeWriteResponseSink response_sink=nullptr; // required; the adapter supplies SDK defaults
};
struct NativeDeferredWriteStats {
  std::uint64_t queued=0,completed=0,cpu_forced=0,worker_submitted=0,advised=0,response_attempts=0;
  bool native_pending=false,worker_busy=false,stopping=false,thread_exited=false;
  bool retirement_confirmed=true,disabled=false;
};
enum class NativeDeferredStop {running,quiescent,retirement_unconfirmed};
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
  bool enqueue_locked(FSP_FILE_SYSTEM*,const NativeFileHandle&,std::span<const std::byte>,
      UINT64 offset,bool append,bool constrained);
  void drain_locked(std::unique_lock<std::mutex>&);
  bool publication_pending_locked() const noexcept{return stats_.native_pending;}
  NativeDeferredWriteStats stats_locked() const noexcept{return stats_;}
  // Explicit control-plane observation only; no callback buffer is retained.
  // A new accepted request clears the preceding completion.
  std::optional<NativeUniformWriteResult> completion_receipt_locked() const noexcept{return completion_;}
  void publish_locked(const GpuHealthEvidence&);
  NativeDeferredStop stop();
 private:
  struct Request {
    FSP_FILE_SYSTEM* filesystem=nullptr;UINT64 hint=0;
    NativeFileHandle handle;std::vector<std::byte> bytes;
    UINT64 offset=0;bool append=false,constrained=false;
  };
  void run() noexcept;
  NativeFileSession& session_;std::mutex& control_;std::atomic<bool>& offline_;
  const NativeDeferredWriteOptions options_;
  const NativeFileInfoAllocation info_policy_;
  GpuHealthEvidence health_{};
  std::condition_variable changed_;
  std::unique_ptr<Request> request_;
  NativeDeferredWriteStats stats_;
  std::optional<NativeUniformWriteResult> completion_;
  bool force_cpu_=false;
  std::thread thread_;
};
}
