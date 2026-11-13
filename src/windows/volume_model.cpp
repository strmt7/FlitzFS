#include "volume_model.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace flitzfs {
namespace {
constexpr std::uint64_t mib=1024ULL*1024;
constexpr std::uint64_t image_capacity=4*mib,area_capacity=131072;
StoreIdentity fresh_identity() {
  StoreIdentity id{};
  if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(id.data()),
      static_cast<ULONG>(id.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 ||
      std::all_of(id.begin(),id.end(),[](std::byte b){return b==std::byte{};}))
    throw std::runtime_error("Volume identity generation failed");
  return id;
}
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
VolumeModel::~VolumeModel()=default;
void VolumeModel::create() {
  require(!domain_ && !session_,"Volume already owned; discard explicitly before creating another");
  // Fresh local owners provide exception-safe retirement before publication.
  auto domain=std::make_unique<MemoryVolumeDomain>(MemoryVolumeDomain::create(12*mib));
  auto head=domain->create_store(area_capacity),image=domain->create_store(image_capacity);
  auto advice=domain->create_store(area_capacity);
  auto session=std::make_unique<NativeFileSession>(NativeFileSession::create_volume(
      head,fresh_identity(),image,fresh_identity(),{StoreCompressionMode::automatic,3,false}));
  const auto advisory=session->create_optimization_area(advice,1);
  require(advisory.phase==NativeOptimizationPhase::ready,"Optimization area creation failed");
  domain->verify_resident();
  domain_=std::move(domain);head_=std::move(head);image_=std::move(image);
  advice_=std::move(advice);session_=std::move(session);
}
void VolumeModel::create_text(std::string_view name,std::string_view text) {
  require(ready(),"No volume");
  require(!name.empty() && name.size()<=127 && name.find('/')==std::string_view::npos &&
      name.find('\\')==std::string_view::npos && text.size()<=text_limit,"Text input limit");
  // Engine creation rejects existing names. It never overwrites another file.
  // Creation and data write are separate durable transactions; a failed write
  // can leave the new empty name. Report failure and retain the volume.
  session_->create_entry(name);
  auto file=session_->open(name,{true,true,false},NativeOpenKind::file);
  try {
    session_->write(file,0,std::as_bytes(std::span(text.data(),text.size())));
    session_->flush(file);
    const auto actual=session_->read(file,0,static_cast<std::uint32_t>(text_limit+1));
    require(actual.size()==text.size() &&
        (actual.empty() || std::memcmp(actual.data(),text.data(),text.size())==0),"File readback mismatch");
    session_->cleanup(file,false);session_->close(file);
  } catch(...) {
    // Do not invent successful Cleanup/Close after an ambiguous write.
    if(!session_->poisoned()) {
      try{session_->cleanup(file,false);session_->close(file);}catch(...){session_->fence();}
    }
    throw;
  }
}
std::string VolumeModel::read_text(std::string_view name) const {
  require(ready(),"No volume");
  auto snapshot=session_->snapshot();
  const auto info=snapshot.stat(name);
  require(!info.directory && info.logical_bytes<=text_limit,"File exceeds editor limit");
  const auto bytes=snapshot.read(name,0,static_cast<std::uint32_t>(text_limit));
  if(bytes.empty())return {};
  return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
bool VolumeModel::optimize() {
  require(ready() && !session_->poisoned(),"Volume unavailable");
  auto target=domain_->create_store(image_capacity);
  if(!session_->begin_idle_recompression(target,fresh_identity(),
      {StoreCompressionMode::automatic,9,false}))return false;
  try {
    // Each engine advance is separately bounded. The application controller
    // serializes this explicit small-volume operation; no autonomous idle claim.
    for(std::size_t i=0;i<=kStoreMaxObjects;++i) {
      const auto progress=session_->advance_recompression();
      if(progress==NativeFileSession::RecompressionProgress::stale) {
        session_->cancel_recompression();return false;
      }
      if(progress==NativeFileSession::RecompressionProgress::ready) {
        const auto published=session_->publish_idle_recompression();
        if(published)image_=std::move(target);
        return published.has_value();
      }
    }
    throw std::runtime_error("Recompression step bound exceeded");
  } catch(...) {session_->cancel_recompression();throw;}
}
VolumeView VolumeModel::view() const {
  VolumeView out;
  if(!session_)return out;
  domain_->verify_resident();
  out.ready=true;out.fenced=session_->poisoned();
  const auto use=domain_->usage();out.allocated=use.allocated;out.capacity=use.capacity;
  if(out.fenced)return out; // Preserve ownership without trying a poisoned reader.
  auto snapshot=session_->snapshot();const auto info=snapshot.storage_info();
  out.payload_bytes=info.stored_payload_bytes;
  out.physical_bytes=head_->extent()+image_->extent()+advice_->extent();
  out.epoch=session_->volume_info().selection.epoch;
  out.files=snapshot.list();
  for(const auto& file:out.files)out.logical_bytes+=file.logical_bytes;
  return out;
}
void VolumeModel::discard() {
  if(domain_)domain_->verify_resident();
  session_.reset();advice_.reset();image_.reset();head_.reset();domain_.reset();
  require(MemoryVolumeDomain::cleanup_healthy(),"Memory cleanup failed; restart the application");
}
} // namespace flitzfs
