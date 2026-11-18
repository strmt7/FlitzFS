#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace flitzfs {
enum class VolumeAttachmentState { unavailable, empty, starting, attached, detached, blocked };
struct VolumeAttachmentView {
  VolumeAttachmentState state=VolumeAttachmentState::unavailable;
  std::wstring location;
  std::int32_t status=0;
  bool operator==(const VolumeAttachmentView&) const = default;
  bool retained() const noexcept {
    return state!=VolumeAttachmentState::unavailable && state!=VolumeAttachmentState::empty;
  }
};

// Owned and called only by the volume application worker. The notification
// handle survives this backend and every native session that uses it.
class VolumeAttachment {
 public:
  virtual ~VolumeAttachment()=default;
  virtual bool retains_volume() const noexcept=0;
  virtual VolumeAttachmentView refresh()=0;
  virtual void attach(HANDLE state_changed)=0;
  virtual void detach()=0;
  virtual void discard()=0;
};
}
