#pragma once
#include "hip_lz4_compute.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"
#include "hipcomp_uniform_lz4_api.hpp"
#include "hip_phase_profile.hpp"
namespace flitzfs::hip_detail {
// A configuration-qualified maximum kernel quantum expressed as work, not a
// universal input cap. A trusted caller must obtain this from its admitted
// time/grid/resource plan. It does not authorize module loading or discovery.
struct UniformLz4ComputeGrant {
  std::uint32_t maximum_chunks=0;
  std::uint64_t maximum_device_bytes=0;
  std::uint32_t chunk_bytes=0;
};
inline Lz4DeviceInfo uniform_lz4_device_info(const hipDeviceProp_t& p,
    Lz4KernelCoverage coverage,std::uint32_t chunks,int priority){
  const auto end=std::find(std::begin(p.gcnArchName),std::end(p.gcnArchName),'\0');
  const auto wave=p.warpSize;const auto mask=wave==32?1U:wave==64?2U:0U;
  lz4_require(end!=std::end(p.gcnArchName) &&
      std::string_view(p.gcnArchName,static_cast<std::size_t>(end-std::begin(p.gcnArchName)))==coverage.architecture &&
      mask && (mask&coverage.wave_mask) && p.maxThreadsPerBlock>=wave && p.maxThreadsDim[0]>=wave &&
      p.maxThreadsDim[1]>=1 && p.maxThreadsDim[2]>=1 &&
      chunks && p.maxGridSize[0]>0 && chunks<=static_cast<std::uint32_t>(p.maxGridSize[0]) &&
      p.maxGridSize[1]>=1 && p.maxGridSize[2]>=1 && p.sharedMemPerBlock>0,
      HipWorkerExit::unsupported_configuration,"uniform compression launch geometry not qualified",
      HipWorkerStage::device_properties);
  return {priority,static_cast<std::uint32_t>(wave),p.sharedMemPerBlock};
}
template<class Initialize,class MemoryPolicy>
UniformLz4Candidates execute_uniform_lz4_compute(const HipApi& api,Lz4Attributes attributes,
    const uniform_lz4::KernelApi& kernel,const UniformLz4Request& request,
    Lz4KernelCoverage coverage,UniformLz4ComputeGrant grant,
    Initialize&& initialize,MemoryPolicy&& memory,HipPhaseRecorder* profile=nullptr){
  lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU uniform LZ4 verification unavailable");
  lz4_require(request.request_id && request.batch.ready() && uniform_lz4::compatible_kernel_api(kernel) &&
      grant.chunk_bytes && request.batch.layout().chunk_bytes==grant.chunk_bytes &&
      grant.maximum_chunks && request.batch.layout().chunks<=grant.maximum_chunks &&
      grant.maximum_device_bytes && request.batch.layout().total_bytes<=grant.maximum_device_bytes,
      HipWorkerExit::invalid_request,"uniform compression request exceeds qualified grant");
  lz4_require(coverage.architecture.starts_with("gfx") && coverage.architecture.size()>3 &&
      coverage.architecture.size()<=32 && coverage.wave_mask && !(coverage.wave_mask&~3U) &&
      std::all_of(coverage.architecture.begin(),coverage.architecture.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='0'&&c<='9');}),
      HipWorkerExit::invalid_request,"invalid uniform compression coverage");
  lz4_require(api.init && api.properties && api.set_device && api.priority_range &&
      api.create_stream && api.destroy_stream && api.synchronize && api.allocate &&
      api.release && api.copy && api.launch,HipWorkerExit::runtime_unavailable,
      "uniform compression runtime table incomplete",HipWorkerStage::runtime_api);
  const auto plan=request.batch.layout();
  if(profile)profile->begin(HipPhase::device_initialize);
  memory(HipMemoryPoint::pre_initialize);
  const auto device=initialize();
  const auto mask=device.wave==32?1U:device.wave==64?2U:0U;
  lz4_require(mask && (coverage.wave_mask&mask),HipWorkerExit::unsupported_configuration,
      "uniform compression wave not qualified",HipWorkerStage::device_properties);
  const auto selected=device.wave==32?kernel.wave32:kernel.wave64;
  check_lz4_attributes(attributes,selected,device);
  if(profile)profile->end(HipPhase::device_initialize);
  DeviceResources resources(api);
  try{
    if(profile)profile->begin(HipPhase::stream_allocate);
    check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,device.priority),
        "create uniform compression stream",HipWorkerStage::stream_create);
    memory(HipMemoryPoint::initialized);
    auto upload=uniform_lz4_upload(request.batch);
    std::vector<std::byte> readback(static_cast<std::size_t>(plan.total_bytes));
    memory(HipMemoryPoint::initialized); // Include these real host buffer costs.
    check_hip(api.allocate(&resources.buffers[0],static_cast<std::size_t>(plan.total_bytes)),
        "allocate uniform compression slab",HipWorkerStage::execution);
    const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
    lz4_require(address && !(address&7U) && plan.total_bytes<=~std::uintptr_t{0}-address,
        HipWorkerExit::device_fault,"invalid uniform device slab address",HipWorkerStage::execution);
    if(profile)profile->end(HipPhase::stream_allocate);
    if(profile)profile->begin(HipPhase::host_to_device);
    check_hip(api.copy(resources.buffers[0],upload.data(),upload.size(),hipMemcpyHostToDevice),
        "upload uniform compression slab",HipWorkerStage::execution);
    if(profile)profile->end(HipPhase::host_to_device);
    auto* slab=static_cast<unsigned char*>(resources.buffers[0]);
    auto allocated=plan.total_bytes;auto layout=plan;void* arguments[]{&slab,&allocated,&layout};
    if(profile)profile->begin(HipPhase::kernel_wait);
    check_hip(api.launch(reinterpret_cast<const void*>(selected),dim3(plan.chunks),dim3(device.wave),
        arguments,0,resources.stream),"submit uniform compression kernel",HipWorkerStage::execution);
    check_hip(api.synchronize(resources.stream),"complete uniform compression kernel",HipWorkerStage::execution);
    if(profile)profile->end(HipPhase::kernel_wait);
    if(profile)profile->begin(HipPhase::device_to_host);
    check_hip(api.copy(readback.data(),resources.buffers[0],readback.size(),hipMemcpyDeviceToHost),
        "read uniform compression slab",HipWorkerStage::execution);
    if(profile)profile->end(HipPhase::device_to_host);
    if(profile)profile->begin(HipPhase::cpu_verification);
    UniformLz4Candidates result;
    try{result=verify_uniform_lz4_readback(request.batch,readback);}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::integrity_failure,
        "uniform compression failed independent CPU verification",HipWorkerStage::execution);}
    if(profile)profile->end(HipPhase::cpu_verification);
    if(profile)profile->begin(HipPhase::post_admission);
    memory(HipMemoryPoint::completed);
    if(profile)profile->end(HipPhase::post_admission);
    if(profile)profile->begin(HipPhase::resources_release);
    lz4_require(resources.release_all()==hipSuccess,HipWorkerExit::device_fault,
        "uniform compression retirement failed",HipWorkerStage::execution);
    if(profile)profile->end(HipPhase::resources_release);
    return result;
  }catch(...){
    const auto failure=std::current_exception();
    lz4_require(resources.release_all()==hipSuccess && !resources.retirement_failed,
        HipWorkerExit::device_fault,"uniform compression failure retirement unconfirmed",HipWorkerStage::execution);
    std::rethrow_exception(failure);
  }
}
}

namespace flitzfs {
UniformLz4Candidates run_hip_uniform_lz4_in_worker(HMODULE runtime,
    const uniform_lz4::KernelApi&,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const UniformLz4Request&,hip_detail::UniformLz4ComputeGrant,
    std::span<const hip_detail::Lz4KernelCoverage>,HipPhaseRecorder* profile=nullptr);
}
