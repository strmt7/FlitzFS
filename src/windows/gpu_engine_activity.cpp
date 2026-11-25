#include "gpu_engine_activity.hpp"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <compare>

namespace flitzfs {
namespace {
constexpr std::uint32_t million=1000000;
struct EngineIdentity {
  std::array<std::byte,8> luid;
  std::uint32_t physical,engine;
  auto operator<=>(const EngineIdentity&) const=default;
};
struct EngineLoad {
  EngineIdentity identity;
  std::uint32_t busy;
  bool valid;
};

std::optional<std::uint32_t> number(std::wstring_view text,unsigned base) noexcept {
  if(text.empty())return {};
  std::uint32_t value=0;
  for(const auto ch:text) {
    unsigned digit=base;
    if(ch>=L'0' && ch<=L'9')digit=static_cast<unsigned>(ch-L'0');
    else if(ch>=L'a' && ch<=L'f')digit=10+static_cast<unsigned>(ch-L'a');
    else if(ch>=L'A' && ch<=L'F')digit=10+static_cast<unsigned>(ch-L'A');
    if(digit>=base || value>((std::numeric_limits<std::uint32_t>::max)()-digit)/base)return {};
    value=value*base+digit;
  }
  return value;
}
std::optional<std::uint32_t> hex_word(std::wstring_view text) noexcept {
  return text.size()==10 && text.starts_with(L"0x")?number(text.substr(2),16):std::nullopt;
}
std::optional<EngineIdentity> parse_engine(std::wstring_view name) noexcept {
  const auto take=[&]() {
    const auto end=name.find(L'_');
    const auto field=name.substr(0,end);
    name.remove_prefix(end==std::wstring_view::npos?name.size():end+1);
    return field;
  };
  if(take()!=L"pid" || !number(take(),10) || take()!=L"luid")return {};
  const auto high=hex_word(take());
  const auto low=hex_word(take());
  if(!high || !low || (!*high && !*low) || take()!=L"phys")return {};
  const auto physical=number(take(),10);
  if(!physical || take()!=L"eng")return {};
  const auto engine=number(take(),10);
  if(!engine || take()!=L"engtype" || name.empty())return {};
  const LUID luid{*low,std::bit_cast<LONG>(*high)};
  return EngineIdentity{std::bit_cast<std::array<std::byte,8>>(luid),*physical,*engine};
}
}

bool WindowsGpuEngineActivity::fresh(std::uint64_t now) const noexcept {
  return captured_ms && now>=captured_ms && now-captured_ms<=gpu_engine_sample_ms;
}
bool WindowsGpuEngineActivity::admits(const std::array<std::byte,8>& luid,std::uint64_t now,
    std::uint32_t required_idle) const noexcept {
  if(!fresh(now) || !required_idle || required_idle>million)return false;
  const auto found=std::ranges::find(adapters,luid,&Adapter::luid);
  return found!=adapters.end() && found->busy_ppm && *found->busy_ppm<=million-required_idle;
}
std::optional<WindowsGpuEngineActivity> aggregate_windows_gpu_engines(
    std::span<const WindowsGpuEngineCounter> counters,std::uint64_t captured) {
  if(counters.empty() || !captured)return {};
  std::vector<EngineLoad> rows;
  rows.reserve(counters.size());
  for(const auto& counter:counters) {
    const auto identity=parse_engine(counter.instance);
    if(!identity)return {};
    const auto valid=counter.valid && std::isfinite(counter.busy_percent) && counter.busy_percent>=0;
    const auto busy=valid?static_cast<std::uint32_t>(std::ceil((std::min)(100.0,counter.busy_percent)*10000)):0U;
    rows.push_back({*identity,busy,valid});
  }
  std::ranges::sort(rows,{},&EngineLoad::identity);
  WindowsGpuEngineActivity activity;
  activity.captured_ms=captured;
  for(auto first=rows.begin();first!=rows.end();) {
    const auto identity=first->identity;
    auto last=first;
    std::uint32_t busy=0;
    bool valid=true;
    for(;last!=rows.end() && last->identity==identity;++last) {
      valid=valid && last->valid;
      busy=(std::min)(million,busy+last->busy);
    }
    if(activity.adapters.empty() || activity.adapters.back().luid!=identity.luid)
      activity.adapters.push_back({identity.luid,0});
    auto& maximum=activity.adapters.back().busy_ppm;
    if(!valid)maximum.reset();
    else if(maximum)*maximum=(std::max)(*maximum,busy);
    first=last;
  }
  return activity;
}

struct WindowsGpuEngineSampler::Impl {
  PDH_HQUERY query=nullptr;
  PDH_HCOUNTER counter=nullptr;
  std::uint64_t previous_ms=0;
  ~Impl(){if(query)PdhCloseQuery(query);}
  void reset_query() noexcept {
    if(query)PdhCloseQuery(query);
    query=nullptr;
    counter=nullptr;
  }
};

WindowsGpuEngineSampler::WindowsGpuEngineSampler() noexcept=default;
WindowsGpuEngineSampler::~WindowsGpuEngineSampler()=default;
std::shared_ptr<const WindowsGpuEngineActivity> WindowsGpuEngineSampler::current(std::uint64_t now) const noexcept {
  auto latest=latest_.load(std::memory_order_acquire);
  if(!latest || !latest->fresh(now))return {};
  return latest;
}
std::optional<bool> WindowsGpuEngineSampler::available(const std::array<std::byte,8>& luid,
    std::uint64_t now,std::uint32_t required_idle) const noexcept {
  const auto snapshot=current(now);
  if(!snapshot)return {};
  return snapshot->admits(luid,now,required_idle);
}
void WindowsGpuEngineSampler::observe(std::uint64_t allowance) noexcept {
  try {
    if(!allowance)return;
    if(!impl_)impl_=std::make_unique<Impl>();
    auto& state=*impl_;
    const auto now=GetTickCount64();
    const bool ordered=state.previous_ms && now>=state.previous_ms;
    const auto elapsed=ordered?now-state.previous_ms:0;
    if(ordered && elapsed<gpu_engine_sample_ms)return;
    const bool primed=state.query!=nullptr;
    const bool measured_window=primed && ordered && elapsed<=2*gpu_engine_sample_ms;
    state.previous_ms=now;
    latest_.store({},std::memory_order_release);
    if(!primed && (PdhOpenQueryW(nullptr,0,&state.query)!=ERROR_SUCCESS ||
        PdhAddEnglishCounterW(state.query,L"\\GPU Engine(*)\\Utilization Percentage",0,&state.counter)!=ERROR_SUCCESS)) {
      state.reset_query();
      return;
    }
    if(PdhCollectQueryData(state.query)!=ERROR_SUCCESS) {
      state.reset_query();
      return;
    }
    // A sample after a long idle gap supplies a new baseline. Its long
    // average could hide a recent change in competing GPU work.
    if(!measured_window)return;
    DWORD bytes=0,count=0;
    constexpr auto format=PDH_FMT_DOUBLE|PDH_FMT_NOCAP100;
    if(PdhGetFormattedCounterArrayW(state.counter,format,&bytes,&count,nullptr)!=PDH_MORE_DATA || !bytes)return;
    // The item buffer plus fitted rows and adapter summary all draw from the
    // current allowance. Refuse a growing population on this observation.
    constexpr auto per_counter=sizeof(WindowsGpuEngineCounter)+sizeof(EngineLoad)+
        2*sizeof(WindowsGpuEngineActivity::Adapter);
    const auto scratch=std::uint64_t(bytes)+
        std::uint64_t(bytes/sizeof(PDH_FMT_COUNTERVALUE_ITEM_W))*per_counter;
    if(scratch>allowance)return;
    std::vector<std::byte> buffer(bytes);
    auto* items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if(PdhGetFormattedCounterArrayW(state.counter,format,&bytes,&count,items)!=ERROR_SUCCESS ||
        !count || count>buffer.size()/sizeof(*items))return;
    std::vector<WindowsGpuEngineCounter> counters;
    counters.reserve(count);
    for(DWORD i=0;i<count;++i) {
      if(!items[i].szName)return;
      const auto& observation=items[i].FmtValue;
      const bool valid=observation.CStatus==PDH_CSTATUS_VALID_DATA || observation.CStatus==PDH_CSTATUS_NEW_DATA;
      counters.push_back({items[i].szName,observation.doubleValue,valid});
    }
    if(auto observed=aggregate_windows_gpu_engines(counters,GetTickCount64()))
      latest_.store(std::make_shared<WindowsGpuEngineActivity>(std::move(*observed)),std::memory_order_release);
  }catch(const std::exception&) {}
}
}
