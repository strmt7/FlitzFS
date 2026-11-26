#include "memory_volume_factory.hpp"
#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/volume_head.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace flitzfs {
namespace {
constexpr auto unit=kStoreCommitUnit;
constexpr auto minimum_image=3*unit; // Two root slots and the aligned first object pages.
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool unselected(const std::shared_ptr<StoreByteIo>& backing) noexcept {
  return backing && backing.use_count()==1;
}
}
MemoryVolumeLayout plan_memory_volume(const MemoryVolumeObservation& m,bool indexed_coherence) {
  // This is a virtual reservation budget. Every committed page separately
  // consumes measured physical and commit headroom at the point of use.
  constexpr auto maximum_addressable=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  const auto capacity=(std::min)(usable_memory_volume_headroom(m),maximum_addressable)/unit*unit;
  constexpr auto areas=MemoryVolumeLayout::area_capacity*(NativeFileSession::max_optimization_bindings+1);
  require(capacity>=areas+2*minimum_image,"Insufficient headroom for image roots and advisory space");
  const auto space=capacity-areas;
  // Preserve the same total advisory share for the selected catalog and its
  // replacement. Each store reserves and locks memory only when needed.
  const auto advisory=space/(2*kOptimizationGroupScanInputPerWriteByte)/unit*unit;
  const auto coherence=indexed_coherence && advisory>=minimum_image && 2*advisory<=space-2*minimum_image?advisory:0;
  return {capacity,(space-2*coherence)/2/unit*unit,coherence};
}
StoreIdentity fresh_volume_identity() {
  StoreIdentity id{};
  require(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(id.data()),static_cast<ULONG>(id.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0 &&
      std::any_of(id.begin(),id.end(),[](auto b){return b!=std::byte{};}),"Volume identity generation failed");
  return id;
}
MemoryVolumeBacking MemoryVolumeBacking::create(bool indexed_coherence) {
  MemoryVolumeBacking out;
  out.layout=plan_memory_volume(observe_memory_volume_headroom(),indexed_coherence);
  out.volume_identity=fresh_volume_identity();out.image_identity=fresh_volume_identity();
  require(out.volume_identity!=out.image_identity,"Volume identity collision");
  out.domain=std::make_unique<MemoryVolumeDomain>(MemoryVolumeDomain::create(out.layout.domain_capacity));
  out.head=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.image=out.domain->create_store(out.layout.image_capacity);
  out.advice=out.domain->create_store(MemoryVolumeLayout::area_capacity);
  out.domain->verify_resident();return out;
}
void MemoryVolumeBacking::configure_coherence(NativeFileSession& session) noexcept {
  if(!layout.coherence_capacity)return;
  try {
    if(!coherence_storage_) {
      coherence_storage_=std::make_shared<IndexedCoherenceStorage>(PagedStoreLimits{layout.coherence_capacity},[this] {
        const auto free=std::ranges::find_if(coherence_,[](const auto& slot){return !slot;});
        if(free==coherence_.end())return std::shared_ptr<StoreByteIo>{};
        *free=domain->create_store(layout.coherence_capacity);
        return *free;
      });
    }
    session.configure_indexed_coherence(coherence_storage_);
  }catch(const std::exception&) {
    // Optional learning cannot stop an otherwise valid mount or remount.
  }
}
bool MemoryVolumeBacking::coherence_retirement_pending() const noexcept {
  return MemoryVolumeDomain::cleanup_healthy() && std::ranges::any_of(coherence_,unselected);
}
void MemoryVolumeBacking::advance_coherence_retirement() {
  if(!MemoryVolumeDomain::cleanup_healthy())return;
  const auto old=std::ranges::find_if(coherence_,unselected);
  if(old!=coherence_.end())(void)domain->retire_store(*old);
}
std::shared_ptr<StoreByteIo> MemoryVolumeBacking::resolve_image(const StoreIdentity& selected) const {
  if(selected==image_identity)return image;
  require(phase_==CompactionPhase::uncertain && selected==alternate_identity_ &&
      alternate_ && domain->owns_image(*alternate_),"Unknown selected memory image");
  return alternate_;
}
std::uint64_t MemoryVolumeBacking::staging_capacity() const noexcept {
  const auto usage=domain->usage();
  return (std::min)(layout.image_capacity,usage.capacity-usage.reserved);
}
std::uint64_t MemoryVolumeBacking::seal_workspace_bytes(const NativeFileSession& session,
    std::uint64_t staging_bytes) const {
  const auto input=session.workload_info();
  if(!input.enabled || input.committed_input_bytes<=attempted_seal_input_ ||
      input.committed_input_bytes-attempted_seal_input_<seal_retry_input_)return 0;
  return NativeHybridBacking::workspace_capacity(volume_identity,image->contract(),
      session.packed_folded_end_bound(image->contract().write_failure_unit_bytes),staging_bytes,0);
}
bool MemoryVolumeBacking::selected_image_sealed() const noexcept {
  return dynamic_cast<const NativeHybridBacking*>(image.get())!=nullptr;
}
bool MemoryVolumeBacking::compaction_preparation_pending(const NativeFileSession& session) const noexcept {
  return hybrid_?hybrid_->source_current(session) && hybrid_->can_continue_preparation():
      session.idle_compaction_preparation_pending();
}
bool MemoryVolumeBacking::compaction_pending(const NativeFileSession& session,bool allow_cold) const {
  if(phase_==CompactionPhase::retiring)return true;
  if(phase_==CompactionPhase::uncertain || session.poisoned())return false;
  if(hybrid_)return true;
  if(!session.idle_compaction_context_ready())return false;
  if(phase_!=CompactionPhase::idle)return true;
  if(const auto repair=session.indexed_capacity_repair_generation())return repair!=attempted_repair_;
  const auto space=session.indexed_volume_space();
  const auto obsolete=space.obsolete_bytes;
  return (obsolete>attempted_obsolete_ && obsolete-attempted_obsolete_>=abandoned_bytes_ &&
      space.reclamation_worthwhile()) || (allow_cold &&
      (session.idle_cold_compaction_pending() || seal_workspace_bytes(session,staging_capacity())));
}
void MemoryVolumeBacking::retire_alternate(bool charge_attempt) {
  if(charge_attempt && alternate_)abandoned_bytes_=alternate_->extent();
  hybrid_.reset();
  phase_=alternate_ || workspace_ || private_parts_[0] || private_parts_[1]?
      CompactionPhase::retiring:CompactionPhase::idle;
}
void MemoryVolumeBacking::adopt_alternate() noexcept {
  image.swap(alternate_);
  std::swap(image_identity,alternate_identity_);
  attempted_obsolete_=0;
  abandoned_bytes_=0;
  attempted_repair_=0;
  phase_=CompactionPhase::retiring;
}
MemoryCompactionStep MemoryVolumeBacking::advance_retirement() {
  if(const auto* old=dynamic_cast<const NativeHybridBacking*>(alternate_.get())) {
    if(!unselected(alternate_))return MemoryCompactionStep::pinned;
    private_parts_=old->components();
    alternate_.reset();
  }
  for(auto* store:{&workspace_,&alternate_,&private_parts_[0],&private_parts_[1]})if(*store) {
    const auto step=domain->retire_store(*store);
    if(step==MemoryStoreRetirement::pinned)return MemoryCompactionStep::pinned;
    break;
  }
  if(!workspace_ && !alternate_ && !private_parts_[0] && !private_parts_[1]) {
    alternate_identity_={};
    phase_=CompactionPhase::idle;
  }
  return MemoryCompactionStep::retiring;
}
MemoryCompactionStep MemoryVolumeBacking::advance_sealing(NativeFileSession& session) {
  using Phase=NativeHybridBacking::BuildPhase;
  if(!hybrid_->source_current(session) || session.indexed_capacity_repair_generation()) {
    retire_alternate(false);
    return MemoryCompactionStep::retiring;
  }
  const auto phase=hybrid_->phase();
  if(phase==Phase::awaiting_tail && workspace_) {
    const auto step=domain->retire_store(workspace_);
    return step==MemoryStoreRetirement::pinned?MemoryCompactionStep::pinned:MemoryCompactionStep::copying;
  }
  if(phase==Phase::awaiting_base || phase==Phase::awaiting_tail) {
    auto& storage=private_parts_[phase==Phase::awaiting_base?0:1];
    storage=domain->create_store(hybrid_->required_store_bytes());
    hybrid_->provide_store(session,storage);
    return MemoryCompactionStep::copying;
  }
  if(phase!=Phase::ready) {
    return hybrid_->advance(session)?MemoryCompactionStep::ready:MemoryCompactionStep::copying;
  }
  phase_=CompactionPhase::uncertain;
  std::shared_ptr<NativeHybridBacking> candidate;
  bool published=false;
  try {published=hybrid_->publish(session,candidate);}
  catch(...) {
    alternate_=std::move(candidate);
    if(alternate_)private_parts_.fill({});
    else phase_=CompactionPhase::copying;
    throw;
  }
  hybrid_.reset();
  if(!published) {
    retire_alternate(false);
    return MemoryCompactionStep::retiring;
  }
  alternate_=std::move(candidate);
  private_parts_.fill({});
  adopt_alternate();
  return MemoryCompactionStep::published;
}
MemoryCompactionStep MemoryVolumeBacking::advance_compaction(NativeFileSession& session,bool allow_cold) {
  if(!compaction_pending(session,allow_cold))return MemoryCompactionStep::idle;
  if(phase_==CompactionPhase::retiring)return advance_retirement();
  try {
    if(hybrid_)return advance_sealing(session);
    if(phase_==CompactionPhase::idle) {
      const bool reclaim=compaction_pending(session,false);
      if(!reclaim && allow_cold && !session.idle_cold_compaction_pending()) {
        const auto staging=(std::min)(staging_capacity(),
            usable_memory_volume_headroom(observe_memory_volume_headroom()));
        const auto reserve=seal_workspace_bytes(session,staging);
        if(!reserve)return MemoryCompactionStep::pinned;
        attempted_seal_input_=session.workload_info().committed_input_bytes;
        seal_retry_input_=reserve;
        alternate_identity_=fresh_volume_identity();
        workspace_=domain->create_store(reserve);
        hybrid_.emplace(NativeHybridBacking::prepare(session,workspace_,image->contract(),alternate_identity_,
            layout.image_capacity,staging));
        phase_=CompactionPhase::copying;
        return MemoryCompactionStep::copying;
      }
      if(reclaim) {
        attempted_repair_=session.indexed_capacity_repair_generation();
        attempted_obsolete_=session.indexed_volume_space().obsolete_bytes;
        abandoned_bytes_=0;
      }
      alternate_identity_=fresh_volume_identity();
      alternate_=domain->create_store(layout.image_capacity);
      if(!session.begin_idle_compaction(alternate_,alternate_identity_,
          reclaim?PagedRewriteMode::reconcile_foreground:PagedRewriteMode::cold_compression)) {
        retire_alternate(false);
        return MemoryCompactionStep::retiring;
      }
      phase_=CompactionPhase::copying;
      return MemoryCompactionStep::copying;
    }
    if(phase_==CompactionPhase::copying) {
      switch(session.advance_recompression()) {
        case NativeFileSession::RecompressionProgress::copying:return MemoryCompactionStep::copying;
        case NativeFileSession::RecompressionProgress::ready:
          phase_=CompactionPhase::ready;return MemoryCompactionStep::ready;
        case NativeFileSession::RecompressionProgress::declined:
          retire_alternate(false);return MemoryCompactionStep::retiring;
        case NativeFileSession::RecompressionProgress::stale:
          retire_alternate(true);return MemoryCompactionStep::retiring;
      }
    }
    if(session.idle_compaction_preparation_pending()) {
      phase_=CompactionPhase::copying;
      return MemoryCompactionStep::copying;
    }
    phase_=CompactionPhase::uncertain;
    if(!session.publish_idle_recompression()) {
      retire_alternate(true);
      return MemoryCompactionStep::retiring;
    }
    adopt_alternate();
    return MemoryCompactionStep::published;
  }catch(const std::exception&) {
    if(phase_==CompactionPhase::uncertain && session.poisoned())throw;
    session.cancel_recompression();
    retire_alternate(true);
    if(session.poisoned())throw;
    return MemoryCompactionStep::failed;
  }
}
void MemoryVolumeBacking::cancel_compaction(NativeFileSession& session) noexcept {
  session.cancel_recompression();
  hybrid_.reset();
  if(phase_==CompactionPhase::copying || phase_==CompactionPhase::ready)
    phase_=CompactionPhase::retiring;
}
void MemoryVolumeBacking::release_unselected_after_join() noexcept {
  for(auto& backing:coherence_)if(unselected(backing))backing.reset();
  hybrid_.reset();
  if(phase_==CompactionPhase::uncertain)return;
  alternate_.reset();
  workspace_.reset();
  private_parts_.fill({});
  alternate_identity_={};
  phase_=CompactionPhase::idle;
}
NativeStartupRepairImage MemoryVolumeBacking::startup_repair_image(const NativeVolumeHeadInfo& authority) {
  require(domain && image && domain->owns_image(*image) && !authority.degraded &&
      authority.selection.volume_identity==volume_identity &&
      authority.selection.placement==NativeVolumePlacement::external,"Memory startup repair needs confirmed authority");
  // The closed owner independently verified the selected bytes and both head
  // peers. A retained alternate from a failed attempt may now be unselected.
  settle_retained_selection(authority.selection.image_identity);
  require(authority.selection.image_identity==image_identity &&
      (!alternate_ || (alternate_.use_count()==1 && domain->owns_image(*alternate_))),"Memory startup repair image is pinned");
  alternate_.reset();
  workspace_.reset();
  private_parts_.fill({});
  alternate_identity_={};
  phase_=CompactionPhase::idle;
  const auto next=fresh_volume_identity();
  alternate_=domain->create_store(layout.image_capacity);
  alternate_identity_=next;
  phase_=CompactionPhase::uncertain;
  return {alternate_,next};
}
void MemoryVolumeBacking::settle_retained_selection(const StoreIdentity& selected) noexcept {
  if(phase_!=CompactionPhase::uncertain)return;
  if(selected==alternate_identity_)adopt_alternate();
  phase_=CompactionPhase::retiring;
}
NativeVolumeHeadView MemoryVolumeBacking::open_owned_head(bool indexed_recovery) const {
  require(domain && head && image && head!=image && domain->owns(*head) && domain->owns_image(*image),
      "Volume inspection requires its original owned backing");
  domain->verify_resident();
  const auto resolve=[&](const StoreIdentity& selected){return resolve_image(selected);};
  return indexed_recovery?NativeVolumeHeadView::open_indexed_recovery(head,volume_identity,resolve):
      NativeVolumeHeadView::open(head,volume_identity,resolve);
}
ReadOnlyProjection MemoryVolumeBacking::seal_checked(NativeVolumeHeadView checked) {
  const auto extent=resolve_image(checked.info().selection.image_identity)->extent();
  auto projection=ReadOnlyProjection::from_owned_volume(std::move(checked),extent);
  coherence_storage_.reset();
  for(auto& backing:coherence_)if(unselected(backing))backing.reset();
  domain->seal_read_only(dynamic_cast<const NativeHybridBacking*>(image.get()),
      dynamic_cast<const NativeHybridBacking*>(alternate_.get()));
  return projection;
}
ReadOnlyProjection MemoryVolumeBacking::open_read_only() {
  return seal_checked(open_owned_head());
}
}
