#include "maintenance_host.hpp"
#include <pdh.h>
#include <pdhmsg.h>
#include <chrono>
#include <cmath>
#include <limits>
namespace flitzfs {
namespace {
using Clock=std::chrono::steady_clock;
bool observe_memory(NativeMaintenanceObservation& observation) noexcept {
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))return false;
  observation.physical_total=memory.ullTotalPhys;observation.physical_available=memory.ullAvailPhys;
  observation.commit_limit=memory.ullTotalPageFile;observation.commit_available=memory.ullAvailPageFile;
  return true;
}
}
struct WindowsMaintenanceHost::Impl {
  PDH_HQUERY query=nullptr;PDH_HCOUNTER counter=nullptr;
  MaintenanceHostStatus state=MaintenanceHostStatus::warming;
  std::uint64_t previous=0,sequence=0,next=0;
  NativeMaintenanceObservation sample;
  Clock::time_point started,issued;
  ~Impl(){if(query)PdhCloseQuery(query);}
  bool initialize() noexcept {
    if(state==MaintenanceHostStatus::unavailable)return false;
    if(query)return true;
    if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query,L"\\Processor Information(_Total)\\% Processor Time",0,&counter)!=ERROR_SUCCESS) {
      state=MaintenanceHostStatus::unavailable;return false;
    }
    return true;
  }
};
WindowsMaintenanceHost::WindowsMaintenanceHost():impl_(std::make_unique<Impl>()){}
WindowsMaintenanceHost::~WindowsMaintenanceHost()=default;
MaintenanceHostStatus WindowsMaintenanceHost::status() const noexcept{return impl_->state;}
std::uint64_t WindowsMaintenanceHost::next_delay_ms() const noexcept {
  const auto now=GetTickCount64();return impl_->next>now?impl_->next-now:0;
}
NativeMaintenanceObservation WindowsMaintenanceHost::observe(bool foreground) noexcept {
  auto& p=*impl_;NativeMaintenanceObservation o;o.foreground_pending=foreground;
  p.sample={};p.started=Clock::now();
  if(p.state==MaintenanceHostStatus::unavailable)return o;
  if(p.sequence==(std::numeric_limits<std::uint64_t>::max)()){p.state=MaintenanceHostStatus::unavailable;return o;}
  o.sequence=++p.sequence;const auto before=GetTickCount64();o.captured_ms=before;
  if(foreground)return o;
  if(before<p.next){p.state=MaintenanceHostStatus::waiting;return o;}
  if(!p.initialize())return o;
  const auto collected=PdhCollectQueryData(p.query);const auto captured=GetTickCount64();
  o.captured_ms=captured;p.next=captured+1000;
  const auto elapsed=p.previous && captured>=p.previous?captured-p.previous:0;p.previous=captured;
  if(collected!=ERROR_SUCCESS){p.state=MaintenanceHostStatus::unavailable;return o;}
  if(elapsed<1000 || elapsed>2000){p.state=MaintenanceHostStatus::warming;return o;}
  PDH_FMT_COUNTERVALUE value{};
  if(PdhGetFormattedCounterValue(p.counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,nullptr,&value)!=ERROR_SUCCESS ||
      (value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA) ||
      !std::isfinite(value.doubleValue) || value.doubleValue<0 || value.doubleValue>100) {
    p.state=MaintenanceHostStatus::unavailable;return o;
  }
  if(!observe_memory(o)){p.state=MaintenanceHostStatus::unavailable;return o;}
  // Normalized ratio units; round busy upward so conversion never admits a
  // sample that was just over the requested CPU threshold.
  o.cpu_total_ticks=1000000;o.cpu_busy_ticks=static_cast<std::uint64_t>(std::ceil(value.doubleValue*10000));
  o.cpu_window_ms=elapsed;o.valid=true;p.state=MaintenanceHostStatus::ready;
  p.sample=o;p.issued=Clock::now();return o;
}
std::optional<NativeMaintenanceObservation> WindowsMaintenanceHost::continue_preparation(
    std::uint32_t maximum_age_ms) noexcept {
  auto& p=*impl_;
  if(!p.sample.valid || p.sequence==(std::numeric_limits<std::uint64_t>::max)())return {};
  const auto begin=Clock::now();
  const auto budget=std::chrono::nanoseconds(p.sample.cpu_window_ms*maintenance_wall_share_ppm);
  const auto used=begin-p.started;
  // Leave room for another step of the cost just observed. The owner still
  // charges the full duration if an individual synchronous step overruns.
  if(used>=budget || begin-p.issued>budget-used)return {};
  const auto fresh=[&] {
    const auto now=GetTickCount64();
    return now>=p.sample.captured_ms && now-p.sample.captured_ms<=maximum_age_ms;
  };
  auto observation=p.sample;
  if(!fresh())return {};
  if(!observe_memory(observation)){p.sample={};p.state=MaintenanceHostStatus::unavailable;return {};}
  const auto refreshed=Clock::now()-p.started;
  if(!fresh() || refreshed>=budget || begin-p.issued>budget-refreshed)return {};
  observation.sequence=++p.sequence;observation.continued=true;
  p.issued=begin;
  return observation;
}
MaintenanceThreadScope::MaintenanceThreadScope() noexcept:
    active_(SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN)!=FALSE){}
MaintenanceThreadScope::~MaintenanceThreadScope(){finish();}
bool MaintenanceThreadScope::finish() noexcept {
  if(!active_)return true;
  if(!SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END))return false;
  active_=false;return true;
}
}
