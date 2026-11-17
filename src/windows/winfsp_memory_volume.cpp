#include "winfsp_memory_volume.hpp"
#include "winfsp_native.hpp"
#include <stdexcept>
namespace flitzfs {
WinFspMemoryVolume::WinFspMemoryVolume(WinFspSessionApi api,NativeWriteMountPlan plan) {
  if(!valid_native_write_plan(plan))throw std::invalid_argument("Invalid memory-volume mount plan");
  backing_=MemoryVolumeBacking::create();
  auto adapter=WinFspNativeWritable::create_managed_paged(backing_.head,backing_.volume_identity,
      backing_.image,backing_.image_identity,{backing_.layout.image_capacity},
      {StoreCompressionMode::automatic,3,false},backing_.advice);
  backing_.domain->verify_resident();
  controller_=std::make_unique<WinFspVolumeController>(std::move(adapter),api,std::move(plan),*backing_.domain);
}
WinFspMemoryVolume::~WinFspMemoryVolume()=default;
WinFspControllerView WinFspMemoryVolume::view() const{return controller_->view();}
MemoryVolumeLayout WinFspMemoryVolume::layout() const noexcept{return backing_.layout;}
void WinFspMemoryVolume::request_stop() noexcept{controller_->request_stop();}
void WinFspMemoryVolume::stop_and_join() noexcept{controller_->stop_and_join();joined_=true;}
NativeVolumeHeadView WinFspMemoryVolume::inspect_retired() const {
  if(!joined_)throw std::runtime_error("Memory-volume inspection requires explicit controller join");
  backing_.domain->verify_resident();
  return NativeVolumeHeadView::open(backing_.head,backing_.volume_identity,[&](const StoreIdentity& id) {
    if(id!=backing_.image_identity)throw std::runtime_error("Unknown retained image identity");return backing_.image;
  });
}
ReadOnlyProjection WinFspMemoryVolume::open_read_only() {
  if(!joined_)throw std::runtime_error("Read-only mount requires explicit writable controller join");
  return backing_.open_read_only();
}
}
