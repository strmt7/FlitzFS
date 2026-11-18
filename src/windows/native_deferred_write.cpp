#include "native_deferred_write.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace flitzfs {
namespace {
void increment(std::uint64_t& n) noexcept{if(n!=(std::numeric_limits<std::uint64_t>::max)())++n;}

}
NativeDeferredWriteService::NativeDeferredWriteService(NativeFileSession& session,std::mutex& control,
    std::atomic<bool>& offline,NativeDeferredWriteOptions options,NativeFileInfoAllocation info_policy)
    :session_(session),control_(control),offline_(offline),options_(std::move(options)),info_policy_(info_policy){
  if(!native_windows_write_limit(info_policy_))
    throw std::invalid_argument("deferred write allocation policy is unavailable");
  if(options_.expected!=WorkerExecution::hip && options_.expected!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("invalid deferred backend");
  if(!options_.hint_query||!options_.response_sink)
    throw std::invalid_argument("deferred transport callbacks are required");
  if(options_.controller_health) {
    health_=*options_.controller_health;
    const auto& memory=health_.memory;
    const auto gate=check_gpu_memory_budget(memory,memory.luid,memory.node_mask,memory.captured_ms,0);
    budget_disabled_=!health_.compatible || !health_.sequence ||
        health_.configuration!=options_.binding.configuration ||
        gate==GpuGateCode::invalid_budget ||
        (options_.expected==WorkerExecution::hip && !options_.controller_route_pins);
  }
  thread_=std::thread([this]{run();});
}
NativeDeferredWriteService::~NativeDeferredWriteService(){
  {std::lock_guard guard(control_);stats_.stopping=true;force_cpu_=true;changed_.notify_all();}
  if(thread_.joinable())thread_.join();
}
bool NativeDeferredWriteService::enqueue_locked(FSP_FILE_SYSTEM* fs,const NativeFileHandle& handle,
    std::span<const std::byte> bytes,UINT64 offset,bool append,bool constrained){
  if(!fs||stats_.stopping||stats_.disabled||stats_.worker_busy||stats_.native_pending||bytes.empty()||
      bytes.size()>native_windows_write_limit(info_policy_) || append!=(offset==UINT64_MAX))return false;
  if(controller_collects_budget() && (!options_.allow_gpu || !gpu_lz4_cpu_verification_available()))return false;
  if(options_.allow_gpu) {
    if(!options_.helper_independent || !options_.workload_qualified || !options_.cost.qualified ||
        !health_.compatible || health_.configuration!=options_.binding.configuration ||
        !valid_process_budget(options_.budget) || !options_.deadline_ms ||
        options_.deadline_ms>options_.budget.timeout_ms || bytes.size()<kNativeExtentBytes ||
        (controller_collects_budget() && (!budget_event_ || budget_disabled_)))return false;
    const auto now=GetTickCount64();
    const auto gate=check_gpu_memory_budget(health_.memory,health_.memory.luid,health_.memory.node_mask,now,0);
    if(gate!=GpuGateCode::admitted) {
      if(gate==GpuGateCode::stale_budget && now>=health_.memory.captured_ms &&
          (!last_budget_probe_ || (now>=last_budget_probe_ && now-last_budget_probe_>=kGpuMaxBudgetAgeMs)))
        request_budget_locked(health_.memory.captured_ms,false);
      return false;
    }
  }
  UINT64 hint=0;
  if(!options_.hint_query(hint))return false;
  // Authorization and complete output prerequisites precede copying/defer.
  const auto state=session_.mutation_info(handle);FSP_FSCTL_FILE_INFO info{};
  if(!encode_native_file_info(state,info_policy_,info))
    throw std::invalid_argument("deferred write metadata unavailable");
  std::unique_ptr<Request> request;
  try {
    request=std::make_unique<Request>();
    request->bytes=own_native_uniform_source(bytes,info_policy_);
  }catch(const std::bad_alloc&) {
    return false; // The synchronous CPU path can still use the caller's live buffer.
  }
  request->filesystem=fs;request->hint=hint;request->handle=handle;
  request->offset=offset;request->append=append;request->constrained=constrained;
  request_=std::move(request);completion_.reset();force_cpu_=false;
  stats_.native_pending=true;stats_.worker_busy=true;increment(stats_.queued);
  changed_.notify_all();return true;
}
void NativeDeferredWriteService::drain_locked(std::unique_lock<std::mutex>& lock){
  if(!stats_.native_pending)return;
  force_cpu_=true;changed_.notify_all();
  changed_.wait(lock,[this]{return !stats_.native_pending;});
}
void NativeDeferredWriteService::publish_locked(const GpuHealthEvidence& health){
  if(controller_collects_budget())return; // One authority for a configured controller.
  health_=health;changed_.notify_all();
}
void NativeDeferredWriteService::disable_gpu_budget_locked() noexcept {
  budget_task_.reset();
  budget_work_.store(false,std::memory_order_release);
  budget_disabled_=true;
  force_cpu_=force_cpu_ || stats_.native_pending;
  changed_.notify_all();
}
void NativeDeferredWriteService::request_budget_locked(std::uint64_t after_ms,bool completion) noexcept {
  if(!controller_collects_budget() || !budget_event_ || budget_disabled_ || stats_.stopping ||
      (budget_task_ && budget_task_->after_ms>=after_ms))return;
  if(budget_sequence_==(std::numeric_limits<std::uint64_t>::max)()) {
    disable_gpu_budget_locked();
    return;
  }
  budget_for_completion_=completion;
  budget_task_=NativeGpuBudgetTask{++budget_sequence_,after_ms,health_.memory.luid,health_.memory.node_mask};
  budget_work_.store(true,std::memory_order_release);
  if(!SetEvent(budget_event_))disable_gpu_budget_locked();
}
std::optional<NativeGpuBudgetTask> NativeDeferredWriteService::budget_task_locked() const noexcept {
  return stats_.stopping || budget_disabled_?std::nullopt:budget_task_;
}
bool NativeDeferredWriteService::accept_budget_locked(const NativeGpuBudgetTask& task,
    const GpuMemorySnapshot& observed,std::uint64_t now) noexcept {
  if(stats_.stopping || budget_disabled_ || !budget_task_ || budget_task_->sequence!=task.sequence)return false;
  const auto pending=*budget_task_;
  budget_task_.reset();
  budget_work_.store(false,std::memory_order_release);
  last_budget_probe_=now;
  const auto gate=check_gpu_memory_budget(observed,pending.luid,pending.node_mask,now,0);
  if(gate==GpuGateCode::invalid_budget || gate==GpuGateCode::wrong_adapter ||
      observed.dedicated_bytes!=health_.memory.dedicated_bytes ||
      health_.sequence==(std::numeric_limits<std::uint64_t>::max)()) {
    disable_gpu_budget_locked();
    return false;
  }
  if(gate!=GpuGateCode::stale_budget && observed.captured_ms>pending.after_ms) {
    ++health_.sequence;
    health_.memory=observed;
    changed_.notify_all();
    return true; // A valid pressure observation can still select CPU.
  }
  if(budget_for_completion_) {
    force_cpu_=true;
    changed_.notify_all();
  }
  return false;
}
NativeDeferredStop NativeDeferredWriteService::stop(){
  std::unique_lock guard(control_);stats_.stopping=true;force_cpu_=true;changed_.notify_all();
  if(!thread_.joinable())return stats_.retirement_confirmed?NativeDeferredStop::quiescent:
      NativeDeferredStop::retirement_unconfirmed;
  if(WaitForSingleObject(thread_.native_handle(),0)!=WAIT_OBJECT_0)return NativeDeferredStop::running;
  // The OS thread has exited: join cannot need control_. Keep the lock
  // so simultaneous stop callers cannot race joinable()/join().
  thread_.join();
  return stats_.retirement_confirmed?NativeDeferredStop::quiescent:NativeDeferredStop::retirement_unconfirmed;
}
void NativeDeferredWriteService::run() noexcept{
  try{
    // Persistent for the configured volume: failure/recovery history survives
    // successive writes. No callback can access this coordinator.
    ControlledNativeUniformWrite operation(session_,options_.expected);
    for(;;){
      std::unique_lock guard(control_);
      changed_.wait(guard,[this]{return stats_.stopping||stats_.native_pending;});
      if(!stats_.native_pending){if(stats_.stopping)break;continue;}
      const auto started=GetTickCount64();
      auto charged=options_.cost;
      if(!uniform_lz4::add(charged.fixed_bytes,sizeof(Request)+sizeof(FSP_FSCTL_TRANSACT_RSP)+
          2*sizeof(NativeUniformWriteResult),charged.fixed_bytes))charged.qualified=false;
      const bool consent=options_.allow_gpu&&options_.helper_independent&&!force_cpu_&&!stats_.stopping;
      const auto admission=operation.begin(request_->handle,request_->bytes,request_->offset,
          request_->append,request_->constrained,{},info_policy_,
          options_.binding,consent,options_.workload_qualified,health_,started,charged,options_.window,
          options_.budget,options_.deadline_ms,&request_->bytes);
      if(admission.worker.decision.submitted)increment(stats_.worker_submitted);
      else if(controller_collects_budget()) {
        const auto reason=admission.worker.decision.route.reason;
        if(reason==GpuRouteReason::configuration_blocked || reason==GpuRouteReason::device_quarantined ||
            reason==GpuRouteReason::exhausted)disable_gpu_budget_locked();
      }
      NativeUniformWriteResult result{};bool completed=false;
      if(!NT_SUCCESS(admission.admission.status)){
        result.outcome=admission.admission;
        if(result.outcome.status==STATUS_INSUFFICIENT_RESOURCES && !result.outcome.offline_required)
          result.outcome=dispatch_native_write(session_,request_->handle,request_->bytes.data(),
              request_->offset,static_cast<ULONG>(request_->bytes.size()),request_->append,
              request_->constrained,{},info_policy_,&result.transferred,&result.info);
        completed=true;
      }
      std::uint64_t validation_finished=0;
      bool validation_seen=false,completion_requested=false;
      while(!completed){
        const bool force=force_cpu_||stats_.stopping||offline_.load(std::memory_order_acquire);
        const auto snapshot=health_.memory;const auto now=GetTickCount64();
        guard.unlock();
        if(force){(void)operation.use_cpu_fallback();(void)operation.request_cancel();}
        else if(!operation.awaiting_fresh_observation() || snapshot.captured_ms>validation_finished ||
            now-started>=options_.deadline_ms){
          (void)operation.poll(snapshot,now);
          if(operation.awaiting_fresh_observation() && !validation_seen){
            validation_finished=GetTickCount64();validation_seen=true;
          } // AFTER CPU validation returned
        }
        guard.lock();
        if(validation_seen && !completion_requested && !force_cpu_ && !stats_.stopping) {
          completion_requested=true;
          request_budget_locked(validation_finished,true);
        }
        if(force){increment(stats_.cpu_forced);}
        if(auto finished=operation.finish()){result=*finished;completed=true;}
        else changed_.wait_for(guard,std::chrono::milliseconds(5));
      }
      if(budget_for_completion_) {
        budget_task_.reset();
        budget_work_.store(false,std::memory_order_release);
      }
      completion_=result;
      if(result.outcome.offline_required)offline_.store(true,std::memory_order_release);
      if(result.candidate_count)increment(stats_.advised);
      FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
      static_assert(sizeof(response)<=(std::numeric_limits<UINT16>::max)());
      response.Size=static_cast<UINT16>(sizeof(response));response.Kind=FspFsctlTransactWriteKind;
      response.Hint=request_->hint;response.IoStatus.Status=static_cast<UINT32>(result.outcome.status);
      response.IoStatus.Information=result.transferred;response.Rsp.Write.FileInfo=result.info;
      auto* filesystem=request_->filesystem;
      stats_.native_pending=false;increment(stats_.completed);changed_.notify_all();
      guard.unlock();
      options_.response_sink(filesystem,response);
      guard.lock();increment(stats_.response_attempts);guard.unlock();
      bool retired=operation.try_retire();
      const auto allowance=valid_process_budget(options_.budget)?std::uint64_t(options_.budget.timeout_ms)+4000:4000;
      while(!retired && GetTickCount64()-started<allowance){
        (void)operation.request_cancel();
        std::unique_lock wait(control_);
        changed_.wait_for(wait,std::chrono::milliseconds(5));wait.unlock();
        retired=operation.try_retire();
      }
      guard.lock();stats_.retirement_confirmed=retired;stats_.worker_busy=false;
      request_.reset();changed_.notify_all();
      if(!retired){stats_.disabled=true;break;} // no retry, restart or discovery
      if(stats_.stopping)break;
    }
  }catch(...){
    FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));FSP_FILE_SYSTEM* filesystem=nullptr;
    {
      std::lock_guard guard(control_);
      // Fail an accepted pending request once; never replay a mutation.
      if(stats_.native_pending&&request_){
        response.Size=static_cast<UINT16>(sizeof(response));response.Kind=FspFsctlTransactWriteKind;
        response.Hint=request_->hint;response.IoStatus.Status=static_cast<UINT32>(STATUS_IO_DEVICE_ERROR);
        filesystem=request_->filesystem;increment(stats_.completed);
      }
      offline_.store(true,std::memory_order_release);stats_.disabled=true;
      stats_.native_pending=false;stats_.retirement_confirmed=false;changed_.notify_all();
    }
    if(filesystem){
      options_.response_sink(filesystem,response);
      std::lock_guard guard(control_);increment(stats_.response_attempts);
    }
  }
  std::lock_guard guard(control_);stats_.thread_exited=true;changed_.notify_all();
}
}
