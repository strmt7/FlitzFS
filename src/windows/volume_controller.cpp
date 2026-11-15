#include "volume_controller.hpp"
#include <stdexcept>
#include <algorithm>
#include <chrono>
namespace flitzfs {
VolumeController::VolumeController(HWND window):window_(window) {
  if(!window)throw std::runtime_error("Controller requires an owned window");
  worker_=std::thread([this]{run();});
}
VolumeController::~VolumeController() {
  // Normal GUI teardown first obtains an explicit stop and observes retirement.
  // Exceptional owner teardown still must join; never detach a stale worker.
  {
    std::lock_guard lock(mutex_);stop_requested_=true;discard_on_stop_=true;foreground_pending_=true;wake_.notify_one();
  }
  if(worker_.joinable())worker_.join();
}
bool VolumeController::submit(VolumeCommand command) {
  if(command.kind==VolumeCommandKind::stop || command.name.size()>127 ||
      command.text.size()>VolumeModel::text_limit)return false;
  std::lock_guard lock(mutex_);
  if(view_.busy || stop_requested_ || view_.stopped || command_)return false;
  command_=std::move(command);view_.busy=true;view_.text.reset();foreground_pending_=true;wake_.notify_one();return true;
}
bool VolumeController::stop(bool discard) {
  std::lock_guard lock(mutex_);
  if(view_.busy || command_ || stop_requested_)return false;
  if(view_.volume.ready && !discard)return false;
  stop_requested_=true;discard_on_stop_=discard;view_.stopping=true;foreground_pending_=true;wake_.notify_one();return true;
}
VolumeControllerView VolumeController::view() const {
  std::lock_guard lock(mutex_);return view_;
}
void VolumeController::acknowledge_notification() noexcept {
  std::lock_guard lock(mutex_);notified_=false;
}
void VolumeController::notify() noexcept {
  // Caller holds mutex_. Repeated completion messages collapse to one.
  if(!notified_) {
    notified_=PostMessageW(window_,completion_message,0,0)!=FALSE;
    if(!notified_)view_.notification_failed=true;
  }
}
void VolumeController::run() noexcept {
  try {
    {
      VolumeModel model;WindowsMaintenanceHost host;
      bool maintenance_disabled=false;std::uint64_t next_opportunity=0;
      for(;;) {
        std::optional<VolumeCommand> command;
        {
          std::unique_lock lock(mutex_);
          const auto ready=[this]{return stop_requested_||command_.has_value();};
          if(!ready()) {
            if(!maintenance_disabled && model.maintenance_pending()) {
              const auto now=GetTickCount64();
              if(next_opportunity>now)wake_.wait_for(lock,
                  std::chrono::milliseconds((std::min)(next_opportunity-now,std::uint64_t(60000))),ready);
              if(!ready() && GetTickCount64()<next_opportunity)continue;
            } else wake_.wait(lock,ready);
          }
          if(stop_requested_) {
            if(model.ready() && !discard_on_stop_)throw std::runtime_error("Unapproved temporary volume discard");
            break;
          }
          if(command_){command=std::move(command_);command_.reset();foreground_pending_=false;}
        }
        if(!command) {
          const auto started=std::chrono::steady_clock::now();
          const auto old=model.maintenance_progress();auto progress=old;
          // No shared queue/UI lock is held while this thread has reduced
          // resource priorities. A queued command preempts the next quantum.
          MaintenanceThreadScope priority;
          if(!priority.active())maintenance_disabled=true;
          else try {
            const auto observed=host.observe(foreground_pending_.load());
            if(!foreground_pending_.load() && host.status()!=MaintenanceHostStatus::waiting)
              progress=model.maintain_groups(observed,GetTickCount64());
            if(host.status()==MaintenanceHostStatus::unavailable)maintenance_disabled=true;
          }catch(...){maintenance_disabled=true;}
          if(!priority.finish())maintenance_disabled=true;
          if(maintenance_disabled){model.stop_maintenance();progress=model.maintenance_progress();}
          const auto delay=(std::max)(host.next_delay_ms(),progress.next_opportunity_delay_ms);
          const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
          const auto used=elapsed>0?static_cast<std::uint64_t>(elapsed):1;
          // Include collector, residency and priority overhead in the helper's
          // 1% elapsed-work pacing, beyond the core's separately charged work.
          const auto full_delay=used/10000+(used%10000!=0);
          next_opportunity=GetTickCount64()+(std::max)(delay,full_delay);
          std::lock_guard lock(mutex_);
          ++view_.maintenance_quanta;view_.maintenance_elapsed_ns+=used;
          view_.maintenance_maximum_quantum_ns=(std::max)(view_.maintenance_maximum_quantum_ns,used);
          const bool changed=view_.maintenance_host!=host.status() || view_.maintenance_disabled!=maintenance_disabled ||
              view_.volume.maintenance.phase!=progress.phase || view_.volume.maintenance.deferral!=progress.deferral ||
              progress.published_groups!=old.published_groups;
          view_.maintenance_host=host.status();view_.maintenance_disabled=maintenance_disabled;
          view_.volume.maintenance=progress;view_.volume.maintenance_pending=model.maintenance_pending();
          view_.volume.fenced=model.fenced();
          if(progress.retained_history_bytes_added>=old.retained_history_bytes_added)
            view_.volume.physical_bytes+=progress.retained_history_bytes_added-old.retained_history_bytes_added;
          if(changed){++view_.generation;notify();}
          continue;
        }
        std::string message;std::optional<std::string> text;VolumeView volume;
        try {
          switch(command->kind) {
            case VolumeCommandKind::create:model.create();message="Memory volume created. Not mounted.";break;
            case VolumeCommandKind::create_text:
              model.create_text(command->name,command->text);
              message="New file saved and byte-verified.";break;
            case VolumeCommandKind::read_text:text=model.read_text(command->name);message="File read and integrity-checked.";break;
            case VolumeCommandKind::optimize:
              message=model.optimize()?"Verified image maintenance completed.":"Current image retained; no new maintenance selected.";
              if(model.optimization_info().phase!=NativeOptimizationPhase::ready)
                message="Optimization deferred; optional state unavailable. File access remains available.";
              break;
            case VolumeCommandKind::discard:model.discard();message="Temporary volume explicitly discarded.";break;
            case VolumeCommandKind::stop:throw std::runtime_error("Invalid queued stop");
          }
        } catch(const std::exception& e){message=std::string("Operation failed: ")+e.what();}
        try{volume=model.view();}
        catch(const std::exception& e){
          volume.ready=model.ready();volume.fenced=true;
          message=std::string("Volume retained; status query failed: ")+e.what();
        }
        {
          std::lock_guard lock(mutex_);view_.volume=std::move(volume);
          view_.message=std::move(message);view_.text=std::move(text);
          view_.busy=false;++view_.generation;++view_.content_generation;notify();
        }
      }
    } // All engine owners and allocations retire before stopped is exposed.
    std::lock_guard lock(mutex_);
    view_.volume={};view_.stopped=true;view_.busy=false;++view_.generation;++view_.content_generation;
    view_.message=MemoryVolumeDomain::cleanup_healthy()?"Controller retired.":"Controller retired; memory cleanup failed.";
    notify();
  } catch(...) {
    std::lock_guard lock(mutex_);view_.volume={};view_.stopped=true;view_.busy=false;
    view_.message="Controller terminated after an internal failure.";notify();
  }
}
} // namespace flitzfs
