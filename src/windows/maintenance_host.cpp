#include "maintenance_host.hpp"
#include <pdh.h>
#include <pdhmsg.h>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
namespace flitzfs {
std::optional<detail::WindowsProcessorDelta> detail::windows_processor_delta(
    WindowsProcessorTimes before,WindowsProcessorTimes after) noexcept {
  if(after.idle<before.idle || after.kernel<before.kernel || after.user<before.user)return {};
  const auto idle=after.idle-before.idle,kernel=after.kernel-before.kernel,user=after.user-before.user;
  if(idle>kernel || user>(std::numeric_limits<std::uint64_t>::max)()-kernel)return {};
  const auto total=kernel+user;
  if(!total)return {};
  return WindowsProcessorDelta{total,total-idle};
}
namespace {
using Clock=std::chrono::steady_clock;
struct ProcessorTopology {
  WORD groups=0;
  DWORD active=0;
  bool operator==(const ProcessorTopology&) const=default;
  bool valid() const noexcept{return groups && active && (groups!=1 || active<=64);}
};
ProcessorTopology processor_topology() noexcept {
  return {GetActiveProcessorGroupCount(),GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)};
}
std::optional<detail::WindowsProcessorTimes> system_times() noexcept {
  FILETIME idle{},kernel{},user{};
  if(!GetSystemTimes(&idle,&kernel,&user))return {};
  const auto ticks=[](FILETIME value) {
    return (std::uint64_t{value.dwHighDateTime}<<32)|value.dwLowDateTime;
  };
  return detail::WindowsProcessorTimes{ticks(idle),ticks(kernel),ticks(user)};
}
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
  ProcessorTopology topology;
  std::optional<detail::WindowsProcessorTimes> previous_times;
  bool use_pdh=false;
  NativeMaintenanceObservation sample;
  Clock::time_point started,issued;
  ~Impl(){if(query)PdhCloseQuery(query);}
  void reset_cpu(ProcessorTopology selected) noexcept {
    if(query)PdhCloseQuery(query);
    query=nullptr;
    counter=nullptr;
    previous=0;
    previous_times.reset();
    topology=selected;
    use_pdh=selected.groups!=1;
  }
  bool initialize_pdh() noexcept {
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
  p.state=MaintenanceHostStatus::warming;
  const auto topology=processor_topology();
  if(!topology.valid()){p.reset_cpu({});p.next=before+1000;return o;}
  if(topology!=p.topology)p.reset_cpu(topology);

  // GetSystemTimes covers the whole machine only with one processor group.
  // Retain the aggregate PDH counter when that scope cannot be guaranteed.
  const auto times=p.use_pdh?std::optional<detail::WindowsProcessorTimes>{}:system_times();
  if(!times) {
    if(!p.use_pdh){p.use_pdh=true;p.previous=0;p.previous_times.reset();}
    if(!p.initialize_pdh())return o;
    if(PdhCollectQueryData(p.query)!=ERROR_SUCCESS){p.state=MaintenanceHostStatus::unavailable;return o;}
  }
  const auto captured=GetTickCount64();
  o.captured_ms=captured;
  p.next=captured+1000;
  if(processor_topology()!=topology){p.reset_cpu({});return o;}
  const auto previous=std::exchange(p.previous,captured);
  const auto earlier=std::exchange(p.previous_times,times);
  const auto elapsed=previous && captured>=previous?captured-previous:0;
  if(elapsed<1000 || elapsed>2000)return o;

  if(times) {
    if(!earlier)return o;
    const auto delta=detail::windows_processor_delta(*earlier,*times);
    if(!delta)return o;
    o.cpu_total_ticks=delta->total;
    o.cpu_busy_ticks=delta->busy;
  } else {
    PDH_FMT_COUNTERVALUE value{};
    if(PdhGetFormattedCounterValue(p.counter,PDH_FMT_DOUBLE|PDH_FMT_NOCAP100,nullptr,&value)!=ERROR_SUCCESS ||
        (value.CStatus!=PDH_CSTATUS_VALID_DATA && value.CStatus!=PDH_CSTATUS_NEW_DATA) ||
        !std::isfinite(value.doubleValue) || value.doubleValue<0 || value.doubleValue>100) {
      p.state=MaintenanceHostStatus::unavailable;return o;
    }
    // Round busy upward to preserve the admission threshold.
    o.cpu_total_ticks=1000000;
    o.cpu_busy_ticks=static_cast<std::uint64_t>(std::ceil(value.doubleValue*10000));
  }
  if(!observe_memory(o)){p.state=MaintenanceHostStatus::unavailable;return o;}
  o.cpu_window_ms=elapsed;o.valid=true;p.state=MaintenanceHostStatus::ready;
  p.sample=o;p.issued=Clock::now();return o;
}
std::optional<NativeMaintenanceObservation> WindowsMaintenanceHost::continue_preparation(
    std::uint32_t maximum_age_ms) noexcept {
  auto& p=*impl_;
  if(!p.sample.valid || p.sequence==(std::numeric_limits<std::uint64_t>::max)())return {};
  const auto begin=Clock::now();
  if(processor_topology()!=p.topology){p.sample={};p.state=MaintenanceHostStatus::warming;return {};}
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
