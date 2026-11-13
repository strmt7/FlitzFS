#pragma once
#include "volume_model.hpp"
#include <windows.h>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace flitzfs {
enum class VolumeCommandKind {create,create_text,read_text,optimize,discard,stop};
struct VolumeCommand {VolumeCommandKind kind;std::string name,text;};
struct VolumeControllerView {
  VolumeView volume;std::string message="No volume. Create a temporary memory volume to begin.";
  std::optional<std::string> text;
  std::uint64_t generation=0;
  bool busy=false,stopping=false,stopped=false,notification_failed=false;
};
// One owned worker, one admitted command, one coalesced scalar notification.
// No heap pointer crosses the window message queue. Destroy only after stop.
class VolumeController final {
 public:
  static constexpr UINT completion_message=WM_APP+41;
  explicit VolumeController(HWND);
  ~VolumeController();
  bool submit(VolumeCommand);
  VolumeControllerView view() const;
  // Refuses an implicit discard. Caller must have explicitly obtained it.
  bool stop(bool discard_temporary_volume);
 private:
  void run() noexcept;
  void notify() noexcept;
  HWND window_;mutable std::mutex mutex_;std::condition_variable wake_;
  std::optional<VolumeCommand> command_;VolumeControllerView view_;
  bool stop_requested_=false,discard_on_stop_=false,notified_=false;
  std::thread worker_;
};
}
