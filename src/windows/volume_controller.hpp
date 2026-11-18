#pragma once
#include "volume_attachment.hpp"
#include "volume_model.hpp"
#include "maintenance_host.hpp"
#include <windows.h>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>

namespace flitzfs {
enum class VolumeCommandKind {create,create_text,read_text,optimize,discard,stop,attach,detach};
struct VolumeCommand {VolumeCommandKind kind;std::string name,text;};
struct VolumeControllerView {
  VolumeView volume;std::string message="No volume. Create a temporary memory volume to begin.";
  VolumeAttachmentView attachment;
  std::optional<std::string> text;
  std::uint64_t generation=0,content_generation=0;
  bool busy=false,stopping=false,stopped=false,notification_failed=false;
  MaintenanceHostStatus maintenance_host=MaintenanceHostStatus::warming;
  bool maintenance_disabled=false;
  std::uint64_t maintenance_quanta=0,maintenance_elapsed_ns=0,maintenance_maximum_quantum_ns=0;
  bool has_volume() const noexcept {return volume.ready || attachment.retained();}
  bool idle() const noexcept {return !busy && !stopping && !stopped;}
};
// One owned worker, one admitted command, one coalesced scalar notification.
// No heap pointer crosses the window message queue. Destroy only after stop.
class VolumeController final {
 public:
  static constexpr UINT completion_message=WM_APP+41;
  explicit VolumeController(HWND,std::unique_ptr<VolumeAttachment> attachment={});
  ~VolumeController();
  bool submit(VolumeCommand);
  VolumeControllerView view() const;
  // Window owner calls when consuming completion_message, before refreshing.
  // Background progress never queues a second unconsumed notification.
  void acknowledge_notification() noexcept;
  // Refuses an implicit discard. Caller must have explicitly obtained it.
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
  HWND window_;mutable std::mutex mutex_;WakeEvent wake_;
  std::unique_ptr<VolumeAttachment> attachment_;
  std::optional<VolumeCommand> command_;VolumeControllerView view_;
  bool stop_requested_=false,discard_on_stop_=false,notified_=false;
  std::atomic<bool> foreground_pending_{false};
  std::thread worker_;
};
}
