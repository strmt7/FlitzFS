#pragma once
#include "hip_uniform_lz4_compute.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#include "uniform_lz4_sweep_trace.hpp"
namespace flitzfs::hip_detail {
struct SweepEventApi {
  decltype(&hipEventCreate) create;
  decltype(&hipEventRecord) record;
  decltype(&hipEventElapsedTime) elapsed;
  decltype(&hipEventDestroy) destroy;
};
inline SweepEventApi bind_sweep_event_api(HMODULE runtime) {
  return {
      std::bit_cast<decltype(&hipEventCreate)>(GetProcAddress(runtime,"hipEventCreate")),
      std::bit_cast<decltype(&hipEventRecord)>(GetProcAddress(runtime,"hipEventRecord")),
      std::bit_cast<decltype(&hipEventElapsedTime)>(GetProcAddress(runtime,"hipEventElapsedTime")),
      std::bit_cast<decltype(&hipEventDestroy)>(GetProcAddress(runtime,"hipEventDestroy"))
  };
}

// The diagnostic owns exactly one pair and reuses it only after a full stream
// wait. Its API and resources are never acquired on ordinary request paths.
class SweepEventPair final {
 public:
  explicit SweepEventPair(const SweepEventApi* api):api_(api) {
    lz4_require(!api_ || (api_->create && api_->record && api_->elapsed && api_->destroy),
        HipWorkerExit::runtime_unavailable,"incomplete HIP sweep event API",HipWorkerStage::runtime_api);
  }
  SweepEventPair(const SweepEventPair&) = delete;
  SweepEventPair& operator=(const SweepEventPair&) = delete;
  ~SweepEventPair(){(void)retire();}

  void create(){
    for(auto& owned:events_){
      hipEvent_t handle=nullptr;
      check_hip(api_->create(&handle),"create sweep event",HipWorkerStage::sweep_event_create);
      lz4_require(handle && handle!=events_.front(),HipWorkerExit::device_fault,
          "invalid sweep event handle",HipWorkerStage::sweep_event_create);
      owned=handle;
    }
  }
  void record_start(hipStream_t stream) {
    check_hip(api_->record(events_.front(),stream),"record sweep start event",HipWorkerStage::sweep_event_record);
  }
  void record_stop(hipStream_t stream) {
    check_hip(api_->record(events_.back(),stream),"record sweep stop event",HipWorkerStage::sweep_event_record);
  }
  std::uint64_t interval_ns() {
    float milliseconds=std::numeric_limits<float>::quiet_NaN();
    const auto status=api_->elapsed(&milliseconds,events_.front(),events_.back());
    check_hip(status,"read sweep event interval",status==hipErrorNotReady?
        HipWorkerStage::sweep_event_not_ready:HipWorkerStage::sweep_event_elapsed);
    const auto ns=static_cast<double>(milliseconds)*1'000'000.0;
    lz4_require(ns>=0 && ns<=static_cast<double>(hip_profile_limit_ns),HipWorkerExit::device_fault,
        "invalid sweep event interval",HipWorkerStage::sweep_event_invalid_sample);
    return static_cast<std::uint64_t>(ns);
  }
  bool retire() noexcept {
    for(auto& handle:events_){
      if(!handle)continue;
      if(api_->destroy(handle)!=hipSuccess)retirement_failed_=true;
      handle=nullptr;
    }
    return !retirement_failed_;
  }
 private:
  const SweepEventApi* api_;
  std::array<hipEvent_t,2> events_{};
  bool retirement_failed_=false;
};
// The exact admitted geometry supplies setup before the request is consumed.
// A receiver may give up the prepared lease without submitting device work.
template<class Receive,class Initialize,class MemoryPolicy>
UniformLz4Candidates execute_uniform_lz4_sweep_plan(const HipApi& api,Lz4Attributes attributes,
    const uniform_lz4::KernelApi& kernel,const UniformLz4SweepPlan& sweep,Lz4KernelCoverage coverage,
    Receive&& receive,Initialize&& initialize,MemoryPolicy&& memory,UniformLz4SweepTrace* trace=nullptr,
    UniformLz4SweepRecorder::Clock clock=hip_phase_detail::clock_ns,const SweepEventApi* event_api=nullptr,
    HipStreamCopy stream_copy=nullptr) {
  lz4_require((!event_api || trace) && (!stream_copy || !trace),HipWorkerExit::invalid_request,
      "incompatible sweep timing mode and transfer path");
  lz4_require(gpu_lz4_cpu_verification_available(),HipWorkerExit::unsupported_configuration,
      "independent CPU uniform sweep verification unavailable");
  uniform_lz4::Layout whole{};
  bool canonical=false;
  if(uniform_lz4::make_layout(sweep.whole.input_bytes,sweep.whole.chunk_bytes,whole)) {
    try {canonical=sweep==plan_uniform_lz4_sweep(whole,sweep.quantum_chunks,sweep.device_peak_bytes);}
    catch(const std::invalid_argument&) {}
  }
  lz4_require(canonical,HipWorkerExit::invalid_request,"invalid prepared uniform sweep geometry");
  uniform_lz4_runtime_preflight(api,kernel,coverage);
  UniformLz4SweepRecorder recorder(trace,sweep.windows,clock);
  DeviceResources resources(api);
  SweepEventPair events(event_api);
  const auto copy=[&](void* destination,const void* source,std::size_t bytes,hipMemcpyKind kind,
      const char* operation,HipWorkerStage stage) {
    const auto status=stream_copy?stream_copy(destination,source,bytes,kind,resources.stream):
        api.copy(destination,source,bytes,kind);
    check_hip(status,operation,stage);
  };
  const auto retire=[&]{
    const auto events_retired=events.retire();
    const auto state=resources.release_all();
    return events_retired && state==hipSuccess && !resources.retirement_failed;
  };
  std::vector<std::byte> upload,readback;
  const auto peak=static_cast<std::size_t>(sweep.device_peak_bytes);
  try {
    recorder.begin(UniformSweepPhase::setup);
    memory(HipMemoryPoint::pre_initialize);
    const auto device=initialize();
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
    check_hip(api.allocate(&resources.buffers[0],peak),"allocate reusable uniform slab",HipWorkerStage::sweep_slab);
    const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
    lz4_require(address && !(address&7U) && sweep.device_peak_bytes<=~std::uintptr_t{0}-address,
        HipWorkerExit::device_fault,"invalid reusable uniform slab address",HipWorkerStage::sweep_slab);
    if(event_api)events.create();
    recorder.end(UniformSweepPhase::setup);
    const auto* request=receive();
    auto result=request?compute_uniform_lz4_sweep(*request,sweep,[&](const UniformLz4Request& part){
      // Admission includes retained setup, complete result capacity, both host
      // workspaces and this owned quantum, against the original baseline.
      recorder.begin(UniformSweepPhase::admission);
      memory(HipMemoryPoint::initialized);
      recorder.end(UniformSweepPhase::admission);recorder.begin(UniformSweepPhase::prepare);
      const auto plan=part.batch.layout();
      lz4_require(plan.chunks<=sweep.quantum_chunks && plan.total_bytes<=sweep.device_peak_bytes,
          HipWorkerExit::invalid_request,"reused quantum exceeds admitted geometry");
      std::fill(upload.begin(),upload.end(),std::byte{0xd3});
      initialize_uniform_lz4_upload(part.batch,std::span(upload).first(static_cast<std::size_t>(plan.total_bytes)));
      recorder.end(UniformSweepPhase::prepare);recorder.begin(UniformSweepPhase::upload);
      copy(resources.buffers[0],upload.data(),peak,hipMemcpyHostToDevice,
          "upload reusable uniform slab",HipWorkerStage::sweep_upload);
      recorder.end(UniformSweepPhase::upload);
      auto* slab=static_cast<unsigned char*>(resources.buffers[0]);
      // The kernel receives the canonical ACTIVE extent within the larger owned
      // allocation. The remaining bytes stay protected and are read back too.
      auto allocated=plan.total_bytes;auto layout=plan;void* arguments[]{&slab,&allocated,&layout};
      const auto launch=[&]{
        check_hip(api.launch(reinterpret_cast<const void*>(selected),dim3(plan.chunks),dim3(device.wave),
            arguments,0,resources.stream),"submit reusable uniform quantum",HipWorkerStage::sweep_launch);
      };
      const auto synchronize=[&]{
        check_hip(api.synchronize(resources.stream),"complete reusable uniform quantum",HipWorkerStage::sweep_wait);
      };
      recorder.begin(UniformSweepPhase::kernel_wait);
      if(event_api){
        events.record_start(resources.stream);
        UniformLz4KernelInterval interval;
        interval.launch_call_ns=recorder.time_kernel_call(launch);
        events.record_stop(resources.stream);
        interval.sync_call_ns=recorder.time_kernel_call(synchronize);
        interval.stream_interval_ns=events.interval_ns();
        recorder.end(UniformSweepPhase::kernel_wait,interval);
      } else {
        launch();
        if(!stream_copy)synchronize();
        recorder.end(UniformSweepPhase::kernel_wait);
      }
      recorder.begin(UniformSweepPhase::download);
      // A blocking stream-bound readback completes the preceding kernel and
      // transfer on this stream before the CPU inspects the host buffer.
      copy(readback.data(),resources.buffers[0],peak,hipMemcpyDeviceToHost,
          "read reusable uniform slab",HipWorkerStage::sweep_readback);
      recorder.end(UniformSweepPhase::download);recorder.begin(UniformSweepPhase::verification);
      UniformLz4Candidates candidate;
      try{
        if(!std::all_of(readback.begin()+static_cast<std::ptrdiff_t>(plan.total_bytes),readback.end(),
            [](auto value){return value==std::byte{0xd3};}))
          throw std::invalid_argument("reusable slab unused tail changed");
        candidate=verify_uniform_lz4_readback(part.batch,
            std::span<const std::byte>(readback).first(static_cast<std::size_t>(plan.total_bytes)));
      }catch(const std::bad_alloc&){throw;}
      catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::integrity_failure,
          "reused uniform quantum failed CPU/guard verification",HipWorkerStage::sweep_verify);}
      recorder.end(UniformSweepPhase::verification);recorder.begin(UniformSweepPhase::completion);
      memory(HipMemoryPoint::completed);
      recorder.end(UniformSweepPhase::completion);
      return candidate;
    }):UniformLz4Candidates{};
    if(request)recorder.begin(UniformSweepPhase::retirement);
    lz4_require(retire(),HipWorkerExit::device_fault,"reusable uniform retirement failed",HipWorkerStage::sweep_retire);
    if(request) {
      recorder.end(UniformSweepPhase::retirement);
      recorder.finish();
    }
    return result;
  }catch(...){
    const auto failure=std::current_exception();
    lz4_require(retire(),HipWorkerExit::device_fault,"reusable uniform failure retirement unconfirmed",HipWorkerStage::sweep_retire);
    std::rethrow_exception(failure);
  }
}
template<class Initialize,class MemoryPolicy>
UniformLz4Candidates execute_uniform_lz4_reused_sweep(const HipApi& api,Lz4Attributes attributes,
    const uniform_lz4::KernelApi& kernel,const UniformLz4Request& request,
    Lz4KernelCoverage coverage,UniformLz4ComputeGrant grant,
    Initialize&& initialize,MemoryPolicy&& memory,UniformLz4SweepTrace* trace=nullptr,
    UniformLz4SweepRecorder::Clock clock=hip_phase_detail::clock_ns,const SweepEventApi* event_api=nullptr,
    HipStreamCopy stream_copy=nullptr) {
  lz4_require(request.request_id && request.batch.ready() && grant.chunk_bytes &&
      request.batch.layout().chunk_bytes==grant.chunk_bytes,
      HipWorkerExit::invalid_request,"uniform sweep request/grant mismatch");
  UniformLz4SweepPlan sweep;
  try{sweep=plan_uniform_lz4_sweep(request.batch,grant.maximum_chunks,grant.maximum_device_bytes);}
  catch(const std::exception&){throw HipWorkerFailure(HipWorkerExit::invalid_request,"invalid uniform sweep grant");}
  return execute_uniform_lz4_sweep_plan(api,attributes,kernel,sweep,coverage,[&]{return &request;},
      initialize,memory,trace,clock,event_api,stream_copy);
}
}
