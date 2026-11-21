#pragma once
#include "volume_attachment.hpp"
#include "volume_model.hpp"
#include "maintenance_host.hpp"
#include <windows.h>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>
#include <functional>

namespace flitzfs {
enum class VolumeCommandKind {create,create_text,read_text,optimize,discard,stop,attach,attach_file,detach};
struct VolumeCommand {
  VolumeCommandKind kind;
  std::string name,text;
  std::optional<VolumeFileRequest> file{};
};
struct VolumeControllerView {
  VolumeView volume;std::string message="No volume. Create a temporary memory volume to begin.";
  VolumeAttachmentView attachment;
  std::optional<std::string> text;
  std::uint64_t generation=0,content_generation=0;
  bool busy=false,stopping=false,stopped=false,notification_failed=false;
  bool file_selection_available=false;
  MaintenanceHostStatus maintenance_host=MaintenanceHostStatus::warming;
  bool maintenance_disabled=false;
  std::uint64_t maintenance_quanta=0,maintenance_elapsed_ns=0,maintenance_maximum_quantum_ns=0;
  std::uint64_t continued_preparation_steps=0;
  bool has_volume() const noexcept {return volume.ready || attachment.retained();}
  bool can_select_file() const noexcept {
    return file_selection_available && idle() && !volume.ready &&
        attachment.state==VolumeAttachmentState::empty;
  }
  bool can_exit_without_discard() const noexcept {
    return !volume.ready && (!attachment.retained() ||
        (attachment.persistent && attachment.state==VolumeAttachmentState::detached));
  }
  bool idle() const noexcept {return !busy && !stopping && !stopped;}
};
// One owned worker, one admitted command, one coalesced scalar notification.
// No heap pointer crosses the window message queue. Destroy only after stop.
class VolumeController final {
 public:
  static constexpr UINT completion_message=WM_APP+41;
  using FileAttachmentFactory=std::function<std::unique_ptr<VolumeAttachment>(const VolumeFileRequest&)>;
  explicit VolumeController(HWND,std::unique_ptr<VolumeAttachment> attachment={},FileAttachmentFactory={});
  ~VolumeController();
  bool submit(VolumeCommand);
  VolumeControllerView view() const;
  // Window owner calls when consuming completion_message, before refreshing.
  // Background progress never queues a second unconsumed notification.
  void acknowledge_notification() noexcept;
  // Exit closes a detached persistent file. Discarding a temporary volume
  // requires the caller's explicit approval.
  bool stop(bool discard_temporary_volume);
 private:
  struct WakeEvent {
    HANDLE value;
    WakeEvent();
    ~WakeEvent();
  };
  void run() noexcept;
  void notify() noexcept;
  void refresh_attachment();
  VolumeAttachment* active_attachment() const noexcept;
  HWND window_;mutable std::mutex mutex_;WakeEvent wake_;
  std::unique_ptr<VolumeAttachment> attachment_,selected_file_;
  FileAttachmentFactory make_file_attachment_;
  std::optional<VolumeCommand> command_;VolumeControllerView view_;
  bool stop_requested_=false,discard_on_stop_=false,notified_=false;
  std::atomic<bool> foreground_pending_{false};
  std::thread worker_;
};
}
