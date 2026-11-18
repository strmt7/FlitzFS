#include "volume_model.hpp"
#include "memory_volume_factory.hpp"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace flitzfs {
namespace {
constexpr auto area_capacity=MemoryVolumeLayout::area_capacity;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
}
VolumeModel::~VolumeModel()=default;
void VolumeModel::create() {
  require(!domain_ && !session_,"Volume already owned; discard explicitly before creating another");
  // Fresh local owners provide exception-safe retirement before publication.
  auto backing=MemoryVolumeBacking::create();
  auto session=std::make_unique<NativeFileSession>(NativeFileSession::create_paged_volume(
      backing.head,backing.volume_identity,backing.image,backing.image_identity,
      {backing.layout.image_capacity},{StoreCompressionMode::automatic,3,false}));
  const auto workload=session->enable_workload_accounting();
  require(workload.enabled&&!workload.committed_input_bytes,"New workload accounting failed");
  const auto advisory=session->create_budgeted_optimization_area(backing.advice,1);
  require(advisory.phase==NativeOptimizationPhase::ready,"Optimization area creation failed");
  auto maintenance=std::make_unique<NativeGroupMaintenance>(*session);
  backing.domain->verify_resident();
  domain_=std::move(backing.domain);head_=std::move(backing.head);image_=std::move(backing.image);
  advice_[0]=std::move(backing.advice);session_=std::move(session);
  maintenance_=std::move(maintenance);maintenance_pending_=true;image_capacity_=backing.layout.image_capacity;last_optimized_sequence_=0;
}
void VolumeModel::create_text(std::string_view name,std::string_view text) {
  require(ready(),"No volume");
  require(!name.empty() && name.size()<=127 && name.find('/')==std::string_view::npos &&
      name.find('\\')==std::string_view::npos && text.size()<=text_limit,"Text input limit");
  // Engine creation rejects existing names. It never overwrites another file.
  // Creation and data write are separate durable transactions; a failed write
  // can leave the new empty name. Report failure and retain the volume.
  session_->create_entry(name);
  maintenance_pending_=true;
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
NativeOptimizationInfo VolumeModel::optimization_info() const noexcept {
  return session_?session_->optimization_info():NativeOptimizationInfo{};
}
void VolumeModel::adopt_attempt(unsigned slot,std::shared_ptr<StoreByteIo>& next) noexcept {
  static_assert(std::is_nothrow_move_assignable_v<std::shared_ptr<StoreByteIo>>);
  // slot was admitted before allocation. Core failure may still own a partial
  // candidate; record that mapping even when its creation threw.
  if(session_->optimization_info().binding_attempts==slot+1)advice_[slot]=std::move(next);
}
VolumeModel::AdviceMaintenance VolumeModel::maintain_optimization_state() {
  require(ready()&&!session_->poisoned(),"Volume unavailable");
  domain_->verify_resident();
  auto info=session_->optimization_info();
  require(info.binding_attempts&&info.binding_attempts<=advice_.size()&&
      advice_[info.binding_attempts-1],"Optimization mapping unavailable");
  if(info.phase==NativeOptimizationPhase::ready) {
    try {
      // Identical publication verifies both actual records without writing,
      // advancing sequence or replenishing/consuming an allowance.
      const auto current=session_->optimization_snapshot();
      session_->publish_optimization_state(current.state);
      return AdviceMaintenance::ready;
    }catch(...) {
      info=session_->optimization_info();
      // Allocation/preflight refusal that left advice healthy is not damage.
      if(info.phase!=NativeOptimizationPhase::terminal&&
          info.phase!=NativeOptimizationPhase::unavailable)throw;
    }
  }
  require(info.session_ready,"Volume unavailable after advisory audit");
  if(info.phase!=NativeOptimizationPhase::terminal&&
      info.phase!=NativeOptimizationPhase::unavailable)return AdviceMaintenance::unavailable;
  const auto slot=info.binding_attempts;
  if(slot>=advice_.size())return AdviceMaintenance::unavailable;
  // Establish the checked loss baseline before allocating a candidate. An
  // unchanged opportunity consumes no new provider slot or source snapshot.
  if(!session_->prepare_budgeted_optimization_rebuild())return AdviceMaintenance::unavailable;
  auto next=domain_->create_store(area_capacity);
  NativeOptimizationInfo created;
  try{created=session_->rebuild_budgeted_optimization_area(next,1);}
  catch(...){adopt_attempt(slot,next);throw;}
  adopt_attempt(slot,next);
  return created.phase==NativeOptimizationPhase::ready?
      AdviceMaintenance::rebuilt:AdviceMaintenance::unavailable;
}
bool VolumeModel::optimize() {
  require(ready() && !session_->poisoned(),"Volume unavailable");
  if(maintain_optimization_state()==AdviceMaintenance::unavailable)return false;
  const auto sequence=session_->snapshot().sequence();
  if(sequence==last_optimized_sequence_)return false;
  auto target=domain_->create_store(image_capacity_);
  // Explicit user maintenance only. Managed paged handoff independently checks
  // both copies and preserves contexts/advice; autonomous staging is separate.
  session_->recompress_volume(target,fresh_volume_identity(),{StoreCompressionMode::automatic,9,false});
  image_=std::move(target);last_optimized_sequence_=sequence;maintenance_pending_=true;return true;
}
bool VolumeModel::maintenance_pending() const noexcept {
  return maintenance_pending_ && maintenance_ && session_ && !session_->poisoned();
}
NativeMaintenanceProgress VolumeModel::maintenance_progress() const noexcept {
  return maintenance_?maintenance_->progress():NativeMaintenanceProgress{NativeMaintenancePhase::failed};
}
void VolumeModel::stop_maintenance() noexcept {maintenance_.reset();maintenance_pending_=false;}
NativeMaintenanceProgress VolumeModel::maintain_groups(const NativeMaintenanceObservation& observation,std::uint64_t now) {
  require(ready(),"No volume");
  if(!maintenance_pending())return maintenance_progress();
  if(observation.valid && !observation.foreground_pending)domain_->verify_working_set();
  const auto result=maintenance_->advance(observation,now);
  if(result.phase==NativeMaintenancePhase::idle || result.phase==NativeMaintenancePhase::failed)maintenance_pending_=false;
  return result;
}
VolumeView VolumeModel::view() const {
  VolumeView out;
  if(!session_)return out;
  domain_->verify_working_set();
  out.ready=true;out.fenced=session_->poisoned();out.optimization=optimization_info();
  out.maintenance=maintenance_progress();out.maintenance_pending=maintenance_pending();
  const auto use=domain_->usage();out.reserved=use.reserved;out.resident=use.resident;out.capacity=use.capacity;
  if(out.fenced)return out; // Preserve ownership without trying a poisoned reader.
  auto snapshot=session_->snapshot();const auto info=snapshot.storage_info();
  out.payload_bytes=info.stored_payload_bytes;
  out.physical_bytes=head_->extent()+image_->extent();
  for(const auto& area:advice_)if(area)out.physical_bytes+=area->extent();
  out.epoch=session_->volume_info().selection.epoch;
  out.files=snapshot.list();
  for(const auto& file:out.files)out.logical_bytes+=file.logical_bytes;
  return out;
}
void VolumeModel::discard() {
  if(domain_)domain_->verify_resident();
  maintenance_.reset();maintenance_pending_=false;session_.reset();for(auto& area:advice_)area.reset();
  image_.reset();head_.reset();domain_.reset();image_capacity_=last_optimized_sequence_=0;
  require(MemoryVolumeDomain::cleanup_healthy(),"Memory cleanup failed; restart the application");
}
} // namespace flitzfs
