#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace gpu_probe_fixture {
inline void check(bool value,const char* message) {
  if(!value)throw std::runtime_error(message);
}
// Shared diagnostic-only budget. No filesystem operation constructs this.
class Budget {
  static constexpr std::uint64_t mib=1024*1024;
 public:
  Budget():Budget(128*mib,128*mib) {}
  Budget(std::uint64_t parent_bytes,std::uint64_t child_bytes) {
    constexpr auto maximum=(std::numeric_limits<SIZE_T>::max)()-512*mib;
    check(parent_bytes && child_bytes && child_bytes<=maximum && parent_bytes<=maximum-child_bytes,
        "invalid probe job commitment");
    const auto job_bytes=parent_bytes+child_bytes;
    MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
    check(GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys>=job_bytes+512*mib &&
        memory.ullAvailPageFile>=job_bytes+512*mib,"probe physical/commit headroom unavailable");
    job_ = CreateJobObjectW(nullptr, nullptr);
    check(job_ != nullptr, "probe job unavailable");
    try {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      // The child has its own creation-time kill-on-close job. This parent
      // budget stays enforced until its member processes exit, including CRT rundown.
      limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY |
          JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
          JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_PROCESS_TIME;
      limits.BasicLimitInformation.ActiveProcessLimit = 2;
      limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
      limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 10LL*10000000;
      limits.ProcessMemoryLimit = static_cast<SIZE_T>((std::max)(parent_bytes,child_bytes));
      limits.JobMemoryLimit = static_cast<SIZE_T>(job_bytes);
      check(SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
          "probe memory/process budget unavailable");
      JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
      cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
      cpu.CpuRate = 2000;
      check(SetInformationJobObject(job_, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)) &&
          AssignProcessToJobObject(job_, GetCurrentProcess()), "probe CPU/job admission failed");
    } catch (...) { CloseHandle(job_); job_ = nullptr; throw; }
  }
  ~Budget() { if (job_) CloseHandle(job_); }
  Budget(const Budget&) = delete;
  Budget& operator=(const Budget&) = delete;
  std::uint64_t peak_commit() const {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
    check(QueryInformationJobObject(job_, JobObjectExtendedLimitInformation,
        &measured, sizeof(measured), nullptr), "probe job accounting unavailable");
    return measured.PeakJobMemoryUsed;
  }
 private:
  HANDLE job_ = nullptr;
};
} // namespace gpu_probe_fixture
