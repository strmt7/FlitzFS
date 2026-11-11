#include "gpu_topology.hpp"
#include <iostream>
#include <stdexcept>
namespace {
using namespace flitzfs;
unsigned checks=0;
void require(bool ok){++checks;if(!ok)throw std::runtime_error("invalid single-node proof decision");}
}
int main(){
  try {
    const std::array<std::byte,8> expected{std::byte{7}};
    const WindowsGpuTopologyResult good{WindowsGpuTopologyStatus::success,
        WindowsGpuTopologyPhase::complete,0,expected,1,true};
    require(verified_single_gpu_node(good,expected));
    for(unsigned count:{0U,2U,31U,32U,33U,0xffffffffU}){
      auto value=good;value.node_count=count;require(!verified_single_gpu_node(value,expected));
    }
    for(unsigned status=1;status<=static_cast<unsigned>(WindowsGpuTopologyStatus::fixture_forbidden);++status){
      auto value=good;value.status=static_cast<WindowsGpuTopologyStatus>(status);
      require(!verified_single_gpu_node(value,expected));
    }
    for(unsigned phase=0;phase<static_cast<unsigned>(WindowsGpuTopologyPhase::complete);++phase){
      auto value=good;value.phase=static_cast<WindowsGpuTopologyPhase>(phase);
      require(!verified_single_gpu_node(value,expected));
    }
    for(auto error:{1,-1,static_cast<std::int32_t>(0x887a0005U)}){
      auto value=good;value.native_status=error;require(!verified_single_gpu_node(value,expected));
    }
    for(std::size_t i=0;i<expected.size();++i){
      auto value=good;value.luid[i]^=std::byte{1};require(!verified_single_gpu_node(value,expected));
    }
    auto value=good;value.amd_hardware=false;require(!verified_single_gpu_node(value,expected));
    value=good;value.luid={};require(!verified_single_gpu_node(value,expected));
    require(!verified_single_gpu_node(value,{}));
    require(!verified_single_gpu_node(good,{}));
    require(!verified_single_gpu_node({},expected));
    std::cout<<"GPU topology CPU proof: "<<checks<<" checks\n";return 0;
  } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
