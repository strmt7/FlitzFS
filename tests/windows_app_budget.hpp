#pragma once
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <iostream>
#include <limits>
#include <optional>
namespace flitzfs::app_fixture {
// Application qualification uses current host headroom, including sanitizer
// overhead. This is not a GPU experiment or a product memory allocation limit.
inline std::uint64_t additional_memory_allowance(std::optional<std::uint64_t> requested={},bool enforced=false) {
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
  const auto headroom=(std::min)(m.ullAvailPhys-reserve,m.ullAvailPageFile-reserve);
  // The supervisor may report its allocation through a launcher such as
  // CTest. Keep the request within a fresh share of unreserved memory.
  const auto available=requested?(std::min)(enforced?headroom:headroom/4,*requested):headroom/8;
  const auto allowed=available/mib*mib;
  if(allowed<128*mib)throw std::runtime_error("Insufficient headroom for application qualification");
  return allowed;
}
inline std::uint64_t current_private_bytes() {
  PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
  if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)) ||
      !memory.PrivateUsage)throw std::runtime_error("Application process commit telemetry unavailable");
  return memory.PrivateUsage;
}
inline std::optional<std::uint64_t> enclosing_memory_limit() noexcept {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if(!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&limits,sizeof(limits),nullptr) ||
      !(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_JOB_MEMORY) || !limits.JobMemoryLimit)return {};
  auto limit=static_cast<std::uint64_t>(limits.JobMemoryLimit);
  if(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_PROCESS_MEMORY)
    limit=(std::min)(limit,static_cast<std::uint64_t>(limits.ProcessMemoryLimit));
  return limit;
}
inline std::optional<std::uint64_t> requested_memory_limit() noexcept {
  BOOL assigned=FALSE;
  if(!IsProcessInJob(GetCurrentProcess(),nullptr,&assigned) || !assigned)return {};
  std::array<wchar_t,24> buffer{};
  const auto length=GetEnvironmentVariableW(L"FLITZFS_VALIDATION_MEMORY_BYTES",buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if(!length || length>=buffer.size())return {};
  std::uint64_t bytes=0;
  for(DWORD i=0;i<length;++i) {
    const auto ch=buffer[i];
    if(ch<L'0' || ch>L'9')return {};
    const auto digit=static_cast<std::uint64_t>(ch-L'0');
    if(bytes>((std::numeric_limits<std::uint64_t>::max)()-digit)/10)return {};
    bytes=bytes*10+digit;
  }
  return bytes?std::optional<std::uint64_t>(bytes):std::nullopt;
}
// A timing fixture may rely on a verified supervisor CPU ceiling to avoid
// multiplying it by a second fixture-only cap. The new local job still
// enforces the same memory, process-count, priority and CPU-time limits.
inline DWORD supervised_cpu_rate() noexcept {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
  constexpr DWORD required=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
      JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  if(!QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&job,sizeof(job),nullptr) ||
      !QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,&rate,sizeof(rate),nullptr) ||
      (job.BasicLimitInformation.LimitFlags&required)!=required ||
      (job.BasicLimitInformation.LimitFlags&(JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) ||
      job.BasicLimitInformation.PriorityClass!=BELOW_NORMAL_PRIORITY_CLASS ||
      rate.ControlFlags!=(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) ||
      !rate.CpuRate || rate.CpuRate>2000)return 0;
  return rate.CpuRate;
}
class Budget final {
 public:
  explicit Budget(bool report_memory=true,bool use_supervisor_cpu_cap=false):report_memory_(report_memory) {
    const auto existing_cpu=use_supervisor_cpu_cap?supervised_cpu_rate():0;
    if(use_supervisor_cpu_cap && !existing_cpu)
      throw std::runtime_error("Timing requires a verified supervisor CPU cap of at most 20 percent");
    // Available headroom already excludes this running process's CRT/sanitizer
    // commit. A total-process quota must include that measured baseline too.
    const auto baseline=current_private_bytes();
    const auto enclosing=enclosing_memory_limit();
    const auto requested=enclosing?enclosing:requested_memory_limit();
    std::optional<std::uint64_t> remaining;
    if(requested)remaining=*requested>baseline?*requested-baseline:0;
    const auto additional=additional_memory_allowance(remaining,enclosing.has_value());
    if(baseline>(std::numeric_limits<SIZE_T>::max)()-additional)
      throw std::runtime_error("Application process commit budget overflow");
    memory_=baseline+additional;job_=CreateJobObjectW(nullptr,nullptr);
    if(report_memory_)std::cout<<"Application fixture memory allowance: "<<memory_<<" bytes; existing_private="
        <<baseline<<" additional_headroom="<<additional<<" enclosing_job_limit="<<enclosing.value_or(0)
        <<" requested_limit="<<requested.value_or(0)
        <<" fixture_cpu_rate="<<(existing_cpu?0:2000)<<" supervisor_cpu_rate="<<existing_cpu<<std::endl;
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
      if((!existing_cpu && !SetInformationJobObject(job_,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu))) ||
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
