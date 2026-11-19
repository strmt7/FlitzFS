#include "winfsp_memory_volume.hpp"
#include "winfsp_native.hpp"
#include "flitzfs/optimization_state.hpp"
#include <stdexcept>
namespace flitzfs {
namespace {
constexpr StoreCompressionOptions memory_compression{StoreCompressionMode::automatic,3,false};
}
WinFspMemoryVolume::WinFspMemoryVolume(WinFspSessionApi api,NativeWriteMountPlan plan,NativeImageKind kind,
    std::optional<NativeDeferredWriteOptions> acceleration,std::optional<NativeAnchorSearchOptions> anchor,
    HANDLE state_changed):kind_(kind),state_changed_(state_changed) {
  if(!valid_native_write_plan(plan))throw std::invalid_argument("Invalid memory-volume mount plan");
  if(kind_!=NativeImageKind::paged && kind_!=NativeImageKind::indexed_paged)
    throw std::invalid_argument("Memory volume needs a supported paged layout");
  backing_=MemoryVolumeBacking::create(kind_==NativeImageKind::indexed_paged);
  const PagedStoreLimits limits{backing_.layout.image_capacity};
  auto adapter=kind_==NativeImageKind::indexed_paged?
      WinFspNativeWritable::create_managed_indexed(backing_.head,backing_.volume_identity,
          backing_.image,backing_.image_identity,limits,memory_compression,backing_.advice):
      WinFspNativeWritable::create_managed_paged(backing_.head,backing_.volume_identity,
          backing_.image,backing_.image_identity,limits,memory_compression,backing_.advice);
  backing_.domain->verify_resident();
  start_controller(std::move(adapter),api,std::move(plan),std::move(acceleration),std::move(anchor));
}
void WinFspMemoryVolume::start_controller(std::unique_ptr<WinFspNativeWritable> adapter,WinFspSessionApi api,
    NativeWriteMountPlan plan,std::optional<NativeDeferredWriteOptions> acceleration,
    std::optional<NativeAnchorSearchOptions> anchor) {
  if(kind_==NativeImageKind::indexed_paged)adapter->configure_memory_maintenance(backing_);
  if(acceleration && acceleration->controller_health &&
      (!acceleration->measured_shape || acceleration->measured_shape->matches_storage(
          memory_compression,backing_.image->contract().write_failure_unit_bytes)))
    adapter->enable_deferred_writes_hosted(std::move(*acceleration));
  controller_=std::make_unique<WinFspVolumeController>(std::move(adapter),api,std::move(plan),*backing_.domain,
      std::move(anchor),state_changed_);
  joined_=false;
}
WinFspMemoryVolume::~WinFspMemoryVolume()=default;
WinFspControllerView WinFspMemoryVolume::view() const{return controller_->view();}
MemoryVolumeLayout WinFspMemoryVolume::layout() const noexcept{return backing_.layout;}
void WinFspMemoryVolume::request_stop() noexcept{controller_->request_stop();}
void WinFspMemoryVolume::stop_and_join() noexcept {
  controller_->stop_and_join();
  backing_.release_unselected_after_join();
  joined_=true;
}
void WinFspMemoryVolume::remount(WinFspSessionApi api,NativeWriteMountPlan plan,
    std::optional<NativeDeferredWriteOptions> acceleration,std::optional<NativeAnchorSearchOptions> anchor) {
  if(!valid_native_write_plan(plan) || !api.complete())
    throw std::invalid_argument("Invalid memory-volume remount provider or plan");
  if(!joined_ || sealed_ || !controller_->view().write_restart_ready)
    throw std::runtime_error("Remount requires a retired writable memory volume");
  if(backing_.image.use_count()!=1 || (backing_.alternate_ && backing_.alternate_.use_count()!=1))
    throw std::runtime_error("Release memory-volume inspection views before remount");
  backing_.domain->verify_resident();
  const auto advice=read_optimization_state(*backing_.advice,backing_.volume_identity);
  const auto advice_sequence=advice.code==OptimizationReadCode::available?advice.sequence:0;
  StoreIdentity selected{};
  const NativeVolumeImageResolver resolve=[&](const auto& id) {
    auto image=backing_.resolve_image(id);
    selected=id;
    return image;
  };
  auto adapter=WinFspNativeWritable::resume_managed_impl(backing_.head,backing_.volume_identity,{},resolve,
      memory_compression,advice_sequence?backing_.advice:nullptr,advice_sequence,kind_==NativeImageKind::indexed_paged);
  backing_.settle_retained_selection(selected);
  start_controller(std::move(adapter),api,std::move(plan),std::move(acceleration),std::move(anchor));
}
NativeVolumeHeadView WinFspMemoryVolume::inspect_retired() const {
  if(!joined_)throw std::runtime_error("Memory-volume inspection requires explicit controller join");
  return backing_.open_owned_head(kind_==NativeImageKind::indexed_paged);
}
ReadOnlyProjection WinFspMemoryVolume::open_read_only() {
  if(!joined_ || !controller_->view().write_restart_ready)
    throw std::runtime_error("Memory-volume sealing requires confirmed owner retirement");
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
  auto projection=backing_.seal_checked(std::move(checked));
  sealed_=true;
  return projection;
}
}
