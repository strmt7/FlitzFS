#pragma once
#include "bounded_process.hpp"
#include "flitzfs/worker_wire.hpp"
#include <memory>
#include <exception>
#include <span>
#include <stop_token>
#include <string_view>

namespace flitzfs {
// Explicit sized transport; byte bounds come from the caller's resource-sized
// window. Legacy entry points keep their original fixed framing unchanged.
struct WorkerChannelCapacity {
  std::size_t request_bytes=0,reply_bytes=0;
};
inline constexpr std::wstring_view kPreparedWorkerChannelArgument=L"--prepared-worker-channel";
bool valid_worker_channel_capacity(WorkerChannelCapacity,ProcessBudget) noexcept;
// Minimum available physical and commit bytes at sized channel admission.
// A caller can check this before allocating its input. Zero means invalid.
std::uint64_t required_sized_worker_headroom(WorkerChannelCapacity,ProcessBudget) noexcept;
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
    ProcessBudget budget = {},std::stop_token stop={},std::shared_ptr<const void> pinned_route={});

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
  static WorkerExchangeSession start_sized(const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments, std::span<const std::byte> request,
      WorkerChannelCapacity capacity, ProcessBudget budget);
  static WorkerExchangeSession start_sized(const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments, SharedWorkerRequest request,
      WorkerChannelCapacity capacity, ProcessBudget budget,
      std::shared_ptr<const void> pinned_route={});
  // Starts one bounded child with a fixed frame size and no request contents.
  // Only the single controlling owner may wait for readiness or provide the frame.
  static WorkerExchangeSession start_prepared_sized(const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments, WorkerChannelCapacity capacity, ProcessBudget budget,
      std::shared_ptr<const void> pinned_route={});
  bool wait_prepared(std::uint32_t milliseconds=0) const noexcept;
  bool submit_prepared(SharedWorkerRequest) noexcept;
  // For a separate qualifying thread. Stops through the owned supervisor
  // and returns bytes only after exact worker and supervisor retirement.
  static WorkerExchangeResult run_sized(const std::filesystem::path& helper,
      const std::vector<std::wstring>& arguments, SharedWorkerRequest request,
      WorkerChannelCapacity capacity, ProcessBudget budget,std::stop_token,
      std::shared_ptr<const void> pinned_route={});
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
  // Borrow the exact supervisor handle while this serialized owner keeps the
  // session alive. A signalled handle still requires retirement_confirmed().
  void* supervisor_wait_handle() const noexcept;
  std::uint32_t process_id() const noexcept;
  // Signals only. TerminateJobObject and exit waits run on the supervisor.
  bool request_cancel() const noexcept;
  // Diagnostic/controller admission only; zero-time thread observation, no
  // blocking wait, discovery scan or automatic retry.
  static bool available() noexcept;
 private:
  static WorkerExchangeSession start_impl(const std::filesystem::path&,
      const std::vector<std::wstring>&,std::span<const std::byte>,
      WorkerChannelCapacity,ProcessBudget,bool sized,SharedWorkerRequest snapshot={},
      std::shared_ptr<const void> pinned_route={},bool prepared=false);
  struct Impl;
  explicit WorkerExchangeSession(std::unique_ptr<Impl>) noexcept;
  WorkerExchangeResult wait_result(); // Legacy synchronous adapter only.
  friend WorkerExchangeResult run_worker_exchange(const std::filesystem::path&,
      const std::vector<std::wstring>&, std::span<const std::byte>, ProcessBudget,std::stop_token,std::shared_ptr<const void>);
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
  // The trusted child must independently obtain its exact window capacities.
  // Requires an already enforced bounded single-process Job Object; it never
  // infers arbitrary allocation sizes from writable shared metadata.
  static WorkerChannel open_sized(std::wstring_view token,WorkerChannelCapacity);
  static WorkerChannel open_prepared_sized(std::wstring_view token,WorkerChannelCapacity,
      std::uint32_t timeout_ms);
  // The child signals readiness only after its own setup succeeds. It then
  // waits once for the full frame and copies it into its private buffer.
  bool receive_prepared();
  ~WorkerChannel();
  WorkerChannel(WorkerChannel&&) noexcept;
  WorkerChannel& operator=(WorkerChannel&&) noexcept;
  WorkerChannel(const WorkerChannel&) = delete;
  WorkerChannel& operator=(const WorkerChannel&) = delete;
  std::span<const std::byte> request() const noexcept;
  // Transfer the verified-length private allocation. Future request() calls
  // are empty, and the channel still owns its reply publication endpoint.
  SharedWorkerRequest take_request_snapshot();
  void publish(std::span<const std::byte> response);
 private:
  struct Impl;
  explicit WorkerChannel(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
