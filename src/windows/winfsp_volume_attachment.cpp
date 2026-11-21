#include "winfsp_volume_attachment.hpp"
#include "winfsp_memory_volume.hpp"
#include "winfsp_native.hpp"
#include "flitzfs/native_volume_file.hpp"
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

}

class WinFspVolumeAttachment final:public VolumeAttachment {
 public:
  explicit WinFspVolumeAttachment(WinFspSessionApi (*load_api)(),
      std::optional<WinFspVolumeFileSelection> file={}):load_api_(load_api),selection_(std::move(file)) {
    if(!load_api_)throw std::invalid_argument("Missing WinFsp runtime selector");
    if(selection_ && selection_->create && selection_->volume==StoreIdentity{})
      throw std::invalid_argument("Missing new volume file identity");
    set_state(VolumeAttachmentState::empty);
  }
  bool retains_volume() const noexcept override {return bool(memory_) || bool(file_);}

  void attach(HANDLE state_changed) override {
    if(retains_volume() && state_.state!=VolumeAttachmentState::detached)
      throw std::runtime_error("The previous attachment has not retired");
    auto target=std::make_unique<MountLocation>();
    const auto api=load_api_();
    NativeWriteMountPlan plan{target->path,true,true};
    if(!api.complete() || !valid_native_write_plan(plan))
      throw std::runtime_error("The selected Windows attachment provider is incomplete");
    if(selection_) {
      // A failed preflight must leave the requested backing untouched. The
      // session rechecks the mount point immediately before using it.
      wchar_t device[]=L"" FSP_FSCTL_DISK_DEVICE_NAME;
      const auto status=api.preflight(device,plan.directory.data());
      if(!NT_SUCCESS(status))
        throw std::runtime_error("WinFsp could not prepare the volume attachment. Windows status "+
            std::to_string(status));
      start_file(api,std::move(plan),state_changed);
    }
    else if(memory_)memory_->remount(api,std::move(plan));
    else memory_=std::make_unique<WinFspMemoryVolume>(api,std::move(plan),NativeImageKind::indexed_paged,
        std::nullopt,std::nullopt,state_changed);
    target_=std::move(target);
    set_state(VolumeAttachmentState::starting);
  }

  VolumeAttachmentView refresh() override {
    if(!target_ || state_.state==VolumeAttachmentState::blocked)return state_;
    const auto status=file_controller_?file_controller_->view():memory_->view();
    if(status.state==SessionState::running) {
      if(state_.state!=VolumeAttachmentState::attached)
        set_state(VolumeAttachmentState::attached,target_->path);
      state_.integrity_warning=status.payload_scrub_faults!=0;
    } else if(status.state==SessionState::failed || status.state==SessionState::stopped || status.retired) {
      retire();
    }
    return state_;
  }

  void detach() override {
    if(!target_ || state_.state!=VolumeAttachmentState::attached)
      throw std::runtime_error("No running volume attachment");
    retire();
  }

  void discard() override {
    if(state_.state!=VolumeAttachmentState::detached)
      throw std::runtime_error("Detach the volume before closing it");
    memory_.reset();
    file_.reset();
    set_state(VolumeAttachmentState::empty);
  }

 private:
  void set_state(VolumeAttachmentState state,std::wstring location={},std::int32_t status=0) {
    state_={state,std::move(location),status,selection_.has_value()};
    if(file_)
      if(const auto* path=std::get_if<WinFspVolumeFileSelection::WindowsFile>(&selection_->backing))
        state_.backing_path=path->path.native();
  }

  void start_file(WinFspSessionApi api,NativeWriteMountPlan plan,HANDLE state_changed) {
    auto& selection=*selection_;
    constexpr StoreCompressionOptions foreground{StoreCompressionMode::automatic,3,false};
    std::unique_ptr<WinFspNativeWritable> adapter;
    if(file_) {
      adapter=WinFspNativeWritable::resume_file_volume(file_,selection.volume,foreground);
    } else {
      std::shared_ptr<StoreByteIo> backing;
      if(const auto* path=std::get_if<WinFspVolumeFileSelection::WindowsFile>(&selection.backing)) {
        const auto parents=pin_volume_path_parents(path->path.native());
        backing=selection.create?
            NativeVolumeFile::create_backing(path->path,path->qualification,selection.create->limits):
            NativeVolumeFile::reopen_backing(path->path,path->qualification);
      } else {
        backing=std::get<std::shared_ptr<StoreByteIo>>(selection.backing);
      }
      if(!backing)throw std::invalid_argument("Missing qualified volume file backing");
      // Keep the exclusive backing if initialization, a write fence, or the
      // dispatcher fails. An existing file first needs the native image audit.
      const auto retain_backing=[&](StoreIdentity volume) {
        selection.volume=volume;
        file_=backing;
        set_state(VolumeAttachmentState::detached);
      };
      if(selection.create) {
        retain_backing(selection.volume);
        const auto initial=std::exchange(selection.create,std::nullopt);
        adapter=WinFspNativeWritable::create_file_volume(file_,selection.volume,
            initial->image,initial->limits,foreground);
      } else {
        const auto expected=selection.volume==StoreIdentity{}?std::nullopt:std::optional{selection.volume};
        adapter=WinFspNativeWritable::resume_file_volume(backing,expected,foreground,retain_backing);
      }
    }
    file_controller_=std::make_unique<WinFspVolumeController>(std::move(adapter),api,std::move(plan),
        std::nullopt,state_changed);
  }

  void retire() {
    WinFspControllerView final;
    if(file_controller_) {
      file_controller_->stop_and_join();
      final=file_controller_->view();
    } else {
      memory_->stop_and_join();
      final=memory_->view();
    }
    const auto status=static_cast<std::int32_t>(final.start_status);
    if(final.write_restart_ready && target_->absent()) {
      set_state(VolumeAttachmentState::detached,{},status);
      file_controller_.reset();
      target_.reset();
    } else {
      set_state(VolumeAttachmentState::blocked,{},status);
    }
    state_.integrity_warning=final.payload_scrub_faults!=0;
  }

  WinFspSessionApi (*load_api_)();
  std::optional<WinFspVolumeFileSelection> selection_;
  std::unique_ptr<MountLocation> target_;
  std::unique_ptr<WinFspMemoryVolume> memory_;
  std::shared_ptr<StoreByteIo> file_;
  std::unique_ptr<WinFspVolumeController> file_controller_;
  VolumeAttachmentView state_;
};

std::unique_ptr<VolumeAttachment> make_winfsp_memory_attachment(WinFspSessionApi (*load_api)()) {
  return std::make_unique<WinFspVolumeAttachment>(load_api);
}

std::unique_ptr<VolumeAttachment> make_winfsp_volume_file_attachment(WinFspSessionApi (*load_api)(),
    WinFspVolumeFileSelection selection) {
  return std::make_unique<WinFspVolumeAttachment>(load_api,std::move(selection));
}
}
