#include "gpu_budget.hpp"
#include <iostream>
#include <stdexcept>

int main(){try{
  // Invalid requests are rejected before a DXGI factory or adapter is queried.
  std::array<std::byte,8> id{};
  for(auto mask:{0U,1U,3U,0xffffffffU}){
    const auto r=flitzfs::query_windows_gpu_budget(id,mask);
    if(r.status!=flitzfs::WindowsGpuBudgetStatus::invalid_identity || r.snapshot.valid)
      throw std::runtime_error("zero LUID reached OS telemetry");
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
