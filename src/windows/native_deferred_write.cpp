#include "native_deferred_write.hpp"
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
    std::atomic<bool>& offline,NativeDeferredWriteOptions options)
    :session_(session),control_(control),offline_(offline),options_(std::move(options)){
  if(options_.expected!=WorkerExecution::hip && options_.expected!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("invalid deferred backend");
  if(!options_.hint_query||!options_.response_sink)
    throw std::invalid_argument("deferred transport callbacks are required");
  thread_=std::thread([this]{run();});
}
NativeDeferredWriteService::~NativeDeferredWriteService(){
  {std::lock_guard guard(control_);stats_.stopping=true;force_cpu_=true;changed_.notify_all();}
  if(thread_.joinable())thread_.join();
}
bool NativeDeferredWriteService::enqueue_locked(FSP_FILE_SYSTEM* fs,const NativeFileHandle& handle,
    std::span<const std::byte> bytes,UINT64 offset,bool append,bool constrained){
  if(!fs||stats_.stopping||stats_.disabled||stats_.worker_busy||stats_.native_pending||bytes.empty())return false;
  if(options_.allow_gpu && (!options_.helper_independent||!options_.workload_qualified||
      !options_.cost.qualified||!health_.compatible||health_.configuration!=options_.binding.configuration||
      !valid_process_budget(options_.budget)||!options_.deadline_ms||
      options_.deadline_ms>options_.budget.timeout_ms||bytes.size()<kNativeExtentBytes||
      check_gpu_memory_budget(health_.memory,health_.memory.luid,health_.memory.node_mask,
          GetTickCount64(),0)!=GpuGateCode::admitted))return false;
  UINT64 hint=0;
  if(!options_.hint_query(hint))return false;
  // Authorization and complete output prerequisites precede copying/defer.
  const auto state=session_.mutation_info(handle);FSP_FSCTL_FILE_INFO info{};
  if(!encode_native_file_info(state,NativeFileInfoAllocation::native_reserved_cache,info))
    throw std::invalid_argument("deferred write metadata unavailable");
  auto request=std::make_unique<Request>();
  request->bytes=own_native_uniform_source(bytes);
  request->filesystem=fs;request->hint=hint;request->handle=handle;
  request->offset=offset;request->append=append;request->constrained=constrained;
  request_=std::move(request);force_cpu_=false;
  stats_.native_pending=true;stats_.worker_busy=true;increment(stats_.queued);
  changed_.notify_all();return true;
}
void NativeDeferredWriteService::drain_locked(std::unique_lock<std::mutex>& lock){
  if(!stats_.native_pending)return;
  force_cpu_=true;changed_.notify_all();
  changed_.wait(lock,[this]{return !stats_.native_pending;});
}
void NativeDeferredWriteService::publish_locked(const GpuHealthEvidence& health){
  health_=health;changed_.notify_all();
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
      if(!uniform_lz4::add(charged.fixed_bytes,request_->bytes.size()+sizeof(Request)+
          sizeof(FSP_FSCTL_TRANSACT_RSP),charged.fixed_bytes))charged.qualified=false;
      const bool consent=options_.allow_gpu&&options_.helper_independent&&!force_cpu_&&!stats_.stopping;
      const auto admission=operation.begin(request_->handle,request_->bytes,request_->offset,
          request_->append,request_->constrained,{},NativeFileInfoAllocation::native_reserved_cache,
          options_.binding,consent,options_.workload_qualified,health_,started,charged,options_.window,
          options_.budget,options_.deadline_ms);
      if(admission.worker.decision.submitted)increment(stats_.worker_submitted);
      NativeUniformWriteResult result{};bool completed=false;
      if(!NT_SUCCESS(admission.admission.status)){result.outcome=admission.admission;completed=true;}
      std::uint64_t validation_finished=0;bool validation_seen=false;
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
        if(force){increment(stats_.cpu_forced);}
        if(auto finished=operation.finish()){result=*finished;completed=true;}
        else changed_.wait_for(guard,std::chrono::milliseconds(5));
      }
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
