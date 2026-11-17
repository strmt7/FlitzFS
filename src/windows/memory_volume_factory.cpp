#include "memory_volume_factory.hpp"
#include "flitzfs/native_session.hpp"
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
  require(m.physical_total && m.physical_available<=m.physical_total && m.commit_total &&
      m.commit_available<=m.commit_total,"Inconsistent memory headroom");
  const auto reserve=(std::max)(768*mib,m.physical_total/8);
  require(m.physical_available>reserve && m.commit_available>reserve,"Insufficient volume headroom");
  const auto capacity=(std::min)(MemoryVolumeDomain::maximum_capacity,
      (std::min)(m.physical_available-reserve,m.commit_available-reserve)/256)/unit*unit;
  require(capacity>=9*mib,"Insufficient admitted paged-volume capacity");
  const auto image=(capacity-MemoryVolumeLayout::area_capacity*(NativeFileSession::max_optimization_bindings+1))/2/unit*unit;
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
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  require(GlobalMemoryStatusEx(&m)!=FALSE,"Memory headroom unavailable");
  MemoryVolumeBacking out;
  out.layout=plan_memory_volume({m.ullTotalPhys,m.ullAvailPhys,m.ullTotalPageFile,m.ullAvailPageFile});
  out.volume_identity=fresh_volume_identity();out.image_identity=fresh_volume_identity();
  require(out.volume_identity!=out.image_identity,"Volume identity collision");
  out.domain=std::make_unique<MemoryVolumeDomain>(MemoryVolumeDomain::create(out.layout.domain_capacity));
  out.head=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.image=out.domain->create_store(out.layout.image_capacity);
  out.advice=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.domain->verify_resident();return out;
}
}
