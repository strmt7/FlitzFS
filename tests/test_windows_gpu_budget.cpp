#include "gpu_budget.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace flitzfs;
std::array<std::byte,8> amd_identity() {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory)
    throw std::runtime_error("cannot open DXGI factory for budget measurement");
  for(UINT index=0;;++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto result=factory->EnumAdapters1(index,adapter.GetAddressOf());
    if(result==DXGI_ERROR_NOT_FOUND)throw std::runtime_error("no AMD adapter for budget measurement");
    DXGI_ADAPTER_DESC1 description{};
    if(FAILED(result) || !adapter || FAILED(adapter->GetDesc1(&description)))
      throw std::runtime_error("cannot enumerate DXGI adapter");
    if(description.VendorId!=0x1002 || (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) ||
        !description.DedicatedVideoMemory)continue;
    std::array<std::byte,8> luid{};
    static_assert(sizeof(description.AdapterLuid)==sizeof(luid));
    std::memcpy(luid.data(),&description.AdapterLuid,luid.size());
    return luid;
  }
}
struct BudgetTiming {
  std::int64_t total_ns=0,maximum_ns=0;
  unsigned stale=0;
};
template<class Sample>
BudgetTiming time_budget_samples(std::array<std::byte,8> luid,Sample&& sample) {
  BudgetTiming timing;
  for(unsigned i=0;i<10;++i) {
    const auto begin=std::chrono::steady_clock::now();
    const auto collected=sample();
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-begin).count();
    if(collected.status!=WindowsGpuBudgetStatus::success || !collected.snapshot.valid ||
        collected.snapshot.luid!=luid || collected.snapshot.node_mask!=1)
      throw std::runtime_error("AMD budget sample was unavailable or unbound");
    const auto gate=check_gpu_memory_budget(collected.snapshot,luid,1,GetTickCount64(),0);
    timing.stale+=gate==GpuGateCode::stale_budget;
    timing.total_ns+=elapsed;
    timing.maximum_ns=(std::max)(timing.maximum_ns,elapsed);
  }
  return timing;
}
void measure_amd_budget() {
  const auto luid=amd_identity();
  const auto fresh=[&]{return query_windows_gpu_budget(luid,1);};
  for(unsigned pass=1;pass<=5;++pass) {
    WindowsGpuBudgetSampler sampler(luid,1);
    const auto retained=[&]{return sampler.sample();};
    const bool retained_first=pass%2==0;
    BudgetTiming rebound,reused;
    if(retained_first) {
      reused=time_budget_samples(luid,retained);
      rebound=time_budget_samples(luid,fresh);
    } else {
      rebound=time_budget_samples(luid,fresh);
      reused=time_budget_samples(luid,retained);
    }
    std::cout<<"amd_budget pass="<<pass<<" samples_per_path=10 retained_first="<<retained_first
             <<" rebound_ns="<<rebound.total_ns<<" reused_ns="<<reused.total_ns
             <<" rebound_max_ns="<<rebound.maximum_ns<<" reused_max_ns="<<reused.maximum_ns
             <<" rebound_stale="<<rebound.stale<<" reused_stale="<<reused.stale<<'\n';
  }
}
}

int main(int argc,char** argv){try{
  if(argc==2 && std::string_view(argv[1])=="--measure-amd-budget") {
    measure_amd_budget();
    return 0;
  }
  if(argc!=1)throw std::invalid_argument("unknown budget test mode");
  // Invalid requests are rejected before a DXGI factory or adapter is queried.
  std::array<std::byte,8> id{};
  for(auto mask:{0U,1U,3U,0xffffffffU}){
    const auto r=flitzfs::query_windows_gpu_budget(id,mask);
    if(r.status!=flitzfs::WindowsGpuBudgetStatus::invalid_identity || r.snapshot.valid)
      throw std::runtime_error("zero LUID reached OS telemetry");
    flitzfs::WindowsGpuBudgetSampler sampler(id,mask);
    for(unsigned sample=0;sample<2;++sample) {
      const auto repeated=sampler.sample();
      if(repeated.status!=flitzfs::WindowsGpuBudgetStatus::invalid_identity || repeated.snapshot.valid ||
          repeated.phase!=flitzfs::WindowsGpuBudgetPhase::none)
        throw std::runtime_error("invalid bound collector lost terminal status");
    }
  }
  id[0]=std::byte{1};
  for(auto mask:{0U,3U,5U,0xffffffffU}){
    const auto r=flitzfs::query_windows_gpu_budget(id,mask);
    if(r.status!=flitzfs::WindowsGpuBudgetStatus::invalid_identity || r.snapshot.valid)
      throw std::runtime_error("invalid node mask reached OS telemetry");
  }
  std::cout<<"PASS: invalid GPU identity rejected before DXGI queries; no HIP or compute.\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
