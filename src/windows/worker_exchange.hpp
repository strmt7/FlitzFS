#pragma once
#include "bounded_process.hpp"
#include "flitzfs/worker_wire.hpp"
#include <memory>
#include <exception>
#include <span>
#include <string_view>

namespace flitzfs {
enum class WorkerExchangeCode { completed, worker_failed, timed_out, cancelled, exit_unconfirmed, invalid_response, supervisor_failed, supervisor_exit_unconfirmed };
struct WorkerExchangeResult {
  WorkerExchangeCode code = WorkerExchangeCode::invalid_response;
  ProcessResult process;
  bool publication_observed = false; // Diagnostic only, sampled after confirmed exit; never acceptance.
  std::wstring channel_token; // Nonsecret correlation ID; objects must be gone after confirmed exit.
  std::vector<std::byte> response;
};
// One trusted helper, one exchange, one process-wide in-flight exchange. Reuses
// creation-time Job Object limits. No inherited handles, shell, files or HIP.
// Returns response bytes only AFTER a successful, confirmed child exit and
// bounded transport checks. Caller must still use accept_worker_reply.
// Named pagefile-backed objects use random local-session names and a protected
// current-user DACL. This is not a sandbox against other same-user processes.
WorkerExchangeResult run_worker_exchange(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments, std::span<const std::byte> request,
    ProcessBudget budget = {});

// Explicit asynchronous transport for a supervisor/controller, never an
// automatic filesystem/driver route. Polling results and selecting a separate
// GpuResultGate CPU fallback do not wait for or clean up a helper.
// Abandoning this object detaches ownership, NOT the process: the bounded
// supervisor retains request/mappings/process identity and the admission lease
// until exact-child retirement. No second session/thread is queued.
class WorkerExchangeSession final {
 public:
  static WorkerExchangeSession start(const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments, std::span<const std::byte> request,
      ProcessBudget budget = {});
  ~WorkerExchangeSession();
  WorkerExchangeSession(WorkerExchangeSession&&) noexcept;
  WorkerExchangeSession& operator=(WorkerExchangeSession&&) noexcept;
  WorkerExchangeSession(const WorkerExchangeSession&) = delete;
  WorkerExchangeSession& operator=(const WorkerExchangeSession&) = delete;
  // Immutable after publication. nullptr means pending; a ready reply flag
  // inside the child mapping never authorizes a live-process result.
  const WorkerExchangeResult* try_result() const noexcept;
  std::exception_ptr failure() const noexcept;
  // Confirm BOTH exact child and supervisor-thread exit; zero-time observation.
  bool retirement_confirmed() const noexcept;
  std::uint32_t process_id() const noexcept;
  // Signals only. TerminateJobObject and exit waits run on the supervisor.
  bool request_cancel() const noexcept;
  // Diagnostic/controller admission only; zero-time thread observation, no
  // blocking wait, discovery scan or automatic retry.
  static bool available() noexcept;
 private:
  struct Impl;
  explicit WorkerExchangeSession(std::unique_ptr<Impl>) noexcept;
  WorkerExchangeResult wait_result(); // Legacy synchronous adapter only.
  friend WorkerExchangeResult run_worker_exchange(const std::filesystem::path&,
      const std::vector<std::wstring>&, std::span<const std::byte>, ProcessBudget);
  std::unique_ptr<Impl> impl_;
};

// Child endpoint. The parent appends --worker-channel TOKEN to the supplied
// helper arguments. TOKEN is exactly 32 lowercase hex digits, not a pathname.
// Request bytes are copied into private memory; its shared view is read-only.
// Publish once, then exit successfully. The parent never trusts a ready flag
// alone or reads a response while the child can still mutate it.
class WorkerChannel final {
 public:
  static WorkerChannel open(std::wstring_view token);
  ~WorkerChannel();
  WorkerChannel(WorkerChannel&&) noexcept;
  WorkerChannel& operator=(WorkerChannel&&) noexcept;
  WorkerChannel(const WorkerChannel&) = delete;
  WorkerChannel& operator=(const WorkerChannel&) = delete;
  std::span<const std::byte> request() const noexcept;
  void publish(std::span<const std::byte> response);
 private:
  struct Impl;
  explicit WorkerChannel(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
