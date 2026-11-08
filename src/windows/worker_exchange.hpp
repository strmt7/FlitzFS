#pragma once
#include "bounded_process.hpp"
#include "flitzfs/worker_wire.hpp"
#include <memory>
#include <span>
#include <string_view>

namespace flitzfs {
enum class WorkerExchangeCode { completed, worker_failed, timed_out, exit_unconfirmed, invalid_response };
struct WorkerExchangeResult {
  WorkerExchangeCode code = WorkerExchangeCode::invalid_response;
  ProcessResult process;
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
