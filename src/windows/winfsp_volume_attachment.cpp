#include "winfsp_volume_attachment.hpp"
#include "winfsp_memory_volume.hpp"
#include "volume_directory_pin.hpp"
#include <stdexcept>
#include <utility>
#include <vector>

namespace flitzfs {
namespace {
struct MountLocation {
  std::vector<VolumeDirectoryPin> parents;
  std::wstring path;

  MountLocation() {
    wchar_t temporary[MAX_PATH]{};
    const auto length=GetTempPath2W(static_cast<DWORD>(std::size(temporary)),temporary);
    if(length<=3 || length>=std::size(temporary))
      throw std::runtime_error("Windows did not supply a usable temporary directory");
    const std::wstring_view directory(temporary,length);
    if(directory.back()!=L'\\')throw std::runtime_error("Windows supplied an invalid temporary directory");
    path=directory;
    path+=L"FlitzFS-";
    constexpr wchar_t hex[]=L"0123456789abcdef";
    for(auto b:fresh_volume_identity()) {
      const auto value=std::to_integer<unsigned>(b);
      path+=hex[value>>4];
      path+=hex[value&15U];
    }
    if(!valid_mount_directory(path))throw std::runtime_error("The temporary mount path is unsuitable");
    parents=pin_volume_path_parents(path);
    if(!absent())throw std::runtime_error("The proposed mount path is already in use or inaccessible");
  }

  bool absent() const noexcept {
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
    return GetLastError()==ERROR_FILE_NOT_FOUND;
  }
};

class WinFspMemoryAttachment final:public VolumeAttachment {
 public:
  explicit WinFspMemoryAttachment(WinFspSessionApi (*load_api)()):load_api_(load_api) {
    if(!load_api_)throw std::invalid_argument("Missing WinFsp runtime selector");
  }
  bool retains_volume() const noexcept override {return bool(volume_);}

  void attach(HANDLE state_changed) override {
    if(volume_ && state_.state!=VolumeAttachmentState::detached)
      throw std::runtime_error("The previous attachment has not retired");
    auto target=std::make_unique<MountLocation>();
    auto api=load_api_();
    NativeWriteMountPlan plan{target->path,true,true};
    if(volume_)volume_->remount(api,std::move(plan));
    else volume_=std::make_unique<WinFspMemoryVolume>(api,std::move(plan),NativeImageKind::indexed_paged,
        std::nullopt,std::nullopt,state_changed);
    target_=std::move(target);
    state_={VolumeAttachmentState::starting};
  }

  VolumeAttachmentView refresh() override {
    if(!target_ || state_.state==VolumeAttachmentState::blocked)return state_;
    const auto status=volume_->view();
    if(status.state==SessionState::running) {
      if(state_.state!=VolumeAttachmentState::attached)
        state_={VolumeAttachmentState::attached,target_->path};
    } else if(status.state==SessionState::failed || status.state==SessionState::stopped || status.retired) {
      retire();
    }
    return state_;
  }

  void detach() override {
    if(!target_ || state_.state!=VolumeAttachmentState::attached)
      throw std::runtime_error("No running memory-volume attachment");
    retire();
  }

  void discard() override {
    if(state_.state!=VolumeAttachmentState::detached)
      throw std::runtime_error("Detach the memory volume before discarding it");
    volume_.reset();
    state_={VolumeAttachmentState::empty};
  }

 private:
  void retire() {
    volume_->stop_and_join();
    const auto final=volume_->view();
    if(final.write_restart_ready && target_->absent()) {
      state_={VolumeAttachmentState::detached,{},static_cast<std::int32_t>(final.start_status)};
      target_.reset();
    } else {
      state_={VolumeAttachmentState::blocked,{},static_cast<std::int32_t>(final.start_status)};
    }
  }

  WinFspSessionApi (*load_api_)();
  std::unique_ptr<MountLocation> target_;
  std::unique_ptr<WinFspMemoryVolume> volume_;
  VolumeAttachmentView state_{VolumeAttachmentState::empty};
};
}

std::unique_ptr<VolumeAttachment> make_winfsp_memory_attachment(WinFspSessionApi (*load_api)()) {
  return std::make_unique<WinFspMemoryAttachment>(load_api);
}
}
