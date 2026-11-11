#pragma once
// Test/benchmark containment only; no product caller or owner-PC permission.
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
inline void hosted_digest_limits() {
  const char* a=std::getenv("GITHUB_ACTIONS"),*b=std::getenv("RUNNER_ENVIRONMENT");
  if(!a || !b || std::strcmp(a,"true") || std::strcmp(b,"github-hosted"))
    throw std::runtime_error("digest comparison requires disposable hosted VM");
#ifdef _WIN32
  const auto job=CreateJobObjectW(nullptr,nullptr);
  if(!job)throw std::runtime_error("digest job creation");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|
      JOB_OBJECT_LIMIT_PROCESS_TIME;
  limits.BasicLimitInformation.ActiveProcessLimit=1;
  limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart=15LL*10000000;
  limits.ProcessMemoryLimit=512ULL*1024*1024;limits.JobMemoryLimit=512ULL*1024*1024;
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  cpu.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;cpu.CpuRate=2000;
  if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof limits) ||
      !SetInformationJobObject(job,JobObjectCpuRateControlInformation,&cpu,sizeof cpu) ||
      !AssignProcessToJobObject(job,GetCurrentProcess())) {
    CloseHandle(job);throw std::runtime_error("digest job admission");
  }
  // Assigned kill-on-close handle lives until process exit, as in the owned
  // interruption probe. Never close it in an ordinary stack destructor.
#else
  throw std::runtime_error("timed digest comparison is Windows hosted only");
#endif
}
