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
  WinFspMemoryVolume(WinFspSessionApi,NativeWriteMountPlan,NativeImageKind=NativeImageKind::paged,
      std::optional<NativeDeferredWriteOptions> acceleration=std::nullopt,
      std::optional<NativeAnchorSearchOptions> anchor=std::nullopt);
  ~WinFspMemoryVolume();
  WinFspMemoryVolume(const WinFspMemoryVolume&)=delete;
  WinFspMemoryVolume& operator=(const WinFspMemoryVolume&)=delete;
  WinFspControllerView view() const;
  MemoryVolumeLayout layout() const noexcept;
  void request_stop() noexcept;
  void stop_and_join() noexcept;
  // Restart the selected image after the previous provider and all owned
  // workers have retired. Release all inspection views before reattaching.
  // Mount failure leaves this owner and its data available after another join.
  void remount(WinFspSessionApi,NativeWriteMountPlan,
      std::optional<NativeDeferredWriteOptions> acceleration=std::nullopt,
      std::optional<NativeAnchorSearchOptions> anchor=std::nullopt);
  // Requires this owner's explicit confirmed join, even if view says retired.
  // This const view can also inspect retained indexed file lifetimes.
  NativeVolumeHeadView inspect_retired() const;
  // After dispatcher join, finish any old indexed lifetimes before sealing.
  ReadOnlyProjection open_read_only();
 private:
  void start_controller(std::unique_ptr<WinFspNativeWritable>,WinFspSessionApi,NativeWriteMountPlan,
      std::optional<NativeDeferredWriteOptions>,std::optional<NativeAnchorSearchOptions>);
  MemoryVolumeBacking backing_;
  std::unique_ptr<WinFspVolumeController> controller_;
  const NativeImageKind kind_;
  bool joined_=false,sealed_=false;
};
}
