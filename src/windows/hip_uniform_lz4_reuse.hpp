#pragma once
#include "hip_uniform_lz4_compute.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
namespace flitzfs::hip_detail {
// One authenticated bounded operation; no persistent device owner or retry.
// Initializer must qualify geometry for the plan's MAXIMUM quantum.
template<class Initialize,class MemoryPolicy>
UniformLz4Candidates execute_uniform_lz4_reused_sweep(const HipApi& api,Lz4Attributes attributes,
    const uniform_lz4::KernelApi& kernel,const UniformLz4Request& request,
    Lz4KernelCoverage coverage,UniformLz4ComputeGrant grant,
    Initialize&& initialize,MemoryPolicy&& memory){
  lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU uniform sweep verification unavailable");
  lz4_require(request.request_id && request.batch.ready() && grant.chunk_bytes &&
      request.batch.layout().chunk_bytes==grant.chunk_bytes,
      HipWorkerExit::invalid_request,"uniform sweep request/grant mismatch");
  UniformLz4SweepPlan sweep;
  try{sweep=plan_uniform_lz4_sweep(request.batch,grant.maximum_chunks,grant.maximum_device_bytes);}
  catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::invalid_request,"invalid uniform sweep grant");}
  uniform_lz4_runtime_preflight(api,kernel,coverage);
  DeviceResources resources(api);
  std::vector<std::byte> upload,readback;
  Lz4DeviceInfo device{};bool ready=false;
  const auto peak=static_cast<std::size_t>(sweep.device_peak_bytes);
  try{
    auto result=compute_uniform_lz4_sweep(request,sweep,[&](const UniformLz4Request& part){
      if(!ready){
        memory(HipMemoryPoint::pre_initialize);device=initialize();
        const auto mask=device.wave==32?1U:device.wave==64?2U:0U;
        lz4_require(mask && (mask&coverage.wave_mask),HipWorkerExit::unsupported_configuration,
            "uniform sweep wave not qualified",HipWorkerStage::device_properties);
        const auto selected=device.wave==32?kernel.wave32:kernel.wave64;
        check_lz4_attributes(attributes,selected,device);
        memory(HipMemoryPoint::initialized);
        check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,device.priority),
            "create reusable uniform stream",HipWorkerStage::stream_create);
        lz4_require(resources.stream!=nullptr,HipWorkerExit::device_fault,"null reusable uniform stream",HipWorkerStage::stream_create);
        upload.resize(peak);readback.resize(peak);
        memory(HipMemoryPoint::initialized);
        check_hip(api.allocate(&resources.buffers[0],peak),"allocate reusable uniform slab",HipWorkerStage::execution);
        const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
        lz4_require(address && !(address&7U) && sweep.device_peak_bytes<=~std::uintptr_t{0}-address,
            HipWorkerExit::device_fault,"invalid reusable uniform slab address",HipWorkerStage::execution);
        ready=true;
      }
      // Admission includes retained setup, complete result capacity, both host
      // workspaces and this owned quantum, against the original baseline.
      memory(HipMemoryPoint::initialized);
      const auto plan=part.batch.layout();
      lz4_require(plan.chunks<=sweep.quantum_chunks && plan.total_bytes<=sweep.device_peak_bytes,
          HipWorkerExit::invalid_request,"reused quantum exceeds admitted geometry");
      std::fill(upload.begin(),upload.end(),std::byte{0xd3});
      initialize_uniform_lz4_upload(part.batch,std::span(upload).first(static_cast<std::size_t>(plan.total_bytes)));
      check_hip(api.copy(resources.buffers[0],upload.data(),peak,hipMemcpyHostToDevice),
          "upload reusable uniform slab",HipWorkerStage::execution);
      auto* slab=static_cast<unsigned char*>(resources.buffers[0]);
      // The kernel receives the canonical ACTIVE extent within the larger owned
      // allocation. The remaining bytes stay protected and are read back too.
      auto allocated=plan.total_bytes;auto layout=plan;void* arguments[]{&slab,&allocated,&layout};
      const auto selected=device.wave==32?kernel.wave32:kernel.wave64;
      check_hip(api.launch(reinterpret_cast<const void*>(selected),dim3(plan.chunks),dim3(device.wave),
          arguments,0,resources.stream),"submit reusable uniform quantum",HipWorkerStage::execution);
      check_hip(api.synchronize(resources.stream),"complete reusable uniform quantum",HipWorkerStage::execution);
      check_hip(api.copy(readback.data(),resources.buffers[0],peak,hipMemcpyDeviceToHost),
          "read reusable uniform slab",HipWorkerStage::execution);
      UniformLz4Candidates candidate;
      try{
        if(!std::all_of(readback.begin()+static_cast<std::ptrdiff_t>(plan.total_bytes),readback.end(),
            [](auto value){return value==std::byte{0xd3};}))
          throw std::invalid_argument("reusable slab unused tail changed");
        candidate=verify_uniform_lz4_readback(part.batch,
            std::span<const std::byte>(readback).first(static_cast<std::size_t>(plan.total_bytes)));
      }catch(const std::bad_alloc&){throw;}
      catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::integrity_failure,
          "reused uniform quantum failed CPU/guard verification",HipWorkerStage::execution);}
      memory(HipMemoryPoint::completed);
      return candidate;
    });
    lz4_require(ready && resources.release_all()==hipSuccess && !resources.retirement_failed,
        HipWorkerExit::device_fault,"reusable uniform retirement failed",HipWorkerStage::execution);
    return result;
  }catch(...){
    const auto failure=std::current_exception();
    lz4_require(resources.release_all()==hipSuccess && !resources.retirement_failed,
        HipWorkerExit::device_fault,"reusable uniform failure retirement unconfirmed",HipWorkerStage::execution);
    std::rethrow_exception(failure);
  }
}
}
