#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <iostream>
namespace flitzfs::app_fixture {
// Application qualification uses current host headroom, including sanitizer
// overhead. This is not a GPU experiment or a product memory allocation limit.
inline std::uint64_t memory_allowance() {
  constexpr std::uint64_t mib=1024ULL*1024;
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  if(!GlobalMemoryStatusEx(&m) || !m.ullTotalPhys || m.ullAvailPhys>m.ullTotalPhys ||
      !m.ullTotalPageFile || m.ullAvailPageFile>m.ullTotalPageFile)
    throw std::runtime_error("Application test memory telemetry unavailable");
  const auto reserve=(std::max)(768*mib,m.ullTotalPhys/8);
  if(m.ullAvailPhys<=reserve || m.ullAvailPageFile<=reserve)
    throw std::runtime_error("Application test memory reserve unavailable");
  const auto allowed=(std::min)(m.ullAvailPhys-reserve,m.ullAvailPageFile-reserve)/16/mib*mib;
  if(allowed<128*mib)throw std::runtime_error("Insufficient headroom for application qualification");
  return allowed;
}
class Budget final {
 public:
  Budget() {
    const auto memory=memory_allowance();job_=CreateJobObjectW(nullptr,nullptr);
    std::cout<<"Application fixture memory allowance: "<<memory<<" bytes from fresh reserved headroom\n";
    if(!job_)throw std::runtime_error("Application test job unavailable");
    try {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
      limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|
          JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PRIORITY_CLASS|JOB_OBJECT_LIMIT_PROCESS_TIME;
      limits.BasicLimitInformation.ActiveProcessLimit=2;
      limits.BasicLimitInformation.PriorityClass=BELOW_NORMAL_PRIORITY_CLASS;
      limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart=10LL*10000000;
      limits.ProcessMemoryLimit=static_cast<SIZE_T>(memory);limits.JobMemoryLimit=static_cast<SIZE_T>(memory);
      if(!SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
        throw std::runtime_error("Application test memory/job admission failed");
      JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
      cpu.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;cpu.CpuRate=2000;
      if(!SetInformationJobObject(job_,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu)) ||
          !AssignProcessToJobObject(job_,GetCurrentProcess()))throw std::runtime_error("Application test CPU/job admission failed");
    }catch(...){CloseHandle(job_);job_=nullptr;throw;}
  }
  ~Budget(){if(job_){JOBOBJECT_EXTENDED_LIMIT_INFORMATION measured{};
    if(QueryInformationJobObject(job_,JobObjectExtendedLimitInformation,&measured,sizeof(measured),nullptr))
      std::cout<<"Application fixture peak commit: "<<measured.PeakJobMemoryUsed<<" bytes\n";
    CloseHandle(job_);}}
  Budget(const Budget&)=delete;
  Budget& operator=(const Budget&)=delete;
 private:
  HANDLE job_=nullptr;
};
}
