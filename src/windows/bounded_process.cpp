#include "bounded_process.hpp"
#include <windows.h>
#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace flitzfs {
namespace {
struct Handle {
  HANDLE value = nullptr;
  explicit Handle(HANDLE h = nullptr) : value(h) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};
[[noreturn]] void failed(const char* operation, DWORD code = GetLastError()) {
  throw std::system_error(static_cast<int>(code), std::system_category(), operation);
}
struct Attributes {
  std::vector<std::byte> bytes;
  LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
  Attributes() {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size || size > 65536)
      failed("attribute list sizing");
    bytes.resize(size);
    auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
    if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size)) failed("attribute list initialization");
    list = candidate;
  }
  ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
  Attributes(const Attributes&) = delete;
  Attributes& operator=(const Attributes&) = delete;
};
}
ProcessObserver::ProcessObserver() {
  stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop_) failed("process cancellation event");
}
ProcessObserver::~ProcessObserver() {
  if (process_) CloseHandle(static_cast<HANDLE>(process_));
  if (stop_) CloseHandle(static_cast<HANDLE>(stop_));
}
bool ProcessObserver::request_stop() const noexcept {
  return SetEvent(static_cast<HANDLE>(stop_)) != FALSE;
}
ProcessObservation ProcessObserver::wait_for_exit(std::uint32_t milliseconds) const noexcept {
  if (!process_id()) return ProcessObservation::not_started;
  if (milliseconds > 2000) return ProcessObservation::wait_failed;
  const auto wait = WaitForSingleObject(static_cast<HANDLE>(process_), milliseconds);
  if (wait == WAIT_OBJECT_0) return ProcessObservation::exited;
  if (wait == WAIT_TIMEOUT) return ProcessObservation::running;
  return ProcessObservation::wait_failed;
}
ProcessResult run_bounded_process(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, ProcessBudget budget,
    const std::shared_ptr<ProcessObserver>& supplied_observer) {
  if (!valid_process_budget(budget)) throw std::invalid_argument("unsafe process budget");
  auto command = windows_child_command(executable.native(), arguments);
  const auto observer = supplied_observer ? supplied_observer : std::make_shared<ProcessObserver>();
  if (observer->claimed_.exchange(true))
    throw std::invalid_argument("process observer may be used for only one launch");
  const auto memory_bytes = budget.memory_mib * 1024ULL * 1024ULL;
  MEMORYSTATUSEX available{}; available.dwLength = sizeof(available);
  if (!GlobalMemoryStatusEx(&available)) failed("memory headroom query");
  constexpr ULONGLONG reserve = 512ULL * 1024 * 1024;
  if (available.ullAvailPhys <= reserve || available.ullAvailPageFile <= reserve ||
      memory_bytes > available.ullAvailPhys-reserve || memory_bytes > available.ullAvailPageFile-reserve ||
      (budget.memory_share_divisor &&
       (memory_bytes > (available.ullAvailPhys-reserve)/budget.memory_share_divisor ||
        memory_bytes > (available.ullAvailPageFile-reserve)/budget.memory_share_divisor)))
    throw std::runtime_error("insufficient memory headroom for bounded child");
  const auto attributes = GetFileAttributesW(executable.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) failed("child executable attributes");
  if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::invalid_argument("child executable must be an ordinary file");
  Handle job(CreateJobObjectW(nullptr, nullptr));
  if (!job.value) failed("job creation");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
      JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
  limits.ProcessMemoryLimit = limits.JobMemoryLimit = memory_bytes;
  if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    failed("memory/process job limits");
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
  cpu.CpuRate = budget.cpu_rate;
  if (!SetInformationJobObject(job.value, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)))
    failed("CPU job limit");
  Attributes attribute_list;
  if (!UpdateProcThreadAttribute(attribute_list.list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
        &job.value, sizeof(job.value), nullptr, nullptr)) failed("creation-time job assignment");
  STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attribute_list.list;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW |
        CREATE_UNICODE_ENVIRONMENT | BELOW_NORMAL_PRIORITY_CLASS,
        nullptr, nullptr, &startup.StartupInfo, &process)) failed("bounded child creation");
  // Storage/event ownership was allocated before CreateProcess. Publish the PID
  // only after retaining its exact process HANDLE; it never authorizes PID reopen.
  observer->process_ = process.hProcess;
  observer->process_id_.store(process.dwProcessId, std::memory_order_release);
  const auto child = static_cast<HANDLE>(observer->process_);
  Handle thread(process.hThread);
  // Closing the non-inherited job on any exceptional path terminates its child.
  BOOL assigned = FALSE;
  if (!IsProcessInJob(child, job.value, &assigned) || !assigned)
    failed("creation-time job verification", ERROR_ACCESS_DENIED);
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) failed("bounded child resume");
  ProcessResult result;
  result.process_id = process.dwProcessId;
  const HANDLE waits[] = {child, static_cast<HANDLE>(observer->stop_)};
  auto wait = WaitForMultipleObjects(2, waits, FALSE, budget.timeout_ms);
  if (wait == WAIT_TIMEOUT || wait == WAIT_OBJECT_0 + 1) {
    result.timed_out = wait == WAIT_TIMEOUT;
    result.cancelled = !result.timed_out;
    if (!TerminateJobObject(job.value, result.cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT))
      failed("bounded job termination");
    wait = WaitForSingleObject(child, 2000);
  }
  // Cancellation racing a clean exit must not authorize a response after the
  // caller selected CPU fallback. No termination is needed for an exited child.
  if (WaitForSingleObject(static_cast<HANDLE>(observer->stop_), 0) == WAIT_OBJECT_0)
    result.cancelled = true;
  if (wait == WAIT_FAILED) failed("bounded child wait");
  result.exit_confirmed = wait == WAIT_OBJECT_0;
  DWORD exit_code = 0;
  if (result.exit_confirmed && !GetExitCodeProcess(child, &exit_code))
    failed("bounded child exit status");
  result.exit_code = exit_code;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
  if (!QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &measured, sizeof(measured), nullptr)) failed("bounded child accounting");
  result.peak_committed_bytes = measured.PeakJobMemoryUsed;
  return result;
}
} // namespace flitzfs
