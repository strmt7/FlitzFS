#include "hip_worker_budget.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
namespace {
using namespace flitzfs;
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
const std::array<std::byte,8> identity{std::byte{7}};
GpuMemorySnapshot valid() {
  GpuMemorySnapshot s;s.luid=identity;s.node_mask=1;s.captured_ms=1000;
  s.dedicated_bytes=1024ULL*1024*1024;s.local_budget=s.dedicated_bytes;
  s.local_usage=4096;s.nonlocal_usage=8192;s.valid=true;s.amd_hardware=true;return s;
}
void rejected(const GpuMemorySnapshot& s,HipMemoryPoint point,std::uint64_t now,
    std::uint64_t extra,std::uint64_t baseline,std::string_view expected) {
  try{require_hip_memory(s,identity,1,now,extra,point,baseline);throw std::runtime_error("missing refusal");}
  catch(const HipWorkerFailure& error){
    require(error.code==HipWorkerExit::device_fault,"diagnostic changed legacy recovery category");
    require(hip_worker_stage_name(error.stage)==expected,"wrong failed memory predicate");
    const auto raw=hip_worker_diagnostic_exit(error.code,error.stage);
    require(hip_worker_exit_category(raw)==0x46530005U,"memory recovery category lost");
    require(hip_worker_exit_stage(raw)==error.stage,"memory stage lost at process boundary");
  }
}
}
int main(){
  try {
    struct Point {HipMemoryPoint point;const char* invalid;const char* stale;const char* adapter;const char* headroom;const char* growth;};
    for(const auto row:{
      Point{HipMemoryPoint::pre_initialize,"pre_init_invalid_budget","pre_init_stale_budget","pre_init_wrong_adapter","pre_init_headroom",nullptr},
      Point{HipMemoryPoint::initialized,"initialized_invalid_budget","initialized_stale_budget","initialized_wrong_adapter","initialized_headroom","initialized_nonlocal_growth"},
      Point{HipMemoryPoint::completed,"completed_invalid_budget","completed_stale_budget","completed_wrong_adapter","completed_headroom","completed_nonlocal_growth"}}){
      auto s=valid();
      require_hip_memory(s,identity,1,1000,16492,row.point,s.nonlocal_usage);++checks;
      s.valid=false;rejected(s,row.point,1000,0,8192,row.invalid);
      s=valid();s.amd_hardware=false;rejected(s,row.point,1000,0,8192,row.invalid);
      s=valid();s.local_usage=s.local_budget+1;rejected(s,row.point,1000,0,8192,row.invalid);
      s=valid();rejected(s,row.point,1051,0,8192,row.stale);
      rejected(s,row.point,999,0,8192,row.stale);
      s=valid();s.luid[1]=std::byte{1};rejected(s,row.point,1000,0,8192,row.adapter);
      s=valid();s.node_mask=2;rejected(s,row.point,1000,0,8192,row.adapter);
      s=valid();s.local_usage=s.local_budget-kGpuBudgetReserveBytes;
      require_hip_memory(s,identity,1,1000,0,row.point,8192);++checks;
      rejected(s,row.point,1000,1,8192,row.headroom);
      s=valid();rejected(s,row.point,1000,(std::numeric_limits<std::uint64_t>::max)(),8192,row.headroom);
      if(row.growth){
        rejected(s,row.point,1000,0,8191,row.growth);
        // Invalid/stale telemetry must not be disguised as residency growth.
        s.valid=false;rejected(s,row.point,1000,0,0,row.invalid);
        s=valid();rejected(s,row.point,1051,0,0,row.stale);
        s=valid();s.nonlocal_usage=8191;
        require_hip_memory(s,identity,1,1000,0,row.point,8192);++checks;
      }else{
        require_hip_memory(s,identity,1,1000,0,row.point,0);++checks;
      }
    }
    rejected(valid(),static_cast<HipMemoryPoint>(99),1000,0,8192,"worker_request");
    std::cout<<"HIP worker memory predicates: "<<checks<<" checks; CPU only\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
