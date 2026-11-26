#include "native_deferred_write.hpp"
#include "hip_route_binding.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace flitzfs {
namespace {
void increment(std::uint64_t& n) noexcept{if(n!=(std::numeric_limits<std::uint64_t>::max)())++n;}

}
bool NativeDeferredWriteOptions::matches_controller_route() const noexcept {
  if(!controller_health)return false;
  const auto& health=*controller_health;
  const auto& memory=health.memory;
  const auto gate=check_gpu_memory_budget(memory,memory.luid,memory.node_mask,memory.captured_ms,0);
  return health.compatible && health.sequence && health.configuration==binding.configuration &&
      gate!=GpuGateCode::invalid_budget && (expected==WorkerExecution::cpu_semantics ||
      (controller_route_pins && (!measured_shape || controller_route_pins->uniform_module_pinned()) &&
       controller_route_pins->matches(binding.helper,binding.configuration,binding.runtime_sha256,
           binding.ordinal,memory)));
}
NativeDeferredWriteService::NativeDeferredWriteService(NativeFileSession& session,std::mutex& control,
    std::atomic<bool>& offline,NativeDeferredWriteOptions options,NativeFileInfoAllocation info_policy)
    :session_(session),control_(control),offline_(offline),options_(std::move(options)),info_policy_(info_policy),
     work_event_(CreateEventW(nullptr,FALSE,FALSE,nullptr),CloseHandle){
  if(!work_event_)throw std::runtime_error("deferred write work event unavailable");
  if(!native_windows_write_limit(info_policy_))
    throw std::invalid_argument("deferred write allocation policy is unavailable");
  if(options_.expected!=WorkerExecution::hip && options_.expected!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("invalid deferred backend");
  if(!options_.hint_query||!options_.response_sink)
    throw std::invalid_argument("deferred transport callbacks are required");
  if(options_.prepared_only && (!options_.measured_shape || !options_.controller_health ||
      !options_.binding.sweep || options_.deadline_ms>=options_.budget.timeout_ms))
    throw std::invalid_argument("prepared writes require a measured sweep with separate readiness time");
  if(options_.controller_health) {
    health_=*options_.controller_health;
    budget_disabled_=!options_.matches_controller_route();
  }
  thread_=std::thread([this]{run();});
}
NativeDeferredWriteService::~NativeDeferredWriteService(){
  {std::lock_guard guard(control_);stats_.stopping=true;force_cpu_=true;notify_worker_locked();}
  if(thread_.joinable())thread_.join();
}
GpuLz4ResidentCost NativeDeferredWriteService::charge_request(GpuLz4ResidentCost cost) noexcept {
  if(!uniform_lz4::add(cost.fixed_bytes,sizeof(Request)+sizeof(FSP_FSCTL_TRANSACT_RSP)+
        2*sizeof(NativeUniformWriteResult),cost.fixed_bytes))cost.qualified=false;
  return cost;
}
std::optional<GpuLz4WindowPlan> NativeDeferredWriteService::measured_write_plan(
    const NativeDeferredWriteOptions& options,const GpuHealthEvidence& evidence,std::uint64_t now) noexcept {
  if(!options.measured_shape || !valid_process_budget(options.budget) ||
      !options.deadline_ms || options.deadline_ms>options.budget.timeout_ms)return {};
  const auto bytes=options.measured_shape->input_bytes;
  if(!bytes || bytes%kNativeExtentBytes)return {};
  const auto cost=charge_native_uniform_source(charge_request(options.cost),bytes);
  const auto& sweep=options.binding.sweep;
  if(sweep && !cost.sweep_timing)return {};
  const auto plan=sweep?plan_gpu_lz4_sweep_window(bytes,kNativeExtentBytes,evidence,now,
      cost,options.window,*sweep,options.deadline_ms):
      plan_gpu_lz4_window(bytes,kNativeExtentBytes,evidence,now,cost,options.window);
  UniformLz4Limits limits;
  if(plan.code!=GpuGateCode::admitted || plan.layout.input_bytes!=bytes ||
      !try_uniform_lz4_limits(plan.layout,limits) ||
      !uniform_worker_host_budget_covers(limits,options.budget,plan.host_peak_bytes))return {};
  return plan;
}
bool NativeDeferredWriteService::measured_write_funded(const NativeDeferredWriteOptions& options,
    const GpuHealthEvidence& evidence,std::uint64_t now) noexcept {
  return measured_write_plan(options,evidence,now).has_value();
}
bool native_amd_write_geometry(NativeFileInfoAllocation mode,std::uint64_t offset,
    std::size_t bytes,bool append,bool constrained) noexcept {
  const auto maximum=native_windows_eof_limit(mode);
  return !append && !constrained && bytes>=kNativeExtentBytes && bytes%kNativeExtentBytes==0 &&
      bytes<=native_windows_write_limit(mode) && offset%kNativeExtentBytes==0 &&
      offset<=maximum && bytes<=maximum-offset;
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
        (controller_collects_budget() && (!activity_event_ || budget_disabled_)))return false;
    if(const auto& shape=options_.measured_shape;shape &&
        (bytes.size()!=shape->input_bytes || !session_.matches_managed_foreground(shape->storage) ||
         !native_amd_write_geometry(info_policy_,offset,bytes.size(),append,constrained)))return false;
    const auto now=GetTickCount64();
    const auto gate=check_gpu_memory_budget(health_.memory,health_.memory.luid,health_.memory.node_mask,now,0);
    if(gate!=GpuGateCode::admitted) {
      if(gate==GpuGateCode::stale_budget && now>=health_.memory.captured_ms &&
          (!last_budget_probe_ || (now>=last_budget_probe_ && now-last_budget_probe_>=kGpuMaxBudgetAgeMs)))
        request_budget_locked(health_.memory.captured_ms);
      return false;
    }
    if(options_.measured_shape) {
      const auto plan=measured_write_plan(options_,health_,now);
      if(!plan)return false;
      if(options_.prepared_only) {
        if(!preparation_ready_locked(now) || !preparation_->matches_quantum(plan->quantum))return false;
      }else if(!WorkerExchangeSession::available())return false;
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
  request_=std::move(request);completion_.reset();force_cpu_=false;completion_budget_accepted_=false;
  stats_.native_pending=true;stats_.worker_busy=true;increment(stats_.queued);
  notify_worker_locked();return true;
}
bool NativeDeferredWriteService::preparation_ready_locked(std::uint64_t now) const noexcept {
  return preparation_ && now<preparation_->expires_ms &&
      options_.deadline_ms<=preparation_->expires_ms-now && preparation_->child.wait_prepared();
}
NativeDeferredWriteStats NativeDeferredWriteService::stats_locked() const noexcept {
  auto result=stats_;
  result.preparation_ready=preparation_ready_locked(GetTickCount64());
  return result;
}
void NativeDeferredWriteService::note_cpu_write_locked(std::size_t bytes,UINT64 offset,bool append,
    bool constrained) noexcept {
  if(!options_.prepared_only || !options_.allow_gpu || !options_.workload_qualified ||
      !options_.helper_independent || budget_disabled_ || stats_.stopping || stats_.worker_busy ||
      offline_.load(std::memory_order_acquire) || !activity_event_ ||
      preparation_ || preparation_starting_ || prepare_requested_ ||
      bytes!=options_.measured_shape->input_bytes ||
      !native_amd_write_geometry(info_policy_,offset,bytes,append,constrained) ||
      !session_.matches_managed_foreground(options_.measured_shape->storage))return;
  prepare_requested_=true;
  const auto now=GetTickCount64();
  if(check_gpu_memory_budget(health_.memory,health_.memory.luid,health_.memory.node_mask,now,0)==GpuGateCode::stale_budget)
    request_budget_locked(health_.memory.captured_ms);
  notify_worker_locked();
}
void NativeDeferredWriteService::drain_locked(std::unique_lock<std::mutex>& lock){
  if(!stats_.native_pending)return;
  force_cpu_=true;notify_worker_locked();
  published_.wait(lock,[this]{return !stats_.native_pending;});
}
void NativeDeferredWriteService::publish_locked(const GpuHealthEvidence& health){
  if(controller_collects_budget())return; // One authority for a configured controller.
  health_=health;notify_worker_locked();
}
void NativeDeferredWriteService::disable_gpu_budget_locked() noexcept {
  budget_task_.reset();
  budget_work_.store(false,std::memory_order_release);
  budget_disabled_=true;
  completion_budget_accepted_=false;
  prepare_requested_=false;
  if(stats_.native_pending)force_cpu_=true;
  notify_worker_locked();
}
void NativeDeferredWriteService::signal_activity_locked() noexcept {
  if(activity_event_ && !SetEvent(activity_event_))disable_gpu_budget_locked();
}
void NativeDeferredWriteService::notify_worker_locked() noexcept {
  // The single consumer checks the guarded state again upon waking. An
  // auto-reset event preserves a notification sent just before the wait.
  (void)SetEvent(work_event_.get());
}
void NativeDeferredWriteService::wait_for_work_locked(std::unique_lock<std::mutex>& guard,
    HANDLE supervisor,DWORD timeout) {
  const HANDLE events[]={work_event_.get(),supervisor};
  const DWORD count=supervisor?2:1;
  guard.unlock();
  const auto waited=WaitForMultipleObjects(count,events,FALSE,timeout);
  guard.lock();
  if(waited==WAIT_TIMEOUT || waited<WAIT_OBJECT_0+count)return;
  // The wait failed before native publication. Drain the owned source on
  // CPU and stop this optional service without fencing the volume.
  stats_.disabled=true;
  stats_.stopping=true;
  disable_gpu_budget_locked();
}
void NativeDeferredWriteService::request_budget_locked(std::uint64_t after_ms,
    std::int64_t validation_counter) noexcept {
  if(!controller_collects_budget() || !activity_event_ || budget_disabled_ || stats_.stopping ||
      (budget_task_ && (budget_task_->validation_counter ||
          (!validation_counter && budget_task_->after_ms>=after_ms))))return;
  if(budget_sequence_==(std::numeric_limits<std::uint64_t>::max)()) {
    disable_gpu_budget_locked();
    return;
  }
  budget_task_=NativeGpuBudgetTask{++budget_sequence_,after_ms,health_.memory.luid,health_.memory.node_mask,
      options_.controller_route_pins,validation_counter};
  budget_work_.store(true,std::memory_order_release);
  signal_activity_locked();
}
std::optional<NativeGpuBudgetTask> NativeDeferredWriteService::budget_task_locked() const noexcept {
  return stats_.stopping || budget_disabled_?std::nullopt:budget_task_;
}
bool NativeDeferredWriteService::accept_budget_locked(const NativeGpuBudgetTask& task,
    const GpuMemorySnapshot& observed,std::uint64_t now,std::int64_t capture_counter) noexcept {
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
  const bool completion=pending.validation_counter!=0;
  // Windows allows one counter tick of ambiguity across threads. The
  // coarse observation still governs age and rejects backward movement.
  const bool ordered=completion?observed.captured_ms>=pending.after_ms &&
      capture_counter>1 && capture_counter-1>pending.validation_counter:observed.captured_ms>pending.after_ms;
  if(gate!=GpuGateCode::stale_budget && ordered) {
    ++health_.sequence;
    health_.memory=observed;
    completion_budget_accepted_=completion;
    notify_worker_locked();
    return true; // A valid pressure observation can still select CPU.
  }
  if(completion)force_cpu_=true;
  notify_worker_locked();
  return false;
}
NativeDeferredStop NativeDeferredWriteService::stop(){
  std::unique_lock guard(control_);stats_.stopping=true;force_cpu_=true;notify_worker_locked();
  if(!thread_.joinable())return stats_.retirement_confirmed?NativeDeferredStop::quiescent:
      NativeDeferredStop::retirement_unconfirmed;
  if(WaitForSingleObject(thread_.native_handle(),0)!=WAIT_OBJECT_0)return NativeDeferredStop::running;
  // The OS thread has exited: join cannot need control_. Keep the lock
  // so simultaneous stop callers cannot race joinable()/join().
  thread_.join();
  return stats_.retirement_confirmed?NativeDeferredStop::quiescent:NativeDeferredStop::retirement_unconfirmed;
}
bool NativeDeferredWriteService::retire_unused_preparation_locked(std::unique_lock<std::mutex>& guard,
    NativeUniformWritePreparation& prepared) {
  auto& child=prepared.child;
  const auto handle=static_cast<HANDLE>(child.supervisor_wait_handle());
  if(!handle)return true; // The operation took ownership of this session.
  guard.unlock();
  if(!child.retirement_confirmed()) {
    (void)child.request_cancel();
    (void)WaitForSingleObject(handle,4000);
  }
  const auto retired=child.retirement_confirmed();
  guard.lock();
  if(retired)increment(stats_.unused_preparations_retired);
  else {
    stats_.retirement_confirmed=false;
    stats_.disabled=true;
    stats_.stopping=true;
    disable_gpu_budget_locked();
  }
  return retired;
}
void NativeDeferredWriteService::wait_for_write_locked(std::unique_lock<std::mutex>& guard) {
  while(!stats_.native_pending) {
    const auto now=GetTickCount64();
    if(preparation_) {
      const auto* result=preparation_->child.try_result();
      const auto funded=measured_write_plan(options_,health_,health_.memory.captured_ms);
      const bool changed=budget_disabled_ || offline_.load(std::memory_order_acquire) ||
          !session_.matches_managed_foreground(options_.measured_shape->storage) ||
          !funded || !preparation_->matches_quantum(funded->quantum);
      if(stats_.stopping || changed || result || now>=preparation_->expires_ms) {
        if(result && result->code!=WorkerExchangeCode::timed_out && result->code!=WorkerExchangeCode::cancelled)
          disable_gpu_budget_locked();
        auto retired=std::move(*preparation_);
        preparation_.reset();
        (void)retire_unused_preparation_locked(guard,retired);
        continue;
      }
    }
    if(stats_.stopping)return;
    if(!preparation_ && prepare_requested_ && !budget_task_) {
      prepare_requested_=false;
      const auto plan=measured_write_plan(options_,health_,now);
      if(!plan || budget_disabled_ || offline_.load(std::memory_order_acquire) ||
          !session_.matches_managed_foreground(options_.measured_shape->storage) ||
          !WorkerExchangeSession::available())continue;
      const auto observed=health_;
      preparation_starting_=true;
      guard.unlock();
      std::optional<NativeUniformWritePreparation> pending;
      bool device_ready=true;
      try {
        device_ready=!options_.controller_route_pins || options_.controller_route_pins->device_ready();
        if(device_ready) {
          const auto limits=uniform_lz4_limits(plan->layout);
          const GpuLz4SweepGrant grant{plan->quantum.chunks,plan->quantum.total_bytes};
          auto dispatch=prepare_native_uniform_dispatch(options_.binding,observed,plan->layout,options_.expected,&grant);
          auto arguments=dispatch.arguments;
          if(options_.expected==WorkerExecution::hip)arguments.front()=L"--driver-uniform-lz4-prepared-adapter";
          auto child=WorkerExchangeSession::start_prepared_sized(options_.binding.helper,arguments,
              {static_cast<std::size_t>(limits.request_bytes),static_cast<std::size_t>(limits.reply_bytes)},
              options_.budget,options_.controller_route_pins);
          pending.emplace(NativeUniformWritePreparation{std::move(child),std::move(dispatch),now+options_.budget.timeout_ms});
        }
      }catch(const std::exception&) {} // CPU service continues if preparation cannot be created.
      guard.lock();
      preparation_starting_=false;
      if(!device_ready)disable_gpu_budget_locked();
      if(pending) {
        preparation_.emplace(std::move(*pending));
        increment(stats_.preparations_started);
      }
      continue;
    }
    if(preparation_)
      wait_for_work_locked(guard,preparation_->child.supervisor_wait_handle(),
          static_cast<DWORD>(preparation_->expires_ms-now));
    else wait_for_work_locked(guard,nullptr,INFINITE);
  }
}
void NativeDeferredWriteService::run() noexcept{
  try{
    // Persistent for the configured volume: failure/recovery history survives
    // successive writes. No callback can access this coordinator.
    ControlledNativeUniformWrite operation(session_,options_.expected);
    for(;;){
      std::unique_lock guard(control_);
      wait_for_write_locked(guard);
      if(!stats_.native_pending)break;
      auto prepared=std::move(preparation_);
      preparation_.reset();
      if(options_.controller_route_pins && options_.allow_gpu && !force_cpu_ && !stats_.stopping) {
        guard.unlock();
        const auto ready=options_.controller_route_pins->device_ready();
        guard.lock();
        if(!ready)disable_gpu_budget_locked();
      }
      const auto started=GetTickCount64();
      const auto charged=charge_request(options_.cost);
      const bool consent=options_.allow_gpu&&options_.helper_independent&&!force_cpu_&&!stats_.stopping &&
          (!options_.prepared_only || prepared.has_value());
      NativeUniformWriteStart admission;
      ControlledNativeUniformWrite::SourceGeometry geometry;
      admission.admission=operation.bind_source(request_->handle,request_->bytes,request_->offset,
          request_->append,request_->constrained,{},info_policy_,options_.binding.capture_advice_receipt,
          &request_->bytes,geometry);
      if(NT_SUCCESS(admission.admission.status)) {
        const auto observed=health_;
        // A competing callback can ask for CPU publication during preparation.
        // The source stays owned while the native publication gate stays closed.
        guard.unlock();
        operation.start_advice(admission,geometry,options_.binding,consent,options_.workload_qualified,
            observed,started,charged,options_.window,options_.budget,options_.deadline_ms,
            options_.measured_shape.has_value(),&force_cpu_,options_.controller_route_pins,prepared?&*prepared:nullptr);
        guard.lock();
      }
      if(admission.worker.decision.submitted) {
        increment(stats_.worker_submitted);
        if(admission.used_preparation)increment(stats_.prepared_writes_submitted);
      }else if(controller_collects_budget()) {
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
        const bool ordered_completion=completion_budget_accepted_;
        const auto snapshot=health_.memory;const auto now=GetTickCount64();
        guard.unlock();
        if(force){(void)operation.use_cpu_fallback();(void)operation.request_cancel();}
        else if(!operation.awaiting_fresh_observation() || ordered_completion ||
            (!controller_collects_budget() && snapshot.captured_ms>validation_finished) ||
            now-started>=options_.deadline_ms){
          (void)operation.poll(snapshot,now,ordered_completion);
          if(operation.awaiting_fresh_observation() && !validation_seen){
            validation_finished=GetTickCount64();validation_seen=true;
          } // AFTER CPU validation returned
        }
        guard.lock();
        if(controller_collects_budget() && validation_seen && !completion_requested && !force_cpu_ && !stats_.stopping) {
          completion_requested=true;
          LARGE_INTEGER validated{};
          if(QueryPerformanceCounter(&validated) && validated.QuadPart>0)
            request_budget_locked(validation_finished,validated.QuadPart);
          else disable_gpu_budget_locked();
        }
        if(force){increment(stats_.cpu_forced);}
        if(auto finished=operation.finish()){result=*finished;completed=true;}
        else if(const auto elapsed=GetTickCount64()-started;elapsed<options_.deadline_ms) {
          // A completed supervisor remains signalled. After validation, only
          // a change in CPU demand or a new budget can advance publication.
          const auto supervisor=operation.awaiting_fresh_observation()?nullptr:operation.supervisor_wait_handle();
          wait_for_work_locked(guard,supervisor,static_cast<DWORD>(options_.deadline_ms-elapsed));
        }
      }
      if(budget_task_ && budget_task_->validation_counter) {
        budget_task_.reset();
        budget_work_.store(false,std::memory_order_release);
      }
      completion_=result;
      if(result.outcome.offline_required) {
        offline_.store(true,std::memory_order_release);
        signal_activity_locked();
      }
      if(result.candidate_count)increment(stats_.advised);
      FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
      static_assert(sizeof(response)<=(std::numeric_limits<UINT16>::max)());
      response.Size=static_cast<UINT16>(sizeof(response));response.Kind=FspFsctlTransactWriteKind;
      response.Hint=request_->hint;response.IoStatus.Status=static_cast<UINT32>(result.outcome.status);
      response.IoStatus.Information=result.transferred;response.Rsp.Write.FileInfo=result.info;
      auto* filesystem=request_->filesystem;
      stats_.native_pending=false;increment(stats_.completed);published_.notify_all();
      guard.unlock();
      options_.response_sink(filesystem,response);
      guard.lock();increment(stats_.response_attempts);
      // A failed handoff may leave this job outside the operation. End that
      // reservation before checking whether the operation can retire.
      const bool unused_retired=!prepared || retire_unused_preparation_locked(guard,*prepared);
      guard.unlock();
      bool retired=unused_retired && operation.try_retire();
      const auto allowance=valid_process_budget(options_.budget)?std::uint64_t(options_.budget.timeout_ms)+4000:4000;
      if(unused_retired && !retired) {
        const auto supervisor=operation.supervisor_wait_handle();
        const auto elapsed=GetTickCount64()-started;
        if(supervisor && elapsed<allowance) {
          (void)operation.request_cancel();
          (void)WaitForSingleObject(supervisor,static_cast<DWORD>(allowance-elapsed));
          retired=operation.try_retire();
        }
      }
      guard.lock();stats_.retirement_confirmed=retired;stats_.worker_busy=false;
      request_.reset();
      signal_activity_locked();
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
      stats_.native_pending=false;stats_.retirement_confirmed=false;published_.notify_all();
      signal_activity_locked();
    }
    if(filesystem){
      options_.response_sink(filesystem,response);
      std::lock_guard guard(control_);increment(stats_.response_attempts);
    }
  }
  std::lock_guard guard(control_);stats_.thread_exited=true;
}
}
