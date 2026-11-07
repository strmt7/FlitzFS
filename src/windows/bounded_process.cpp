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
ProcessResult run_bounded_process(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, ProcessBudget budget) {
  if (!valid_process_budget(budget)) throw std::invalid_argument("unsafe process budget");
  auto command = windows_child_command(executable.native(), arguments);
  const auto memory_bytes = budget.memory_mib * 1024U * 1024U;
  MEMORYSTATUSEX available{}; available.dwLength = sizeof(available);
  if (!GlobalMemoryStatusEx(&available)) failed("memory headroom query");
  constexpr ULONGLONG reserve = 512ULL * 1024 * 1024;
  if (available.ullAvailPhys < memory_bytes + reserve ||
      available.ullAvailPageFile < memory_bytes + reserve)
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
  Handle child(process.hProcess), thread(process.hThread);
  // Closing the non-inherited job on any exceptional path terminates its child.
  BOOL assigned = FALSE;
  if (!IsProcessInJob(child.value, job.value, &assigned) || !assigned)
    failed("creation-time job verification", ERROR_ACCESS_DENIED);
  if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) failed("bounded child resume");
  ProcessResult result;
  result.process_id = process.dwProcessId;
  auto wait = WaitForSingleObject(child.value, budget.timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    result.timed_out = true;
    if (!TerminateJobObject(job.value, ERROR_TIMEOUT)) failed("bounded job termination");
    wait = WaitForSingleObject(child.value, 2000);
  }
  if (wait == WAIT_FAILED) failed("bounded child wait");
  result.exit_confirmed = wait == WAIT_OBJECT_0;
  DWORD exit_code = 0;
  if (result.exit_confirmed && !GetExitCodeProcess(child.value, &exit_code))
    failed("bounded child exit status");
  result.exit_code = exit_code;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
  if (!QueryInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &measured, sizeof(measured), nullptr)) failed("bounded child accounting");
  result.peak_committed_bytes = measured.PeakJobMemoryUsed;
  return result;
}
} // namespace flitzfs
