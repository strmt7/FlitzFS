#include "winfsp_memory_volume.hpp"
#include "winfsp_native.hpp"
#include <stdexcept>
namespace flitzfs {
namespace {
constexpr StoreCompressionOptions memory_compression{StoreCompressionMode::automatic,3,false};
}
WinFspMemoryVolume::WinFspMemoryVolume(WinFspSessionApi api,NativeWriteMountPlan plan,NativeImageKind kind)
    :kind_(kind) {
  if(!valid_native_write_plan(plan))throw std::invalid_argument("Invalid memory-volume mount plan");
  if(kind_!=NativeImageKind::paged && kind_!=NativeImageKind::indexed_paged)
    throw std::invalid_argument("Memory volume needs a supported paged layout");
  backing_=MemoryVolumeBacking::create();
  const PagedStoreLimits limits{backing_.layout.image_capacity};
  auto adapter=kind_==NativeImageKind::indexed_paged?
      WinFspNativeWritable::create_managed_indexed(backing_.head,backing_.volume_identity,
          backing_.image,backing_.image_identity,limits,memory_compression):
      WinFspNativeWritable::create_managed_paged(backing_.head,backing_.volume_identity,
          backing_.image,backing_.image_identity,limits,memory_compression,backing_.advice);
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
  return backing_.open_owned_head(kind_==NativeImageKind::indexed_paged);
}
ReadOnlyProjection WinFspMemoryVolume::open_read_only() {
  auto checked=inspect_retired();
  if(checked.filesystem().needs_session_recovery()) {
    // The dispatcher and its contexts have retired. Reuse the normal indexed
    // recovery transaction against the exact checked epoch and image mapping.
    {
      auto recovered=NativeFileSession::resume_closed_indexed_volume(backing_.head,
          backing_.volume_identity,checked.info().selection.epoch,
          [&](const StoreIdentity& id){return backing_.resolve_image(id);},
          memory_compression,NativeDispositionPolicy::windows_global);
    }
    checked=backing_.open_owned_head();
  }
  return backing_.seal_checked(std::move(checked));
}
}
