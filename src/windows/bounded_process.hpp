#pragma once
#include "flitzfs/process_budget.hpp"
#include <filesystem>
#include <atomic>
#include <memory>

namespace flitzfs {
struct ProcessResult;
enum class ProcessObservation { not_started, running, exited, wait_failed };
// One exact process object, retained independently of the launching call. No PID
// reopening/reuse, driver cleanup or process termination occurs in observation.
// request_stop only signals an unnamed event; the SUPERVISOR performs teardown.
class ProcessObserver final {
 public:
  ProcessObserver();
  ~ProcessObserver();
  ProcessObserver(const ProcessObserver&) = delete;
  ProcessObserver& operator=(const ProcessObserver&) = delete;
  std::uint32_t process_id() const noexcept { return process_id_.load(std::memory_order_acquire); }
  bool request_stop() const noexcept;
  // Supervisor-only; at most 2000 ms per call. Zero is a nonblocking observation.
  ProcessObservation wait_for_exit(std::uint32_t milliseconds = 0) const noexcept;
 private:
  friend ProcessResult run_bounded_process(const std::filesystem::path&,
      const std::vector<std::wstring>&, ProcessBudget, const std::shared_ptr<ProcessObserver>&);
  void* process_ = nullptr;
  void* stop_ = nullptr;
  std::atomic<std::uint32_t> process_id_{0};
  std::atomic<bool> claimed_{false};
};
struct ProcessResult {
  std::uint32_t process_id = 0; // identity of the launched helper, not a live process handle
  std::uint32_t exit_code = 0;
  bool timed_out = false;
  bool cancelled = false;
  bool exit_confirmed = false;
  std::size_t peak_committed_bytes = 0;
};
// Run one trusted CPU helper inside a Windows Job Object. Creation-time job
// assignment closes the create-then-assign orphan window. No handle inheritance,
// shell, elevation, driver activity or fallback that removes a failed limit.
// Failures throw; timeout requests termination and waits at most 2s for exit.
// This is NOT a security sandbox or protection against kernel/driver hangs.
ProcessResult run_bounded_process(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, ProcessBudget budget = {},
    const std::shared_ptr<ProcessObserver>& observer = {});
} // namespace flitzfs
