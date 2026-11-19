#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "gpu_budget.hpp"
#include "memory_volume.hpp"
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
#include "worker_exchange.hpp"
#endif
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
  explicit Event(HANDLE notification) {
    if(notification && !DuplicateHandle(GetCurrentProcess(),notification,GetCurrentProcess(),
        &value,EVENT_MODIFY_STATE,FALSE,0))
      throw std::runtime_error("Lifecycle event duplication failed");
  }
  ~Event(){if(value)CloseHandle(value);}
};
}
struct WinFspVolumeController::Impl {
  // Wait handles outlive the worker, session, callbacks and adapter duplicate.
  Event stop{true},activity{false};
  Event status_changed;
  std::atomic<bool> stop_requested{false};
  mutable std::mutex mutex;
  WinFspControllerView current;
  std::unique_ptr<WinFspNativeSession> session;
  WinFspNativeWritable* adapter=nullptr; // Valid only until session.stop().
  NativeWriteMountPlan plan;
  MemoryVolumeDomain* residency=nullptr;
  std::unique_ptr<NativeAnchorSearchController> anchor;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  std::optional<InstalledAnchorProfile> installed;
  HipRouteDiscoveryResult acquisition;
  std::unique_ptr<InstalledAnchorInputWitness> acquisition_inputs;
  std::uint64_t last_acquisition_candidate=0;
  std::atomic<bool> acquisition_finished{false};
  bool acquisition_started=false,acquisition_retired=true;
  std::jthread acquisition_thread;
#endif
  std::thread worker;
  Impl(std::unique_ptr<WinFspNativeWritable> source,WinFspSessionApi api,NativeWriteMountPlan p,
      MemoryVolumeDomain* memory,std::optional<NativeAnchorSearchOptions> acceleration,HANDLE notification):
      status_changed(notification),plan(std::move(p)),residency(memory) {
    if(!source || !valid_native_write_plan(plan))throw std::invalid_argument("Invalid controller source or mount plan");
    source->set_maintenance_event(activity.value);adapter=source.get();
    session=std::make_unique<WinFspNativeSession>(std::move(source),api);
    if(acceleration)anchor=std::make_unique<NativeAnchorSearchController>(std::move(*acceleration));
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    else if(adapter->supports_anchor_search())installed=installed_anchor_profile();
#endif
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
      if(NT_SUCCESS(started) && status_changed.value)SetEvent(status_changed.value);
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
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
          settle_acquisition(true);
#endif
          if(anchor) {
            if(disabled)anchor->cancel();
            else if(adapter->foreground_pending())anchor->yield_to_foreground();
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
          std::uint64_t continued=0;
          MaintenanceThreadScope priority;
          if(!priority.active())disabled=true;
          else try {
            const auto epoch=adapter->foreground_activity();
            const auto interrupted=[&] {
              return stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
                  adapter->foreground_activity()!=epoch;
            };
            auto observed=host.observe(stop_requested.load(std::memory_order_acquire));
            if(!stop_requested.load(std::memory_order_acquire) && host.status()!=MaintenanceHostStatus::waiting) {
              for(;;) {
                if(residency && observed.valid)residency->verify_working_set();
                if(observed.continued && interrupted())break;
                auto step=advance(observed);
                if(observed.continued && step.state==WinFspMaintenanceState::busy)break;
                result=std::move(step);
                if(result.state==WinFspMaintenanceState::unavailable ||
                    result.progress.phase==NativeMaintenancePhase::failed)disabled=true;
                if(observed.continued && result.group_step &&
                    result.progress.deferral==NativeMaintenanceDeferral::none)++continued;
                if(disabled || !result.group_step || !can_continue_preparation(result.progress) || interrupted() ||
                    (anchor && anchor->waiting()) || (gpu && GetTickCount64()>gpu->after_ms))break;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
                if(acquisition_thread.joinable())break;
#endif
                const auto refreshed=host.continue_preparation(adapter->group_maintenance_policy().maximum_age_ms);
                if(!refreshed || interrupted())break;
                observed=*refreshed;
              }
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
          const auto duty_ms=maintenance_delay_ms(used);
          const auto delay=(std::max)({std::uint64_t(1),host.next_delay_ms(),result.progress.next_opportunity_delay_ms,duty_ms});
          next=GetTickCount64()+(std::min)(delay,maximum_finite_wait);
          {
            std::lock_guard lock(mutex);current.host=host.status();current.maintenance_disabled=disabled;
            if(result.state==WinFspMaintenanceState::advanced || result.state==WinFspMaintenanceState::unchanged)
              current.maintenance=result.progress;
            ++current.maintenance_quanta;
            current.continued_preparation_steps+=continued;
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
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    settle_acquisition(false);
#endif
    session->stop();adapter=nullptr;
    bool anchor_retired=!anchor || anchor->stop();
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    anchor_retired=anchor_retired && acquisition_retired;
    acquisition_inputs.reset();
#endif
    std::lock_guard lock(mutex);current.state=session->state();current.start_status=started;
    current.offline=session->requires_offline();
    current.write_restart_ready=anchor_retired && session->write_restart_ready();
    current.controller_failed=failed || !anchor_retired;
    if(anchor)current.anchor_search=anchor->stats();
    current.maintenance_disabled=disabled;current.retired=anchor_retired;
    if(status_changed.value)SetEvent(status_changed.value);
  }
  WinFspMaintenanceResult advance(const NativeMaintenanceObservation& observed) {
    const auto now=GetTickCount64();
    if(!anchor) {
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
      const bool may_acquire=installed && acquisition_retired;
      auto result=session->advance_background_maintenance(observed,now,{0,nullptr,may_acquire});
      begin_acquisition(result,observed);
      return result;
#else
      return session->advance_background_maintenance(observed,now);
#endif
    }
    const auto epoch=adapter->foreground_activity();
    const auto resources=detail::maintenance_resources(observed,now,adapter->group_maintenance_policy());
    const auto funded_extents=resources?detail::maintenance_group_member_limit(*resources,true):0;
    const bool admitted=resources && !resources->cpu_busy && anchor->fits_group_budget(funded_extents);
    const bool foreground=adapter->foreground_pending();
    if(!admitted)anchor->cancel();
    else if(foreground)anchor->yield_to_foreground();
    const bool idle=admitted && !foreground;
    anchor->observe(epoch,now,idle);
    // Preserve PDH sampling while the worker runs. The storage step can wait
    // for advice without allowing the host observation window to expire.
    if(anchor->waiting())return {WinFspMaintenanceState::busy};
    const auto replies=anchor->replies();
    auto result=session->advance_background_maintenance(observed,GetTickCount64(),
        {idle?anchor->stage_grant(now,funded_extents):0,nullptr,true,replies});
    if(!replies.empty()) {
      anchor->settle_reply_checks();
      if(!result.retained_search && result.state!=WinFspMaintenanceState::busy &&
          (result.progress.deferral==NativeMaintenanceDeferral::none ||
           result.progress.deferral==NativeMaintenanceDeferral::capacity))anchor->discard_reply();
    } else if(!stop_requested.load(std::memory_order_acquire)) {
      auto windows=std::span<const NativeSharedBaseSearchWindow>(result.search_batch);
      if(result.search_window)windows=std::span(&result.search_window,1);
      if(!windows.empty()) {
        adapter->signal_foreground_for_anchor(true);
        if(!adapter->foreground_pending() && adapter->foreground_activity()==epoch)
          anchor->start(windows,epoch);
        anchor->observe(adapter->foreground_activity(),GetTickCount64());
        adapter->signal_foreground_for_anchor(anchor->waiting());
      }
    }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    begin_acquisition(result,observed);
#endif
    return result;
  }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  void begin_acquisition(const WinFspMaintenanceResult& result,const NativeMaintenanceObservation& observed) {
    if(!installed || !acquisition_retired || acquisition_thread.joinable() ||
        (anchor && !anchor->can_reacquire()) || result.progress.phase!=NativeMaintenancePhase::preparing ||
        result.progress.deferral!=NativeMaintenanceDeferral::none ||
        result.group_members<2 ||
        stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
        !WorkerExchangeSession::available())return;
    constexpr std::uint64_t mib=1024*1024;
    const auto& budget=installed->worker;
    if(!valid_process_budget(plan_resource_process_budget(budget.memory_mib*mib,observed.physical_available,
        observed.commit_available,budget.memory_share_divisor,budget.cpu_rate,budget.timeout_ms)))return;
    const auto candidate=result.progress.attempted_groups;
    if(acquisition_started) {
      if(candidate==last_acquisition_candidate)return;
      last_acquisition_candidate=candidate;
      if(acquisition.status!=HipDiscoveryStatus::transport_busy &&
          (!acquisition_inputs || !acquisition_inputs->changed()))return;
    }
    last_acquisition_candidate=candidate;
    acquisition_started=true;
    acquisition_inputs.reset();
    // Keep the DXGI factory and its observations on the Windows volume owner.
    try {acquisition_inputs=std::make_unique<InstalledAnchorInputWitness>();}
    catch(...) {}
    acquisition={};
    acquisition_finished.store(false,std::memory_order_relaxed);
    try {
      acquisition_thread=std::jthread([this](std::stop_token stop) noexcept {
        try {
          MaintenanceThreadScope priority;
          acquisition=priority.active()?discover_installed_hip_anchor_route(stop):
              HipRouteDiscoveryResult{HipDiscoveryStatus::resource_deferred};
        }catch(...) {acquisition={HipDiscoveryStatus::resource_deferred};}
        acquisition_finished.store(true,std::memory_order_release);
        SetEvent(activity.value);
      });
    }catch(...) {
      acquisition.status=HipDiscoveryStatus::resource_deferred;
      std::lock_guard lock(mutex);
      current.installed_anchor_status=acquisition.status;
    }
    std::lock_guard lock(mutex);
    ++current.installed_anchor_attempts;
  }
  void settle_acquisition(bool use_route) {
    if(!acquisition_thread.joinable())return;
    if(use_route && !acquisition_finished.load(std::memory_order_acquire))return;
    if(!use_route)acquisition_thread.request_stop();
    acquisition_thread.join();
    acquisition_retired=acquisition.status!=HipDiscoveryStatus::retirement_unconfirmed;
    if(use_route && acquisition.status==HipDiscoveryStatus::ready && acquisition.route &&
        !stop_requested.load(std::memory_order_acquire)) {
      try {
        if(anchor) {
          if(!anchor->rebind(std::move(*acquisition.route)))
            acquisition.status=HipDiscoveryStatus::configuration_quarantined;
        } else {
          anchor=std::make_unique<NativeAnchorSearchController>(NativeAnchorSearchOptions{
              .route=std::move(*acquisition.route),.worker=installed->worker,.minimum_members=installed->minimum_members,
              .maximum_members=installed->maximum_members,.duty_ppm=installed->duty_ppm,
              .dispatches_per_wait=installed->dispatches_per_wait});
        }
      }catch(...) {acquisition.status=HipDiscoveryStatus::resource_deferred;}
    }
    acquisition.route.reset();
    std::lock_guard lock(mutex);
    current.installed_anchor_status=acquisition.status;
    current.installed_anchor_queries=acquisition.identity_queries;
  }
#endif
};
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,std::optional<NativeAnchorSearchOptions> anchor,HANDLE state_changed):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),nullptr,std::move(anchor),state_changed)){}
WinFspVolumeController::~WinFspVolumeController()=default;
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,MemoryVolumeDomain& memory,
    std::optional<NativeAnchorSearchOptions> anchor,HANDLE state_changed):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),&memory,std::move(anchor),state_changed)){}
void WinFspVolumeController::request_stop() noexcept {impl_->request_stop();}
void WinFspVolumeController::stop_and_join() noexcept {
  impl_->request_stop();if(impl_->worker.joinable())impl_->worker.join();
}
WinFspControllerView WinFspVolumeController::view() const {std::lock_guard lock(impl_->mutex);return impl_->current;}
}
