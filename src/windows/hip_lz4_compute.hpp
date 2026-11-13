#pragma once
#include "hip_api.hpp"
#include "hip_device_resources.hpp"
#include "hip_worker_budget.hpp"
#include "flitzfs/gpu_lz4_slab.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "hipcomp_lz4_api.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <exception>
#include <limits>
#include <string_view>
#include <vector>

namespace flitzfs::hip_detail {
// Private, borrowed metadata from a separately authenticated module/object
// inventory. This is not a launch grant, wire record or device assumption.
struct Lz4KernelCoverage {std::string_view architecture;std::uint32_t wave_mask=0;};
struct Lz4DeviceInfo {int priority=0;std::uint32_t wave=0;std::size_t shared_capacity=0;};
using Lz4Attributes=decltype(&hipFuncGetAttributes);
#define FLITZFS_LZ4_API_NAME_INNER(value) #value
#define FLITZFS_LZ4_API_NAME(value) FLITZFS_LZ4_API_NAME_INNER(value)
inline constexpr std::string_view lz4_attributes_symbol=FLITZFS_LZ4_API_NAME(hipFuncGetAttributes);
#undef FLITZFS_LZ4_API_NAME
#undef FLITZFS_LZ4_API_NAME_INNER
inline void lz4_require(bool value,HipWorkerExit code,const char* why,
    HipWorkerStage stage=HipWorkerStage::worker_request) {
  if(!value)throw HipWorkerFailure(code,why,stage);
}
inline void admit_lz4_compute(const candidate_lz4::KernelApi& kernel,
    const Lz4WorkerRequest& request,Lz4KernelCoverage coverage) {
  lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU LZ4 verification unavailable");
  lz4_require(request.request_id && request.batch.ready() &&
      candidate_lz4::compatible_kernel_api(kernel),HipWorkerExit::invalid_request,
      "invalid owned compression request or typed kernel API");
  lz4_require(coverage.architecture.starts_with("gfx") && coverage.architecture.size()>3 &&
      coverage.architecture.size()<=32 && coverage.wave_mask && !(coverage.wave_mask&~3U) &&
      std::all_of(coverage.architecture.begin(),coverage.architecture.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='0'&&c<='9');}),
      HipWorkerExit::invalid_request,"invalid qualified compression target coverage");
}
inline candidate_lz4::Layout lz4_launch_layout(const GpuLz4DeviceLayout& source) {
  candidate_lz4::Layout result{};
  for(std::size_t i=0;i<source.chunks.size();++i) {
    const auto& c=source.chunks[i];result.chunks[i]={c.input_offset,c.input_bytes,c.output_offset,c.output_capacity};
  }
  result.count=source.count;result.input_bytes=source.input_bytes;
  result.input_pointers=source.input_pointers;result.input_sizes=source.input_sizes;
  result.output_pointers=source.output_pointers;result.output_sizes=source.output_sizes;
  result.scratch_offset=source.scratch_offset;result.hash_table_entries=source.hash_table_entries;
  result.scratch_bytes=source.scratch_bytes;result.total_bytes=source.total_bytes;
  lz4_require(candidate_lz4::valid_layout(result,result.total_bytes),
      HipWorkerExit::invalid_request,"compression host/device layout mismatch");
  return result;
}
inline Lz4DeviceInfo lz4_device_info(const hipDeviceProp_t& properties,
    Lz4KernelCoverage coverage,std::uint32_t chunks,int priority) {
  const auto end=std::find(std::begin(properties.gcnArchName),std::end(properties.gcnArchName),'\0');
  const auto wave=properties.warpSize;
  const auto mask=wave==32?1U:wave==64?2U:0U;
  lz4_require(end!=std::end(properties.gcnArchName) &&
      std::string_view(properties.gcnArchName,static_cast<std::size_t>(end-std::begin(properties.gcnArchName)))==
          coverage.architecture && mask && (coverage.wave_mask&mask) &&
      properties.maxThreadsPerBlock>=wave && properties.maxThreadsDim[0]>=wave &&
      properties.maxThreadsDim[1]>=1 && properties.maxThreadsDim[2]>=1 &&
      chunks && chunks<=8 && properties.maxGridSize[0]>=static_cast<int>(chunks) &&
      properties.maxGridSize[1]>=1 && properties.maxGridSize[2]>=1 &&
      properties.sharedMemPerBlock>0, HipWorkerExit::unsupported_configuration,
      "compression architecture or launch geometry is not qualified",HipWorkerStage::device_properties);
  return {priority,static_cast<std::uint32_t>(wave),properties.sharedMemPerBlock};
}
inline void check_lz4_attributes(Lz4Attributes query,candidate_lz4::CompressKernel selected,
    Lz4DeviceInfo device) {
  lz4_require(query && selected,HipWorkerExit::unsupported_configuration,
      "compression function metadata unavailable",HipWorkerStage::kernel_entry);
  hipFuncAttributes first{},second{};
  first.maxThreadsPerBlock=-1;second.maxThreadsPerBlock=(std::numeric_limits<int>::max)();
  first.sharedSizeBytes=(std::numeric_limits<std::size_t>::max)();second.sharedSizeBytes=0;
  check_hip(query(&first,reinterpret_cast<const void*>(selected)),
      "compression function attributes",HipWorkerStage::kernel_entry);
  check_hip(query(&second,reinterpret_cast<const void*>(selected)),
      "corroborate compression function attributes",HipWorkerStage::kernel_entry);
  lz4_require(first.maxThreadsPerBlock==second.maxThreadsPerBlock &&
      first.sharedSizeBytes==second.sharedSizeBytes &&
      first.maxThreadsPerBlock>=static_cast<int>(device.wave) &&
      first.sharedSizeBytes<=device.shared_capacity, HipWorkerExit::unsupported_configuration,
      "compression function metadata missing, inconsistent or excessive",HipWorkerStage::kernel_entry);
}

// One control thread. The private initializer/policy are fixed by the real
// wrapper; CPU qualification injects SDK-typed functions here, never a product
// flag or replacement runtime. Borrowed source/metadata stays immutable.
// No caller receives candidates until every byte check AND retirement passes.
template<class Initialize,class MemoryPolicy>
std::vector<std::vector<std::byte>> execute_lz4_compute(const HipApi& api,
    Lz4Attributes attributes,const candidate_lz4::KernelApi& kernel,
    const Lz4WorkerRequest& request,Lz4KernelCoverage coverage,
    Initialize&& initialize,MemoryPolicy&& memory) {
  admit_lz4_compute(kernel,request,coverage);
  lz4_require(api.init && api.properties && api.set_device && api.priority_range &&
      api.create_stream && api.destroy_stream && api.synchronize && api.allocate &&
      api.release && api.copy && api.launch,HipWorkerExit::runtime_unavailable,
      "compression runtime function table incomplete",HipWorkerStage::runtime_api);
  auto plan=lz4_launch_layout(request.batch.device_layout());
  std::vector<std::byte> readback(plan.total_bytes);
  memory(HipMemoryPoint::pre_initialize);
  const auto device=initialize();
  const auto mask=device.wave==32?1U:device.wave==64?2U:0U;
  lz4_require(mask && (coverage.wave_mask&mask),HipWorkerExit::unsupported_configuration,
      "compression wave is not qualified",HipWorkerStage::device_properties);
  const auto selected=candidate_lz4::select_kernel(kernel,device.wave);
  check_lz4_attributes(attributes,selected,device);
  DeviceResources resources(api);
  try {
    check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,device.priority),
        "create compression stream",HipWorkerStage::stream_create);
    memory(HipMemoryPoint::initialized); // Includes context/stream metadata costs.
    check_hip(api.allocate(&resources.buffers[0],plan.total_bytes),
        "allocate compression slab",HipWorkerStage::execution);
    const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
    std::vector<std::byte> upload;
    try {upload=gpu_lz4_slab_upload(request.batch,address);}
    catch(const std::invalid_argument&){
      throw HipWorkerFailure(HipWorkerExit::device_fault,
          "compression device allocation address invalid",HipWorkerStage::execution);
    }
    check_hip(api.copy(resources.buffers[0],upload.data(),upload.size(),hipMemcpyHostToDevice),
        "upload compression slab",HipWorkerStage::execution);
    auto* slab=static_cast<unsigned char*>(resources.buffers[0]);
    auto allocated=plan.total_bytes;
    void* arguments[]{&slab,&allocated,&plan};
    check_hip(api.launch(reinterpret_cast<const void*>(selected),dim3(plan.count),
        dim3(device.wave),arguments,0,resources.stream),"submit compression kernel",HipWorkerStage::execution);
    check_hip(api.synchronize(resources.stream),"complete compression kernel",HipWorkerStage::execution);
    check_hip(api.copy(readback.data(),resources.buffers[0],readback.size(),hipMemcpyDeviceToHost),
        "read complete compression slab",HipWorkerStage::execution);
    std::vector<std::vector<std::byte>> result;
    try {result=verify_gpu_lz4_slab(request.batch,address,readback);}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){
      throw HipWorkerFailure(HipWorkerExit::integrity_failure,
          "compression readback failed independent CPU verification",HipWorkerStage::execution);
    }
    memory(HipMemoryPoint::completed);
    lz4_require(resources.release_all()==hipSuccess,HipWorkerExit::device_fault,
        "compression resource retirement failed",HipWorkerStage::execution);
    return result;
  }catch(...) {
    const auto failure=std::current_exception();
    lz4_require(resources.release_all()==hipSuccess && !resources.retirement_failed,
        HipWorkerExit::device_fault,"compression failure retirement unconfirmed",HipWorkerStage::execution);
    std::rethrow_exception(failure);
  }
}
} // namespace flitzfs::hip_detail

namespace flitzfs {
// SDK-only compute implementation. Caller must authenticate and retain BOTH
// modules and coverage, supervise the worker, and enforce exact process/result
// retirement. This function neither loads modules nor publishes a frame.
std::vector<std::vector<std::byte>> run_hip_lz4_in_worker(HMODULE runtime,
    const candidate_lz4::KernelApi&,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const Lz4WorkerRequest&,hip_detail::Lz4KernelCoverage);
}
