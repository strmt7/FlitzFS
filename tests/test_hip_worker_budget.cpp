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
  s.local_usage=4096;s.nonlocal_usage=8192;s.valid=true;s.amd_hardware=true;
  s.nonlocal_budget=8ULL<<30;s.physical_total=16ULL<<30;s.physical_available=8ULL<<30;
  s.commit_limit=32ULL<<30;s.commit_available=16ULL<<30;return s;
}
void rejected(const GpuMemorySnapshot& s,HipMemoryPoint point,std::uint64_t now,
    std::uint64_t bytes,std::string_view expected,HipWorkerExit category=HipWorkerExit::device_fault) {
  const auto baseline=valid();
  try{require_hip_memory(s,identity,1,now,bytes,point,&baseline);throw std::runtime_error("missing refusal");}
  catch(const HipWorkerFailure& error){
    require(error.code==category,"wrong recovery category");
    require(hip_worker_stage_name(error.stage)==expected,"wrong failed memory predicate");
    const auto raw=hip_worker_diagnostic_exit(error.code,error.stage);
    require(hip_worker_exit_category(raw)==static_cast<std::uint32_t>(category),"memory recovery category lost");
    require(hip_worker_exit_stage(raw)==error.stage,"memory stage lost at process boundary");
  }
}
}
int main(){
  try {
    using E=HipWorkerExit;
    struct Point {HipMemoryPoint point;const char* invalid;const char* stale;const char* adapter;const char* headroom;const char* growth;};
    for(const auto row:{
      Point{HipMemoryPoint::pre_initialize,"pre_init_invalid_budget","pre_init_stale_budget","pre_init_wrong_adapter","pre_init_headroom",nullptr},
      Point{HipMemoryPoint::initialized,"initialized_invalid_budget","initialized_stale_budget","initialized_wrong_adapter","initialized_headroom","initialized_nonlocal_growth"},
      Point{HipMemoryPoint::completed,"completed_invalid_budget","completed_stale_budget","completed_wrong_adapter","completed_headroom","completed_nonlocal_growth"}}){
      const auto baseline=valid();
      auto s=baseline;
      require_hip_memory(s,identity,1,1000,16492,row.point,&baseline);++checks;
      s.valid=false;rejected(s,row.point,1000,0,row.invalid);
      s=valid();s.amd_hardware=false;rejected(s,row.point,1000,0,row.invalid);
      s=valid();s.local_usage=s.local_budget+1;rejected(s,row.point,1000,0,row.headroom,E::temporary_pressure);
      s=valid();s.physical_total=0;rejected(s,row.point,1000,0,row.invalid);
      s=valid();s.commit_limit=0;rejected(s,row.point,1000,0,row.invalid);
      s=valid();rejected(s,row.point,1051,0,row.stale,E::temporary_pressure);
      rejected(s,row.point,999,0,row.stale,E::temporary_pressure);
      s=valid();s.luid[1]=std::byte{1};rejected(s,row.point,1000,0,row.adapter);
      s=valid();s.node_mask=2;rejected(s,row.point,1000,0,row.adapter);
      s=valid();s.local_usage=s.local_budget-kGpuBudgetReserveBytes;
      require_hip_memory(s,identity,1,1000,0,row.point,&baseline);++checks;
      ++s.local_usage;rejected(s,row.point,1000,0,row.headroom,E::temporary_pressure);
      s=valid();rejected(s,row.point,1000,(std::numeric_limits<std::uint64_t>::max)(),row.headroom,E::temporary_pressure);
      s=valid();s.physical_available=0;rejected(s,row.point,1000,0,row.headroom,E::temporary_pressure);
      s=valid();s.commit_available=0;rejected(s,row.point,1000,0,row.headroom,E::temporary_pressure);
      if(row.growth){
        s=valid();s.nonlocal_usage+=16ULL<<20;
        require_hip_memory(s,identity,1,1000,16492,row.point,&baseline);++checks;
        s.nonlocal_usage+=16ULL<<20;
        rejected(s,row.point,1000,16492,row.growth,E::temporary_pressure);
        s.valid=false;rejected(s,row.point,1000,0,row.invalid);
        s=valid();s.nonlocal_usage=8191;
        require_hip_memory(s,identity,1,1000,0,row.point,&baseline);++checks;
        try {require_hip_memory(s,identity,1,1000,0,row.point);throw std::runtime_error("missing original baseline admitted");}
        catch(const HipWorkerFailure& error){require(error.stage==HipWorkerStage::worker_request,"missing baseline stage");}
      }
    }
    rejected(valid(),static_cast<HipMemoryPoint>(99),1000,0,"worker_request");
    std::cout<<"HIP worker memory envelopes: "<<checks<<" checks; CPU only\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
