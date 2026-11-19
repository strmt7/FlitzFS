#include "volume_controller.hpp"
#include <stdexcept>
#include <algorithm>
#include <chrono>
namespace flitzfs {
namespace {
std::string attachment_message(const VolumeAttachmentView& attachment) {
  const std::string volume=attachment.persistent?"Volume file":"Memory volume";
  switch(attachment.state) {
    case VolumeAttachmentState::unavailable:
    case VolumeAttachmentState::empty:return attachment.persistent?"Volume file closed.":
        "No attachment. Create or attach a memory volume.";
    case VolumeAttachmentState::starting:return volume+" is attaching.";
    case VolumeAttachmentState::attached:return volume+" attached.";
    case VolumeAttachmentState::detached:
      if(attachment.status<0)return volume+" retained after attachment failure. Windows status "+
          std::to_string(attachment.status)+".";
      return volume+" detached and retained.";
    case VolumeAttachmentState::blocked:return volume+" retained because attachment retirement is unconfirmed.";
  }
  return {};
}
}
VolumeController::WakeEvent::WakeEvent():value(CreateEventW(nullptr,FALSE,FALSE,nullptr)) {
  if(!value)throw std::runtime_error("Volume controller event creation failed");
}
VolumeController::WakeEvent::~WakeEvent(){CloseHandle(value);}
VolumeController::VolumeController(HWND window,std::unique_ptr<VolumeAttachment> attachment):
    window_(window),attachment_(std::move(attachment)) {
  if(!window)throw std::runtime_error("Controller requires an owned window");
  if(attachment_)view_.attachment.state=VolumeAttachmentState::empty;
  worker_=std::thread([this]{run();});
}
VolumeController::~VolumeController() {
  // Normal GUI teardown first obtains an explicit stop and observes retirement.
  // Exceptional owner teardown still must join; never detach a stale worker.
  {
    std::lock_guard lock(mutex_);stop_requested_=true;discard_on_stop_=true;foreground_pending_=true;SetEvent(wake_.value);
  }
  if(worker_.joinable())worker_.join();
}
bool VolumeController::submit(VolumeCommand command) {
  if(command.kind==VolumeCommandKind::stop || command.name.size()>127 ||
      command.text.size()>VolumeModel::text_limit)return false;
  std::lock_guard lock(mutex_);
  if(view_.busy || stop_requested_ || view_.stopped || command_)return false;
  const auto state=view_.attachment.state;
  if(command.kind==VolumeCommandKind::attach &&
      (view_.volume.ready || (state!=VolumeAttachmentState::empty && state!=VolumeAttachmentState::detached)))return false;
  if(command.kind==VolumeCommandKind::detach && state!=VolumeAttachmentState::attached)return false;
  if(view_.attachment.retained() && command.kind!=VolumeCommandKind::attach &&
      command.kind!=VolumeCommandKind::detach &&
      (command.kind!=VolumeCommandKind::discard || state!=VolumeAttachmentState::detached))return false;
  if(!SetEvent(wake_.value))return false;
  command_=std::move(command);view_.busy=true;view_.text.reset();foreground_pending_=true;return true;
}
bool VolumeController::stop(bool discard) {
  std::lock_guard lock(mutex_);
  if(view_.busy || command_ || stop_requested_)return false;
  if(!discard && !view_.can_exit_without_discard())return false;
  if(view_.attachment.retained() && view_.attachment.state!=VolumeAttachmentState::detached)return false;
  if(!SetEvent(wake_.value))return false;
  stop_requested_=true;discard_on_stop_=discard;view_.stopping=true;foreground_pending_=true;return true;
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
void VolumeController::refresh_attachment() {
  if(!attachment_)return;
  auto attachment=attachment_->refresh();
  std::lock_guard lock(mutex_);
  if(attachment!=view_.attachment) {
    auto message=attachment_message(attachment);
    view_.attachment=std::move(attachment);
    view_.message=std::move(message);
    ++view_.generation;
    notify();
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
          if(stop_requested_) {
            if(!discard_on_stop_ && (model.ready() || !view_.can_exit_without_discard()))
              throw std::runtime_error("Volume ownership prevents unattended exit");
            break;
          }
          if(command_){command=std::move(command_);command_.reset();foreground_pending_=false;}
        }
        if(!command) {
          refresh_attachment();
          const auto now=GetTickCount64();
          const bool pending=!maintenance_disabled && model.maintenance_pending();
          if(!pending || next_opportunity>now) {
            const auto delay=pending?static_cast<DWORD>((std::min)(next_opportunity-now,std::uint64_t(INFINITE-1))):INFINITE;
            const auto result=WaitForSingleObject(wake_.value,delay);
            if(result!=WAIT_OBJECT_0 && result!=WAIT_TIMEOUT)throw std::runtime_error("Volume controller wait failed");
            continue;
          }
          const auto started=std::chrono::steady_clock::now();
          const auto old=model.maintenance_progress();auto progress=old;
          std::uint64_t continued=0;
          // No shared queue/UI lock is held while this thread has reduced
          // resource priorities. A queued command preempts the next quantum.
          MaintenanceThreadScope priority;
          if(!priority.active())maintenance_disabled=true;
          else try {
            auto observed=host.observe(foreground_pending_.load());
            if(host.status()!=MaintenanceHostStatus::waiting) {
              while(!foreground_pending_.load()) {
                progress=model.maintain_groups(observed,GetTickCount64());
                if(observed.continued && progress.deferral==NativeMaintenanceDeferral::none)++continued;
                if(!can_continue_preparation(progress) || !model.maintenance_pending() || foreground_pending_.load())break;
                const auto refreshed=host.continue_preparation(NativeMaintenancePolicy{}.maximum_age_ms);
                if(!refreshed)break;
                observed=*refreshed;
              }
            }
            if(host.status()==MaintenanceHostStatus::unavailable)maintenance_disabled=true;
          }catch(...){maintenance_disabled=true;}
          if(!priority.finish())maintenance_disabled=true;
          if(maintenance_disabled){model.stop_maintenance();progress=model.maintenance_progress();}
          const auto delay=(std::max)(host.next_delay_ms(),progress.next_opportunity_delay_ms);
          const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
          const auto used=elapsed>0?static_cast<std::uint64_t>(elapsed):1;
          // Include collector, residency and priority overhead in the helper's
          // 1% elapsed-work pacing, beyond the core's separately charged work.
          const auto full_delay=maintenance_delay_ms(used);
          next_opportunity=GetTickCount64()+(std::max)(delay,full_delay);
          std::lock_guard lock(mutex_);
          ++view_.maintenance_quanta;view_.maintenance_elapsed_ns+=used;
          view_.continued_preparation_steps+=continued;
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
        VolumeAttachmentView attachment;
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
            case VolumeCommandKind::attach:
              if(model.ready())throw std::runtime_error("Close the preview volume before attaching");
              attachment_->attach(wake_.value);
              break;
            case VolumeCommandKind::detach:
              attachment_->detach();
              break;
            case VolumeCommandKind::discard:
              if(attachment_ && attachment_->retains_volume()) {
                const bool persistent=attachment_->refresh().persistent;
                attachment_->discard();
                message=persistent?"Volume file closed.":"Temporary volume explicitly discarded.";
              } else {
                model.discard();
                message="Temporary volume explicitly discarded.";
              }
              break;
            case VolumeCommandKind::stop:throw std::runtime_error("Invalid queued stop");
          }
        } catch(const std::exception& e){message=std::string("Operation failed: ")+e.what();}
        if(attachment_) {
          attachment=attachment_->refresh();
          if(message.empty())message=attachment_message(attachment);
        }
        try{volume=model.view();}
        catch(const std::exception& e){
          volume.ready=model.ready();volume.fenced=true;
          message=std::string("Volume retained; status query failed: ")+e.what();
        }
        {
          std::lock_guard lock(mutex_);view_.volume=std::move(volume);
          view_.attachment=std::move(attachment);
          view_.message=std::move(message);view_.text=std::move(text);
          view_.busy=false;++view_.generation;++view_.content_generation;notify();
        }
      }
    } // All engine owners and allocations retire before stopped is exposed.
    if(attachment_) {
      if(attachment_->refresh().state==VolumeAttachmentState::detached)attachment_->discard();
      attachment_.reset();
    }
    std::lock_guard lock(mutex_);
    view_.volume={};view_.attachment={};view_.stopped=true;view_.busy=false;++view_.generation;++view_.content_generation;
    view_.message=MemoryVolumeDomain::cleanup_healthy()?"Controller retired.":"Controller retired; memory cleanup failed.";
    notify();
  } catch(...) {
    std::lock_guard lock(mutex_);view_.volume={};view_.stopped=true;view_.busy=false;
    if(attachment_ && attachment_->retains_volume())view_.attachment={VolumeAttachmentState::blocked};
    view_.message="Controller terminated after an internal failure.";notify();
  }
}
} // namespace flitzfs
