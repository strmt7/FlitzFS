#pragma once
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <limits>
namespace flitzfs::app_fixture {
// Application qualification uses current host headroom, including sanitizer
// overhead. This is not a GPU experiment or a product memory allocation limit.
inline std::uint64_t additional_memory_allowance() {
  constexpr std::uint64_t mib=1024ULL*1024;
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  if(!GlobalMemoryStatusEx(&m) || !m.ullTotalPhys || m.ullAvailPhys>m.ullTotalPhys ||
      !m.ullTotalPageFile || m.ullAvailPageFile>m.ullTotalPageFile)
    throw std::runtime_error("Application test memory telemetry unavailable");
  const auto reserve=(std::max)(768*mib,m.ullTotalPhys/8);
  if(m.ullAvailPhys<=reserve || m.ullAvailPageFile<=reserve)
    throw std::runtime_error("Application test memory reserve unavailable");
  // This process-level qualification envelope includes sanitizer shadow and
  // default quarantine growth, separate from the much smaller volume domain.
  const auto allowed=(std::min)(m.ullAvailPhys-reserve,m.ullAvailPageFile-reserve)/8/mib*mib;
  if(allowed<128*mib)throw std::runtime_error("Insufficient headroom for application qualification");
  return allowed;
}
inline std::uint64_t current_private_bytes() {
  PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
  if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)) ||
      !memory.PrivateUsage)throw std::runtime_error("Application process commit telemetry unavailable");
  return memory.PrivateUsage;
}
class Budget final {
 public:
  explicit Budget(bool report_memory=true):report_memory_(report_memory) {
    // Available headroom already excludes this running process's CRT/sanitizer
    // commit. A total-process quota must include that measured baseline too.
    const auto baseline=current_private_bytes(),additional=additional_memory_allowance();
    if(baseline>(std::numeric_limits<SIZE_T>::max)()-additional)
      throw std::runtime_error("Application process commit budget overflow");
    memory_=baseline+additional;job_=CreateJobObjectW(nullptr,nullptr);
    if(report_memory_)std::cout<<"Application fixture memory allowance: "<<memory_<<" bytes; existing_private="
        <<baseline<<" additional_headroom="<<additional<<std::endl;
    if(!job_)throw std::runtime_error("Application test job unavailable");
    try {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|
          JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PRIORITY_CLASS|JOB_OBJECT_LIMIT_PROCESS_TIME;
      limits.BasicLimitInformation.ActiveProcessLimit=2;
      limits.BasicLimitInformation.PriorityClass=BELOW_NORMAL_PRIORITY_CLASS;
      limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart=10LL*10000000;
      limits.ProcessMemoryLimit=static_cast<SIZE_T>(memory_);limits.JobMemoryLimit=static_cast<SIZE_T>(memory_);
      if(!SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
        throw std::runtime_error("Application test memory/job admission failed");
      JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
      cpu.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;cpu.CpuRate=2000;
      if(!SetInformationJobObject(job_,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu)) ||
          !AssignProcessToJobObject(job_,GetCurrentProcess()))throw std::runtime_error("Application test CPU/job admission failed");
    }catch(...){CloseHandle(job_);job_=nullptr;throw;}
  }
  ~Budget(){if(job_){JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
    if(report_memory_ && QueryInformationJobObject(job_,JobObjectExtendedLimitInformation,&measured,sizeof(measured),nullptr))
      std::cout<<"Application fixture peak commit: "<<measured.PeakJobMemoryUsed<<" bytes\n";
    CloseHandle(job_);}}
  Budget(const Budget&)=delete;
  Budget& operator=(const Budget&)=delete;
  std::uint64_t child_memory_mib(std::uint64_t minimum=128) const {
    constexpr std::uint64_t mib=1024ULL*1024;
    const auto current=current_private_bytes();
    if(current>=memory_)throw std::runtime_error("No aggregate application budget for GUI child");
    // Retain half of current remaining aggregate capacity for the parent.
    // Fresh headroom separately governs the newly created child's whole budget.
    const auto remaining=memory_-current;
    const auto allowed=(std::min)(remaining/2,additional_memory_allowance()/2)/mib;
    if(allowed<minimum)throw std::runtime_error("Insufficient aggregate budget for GUI child");
    return allowed;
  }
 private:
  HANDLE job_=nullptr;
  std::uint64_t memory_=0;
  bool report_memory_;
};
}
