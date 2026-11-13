#include "volume_controller.hpp"
#include <stdexcept>
namespace flitzfs {
VolumeController::VolumeController(HWND window):window_(window) {
  if(!window)throw std::runtime_error("Controller requires an owned window");
  worker_=std::thread([this]{run();});
}
VolumeController::~VolumeController() {
  // Normal GUI teardown first obtains an explicit stop and observes retirement.
  // Exceptional owner teardown still must join; never detach a stale worker.
  {
    std::lock_guard lock(mutex_);stop_requested_=true;discard_on_stop_=true;wake_.notify_one();
  }
  if(worker_.joinable())worker_.join();
}
bool VolumeController::submit(VolumeCommand command) {
  if(command.kind==VolumeCommandKind::stop || command.name.size()>127 ||
      command.text.size()>VolumeModel::text_limit)return false;
  std::lock_guard lock(mutex_);
  if(view_.busy || stop_requested_ || view_.stopped || command_)return false;
  command_=std::move(command);view_.busy=true;view_.text.reset();wake_.notify_one();return true;
}
bool VolumeController::stop(bool discard) {
  std::lock_guard lock(mutex_);
  if(view_.busy || command_ || stop_requested_)return false;
  if(view_.volume.ready && !discard)return false;
  stop_requested_=true;discard_on_stop_=discard;view_.stopping=true;wake_.notify_one();return true;
}
VolumeControllerView VolumeController::view() const {
  std::lock_guard lock(mutex_);return view_;
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
      VolumeModel model;
      for(;;) {
        std::optional<VolumeCommand> command;
        {
          std::unique_lock lock(mutex_);
          wake_.wait(lock,[this]{return stop_requested_||command_.has_value();});
          if(stop_requested_) {
            if(model.ready() && !discard_on_stop_)throw std::runtime_error("Unapproved temporary volume discard");
            break;
          }
          command=std::move(command_);command_.reset();notified_=false;
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
              message=model.optimize()?"Verified smaller image published.":"No beneficial handoff; current image retained.";break;
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
          view_.busy=false;++view_.generation;notify();
        }
      }
    } // All engine owners and allocations retire before stopped is exposed.
    std::lock_guard lock(mutex_);
    view_.volume={};view_.stopped=true;view_.busy=false;++view_.generation;
    view_.message=MemoryVolumeDomain::cleanup_healthy()?"Controller retired.":"Controller retired; memory cleanup failed.";
    notified_=false;notify();
  } catch(...) {
    std::lock_guard lock(mutex_);view_.stopped=true;view_.busy=false;
    view_.message="Controller terminated after an internal failure.";notified_=false;notify();
  }
}
} // namespace flitzfs
