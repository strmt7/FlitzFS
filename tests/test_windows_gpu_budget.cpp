#include "gpu_budget.hpp"
#include "gpu_device_readiness.hpp"
#include <windows.h>
#include <cfg.h>
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

void verify_display_readiness() {
  using Status=WindowsAmdDisplayStatus;
  const HipAdapterHardware selected{0x1002,0x7400,0x12341002,0xb0};
  const HipAdapterHardware other{0x1002,0x7400,0x24561002,0xb0};
  const HipAdapterHardware unrelated{0x1002,0x7400,0x12341002,0xb1};
  WindowsAmdDisplaySnapshot snapshot;
  const auto require=[](bool valid,const char* message) {
    if(!valid)throw std::runtime_error(message);
  };
  require(snapshot.status()==Status::unavailable && snapshot.bind(selected).empty(),
      "Incomplete display inventory granted readiness");
  snapshot.complete=true;
  require(snapshot.status()==Status::absent && !bound_windows_amd_displays_ready({}),
      "Absent display inventory granted readiness");
  snapshot.devices.push_back({L"PCI\\VEN_1002&DEV_7400&SUBSYS_12341002&REV_B0\\1",DN_STARTED,0,true});
  snapshot.devices.push_back({L"pci\\ven_1002&dev_7400&subsys_24561002&rev_b0\\2",DN_STARTED,0,true});
  require(snapshot.status()==Status::ready && snapshot.bind(selected).size()==1 &&
      snapshot.bind(other).size()==1 && snapshot.status(&unrelated)==Status::absent,
      "PCI subsystem or revision binding differed");
  auto& original=snapshot.devices.front();
  original.devnode_status=DN_STARTED|DN_HAS_PROBLEM;
  original.problem_code=CM_PROB_WILL_BE_REMOVED;
  require(snapshot.status(&selected)==Status::not_ready && snapshot.bind(selected).empty() &&
      snapshot.status(&other)==Status::ready && snapshot.status()==Status::ready,
      "Device removal affected the wrong adapter or granted readiness");
  original.devnode_status=DN_STARTED|DN_WILL_BE_REMOVED;
  original.problem_code=0;
  require(snapshot.status(&selected)==Status::not_ready,"Removal without a problem number granted readiness");
  original.devnode_status=0;
  require(snapshot.status(&selected)==Status::not_ready,"A stopped device granted readiness");
  original.devnode_status=DN_STARTED;
  original.observed=false;
  require(snapshot.bind(selected).empty(),"An unobserved device became a route binding");
  original.observed=true;
  require(snapshot.bind(selected).size()==1,"A new healthy observation did not restore readiness");
  snapshot.devices.push_back({L"PCI\\VEN_1002&DEV_7400&SUBSYS_12341002&REV_B0\\3",
      DN_HAS_PROBLEM,CM_PROB_WILL_BE_REMOVED,true});
  require(snapshot.bind(selected).empty() && snapshot.bind(other).size()==1,
      "Indistinguishable device with an unhealthy twin granted a route");
  snapshot.devices.back().devnode_status=DN_STARTED;
  snapshot.devices.back().problem_code=0;
  require(snapshot.bind(selected).size()==2,"Two healthy matching devices lost their full binding");
  std::cout<<"amd_device_readiness model=pass removal=blocked exact_pci_binding=true recovery_observation=true\n";
}

void report_display_readiness() {
  const auto snapshot=observe_windows_amd_displays();
  std::cout<<"amd_device_inventory complete="<<snapshot.complete<<" cards="<<snapshot.devices.size()
      <<" state="<<static_cast<int>(snapshot.status())<<'\n';
  for(const auto& device:snapshot.devices)
    std::cout<<"amd_devnode observed="<<device.observed<<" ready="<<device.ready()
        <<" problem="<<device.problem_code<<'\n';
}

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
  if(argc==2 && std::string_view(argv[1])=="--describe-amd-readiness") {
    report_display_readiness();
    return 0;
  }
  if(argc!=1)throw std::invalid_argument("unknown budget test mode");
  verify_display_readiness();
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
