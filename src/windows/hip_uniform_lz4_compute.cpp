#include "hip_uniform_lz4_reuse.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
#include "flitzfs/process_budget.hpp"
#include <iterator>
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
auto with_uniform_device(HMODULE runtime,int ordinal,std::array<std::byte,8> luid,std::uint32_t node_mask,
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
  const auto read_memory=[&]{return sampler.sample().snapshot;};
  const auto monotonic=[] {return GetTickCount64();};
  const auto memory=[&](HipMemoryPoint point){
    if(point==HipMemoryPoint::completed) {
      require_fresh_hip_completion(read_memory,monotonic,luid,node_mask,bytes,baseline);
      return;
    }
    const auto current=read_memory();
    if(point==HipMemoryPoint::pre_initialize){
      require_hip_memory(current,luid,node_mask,monotonic(),bytes,point);baseline=current;
    }else require_hip_memory(current,luid,node_mask,monotonic(),bytes,point,&baseline);
  };
  WindowsGpuTopologyProbe topology(luid);
  const auto single_node=[&](auto identity){
    return identity==luid && verified_single_gpu_node(topology.sample(),identity);
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
    hip_detail::lz4_require(topology.retire()==0,HipWorkerExit::device_fault,
        "retire uniform topology proof",HipWorkerStage::module_retire);
    selected=hip_detail::select_lz4_coverage(p,targets);
    return hip_detail::uniform_lz4_device_info(p,selected,maximum_chunks,priority);
  };
  return compute(api,attributes,all,initialize,memory);
}
}

std::vector<detail::NativeLz4DecodedFrame> run_hip_native_lz4_decode_in_worker(HMODULE runtime,
    const native_lz4_decode::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,std::span<const detail::NativeLz4DecodeBatch> batches,
    detail::NativeLz4DecodeGrant grant,std::span<const hip_detail::Lz4KernelCoverage> targets) {
  using namespace hip_detail;
  lz4_require(native_lz4_decode::compatible(kernel) && !batches.empty(),
      HipWorkerExit::invalid_request,"invalid native LZ4 kernel table or empty dispatch sequence");
  auto room=grant.maximum_slab_bytes;
  std::uint32_t frames=0,maximum_chunks=0;
  std::size_t maximum_bytes=0;
  for(const auto& batch:batches) {
    const auto layout=batch.layout();
    const auto bytes=batch.upload().size();
    lz4_require(native_lz4_decode::valid_layout(layout,bytes) &&
        layout.chunks<=grant.maximum_frames-frames && bytes<=room,
        HipWorkerExit::invalid_request,"native LZ4 decode sequence exceeds its grant");
    room-=bytes;frames+=layout.chunks;
    maximum_chunks=(std::max)(maximum_chunks,layout.chunks);
    maximum_bytes=(std::max)(maximum_bytes,bytes);
  }
  return with_uniform_device(runtime,ordinal,luid,node_mask,targets,maximum_chunks,maximum_bytes,
      [&](const auto& api,auto attributes,auto,auto&& initialize,auto&& memory) {
        std::vector<std::byte> readback(maximum_bytes);
        std::vector<detail::NativeLz4DecodedFrame> verified;
        verified.reserve(frames);
        memory(HipMemoryPoint::pre_initialize);
        const auto device=initialize();
        const auto selected=device.wave==32?kernel.wave32:kernel.wave64;
        check_lz4_attributes(attributes,selected,device);

        DeviceResources resources(api);
        try {
          check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,device.priority),
              "create native LZ4 decode stream",HipWorkerStage::stream_create);
          memory(HipMemoryPoint::initialized);
          check_hip(api.allocate(&resources.buffers[0],maximum_bytes),
              "allocate native LZ4 decode slab",HipWorkerStage::execution);
          const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
          lz4_require(address && !(address&(native_lz4_decode::alignment-1)) &&
              maximum_bytes<=~std::uintptr_t{0}-address,HipWorkerExit::device_fault,
              "invalid native LZ4 decode slab address",HipWorkerStage::execution);
          auto* slab=static_cast<native_lz4_decode::u8*>(resources.buffers[0]);
          for(const auto& batch:batches) {
            memory(HipMemoryPoint::initialized);
            const auto upload=batch.upload();
            auto layout=batch.layout();
            auto bytes=static_cast<native_lz4_decode::u64>(upload.size());
            check_hip(api.copy(slab,upload.data(),upload.size(),hipMemcpyHostToDevice),
                "upload native LZ4 decode slab",HipWorkerStage::execution);
            void* arguments[]{&slab,&bytes,&layout};
            check_hip(api.launch(reinterpret_cast<const void*>(selected),dim3(layout.chunks),
                dim3(device.wave),arguments,0,resources.stream),
                "submit native LZ4 decode kernel",HipWorkerStage::execution);
            check_hip(api.synchronize(resources.stream),"complete native LZ4 decode kernel",HipWorkerStage::execution);
            const auto descriptor_bytes=static_cast<std::size_t>(native_lz4_decode::input_at(layout,0));
            const auto output_begin=static_cast<std::size_t>(native_lz4_decode::output_at(layout,0));
            // Read back the descriptor and decoded banks. The immutable input
            // bank has no role in CPU output verification.
            check_hip(api.copy(readback.data(),slab,descriptor_bytes,hipMemcpyDeviceToHost),
                "read native LZ4 completion descriptors",HipWorkerStage::execution);
            check_hip(api.copy(readback.data()+output_begin,slab+output_begin,upload.size()-output_begin,
                hipMemcpyDeviceToHost),"read native LZ4 decoded bytes",HipWorkerStage::execution);
            try {
              auto result=batch.verify(std::span(readback).first(upload.size()));
              verified.insert(verified.end(),std::make_move_iterator(result.begin()),
                  std::make_move_iterator(result.end()));
            }catch(const std::invalid_argument&) {
              throw HipWorkerFailure(HipWorkerExit::integrity_failure,
                  "native LZ4 output failed complete CPU verification",HipWorkerStage::execution);
            }
            memory(HipMemoryPoint::completed);
          }
          check_hip(resources.release_all(),"retire native LZ4 compute resources",HipWorkerStage::execution);
          return verified;
        }catch(...) {
          const auto failure=std::current_exception();
          lz4_require(resources.release_all()==hipSuccess && !resources.retirement_failed,
              HipWorkerExit::device_fault,"native LZ4 failure retirement unconfirmed",HipWorkerStage::execution);
          std::rethrow_exception(failure);
        }
      });
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
UniformLz4Candidates run_hip_uniform_lz4_prepared_sweep_in_worker(HMODULE runtime,
    const uniform_lz4::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const UniformLz4SweepPlan& plan,std::span<const hip_detail::Lz4KernelCoverage> targets,
    const std::function<const UniformLz4Request*()>& receive) {
  return with_uniform_device(runtime,ordinal,luid,node_mask,targets,plan.quantum_chunks,plan.device_peak_bytes,
      [&](const auto& api,auto attributes,auto coverage,auto&& initialize,auto&& memory) {
        return hip_detail::execute_uniform_lz4_sweep_plan(api,attributes,kernel,plan,coverage,receive,initialize,memory);
      });
}
}
