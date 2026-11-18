#include "hip_uniform_lz4_reuse.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
#include "flitzfs/process_budget.hpp"
namespace flitzfs {
namespace {
void require_uniform_job(){
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  hip_detail::lz4_require(QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,
      &limits,sizeof(limits),nullptr) && QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,
      &cpu,sizeof(cpu),nullptr),HipWorkerExit::invalid_request,"missing uniform worker job",HipWorkerStage::worker_job);
  constexpr DWORD flags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  ProcessBudget budget;budget.memory_mib=limits.ProcessMemoryLimit/(1024*1024);
  budget.cpu_rate=cpu.CpuRate;budget.memory_share_divisor=16;
  hip_detail::lz4_require((limits.BasicLimitInformation.LimitFlags&flags)==flags &&
      !(limits.BasicLimitInformation.LimitFlags&(JOB_OBJECT_LIMIT_BREAKAWAY_OK|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) &&
      limits.BasicLimitInformation.ActiveProcessLimit==1 &&
      limits.BasicLimitInformation.PriorityClass==BELOW_NORMAL_PRIORITY_CLASS &&
      limits.ProcessMemoryLimit==limits.JobMemoryLimit && limits.ProcessMemoryLimit%(1024*1024)==0 &&
      valid_process_budget(budget) &&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP),
      HipWorkerExit::invalid_request,"unsafe uniform worker job",HipWorkerStage::worker_job);
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  constexpr ULONGLONG reserve=512ULL*1024*1024;
  hip_detail::lz4_require(GlobalMemoryStatusEx(&memory) && memory.ullAvailPhys>reserve &&
      memory.ullAvailPageFile>reserve && limits.ProcessMemoryLimit<=(memory.ullAvailPhys-reserve)/16 &&
      limits.ProcessMemoryLimit<=(memory.ullAvailPageFile-reserve)/16,
      HipWorkerExit::temporary_pressure,"uniform worker exceeds live host share",HipWorkerStage::worker_job);
}
}
namespace {
template<class Compute>
UniformLz4Candidates with_uniform_device(HMODULE runtime,int ordinal,std::array<std::byte,8> luid,std::uint32_t node_mask,
    std::span<const hip_detail::Lz4KernelCoverage> targets,std::uint32_t maximum_chunks,
    std::uint64_t device_bytes,Compute&& compute){
  require_uniform_job();
  hip_detail::lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU uniform LZ4 verification unavailable");
  hip_detail::lz4_require(runtime && ordinal>=0 && ordinal<64 && node_mask && std::has_single_bit(node_mask) &&
      std::any_of(luid.begin(),luid.end(),[](auto b){return b!=std::byte{0};}) &&
      !targets.empty() && targets.size()<=16,HipWorkerExit::invalid_request,"invalid uniform worker binding");
  // Admission of every image precedes initialization. Reuse exact architecture
  // selection, but never the legacy eight-block device geometry.
  for(std::size_t i=0;i<targets.size();++i){
    const auto& t=targets[i];
    hip_detail::lz4_require(t.architecture.starts_with("gfx") && t.architecture.size()>3 &&
        t.architecture.size()<=32 && t.wave_mask && !(t.wave_mask&~3U) &&
        std::all_of(t.architecture.begin(),t.architecture.end(),[](char c){
          return (c>='a'&&c<='z')||(c>='0'&&c<='9');}),
        HipWorkerExit::invalid_request,"invalid uniform target inventory");
    for(std::size_t j=0;j<i;++j)hip_detail::lz4_require(t.architecture!=targets[j].architecture,
        HipWorkerExit::invalid_request,"duplicate uniform target");
  }
  const auto api=hip_detail::bind_api(runtime);
  const auto entry=GetProcAddress(runtime,hip_detail::lz4_attributes_symbol.data());
  hip_detail::lz4_require(entry!=nullptr,HipWorkerExit::runtime_unavailable,
      "uniform function metadata unavailable",HipWorkerStage::runtime_api);
  const auto attributes=std::bit_cast<hip_detail::Lz4Attributes>(entry);
  WindowsGpuBudgetSampler sampler(luid,node_mask);
  GpuMemorySnapshot baseline{};const auto bytes=device_bytes;
  const auto memory=[&](HipMemoryPoint point){
    const auto current=sampler.sample().snapshot;
    if(point==HipMemoryPoint::pre_initialize){
      require_hip_memory(current,luid,node_mask,GetTickCount64(),bytes,point);baseline=current;
    }else require_hip_memory(current,luid,node_mask,GetTickCount64(),bytes,point,&baseline);
  };
  const auto single_node=[](auto identity){
    return verified_single_gpu_node(query_windows_gpu_topology(identity),identity);
  };
  auto selected=targets.front();
  // execute_uniform validates a wave against the union BEFORE initialization;
  // the initializer then selects the actual exact image and checks its mask.
  auto all=selected;for(const auto& t:targets)all.wave_mask|=t.wave_mask;
  const auto initialize=[&]{
    const auto priority=hip_detail::initialize_device(api,ordinal,luid,node_mask,single_node);
    hipDeviceProp_t p{};hip_detail::check_hip(api.properties(&p,ordinal),"uniform launch properties",
        HipWorkerStage::device_properties);
    const auto actual=std::bit_cast<std::array<std::byte,8>>(std::to_array(p.luid));
    hip_detail::lz4_require(actual==luid && (p.luidDeviceNodeMask==node_mask ||
        (!p.luidDeviceNodeMask && node_mask==1 && single_node(luid))),
        HipWorkerExit::unsupported_configuration,"uniform device identity changed",HipWorkerStage::adapter_luid);
    selected=hip_detail::select_lz4_coverage(p,targets);
    return hip_detail::uniform_lz4_device_info(p,selected,maximum_chunks,priority);
  };
  return compute(api,attributes,all,initialize,memory);
}
}
UniformLz4Candidates run_hip_uniform_lz4_in_worker(HMODULE runtime,
    const uniform_lz4::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const UniformLz4Request& request,hip_detail::UniformLz4ComputeGrant grant,
    std::span<const hip_detail::Lz4KernelCoverage> targets,HipPhaseRecorder* profile){
  return with_uniform_device(runtime,ordinal,luid,node_mask,targets,
      request.batch.layout().chunks,request.batch.layout().total_bytes,
      [&](const auto& api,auto attributes,auto coverage,auto&& initialize,auto&& memory){
        return hip_detail::execute_uniform_lz4_compute(api,attributes,kernel,request,coverage,grant,initialize,memory,profile);
      });
}
UniformLz4Candidates run_hip_uniform_lz4_sweep_in_worker(HMODULE runtime,
    const uniform_lz4::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const UniformLz4Request& request,hip_detail::UniformLz4ComputeGrant grant,
    std::span<const hip_detail::Lz4KernelCoverage> targets,UniformLz4SweepTrace* trace){
  hip_detail::lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU uniform sweep verification unavailable");
  UniformLz4SweepPlan plan;
  try{plan=plan_uniform_lz4_sweep(request.batch,grant.maximum_chunks,grant.maximum_device_bytes);}
  catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::invalid_request,"invalid uniform sweep grant");}
  return with_uniform_device(runtime,ordinal,luid,node_mask,targets,plan.quantum_chunks,plan.device_peak_bytes,
      [&](const auto& api,auto attributes,auto coverage,auto&& initialize,auto&& memory){
        return hip_detail::execute_uniform_lz4_reused_sweep(api,attributes,kernel,request,coverage,grant,initialize,memory,trace);
      });
}
}
