#pragma once
#include "memory_volume_factory.hpp"
#include "winfsp_controller.hpp"
#include "flitzfs/volume_head.hpp"
namespace flitzfs {
// Explicit provider/plan composition. Owns temporary backing independently of
// dispatcher retirement; Stop unmounts but never discards the retained volume.
// Single external owner, exactly as WinFspVolumeController. No implicit driver.
class WinFspMemoryVolume final {
 public:
  WinFspMemoryVolume(WinFspSessionApi,NativeWriteMountPlan);
  ~WinFspMemoryVolume();
  WinFspMemoryVolume(const WinFspMemoryVolume&)=delete;
  WinFspMemoryVolume& operator=(const WinFspMemoryVolume&)=delete;
  WinFspControllerView view() const;
  MemoryVolumeLayout layout() const noexcept;
  void request_stop() noexcept;
  void stop_and_join() noexcept;
  // Requires this owner's explicit confirmed join, even if view says retired.
  // The returned CPU view retains its own stores/lease and verifies references.
  NativeVolumeHeadView inspect_retired() const;
 private:
  MemoryVolumeBacking backing_;
  std::unique_ptr<WinFspVolumeController> controller_;
  bool joined_=false;
};
}
