#include "memory_volume_factory.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/volume_head.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <stdexcept>
namespace flitzfs {
namespace {
constexpr std::uint64_t mib=1024ULL*1024,unit=65536;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
}
MemoryVolumeLayout plan_memory_volume(const MemoryVolumeObservation& m) {
  // Keep automatic lock admission within a small share of measured headroom.
  const auto capacity = usable_memory_volume_headroom(m) / 256 / unit * unit;
  constexpr auto areas = MemoryVolumeLayout::area_capacity * (NativeFileSession::max_optimization_bindings + 1);
  require(capacity >= areas, "Insufficient head and advisory space");
  const auto image = (capacity - areas) / 2 / unit * unit;
  require(image >= 4*mib, "Insufficient admitted paged-volume capacity");
  return {capacity,image};
}
StoreIdentity fresh_volume_identity() {
  StoreIdentity id{};
  require(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(id.data()),static_cast<ULONG>(id.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0 &&
      std::any_of(id.begin(),id.end(),[](auto b){return b!=std::byte{};}),"Volume identity generation failed");
  return id;
}
MemoryVolumeBacking MemoryVolumeBacking::create() {
  MemoryVolumeBacking out;
  out.layout=plan_memory_volume(observe_memory_volume_headroom());
  out.volume_identity=fresh_volume_identity();out.image_identity=fresh_volume_identity();
  require(out.volume_identity!=out.image_identity,"Volume identity collision");
  out.domain=std::make_unique<MemoryVolumeDomain>(MemoryVolumeDomain::create(out.layout.domain_capacity));
  out.head=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.image=out.domain->create_store(out.layout.image_capacity);
  out.advice=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.domain->verify_resident();return out;
}
ReadOnlyProjection MemoryVolumeBacking::open_read_only() {
  require(domain && head && image && head!=image && domain->owns(*head) && domain->owns(*image),
      "Read-only volume requires its original owned backing");
  domain->verify_resident();
  auto checked=NativeVolumeHeadView::open(head,volume_identity,[&](const StoreIdentity& selected) {
    require(selected==image_identity,"Unknown selected memory image");return image;
  });
  auto projection=ReadOnlyProjection::from_owned_volume(std::move(checked),image->extent());
  domain->seal_read_only();
  return projection;
}
}
