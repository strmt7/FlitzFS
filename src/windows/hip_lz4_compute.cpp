#include "hip_lz4_compute.hpp"
#include "hip_decode_worker.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
namespace flitzfs {
std::vector<std::vector<std::byte>> run_hip_lz4_in_worker(HMODULE runtime,
    const candidate_lz4::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const Lz4WorkerRequest& request,
    hip_detail::Lz4KernelCoverage coverage) {
  require_hip_worker_job();
  hip_detail::admit_lz4_compute(kernel,request,coverage);
  hip_detail::lz4_require(runtime && ordinal>=0 && ordinal<64 && node_mask &&
      std::has_single_bit(node_mask) &&
      std::any_of(luid.begin(),luid.end(),[](auto b){return b!=std::byte{0};}),
      HipWorkerExit::invalid_request,"invalid compression worker configuration");
  const auto api=hip_detail::bind_api(runtime);
  const auto entry=GetProcAddress(runtime,hip_detail::lz4_attributes_symbol.data());
  hip_detail::lz4_require(entry!=nullptr,HipWorkerExit::runtime_unavailable,
      "compression function-attribute API unavailable",HipWorkerStage::runtime_api);
  const auto attributes=std::bit_cast<hip_detail::Lz4Attributes>(entry);
  GpuMemorySnapshot baseline{};
  const auto bytes=request.batch.device_layout().total_bytes;
  const auto memory=[&](HipMemoryPoint point) {
    const auto current=query_windows_gpu_budget(luid,node_mask).snapshot;
    if(point==HipMemoryPoint::pre_initialize) {
      require_hip_memory(current,luid,node_mask,GetTickCount64(),bytes,point);
      baseline=current;
    }else require_hip_memory(current,luid,node_mask,GetTickCount64(),bytes,point,&baseline);
  };
  const auto single_node=[](auto identity) {
    return verified_single_gpu_node(query_windows_gpu_topology(identity),identity);
  };
  const auto initialize=[&] {
    const auto priority=hip_detail::initialize_device(api,ordinal,luid,node_mask,single_node);
    hipDeviceProp_t properties{};
    hip_detail::check_hip(api.properties(&properties,ordinal),
        "compression launch properties",HipWorkerStage::device_properties);
    const auto observed=std::bit_cast<std::array<std::byte,8>>(std::to_array(properties.luid));
    hip_detail::lz4_require(observed==luid &&
        (properties.luidDeviceNodeMask==node_mask ||
         (!properties.luidDeviceNodeMask && node_mask==1 && single_node(luid))),
        HipWorkerExit::unsupported_configuration,"compression device identity changed",
        HipWorkerStage::adapter_luid);
    return hip_detail::lz4_device_info(properties,coverage,request.batch.count(),priority);
  };
  return hip_detail::execute_lz4_compute(api,attributes,kernel,request,coverage,initialize,memory);
}
} // namespace flitzfs
