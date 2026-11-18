#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "gpu_budget.hpp"
#include "memory_volume.hpp"
#include "../native_maintenance_source.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace flitzfs {
namespace {
struct Event {
  HANDLE value=nullptr;
  explicit Event(bool manual):value(CreateEventW(nullptr,manual,FALSE,nullptr)) {
    if(!value)throw std::runtime_error("Controller event creation failed");
  }
  ~Event(){CloseHandle(value);}
};
}
struct WinFspVolumeController::Impl {
  // Wait handles outlive the worker, session, callbacks and adapter duplicate.
  Event stop{true},activity{false};
  std::atomic<bool> stop_requested{false};
  mutable std::mutex mutex;
  WinFspControllerView current;
  std::unique_ptr<WinFspNativeSession> session;
  WinFspNativeWritable* adapter=nullptr; // Valid only until session.stop().
  NativeWriteMountPlan plan;
  MemoryVolumeDomain* residency=nullptr;
  std::unique_ptr<NativeAnchorSearchController> anchor;
  std::thread worker;
  Impl(std::unique_ptr<WinFspNativeWritable> source,WinFspSessionApi api,NativeWriteMountPlan p,
      MemoryVolumeDomain* memory,std::optional<NativeAnchorSearchOptions> acceleration):
      plan(std::move(p)),residency(memory) {
    if(!source || !valid_native_write_plan(plan))throw std::invalid_argument("Invalid controller source or mount plan");
    source->set_maintenance_event(activity.value);adapter=source.get();
    session=std::make_unique<WinFspNativeSession>(std::move(source),api);
    if(acceleration)anchor=std::make_unique<NativeAnchorSearchController>(std::move(*acceleration));
    worker=std::thread([this]{run();});
  }
  ~Impl(){request_stop();if(worker.joinable())worker.join();}
  void request_stop() noexcept {stop_requested.store(true,std::memory_order_release);SetEvent(stop.value);}
  void run() noexcept {
    bool disabled=false,failed=false;NTSTATUS started=STATUS_CANCELLED;
    try {
      if(!stop_requested.load(std::memory_order_acquire))started=session->start(plan);
      {
        std::lock_guard lock(mutex);current.start_status=started;current.state=session->state();
      }
      if(NT_SUCCESS(started)) {
        // No query is opened until changed content actually needs an opportunity.
        WindowsMaintenanceHost host;
        constexpr auto maximum_finite_wait=std::uint64_t{INFINITE-1};
        std::uint64_t next=0;
        while(!stop_requested.load(std::memory_order_acquire)) {
          if(session->requires_offline() || session->dispatcher_stopped())break;
          if(!disabled && adapter->maintenance_event_failed()) {
            disabled=true;session->cancel_background_maintenance();
          }
          const auto now=GetTickCount64();
          if(anchor) {
            if(disabled || adapter->foreground_pending())anchor->cancel();
            anchor->observe(adapter->foreground_activity(),now);
            adapter->signal_foreground_for_anchor(anchor->waiting());
          }
          const auto gpu=adapter->gpu_budget_task();
          if(gpu && now>gpu->after_ms) {
            // CPU completion uses a different owner and can proceed while the
            // operating system supplies telemetry. Never query under the lock.
            const auto observation=query_windows_gpu_budget(gpu->luid,gpu->node_mask);
            if(stop_requested.load(std::memory_order_acquire))break;
            const auto applied=adapter->complete_gpu_budget_task(*gpu,observation.snapshot,GetTickCount64());
            std::lock_guard lock(mutex);
            ++current.gpu_budget_queries;
            if(!applied)++current.gpu_budget_refusals;
            continue;
          }
          const bool pending=!disabled && session->background_maintenance_pending();
          if(anchor && !pending)anchor->cancel();
          const bool awaiting_anchor=anchor && anchor->waiting();
          if(!pending || now<next) {
            auto delay=pending?(std::min)(next-now,maximum_finite_wait):std::uint64_t{INFINITE};
            if(gpu)delay=(std::min)({delay,gpu->after_ms-now+1,maximum_finite_wait});
            if(awaiting_anchor) {
              const auto due=anchor->deadline();
              delay=(std::min)(delay,due>now?due-now:std::uint64_t{0});
            }
            HANDLE events[]={stop.value,activity.value,anchor?anchor->wake_handle():nullptr};
            const auto count=events[2]?3UL:2UL;
            const auto waited=WaitForMultipleObjects(count,events,FALSE,static_cast<DWORD>(delay));
            if(waited==WAIT_OBJECT_0)break;
            if(waited!=WAIT_TIMEOUT && (waited<WAIT_OBJECT_0+1 || waited>=WAIT_OBJECT_0+count))
              throw std::runtime_error("Controller event wait failed");
            continue;
          }
          const auto begin=std::chrono::steady_clock::now();
          WinFspMaintenanceResult result;
          MaintenanceThreadScope priority;
          if(!priority.active())disabled=true;
          else try {
            const auto observed=host.observe(stop_requested.load(std::memory_order_acquire));
            if(!stop_requested.load(std::memory_order_acquire) && host.status()!=MaintenanceHostStatus::waiting) {
              if(residency && observed.valid)residency->verify_working_set();
              result=advance(observed);
              if(result.state==WinFspMaintenanceState::unavailable ||
                  result.progress.phase==NativeMaintenancePhase::failed)disabled=true;
            }
            if(host.status()==MaintenanceHostStatus::unavailable)
              disabled=true;
          }catch(...){disabled=true;}
          if(!priority.finish())disabled=true;
          if(disabled)session->cancel_background_maintenance();
          const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
          const auto used=elapsed>0?static_cast<std::uint64_t>(elapsed):1;
          // 1% elapsed-work share charges telemetry and wrapper work too. File
          // activity wakes the owner but never erases accumulated pacing debt.
          const auto duty_ms=used/10000+(used%10000!=0);
          const auto delay=(std::max)({std::uint64_t(1),host.next_delay_ms(),result.progress.next_opportunity_delay_ms,duty_ms});
          next=GetTickCount64()+(std::min)(delay,maximum_finite_wait);
          {
            std::lock_guard lock(mutex);current.host=host.status();current.maintenance_disabled=disabled;
            if(result.state==WinFspMaintenanceState::advanced || result.state==WinFspMaintenanceState::unchanged)
              current.maintenance=result.progress;
            ++current.maintenance_quanta;
            if(result.reclaimed)++current.reclamation_quanta;
            if(result.compacted)++current.compactions;
            if(result.compaction_failed)++current.compaction_failures;
            if(anchor)current.anchor_search=anchor->stats();
            current.maintenance_elapsed_ns+=used;
            current.maximum_quantum_ns=(std::max)(current.maximum_quantum_ns,used);
          }
        }
      }
    }catch(...){disabled=true;failed=true;}
    // Normal priority, no view lock, and no independent maintenance thread while
    // the deferred service/dispatcher are joined. Never publish retirement early.
    if(anchor)anchor->cancel();
    session->stop();adapter=nullptr;
    const bool anchor_retired=!anchor || anchor->stop();
    std::lock_guard lock(mutex);current.state=session->state();current.start_status=started;
    current.offline=session->requires_offline();
    current.controller_failed=failed || !anchor_retired;
    if(anchor)current.anchor_search=anchor->stats();
    current.maintenance_disabled=disabled;current.retired=anchor_retired;
  }
  WinFspMaintenanceResult advance(const NativeMaintenanceObservation& observed) {
    const auto now=GetTickCount64();
    if(!anchor)return session->advance_background_maintenance(observed,now);
    const auto epoch=adapter->foreground_activity();
    const auto resources=detail::maintenance_resources(observed,now,adapter->group_maintenance_policy());
    const bool idle=resources && !resources->cpu_busy && !adapter->foreground_pending() &&
        anchor->fits_group_budget(detail::maintenance_group_member_limit(*resources,true));
    if(!idle)anchor->cancel();
    anchor->observe(epoch,now,idle);
    // Preserve PDH sampling while the worker runs. The storage step can wait
    // for advice without allowing the host observation window to expire.
    if(anchor->waiting())return {WinFspMaintenanceState::busy};
    const auto reply=anchor->reply();
    auto result=session->advance_background_maintenance(observed,GetTickCount64(),
        {idle?anchor->stage_grant(now):0,reply.window?&reply:nullptr});
    if(reply.window && !result.search_window && result.state!=WinFspMaintenanceState::busy &&
        result.progress.deferral==NativeMaintenanceDeferral::none)anchor->discard_reply();
    if(!reply.window && result.search_window && !stop_requested.load(std::memory_order_acquire)) {
      adapter->signal_foreground_for_anchor(true);
      if(!adapter->foreground_pending() && adapter->foreground_activity()==epoch)
        anchor->start(std::move(result.search_window),epoch);
      anchor->observe(adapter->foreground_activity(),GetTickCount64());
      adapter->signal_foreground_for_anchor(anchor->waiting());
    }
    return result;
  }
};
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,std::optional<NativeAnchorSearchOptions> anchor):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),nullptr,std::move(anchor))){}
WinFspVolumeController::~WinFspVolumeController()=default;
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,MemoryVolumeDomain& memory,
    std::optional<NativeAnchorSearchOptions> anchor):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),&memory,std::move(anchor))){}
void WinFspVolumeController::request_stop() noexcept {impl_->request_stop();}
void WinFspVolumeController::stop_and_join() noexcept {
  impl_->request_stop();if(impl_->worker.joinable())impl_->worker.join();
}
WinFspControllerView WinFspVolumeController::view() const {std::lock_guard lock(impl_->mutex);return impl_->current;}
}
