#include "winfsp_controller.hpp"
#include "winfsp_native.hpp"
#include "memory_volume.hpp"
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
  std::thread worker;
  Impl(std::unique_ptr<WinFspNativeWritable> source,WinFspSessionApi api,NativeWriteMountPlan p,
      MemoryVolumeDomain* memory=nullptr):plan(std::move(p)),residency(memory) {
    if(!source || !valid_native_write_plan(plan))throw std::invalid_argument("Invalid controller source or mount plan");
    source->set_maintenance_event(activity.value);adapter=source.get();
    session=std::make_unique<WinFspNativeSession>(std::move(source),api);
    worker=std::thread([this]{run();});
  }
  ~Impl(){request_stop();if(worker.joinable())worker.join();}
  void request_stop() noexcept {stop_requested.store(true,std::memory_order_release);SetEvent(stop.value);}
  void run() noexcept {
    bool disabled=false,expired=false,failed=false;NTSTATUS started=STATUS_CANCELLED;
    try {
      if(!stop_requested.load(std::memory_order_acquire))started=session->start(plan);
      {
        std::lock_guard lock(mutex);current.start_status=started;current.state=session->state();
      }
      if(NT_SUCCESS(started)) {
        // No query is opened until changed content actually needs an opportunity.
        WindowsMaintenanceHost host;
        const auto deadline=GetTickCount64()+std::uint64_t(plan.lifetime_seconds)*1000;
        std::uint64_t next=0;
        HANDLE events[]={stop.value,activity.value};
        while(!stop_requested.load(std::memory_order_acquire)) {
          if(session->requires_offline() || session->dispatcher_stopped())break;
          if(!disabled && adapter->maintenance_event_failed()) {
            disabled=true;session->cancel_background_maintenance();
          }
          const auto now=GetTickCount64();
          if(now>=deadline){expired=true;break;}
          const bool pending=!disabled && session->background_maintenance_pending();
          if(!pending || now<next) {
            const auto delay=(std::min)(deadline-now,pending?next-now:deadline-now);
            const auto waited=WaitForMultipleObjects(2,events,FALSE,static_cast<DWORD>(delay));
            if(waited==WAIT_OBJECT_0)break;
            if(waited!=WAIT_TIMEOUT && waited!=WAIT_OBJECT_0+1)throw std::runtime_error("Controller event wait failed");
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
              result=session->advance_background_maintenance(observed,GetTickCount64());
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
          const auto after=GetTickCount64();next=after+(std::min)(delay,after<deadline?deadline-after:0);
          {
            std::lock_guard lock(mutex);current.host=host.status();current.maintenance_disabled=disabled;
            if(result.state==WinFspMaintenanceState::advanced || result.state==WinFspMaintenanceState::unchanged)
              current.maintenance=result.progress;
            ++current.maintenance_quanta;
            if(result.reclaimed)++current.reclamation_quanta;
            current.maintenance_elapsed_ns+=used;
            current.maximum_quantum_ns=(std::max)(current.maximum_quantum_ns,used);
          }
        }
      }
    }catch(...){disabled=true;failed=true;}
    // Normal priority, no view lock, and no independent maintenance thread while
    // the deferred service/dispatcher are joined. Never publish retirement early.
    session->stop();adapter=nullptr;
    std::lock_guard lock(mutex);current.state=session->state();current.start_status=started;
    current.offline=session->requires_offline();current.expired=expired;
    current.controller_failed=failed;
    current.maintenance_disabled=disabled;current.retired=true;
  }
};
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan):impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan))){}
WinFspVolumeController::~WinFspVolumeController()=default;
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,MemoryVolumeDomain& memory):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),&memory)){}
void WinFspVolumeController::request_stop() noexcept {impl_->request_stop();}
void WinFspVolumeController::stop_and_join() noexcept {
  impl_->request_stop();if(impl_->worker.joinable())impl_->worker.join();
}
WinFspControllerView WinFspVolumeController::view() const {std::lock_guard lock(impl_->mutex);return impl_->current;}
}
