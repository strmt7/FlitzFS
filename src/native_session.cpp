#include "flitzfs/native_session.hpp"
#include "indexed_names.hpp"
#include "native_maintenance_source.hpp"
#include "native_volume_file_reclaimer.hpp"
#include "flitzfs/catalog.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <optional>
#include <new>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace flitzfs {
namespace {
using E=NativeSessionErrorCode;
[[noreturn]] void fail(E code,const char* message){throw NativeSessionError(code,message);}
void admit_policy(NativeDispositionPolicy policy) {
  if(policy!=NativeDispositionPolicy::aggregate && policy!=NativeDispositionPolicy::windows_global)
    fail(E::invalid_options,"unknown native disposition policy");
}
void admit_empty(const std::shared_ptr<StoreByteIo>& io) {
  if(!io || io->extent()!=0 || !valid_store_io_contract(io->contract()))
    fail(E::invalid_options,"new qualified empty volume backend required");
  io->admit_write(0); // Read-only headroom admission before retiring an active writer.
}
void retire(NativeFilesystemWriter& writer) noexcept {
  auto retired=std::move(writer); // Its destructor really releases the writer lease.
}
std::uint64_t cold_compaction_input_cost(std::uint64_t candidate_writes) noexcept {
  // Cover the selector, advisory pair and private copy. At most two
  // maintenance bytes may be reserved per settled foreground input byte.
  return candidate_writes/2+candidate_writes%2+(kVolumeHeadBytes+kOptimizationAreaBytes)/2;
}
NativeFilesystemWriter resume_image(const std::shared_ptr<StoreByteIo>& image,
    const NativeFilesystemSnapshot& view,StoreCompressionOptions compression) {
  const auto storage=view.image_storage();
  if(storage.kind==NativeImageKind::paged || storage.kind==NativeImageKind::indexed_paged) {
    auto objects=PagedObjectStoreWriter::resume(image,view.identity(),view.storage_info().generation,
        {storage.maximum_physical_bytes},compression);
    return NativeFilesystemWriter::resume(std::move(objects));
  }
  if(storage.kind!=NativeImageKind::flat)fail(E::invalid_options,"unsupported writable image capability");
  return NativeFilesystemWriter::resume(ObjectStoreWriter::resume(image,view.identity(),
      view.storage_info().generation,compression));
}
void admit_path(const NativeFilesystemWriter& writer,std::string_view path,bool root=false) {
  // Reuse the pure validator, which checks the byte cap before allocating.
  // Never copy a caller's arbitrary-length view into an owned mutation first.
  try {
    if(writer.image_kind()==NativeImageKind::indexed_paged)(void)detail::indexed_path_name(path,root);
    else (void)canonical_catalog_path(path,root);
  }
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){throw NativeFsError(NativeFsErrorCode::invalid_path,"invalid native session path");}
}
}
struct NativeFileSession::Impl {
  enum class Phase:std::uint8_t {active,prepared,overwritten,opened};
  struct Slot {
    std::uint64_t token=0,inode=0;
    NativeFileAccess access;
    bool cleaned=false,cleanup_delete=false,requested=false,basic_write=false;
    NativeCleanupMetadata cleanup_metadata;
    bool security_read=false,security_replace=false;
    bool overwrite=false,supersede=false;
    Phase phase=Phase::active;
    bool initial_delete=false,cleanup_trim=false;
  };
  static_assert(std::is_nothrow_copy_assignable_v<Slot>);
  struct VolumeOwner {
    std::shared_ptr<StoreByteIo> head_io,active_io,candidate_io;
    std::optional<NativeVolumeHeadWriter> head;
    std::optional<NativeFilesystemSnapshot> baseline;
    StoreIdentity volume_id{},active_id{},candidate_id{};
    StoreCompressionOptions foreground;
    std::uint64_t epoch=0;
    bool pending=false,recovery_attempted=false,budget_new=false,budget_bootstrap_claimed=false;
  };
  static_assert(sizeof(VolumeOwner)<=256);
  struct OptimizationOwner {
    std::shared_ptr<StoreByteIo> backing;
    std::optional<OptimizationStateWriter> writer;
    NativeOptimizationPhase phase=NativeOptimizationPhase::unavailable;
    OptimizationReadCode read_code=OptimizationReadCode::io_failure;
    bool budgeted=false;
  };
  static_assert(sizeof(OptimizationOwner)<=64);
  struct ScrubWork {
    StoreIdentity image{};
    std::uint64_t started=0,verified_record_bytes=0;
    bool restore_attempted=false;
    std::optional<OptimizationPayloadScrub> checkpoint;
  } scrub;
  struct StagedRecompression {
    std::shared_ptr<StoreByteIo> target;
    StoreIdentity id;
    std::variant<NativeFilesystemWriter::Recompression,PagedObjectStoreWriter::Rewrite> work;
    bool idle;

    bool matches(const NativeFilesystemWriter& writer) const noexcept {
      if(const auto* paged=std::get_if<PagedObjectStoreWriter::Rewrite>(&work))
        return writer.matches_paged_recompression(*paged);
      return writer.matches_recompression(std::get<NativeFilesystemWriter::Recompression>(work));
    }
  };
  std::unique_ptr<StagedRecompression> recompression;
  StoreIdentity evaluated_image{};
  std::uint64_t evaluated_generation=0;
  bool evaluated_capacity_repair=false;
  std::optional<OptimizationColdCoverage> cold_coverage;
  std::uint64_t cold_screen_input_floor=0;
  // Destruction order retires the image writer BEFORE releasing the head owner.
  std::unique_ptr<VolumeOwner> volume;
  // Retire data/advisory writers before the head owner.
  std::unique_ptr<OptimizationOwner> optimization;
  std::array<std::unique_ptr<OptimizationOwner>,max_optimization_bindings-1> prior_optimization;
  unsigned optimization_bindings=0;
  bool budget_mode=false;
  std::shared_ptr<detail::NativeMaintenanceSource> maintenance;
  std::optional<std::uint64_t> budget_loss_input;
  std::optional<OptimizationColdCoverage> current_cold_coverage() const noexcept {
    if(!cold_coverage || !volume || !volume->head || volume->pending ||
        volume->foreground.mode==StoreCompressionMode::raw)return {};
    const auto& saved=*cold_coverage;
    const auto selected=volume->head->info().selection;
    if(selected.storage.kind!=NativeImageKind::indexed_paged || saved.image!=selected.image_identity ||
        saved.selector_epoch!=selected.epoch || saved.minimum_checkpoint!=selected.minimum_checkpoint ||
        saved.policy_revision!=detail::kIndexedColdEncodingRevision || saved.zstd_version!=store_zstd_version() ||
        saved.foreground_level!=static_cast<std::uint32_t>(volume->foreground.zstd_level))return {};
    return saved;
  }
  // Current and prior areas remain unavailable as image/compaction targets.
  bool owns_optimization_backend(const std::shared_ptr<StoreByteIo>& io) const noexcept {
    if(optimization&&optimization->backing==io)return true;
    for(const auto& prior:prior_optimization)if(prior&&prior->backing==io)return true;
    return false;
  }
  OptimizationStateWriter* scrub_advice() const noexcept {
    if(!volume || volume->pending || recompression || !optimization || !optimization->budgeted ||
        !optimization->writer || optimization->writer->poisoned())return nullptr;
    return &*optimization->writer;
  }
  OptimizationStateWriter* ready_funded_advice() const noexcept {
    if(!volume || volume->pending || !optimization || !optimization->budgeted ||
        optimization->phase!=NativeOptimizationPhase::ready || !optimization->writer ||
        optimization->writer->poisoned() || optimization->writer->budget_repair_pending())return nullptr;
    return &*optimization->writer;
  }
  OptimizationStateWriter* ready_scan_advice() const noexcept {
    return recompression?nullptr:ready_funded_advice();
  }
  bool scan_advice_funded() const noexcept {
    const auto* advice=ready_scan_advice();
    if(!advice)return false;
    const auto settled=advice->budget_settlement();
    const auto input=writer.workload_info();
    return settled && input.enabled && input.committed_input_bytes>=*settled &&
        input.committed_input_bytes-*settled>=kOptimizationGroupScanInputBytes;
  }
  void adopt_coherence(IndexedCoherenceSnapshot catalog,
      std::shared_ptr<IndexedCoherenceWriter> writer_owner={}) {
    if(maintenance->coherence_binding==(std::numeric_limits<std::uint64_t>::max)())
      fail(E::limit,"coherence binding sequence exhausted");
    maintenance->retire_coherence();
    maintenance->coherence.emplace(std::move(catalog));
    maintenance->coherence_writer=std::move(writer_owner);
    ++maintenance->coherence_binding;
  }
  std::shared_ptr<StoreByteIo> reserve_coherence_backing(IndexedCoherenceStorage& storage) {
    auto backing=storage.reserve_();
    if(backing==volume->head_io || backing==volume->active_io ||
        backing==volume->candidate_io || owns_optimization_backend(backing))return {};
    return backing;
  }
  bool has_coherence_retry_evidence() const noexcept {
    if(!maintenance->coherence_retry_input)return true;
    const auto input=writer.workload_info().committed_input_bytes;
    const auto prior=*maintenance->coherence_retry_input;
    return input>=prior && (input-prior)/kOptimizationGroupScanInputPerWriteByte>=
        maintenance->coherence->info().physical_end;
  }
  bool coherence_reserves_credit() const noexcept {
    return maintenance && maintenance->coherence && maintenance->coherence_full &&
        (maintenance->coherence_rewrite || has_coherence_retry_evidence());
  }
  bool coherence_work_pending() const noexcept {
    if(!coherence_reserves_credit())return false;
    const auto* advice=ready_scan_advice();
    if(!advice)return false;
    if(!maintenance->coherence_rewrite)return scan_advice_funded();
    const auto progress=maintenance->coherence_rewrite->progress();
    if(progress.phase!=PagedRewritePhase::awaiting_charge)return true;
    const auto input=writer.workload_info().committed_input_bytes;
    const auto settled=advice->budget_settlement();
    return settled && input>=*settled && input-*settled>=kOptimizationGroupScanInputBytes &&
        (input-*settled-kOptimizationGroupScanInputBytes)/kOptimizationGroupScanInputPerWriteByte>=progress.required_write_bytes;
  }
  void retire_failed_advice_charge(const OptimizationStateWriter& advice,std::uint64_t input) noexcept {
    if(!advice.poisoned())return;
    optimization->phase=NativeOptimizationPhase::terminal;
    optimization->read_code=OptimizationReadCode::io_failure;
    budget_loss_input=input;
  }
  bool reserve_cold_compaction(std::uint64_t bytes) {
    auto* advice=ready_funded_advice();
    if(!advice)return false;
    const auto input=writer.snapshot().workload_info();
    if(!input.enabled)return false;
    try {
      return advice->reserve_cold_compaction(input.committed_input_bytes,
          cold_compaction_input_cost(bytes),current_cold_coverage());
    }catch(...) {
      retire_failed_advice_charge(*advice,input.committed_input_bytes);
      throw;
    }
  }
  bool charge_coherence_copy(std::uint64_t bytes) {
    auto* advice=ready_scan_advice();
    if(!advice)return false;
    const auto actual=writer.snapshot().workload_info();
    const auto state=advice->snapshot().state;
    if(!actual.enabled || !state.group_scan || state.group_scan->image!=volume->active_id)
      throw std::runtime_error("coherence copy has no checked current scan journal");
    try {
      return advice->publish_budgeted(state,actual.committed_input_bytes,current_cold_coverage(),true,bytes)==
          OptimizationStateWriter::BudgetResult::published;
    }catch(...) {
      retire_failed_advice_charge(*advice,actual.committed_input_bytes);
      throw;
    }
  }
  void advance_coherence_work() {
    auto& source=*maintenance;
    auto& catalog=*source.coherence_writer;
    try {
      if(!source.coherence_rewrite) {
        auto output=reserve_coherence_backing(*source.coherence_storage);
        if(!output)throw std::runtime_error("coherence replacement storage unavailable");
        auto pinned=std::make_shared<NativeFilesystemSnapshot>(writer.snapshot());
        source.coherence_rewrite.emplace(catalog.prepare_compaction(std::move(output),
            [pinned=std::move(pinned)](const OptimizationGroupFeature& feature) {
              const auto live=pinned->indexed_extent_token({feature.inode,feature.extent_index});
              return live && live->object==feature.object && live->revision==feature.revision;
            },[this](std::uint64_t bytes){return charge_coherence_copy(bytes);}));
        return;
      }
      if(!catalog.advance_compaction(*source.coherence_rewrite))return;
      auto replacement=std::make_shared<IndexedCoherenceWriter>(catalog.finish_compaction(*source.coherence_rewrite));
      auto snapshot=replacement->snapshot();
      const auto before=source.coherence->info().physical_end;
      const auto remaining=snapshot.info().physical_end;
      source.coherence_rewrite.reset();
      source.coherence.emplace(std::move(snapshot));
      source.coherence_writer=replacement;
      source.coherence_storage->retained_=std::move(replacement);
      source.coherence_full=false;
      source.coherence_retry_input.reset();
      ++source.coherence_compactions;
      if(before>remaining)source.coherence_compacted_bytes_saved+=before-remaining;
    }catch(const std::exception&) {
      // The previously selected catalog never participated in the writes.
      source.coherence_rewrite.reset();
      source.coherence_retry_input=writer.workload_info().committed_input_bytes;
      ++source.coherence_discards;
    }
  }
  bool publish_coherence_scan(OptimizationStateWriter& advice,const OptimizationState& state,
      std::uint64_t input,std::span<const detail::CoherenceScanKeys> keys) {
    if(!writer.maintenance_indexed() || !maintenance || coherence_reserves_credit())return false;
    const auto storage=maintenance->coherence_storage;
    const bool creating=!maintenance->coherence_writer;
    if(creating && !storage)return false;
    const auto settled=*state.settled_input_bytes;
    if(input<settled || input-settled<=kOptimizationGroupScanInputBytes)return false;
    const auto maximum_bytes=(input-settled-kOptimizationGroupScanInputBytes)/
        kOptimizationGroupScanInputPerWriteByte;
    if(creating && maximum_bytes<2*kStoreCommitUnit)return false;
    const auto& features=state.group_scan->representatives;
    auto first=std::ranges::upper_bound(features,maintenance->coherence_offer_after,{},&OptimizationGroupFeature::object);
    if(first==features.end())first=features.begin();
    std::vector<IndexedCoherencePosting> postings;
    postings.reserve(kNativeMaxDirtyExtents);
    auto row=static_cast<std::uint32_t>(first-features.begin());
    auto signature=std::ranges::lower_bound(keys,row,{},&detail::CoherenceScanKeys::representative);
    auto end=first;
    const auto nonzero=[](auto key){return key!=0;};
    for(;end!=features.end();++end,++row) {
      const auto band=detail::coherence_primary_band(end->bands);
      const bool has_keys=signature!=keys.end() && signature->representative==row;
      const auto signature_count=has_keys?std::ranges::count_if(signature->masked,nonzero):0;
      const auto needed=static_cast<std::size_t>(band.has_value())+static_cast<std::size_t>(signature_count);
      if(needed>kNativeMaxDirtyExtents-postings.size())break;
      if(band)postings.push_back({*band,*end});
      if(has_keys) {
        for(std::uint32_t i=0;i<signature->masked.size();++i)
          if(signature->masked[i])postings.push_back({kCoherenceMaskedBand+i,*end,signature->masked[i]});
        ++signature;
      }
    }
    const auto earlier_count=postings.size();
    // Preserve the primary and masked scan coverage. Numeric keys compete for
    // the remaining slots in the same funded batch.
    const auto first_row=static_cast<std::uint32_t>(first-features.begin());
    for(auto extra=std::ranges::lower_bound(keys,first_row,{},&detail::CoherenceScanKeys::representative);
        extra!=signature && postings.size()<kNativeMaxDirtyExtents;++extra) {
      for(std::uint32_t i=0;i<extra->modular.size() && postings.size()<kNativeMaxDirtyExtents;++i) {
        if(extra->modular[i])postings.push_back({kCoherenceModularBand+i,
            features[extra->representative],extra->modular[i]});
      }
    }
    if(postings.empty())return false;
    bool settled_catalog=false;
    const auto reserve=[&](std::uint64_t write_bytes) {
      settled_catalog=advice.publish_budgeted(state,input,current_cold_coverage(),true,write_bytes)==
          OptimizationStateWriter::BudgetResult::published;
      return settled_catalog;
    };
    try {
      auto backing=creating?reserve_coherence_backing(*storage):nullptr;
      if(creating && !backing)return false;
      const auto publish=[&](std::span<const IndexedCoherencePosting> batch) {
        if(creating) {
          auto catalog=std::make_shared<IndexedCoherenceWriter>(IndexedCoherenceWriter::create_reserved(
              backing,volume->volume_id,volume->active_id,storage->limits_,batch,maximum_bytes,reserve));
          auto snapshot=catalog->snapshot();
          adopt_coherence(std::move(snapshot),catalog);
          storage->retained_=std::move(catalog);
        }else {
          auto& catalog=*maintenance->coherence_writer;
          const auto result=catalog.publish_reserved(batch,maximum_bytes,reserve);
          if(result.write_bytes_upper_bound)maintenance->coherence.emplace(catalog.snapshot());
        }
      };
      try{publish(postings);}
      catch(const PagedStoreCapacityError&) {
        // Only a clean pre-publication refusal can retry with the earlier
        // key families. No charge, partial catalog or poisoned owner is reused.
        if(!earlier_count || earlier_count==postings.size() || settled_catalog || advice.poisoned() ||
            (creating?backing->extent()!=0:maintenance->coherence_writer->poisoned()))throw;
        publish(std::span(postings).first(earlier_count));
      }
      maintenance->coherence_offer_after=(end-1)->object;
    }catch(const PagedStoreCapacityError& error) {
      if(advice.poisoned())throw;
      if(maintenance->coherence_writer && maintenance->coherence_writer->poisoned())maintenance->retire_coherence();
      else if(!creating && storage && error.physical_space_exhausted())maintenance->coherence_full=true;
    }catch(const std::exception&) {
      if(advice.poisoned())throw;
      maintenance->retire_coherence();
    }
    // A charged catalog attempt is settled even if its own publication failed.
    return settled_catalog;
  }
  bool publish_scan_advice(OptimizationGroupScan scan,std::span<const detail::CoherenceScanKeys> keys) {
    if(!scan_advice_funded())return false;
    auto& advice=*ready_scan_advice();
    const auto actual=writer.snapshot().workload_info();
    if(!actual.enabled)return false;
    auto state=advice.snapshot().state;
    state.group_scan=std::move(scan);
    try {
      return publish_coherence_scan(advice,state,actual.committed_input_bytes,keys) ||
          (!coherence_reserves_credit() &&
           advice.publish_budgeted(state,actual.committed_input_bytes,current_cold_coverage(),true)==
               OptimizationStateWriter::BudgetResult::published);
    }catch(...) {
      retire_failed_advice_charge(advice,actual.committed_input_bytes);
      throw;
    }
  }
  NativeFilesystemWriter writer;
  std::shared_ptr<detail::NativeVolumeFileReclaimer> persistent_reclaimer;
  bool volume_file_bound=false;
  std::shared_ptr<const void> owner=std::make_shared<const int>(0);
  std::array<Slot,max_contexts> slots{};
  std::array<std::uint64_t,kNativeMaxNodes> latched{};
  // Nonempty initial-delete directories retain contents on disk. Their
  // kernel-like pending state/authority is runtime-only until final Cleanup.
  static constexpr std::uint8_t kExplicit=1,kInitial=2,kVolatile=4;
  std::array<std::uint8_t,kNativeMaxNodes> latch_flags{};
  std::uint64_t next_token=1;
  bool terminal=false;
  NativeDispositionPolicy disposition_policy=NativeDispositionPolicy::aggregate;
  explicit Impl(NativeFilesystemWriter source,NativeDispositionPolicy policy):writer(std::move(source)),disposition_policy(policy) {
    if(writer.poisoned())fail(E::terminal,"cannot own a terminal native writer");
    auto snapshot=writer.snapshot();
    if(snapshot.needs_session_recovery())fail(E::delete_pending,"old file contexts need independent closed-session recovery");
  }
  bool volatile_initial(std::uint64_t inode) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode && (latch_flags[i]&kVolatile))return true;
    return false;
  }
  void admit_destination(std::string_view path) {
    bool any=false;for(auto value:latch_flags)any=any || (value&kVolatile)!=0;
    if(!any)return; // Preserve historical traffic when no volatile intent exists.
    const auto slash=path.rfind('/');
    const auto parent=writer.inspect_path(slash==std::string_view::npos?std::string_view{}:path.substr(0,slash));
    if(volatile_initial(parent.info.inode))fail(E::delete_pending,"destination parent has initial delete pending");
  }
  bool intent(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)
      if(latched[i]==inode && !(latch_flags[i]&kVolatile))return true;
    for(const auto& slot:slots)
      if(slot.token && slot.token!=excluded && slot.inode==inode && slot.requested)return true;
    return false;
  }
  bool explicit_intent(std::uint64_t inode,std::uint64_t excluded) const noexcept {
    for(std::size_t i=0;i<latched.size();++i)
      if(latched[i]==inode && (latch_flags[i]&kExplicit))return true;
    for(const auto& slot:slots)
      if(slot.token && slot.token!=excluded && slot.inode==inode && slot.requested)return true;
    return false;
  }
  void cancel_initial_latch(std::uint64_t inode) noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode) {
      latch_flags[i]=static_cast<std::uint8_t>(latch_flags[i]&kExplicit);
      if(!latch_flags[i])latched[i]=0;
    }
  }
  std::size_t contexts(std::uint64_t inode,std::uint64_t excluded=0) const noexcept {
    std::size_t count=0;
    for(const auto& slot:slots)if(slot.token && slot.token!=excluded && slot.inode==inode)++count;
    return count;
  }
  bool provisional_inode(std::uint64_t inode) const noexcept {
    for(const auto& slot:slots)
      if(slot.token && slot.inode==inode && slot.phase!=Phase::active)return true;
    return false;
  }
  void clear_latch(std::uint64_t inode) noexcept {
    for(std::size_t i=0;i<latched.size();++i)if(latched[i]==inode){latched[i]=0;latch_flags[i]=0;}
  }
  bool visible_delete_pending(std::uint64_t inode, bool unlinked, bool persisted) {
    if (unlinked) return persisted;
    if (persisted != intent(inode)) {
      terminal = true;
      fail(E::terminal, "native context/delete-intent ownership diverged");
    }
    return persisted || volatile_initial(inode);
  }
  NativeInodeState state(const Slot& slot) {
    try {
      auto result=writer.inspect_inode(slot.inode);
      result.delete_pending = visible_delete_pending(slot.inode, result.unlinked, result.delete_pending);
      return result;
    }catch(const NativeFsError& error) {
      if(error.code()==NativeFsErrorCode::not_found) {
        terminal=true;fail(E::terminal,"owned native inode disappeared");
      }
      throw;
    }
  }
};
NativeFileSession::NativeFileSession(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)){}
NativeFileSession::~NativeFileSession()=default;
NativeFileSession::NativeFileSession(NativeFileSession&&) noexcept=default;
NativeFileSession NativeFileSession::create(NativeFilesystemWriter writer) {
  return create(std::move(writer),NativeDispositionPolicy::aggregate);
}
NativeFileSession NativeFileSession::create(NativeFilesystemWriter writer,NativeDispositionPolicy policy) {
  admit_policy(policy);
  if(writer.poisoned())fail(E::terminal,"cannot own a terminal native writer");
  if(!writer.supports_file_contexts())
    fail(E::invalid_options,"native image lacks its file-context capability");
  static_assert(sizeof(Impl)<=16*1024);
  static_assert(std::is_nothrow_move_constructible_v<NativeFileHandle>);
  return NativeFileSession(std::make_unique<Impl>(std::move(writer),policy));
}
NativeFileSession NativeFileSession::create_volume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::shared_ptr<StoreByteIo> image,StoreIdentity image_id,
    StoreCompressionOptions foreground,StoreCheckpointOptions checkpoints,NativeDispositionPolicy policy,
    std::optional<NativeVolumeRootMetadata> root) {
  return create_volume_impl(std::move(head),volume,std::move(image),image_id,{},foreground,checkpoints,policy,root);
}
NativeFileSession NativeFileSession::create_paged_volume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::shared_ptr<StoreByteIo> image,StoreIdentity image_id,PagedStoreLimits limits,
    StoreCompressionOptions foreground,NativeDispositionPolicy policy,std::optional<NativeVolumeRootMetadata> root) {
  if(!limits.maximum_physical_bytes)fail(E::invalid_options,"explicit paged volume capacity required");
  return create_volume_impl(std::move(head),volume,std::move(image),image_id,
      {NativeImageKind::paged,limits.maximum_physical_bytes},foreground,{},policy,root);
}
NativeFileSession NativeFileSession::create_indexed_volume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::shared_ptr<StoreByteIo> image,StoreIdentity image_id,PagedStoreLimits limits,
    StoreCompressionOptions foreground,NativeDispositionPolicy policy,std::optional<NativeVolumeRootMetadata> root,
    NativeVolumePlacement placement) {
  if(!limits.maximum_physical_bytes)fail(E::invalid_options,"explicit indexed volume capacity required");
  return create_volume_impl(std::move(head),volume,std::move(image),image_id,
      {NativeImageKind::indexed_paged,limits.maximum_physical_bytes},foreground,{},policy,root,placement);
}
NativeFileSession NativeFileSession::create_volume_impl(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::shared_ptr<StoreByteIo> image,StoreIdentity image_id,NativeImageStorage storage,
    StoreCompressionOptions foreground,StoreCheckpointOptions checkpoints,NativeDispositionPolicy policy,
    std::optional<NativeVolumeRootMetadata> root,NativeVolumePlacement placement) {
  admit_policy(policy);detail::validate_store_compression(foreground);
  if(volume==StoreIdentity{} || image_id==StoreIdentity{} || volume==image_id || head==image)
    fail(E::invalid_options,"distinct volume/image identities and backends required");
  if(root && (root->security.empty() || !native_security_descriptor_framed(root->security)))
    fail(E::invalid_options,"invalid initial volume root descriptor");
  admit_empty(head);admit_empty(image);
  auto owner=std::make_unique<Impl::VolumeOwner>();
  owner->head_io=head;owner->active_io=image;owner->volume_id=volume;
  owner->active_id=image_id;owner->foreground=foreground;
  auto writer=[&] {
    if(storage.kind==NativeImageKind::flat)
      return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(image,image_id,foreground,checkpoints));
    auto objects=PagedObjectStoreWriter::create_mirrored(image,image_id,{storage.maximum_physical_bytes},foreground);
    if(storage.kind==NativeImageKind::indexed_paged)
      return NativeFilesystemWriter::initialize_indexed(std::move(objects));
    return NativeFilesystemWriter::initialize_shared_bases(std::move(objects));
  }();
  if(root) {
    const NativeBasicMutation basic{1,0,root->basic};
    const NativeSecurityMutation security{1,0,NativeSecurityDescriptor(root->security.begin(),root->security.end())};
    writer.apply_with_metadata({},{&basic,1},{&security,1});
  }
  auto source=writer.snapshot();retire(writer);
  owner->head.emplace(NativeVolumeHeadWriter::create(head,volume,source,placement));
  owner->epoch=owner->head->info().selection.epoch;
  auto session=create(resume_image(image,source,foreground),policy);
  owner->budget_new=true;
  session.impl_->volume=std::move(owner);return session;
}
NativeFilesystemWriter::SharedBaseWork NativeFileSession::prepare_shared_base(NativeExtentSelection base,
    std::span<const NativeExtentSelection> members) {
  auto& self=ready();if(self.recompression || (self.volume && self.volume->pending))
    fail(E::invalid_options,"shared-base preparation defers active handoff");
  return self.writer.prepare_shared_base(base,members);
}
bool NativeFileSession::advance_shared_base(NativeFilesystemWriter::SharedBaseWork& work) {
  auto& self=ready();if(self.recompression || (self.volume && self.volume->pending))
    fail(E::invalid_options,"shared-base step defers active handoff");
  return self.writer.advance_shared_base(work);
}
NativeSharedBaseCommit NativeFileSession::finish_shared_base(NativeFilesystemWriter::SharedBaseWork& work) {
  auto& self=ready();if(self.recompression || (self.volume && self.volume->pending))
    fail(E::invalid_options,"shared-base publication defers active handoff");
  return self.writer.finish_shared_base(work);
}
NativeFileSession NativeFileSession::resume_volume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeVolumeImageResolver& resolver,
    StoreCompressionOptions foreground,NativeDispositionPolicy policy) {
  return resume_volume_impl(std::move(head),volume,epoch,resolver,foreground,policy,false);
}
NativeFileSession NativeFileSession::resume_closed_indexed_volume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeVolumeImageResolver& resolver,
    StoreCompressionOptions foreground,NativeDispositionPolicy policy,const NativeStartupRepairProvider& repair_target) {
  return resume_volume_impl(std::move(head),volume,epoch,resolver,foreground,policy,true,repair_target);
}
NativeFileSession NativeFileSession::resume_volume_impl(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeVolumeImageResolver& resolver,
    StoreCompressionOptions foreground,NativeDispositionPolicy policy,bool closed_indexed,
    const NativeStartupRepairProvider& repair_target,const detail::IndexedReopenAdmission& admission) {
  admit_policy(policy);detail::validate_store_compression(foreground);
  if(!epoch || volume==StoreIdentity{} || !head || !resolver)
    fail(E::invalid_options,"explicit volume/epoch/provider required");
  auto owner=std::make_unique<Impl::VolumeOwner>();
  owner->head_io=head;owner->volume_id=volume;owner->foreground=foreground;
  const NativeVolumeImageResolver select=[&](const auto& id) {
    owner->active_io=resolver(id);return owner->active_io;
  };
  auto selected=NativeVolumeHeadView::open_impl(head,volume,select,closed_indexed,admission);
  if(selected.info().selection.epoch!=epoch)
    fail(E::invalid_options,"stale volume session epoch");
  if(!closed_indexed && selected.filesystem().needs_session_recovery())
    fail(E::delete_pending,"old file-context state requires qualified recovery");
  owner->active_id=selected.filesystem().identity();owner->epoch=epoch;
  const auto& initial=selected.filesystem();
  const auto attach=[&](NativeFilesystemWriter writer) {
    auto session=create(std::move(writer),policy);
    session.impl_->volume=std::move(owner);
    return session;
  };
  if(closed_indexed && !selected.info().degraded && !initial.needs_session_recovery() &&
      !initial.storage_info().degraded_checkpoint) {
    auto retained=NativeVolumeHeadWriter::retain_indexed_reopen(head,selected.info().selection,initial,foreground);
    if(retained){
      owner->head.emplace(std::move(retained->head));
      return attach(std::move(retained->image));
    }
    // A newly degraded selector needs the ordinary checked repair path.
  }
  // The qualified global provider excludes other controllers during startup.
  // Repair selected image metadata first, then acquire/repair its head owner.
  auto repaired=[&] {
    const auto& source=selected.filesystem();
    if(!closed_indexed)return resume_image(owner->active_io,source,foreground);
    auto objects=[&] {
      try {
        return PagedObjectStoreWriter::resume_impl(owner->active_io,source.identity(),
            source.storage_info().generation,{source.image_storage().maximum_physical_bytes},foreground,true);
      }catch(const PagedStoreCapacityError&) {
        if(!repair_target)throw;
        // A previous append repair may already have published a newer root
        // without changing this selector. Verify that exact current source.
        const auto latest=NativeVolumeHeadView::open_indexed_recovery(head,volume,select);
        if(latest.info().selection!=selected.info().selection)
          fail(E::invalid_options,"startup repair selector changed");
        const auto& current=latest.filesystem();
        if(!current.has_degraded_indexed_payloads())throw;
        if(epoch==(std::numeric_limits<std::uint64_t>::max)())
          fail(E::invalid_options,"startup repair selector epoch exhausted");
        auto authority=NativeVolumeHeadWriter::resume_impl(head,volume,epoch,current,true);
        if(authority.info().selection!=latest.info().selection)
          fail(E::invalid_options,"startup repair authority changed");
        auto replacement=repair_target(authority.info());
        if(replacement.identity==StoreIdentity{} || replacement.identity==volume ||
            replacement.identity==current.identity() || replacement.identity==authority.info().selection.parent_identity ||
            replacement.backing==head || replacement.backing==owner->active_io)
          fail(E::invalid_options,"startup repair requires a distinct image");
        admit_empty(replacement.backing);
        auto candidate=[&] {
          auto copy=NativeFilesystemWriter::copy_indexed_image(current,replacement.backing,replacement.identity,foreground,false);
          return NativeFilesystemSnapshot::open(copy.snapshot());
        }();
        const auto published=authority.publish_compacted_impl(current,candidate,true);
        owner->head.emplace(std::move(authority));
        owner->active_io=std::move(replacement.backing);
        owner->active_id=published.selection.image_identity;owner->epoch=published.selection.epoch;
        return PagedObjectStoreWriter::resume(owner->active_io,owner->active_id,candidate.storage_info().generation,
            {candidate.image_storage().maximum_physical_bytes},foreground);
      }
    }();
    return NativeFilesystemWriter::resume_closed_session(std::move(objects));
  }();
  if(owner->head)return attach(std::move(repaired));
  auto checked=repaired.snapshot();retire(repaired);
  owner->head.emplace(NativeVolumeHeadWriter::resume(head,volume,epoch,checked));
  return attach(resume_image(owner->active_io,checked,foreground));
}
NativeVolumeHeadInfo NativeFileSession::volume_info() const {
  auto& self=ready();
  if(!self.volume || !self.volume->head)fail(E::invalid_options,"session has no volume authority");
  return self.volume->head->info();
}
NativeVolumeSpace NativeFileSession::indexed_volume_space() const {
  auto& self=ready();
  const auto storage=volume_info().selection.storage;
  if(storage.kind!=NativeImageKind::indexed_paged)
    fail(E::invalid_options,"indexed space query requires a managed indexed image");
  const auto occupied=self.writer.indexed_append_floor();
  if(!occupied)fail(E::terminal,"indexed writer has no append boundary");
  return {storage.maximum_physical_bytes,
      storage.maximum_physical_bytes-(std::min)(occupied,storage.maximum_physical_bytes),
      self.writer.indexed_obsolete_bytes(),
      self.maintenance?self.maintenance->group_append_shortfall(
          self.writer.maintenance_identity(),self.writer.recompression_generation()):0};
}
void NativeFileSession::bind_persistent_reclamation(std::shared_ptr<detail::NativeVolumeFileReclaimer> reclaimer) {
  auto& self=ready();
  self.volume_file_bound=true;
  self.persistent_reclaimer=std::move(reclaimer);
}
bool NativeFileSession::indexed_payload_repair_pending() const noexcept {
  return !poisoned() && impl_->writer.indexed_payload_repair_pending();
}
std::uint64_t NativeFileSession::indexed_capacity_repair_generation() const noexcept {
  return !poisoned() && impl_->volume?impl_->writer.indexed_capacity_repair_generation():0;
}
PagedPayloadRepairProgress NativeFileSession::repair_indexed_payload() {
  return ready().writer.repair_indexed_payload();
}
bool NativeFileSession::indexed_payload_scrub_eligible() const noexcept {
  return !poisoned() && impl_->writer.indexed_payload_scrub_eligible();
}
PagedPayloadScrubResult NativeFileSession::scrub_indexed_payload(std::uint64_t utc) {
  using State=PagedPayloadScrubState;
  constexpr auto age_limit=28ULL*24*60*60*10'000'000;
  constexpr auto verification_cost=64*kOptimizationAreaBytes;
  auto& self=ready();
  auto& work=self.scrub;
  auto* advice=self.scrub_advice();
  const auto fresh=[utc](std::uint64_t started) {
    return started && utc>=started && utc-started<age_limit;
  };
  if(utc && !work.restore_attempted) {
    work.restore_attempted=true;
    const auto saved=advice?advice->payload_scrub_checkpoint():std::nullopt;
    if(saved && fresh(saved->pass_started_utc) && self.writer.restore_indexed_payload_scrub(saved->position)) {
      work.image=saved->position.image;
      work.started=saved->pass_started_utc;
    }
  }
  auto result=self.writer.scrub_indexed_payload();
  work.checkpoint.reset();
  if(!utc || !result.resumable) {
    work.verified_record_bytes=0;
    if(result.state==State::complete || result.state==State::unverified_index)work.started=0;
    return result;
  }
  const auto& position=*result.resumable;
  if(!work.started || work.image!=position.image) {
    work.started=utc;
    work.image=position.image;
    work.verified_record_bytes=0;
  }
  work.verified_record_bytes+=(std::min)(result.verified_record_bytes,verification_cost-work.verified_record_bytes);
  if(advice && work.verified_record_bytes==verification_cost && fresh(work.started))
    work.checkpoint=OptimizationPayloadScrub{position,work.started};
  return result;
}
bool NativeFileSession::checkpoint_indexed_payload_scrub() {
  auto& self=ready();
  auto* advice=self.scrub_advice();
  auto& work=self.scrub;
  if(!advice || !work.checkpoint)return false;
  const auto progress=*work.checkpoint;
  work.checkpoint.reset();
  work.verified_record_bytes=0;
  if(progress.position.image!=self.volume->active_id)return false;
  try{
    if(!advice->publish_payload_scrub(progress,self.current_cold_coverage()))return false;
    self.optimization->phase=NativeOptimizationPhase::ready;
    self.optimization->read_code=OptimizationReadCode::available;
    return true;
  }catch(const std::exception&) {
    if(advice->poisoned()) {
      self.optimization->phase=NativeOptimizationPhase::terminal;
      self.optimization->read_code=OptimizationReadCode::io_failure;
      self.budget_loss_input.reset();
    }
    return false;
  }
}
bool NativeFileSession::persistent_reclamation_pending(bool allow_cold) const {
  return !poisoned() && impl_->persistent_reclaimer && impl_->persistent_reclaimer->pending(*this,allow_cold);
}
void NativeFileSession::bind_persistent_reclamation_wake(
    const std::shared_ptr<detail::NativeVolumeRangeWake>& wake) noexcept {
  if(impl_ && impl_->persistent_reclaimer)impl_->persistent_reclaimer->bind_wake(wake);
}
NativeVolumeReclaimProgress NativeFileSession::advance_persistent_reclamation(
    const std::function<StoreIdentity()>& next_identity,bool allow_cold) {
  auto& self=ready();
  return self.persistent_reclaimer?self.persistent_reclaimer->advance(*this,next_identity,allow_cold):
      NativeVolumeReclaimProgress::idle;
}
void NativeFileSession::cancel_persistent_reclamation() noexcept {
  if(impl_ && impl_->persistent_reclaimer)impl_->persistent_reclaimer->cancel(*this);
}
NativeWorkloadInfo NativeFileSession::enable_workload_accounting() {
  auto& self=ready();
  if(!self.volume||!self.volume->head||self.volume->pending||self.recompression)
    fail(E::invalid_options,"workload activation requires a managed quiescent control opportunity");
  self.writer.enable_workload_accounting();
  return self.writer.workload_info(); // No post-publication allocation or I/O.
}
NativeWorkloadInfo NativeFileSession::workload_info() const noexcept {
  return poisoned()?NativeWorkloadInfo{}:impl_->writer.workload_info();
}
std::weak_ptr<detail::NativeMaintenanceSource> NativeFileSession::maintenance_source() {
  auto& self=ready();
  if(!self.volume || !self.volume->head || !self.writer.maintenance_supported())
    fail(E::invalid_options,"automatic grouping requires a managed paged volume");
  if(!self.maintenance) {
    auto source=std::make_shared<detail::NativeMaintenanceSource>();
    source->writer=&self.writer;source->terminal=&self.terminal;
    source->automatic_compression=self.volume->foreground.mode==StoreCompressionMode::automatic;
    source->restore_scan=[owner=&self]()->std::optional<OptimizationGroupScan> {
      const auto* advice=owner->ready_scan_advice();
      if(!advice)return {};
      return advice->snapshot().state.group_scan;
    };
    source->scan_funded=[owner=&self]{return owner->scan_advice_funded();};
    source->publish_scan=[owner=&self](OptimizationGroupScan scan,std::span<const detail::CoherenceScanKeys> keys) {
      return owner->publish_scan_advice(std::move(scan),keys);
    };
    source->coherence_pending=[owner=&self]{return owner->coherence_work_pending();};
    source->advance_coherence=[owner=&self]{owner->advance_coherence_work();};
    self.maintenance=std::move(source);
  }
  return self.maintenance;
}
void NativeFileSession::bind_indexed_coherence(std::shared_ptr<StoreByteIo> backing,PagedStoreLimits limits) {
  auto& self=ready();
  if(!self.volume || !self.volume->head || self.volume->pending || self.recompression ||
      !self.writer.maintenance_indexed() || !backing ||
      backing==self.volume->head_io || backing==self.volume->active_io ||
      backing==self.volume->candidate_io || self.owns_optimization_backend(backing))
    fail(E::invalid_options,"coherence catalog needs separate backing and an indexed managed session");
  (void)maintenance_source();
  if(self.maintenance->coherence_storage)fail(E::invalid_options,"coherence storage already configured");
  auto catalog=IndexedCoherenceSnapshot::open(std::move(backing),self.volume->volume_id,self.volume->active_id,limits);
  self.adopt_coherence(std::move(catalog));
}
void NativeFileSession::bind_indexed_coherence_writer(IndexedCoherenceWriter writer) {
  auto& self=ready();
  if(!self.ready_scan_advice() || !self.writer.maintenance_indexed())
    fail(E::invalid_options,"writable coherence catalog needs indexed storage and funded advice");
  (void)maintenance_source();
  if(self.maintenance->coherence_storage)fail(E::invalid_options,"coherence storage already configured");
  auto catalog=writer.snapshot();
  if(catalog.volume()!=self.volume->volume_id || catalog.image()!=self.volume->active_id)
    fail(E::invalid_options,"writable coherence catalog identity differs");
  self.adopt_coherence(std::move(catalog),std::make_shared<IndexedCoherenceWriter>(std::move(writer)));
}
void NativeFileSession::configure_indexed_coherence(std::shared_ptr<IndexedCoherenceStorage> storage) {
  auto& self=ready();
  if(!storage || !storage->session_.expired() || !self.writer.maintenance_indexed() ||
      !self.volume || self.volume->pending || self.recompression)
    fail(E::invalid_options,"coherence storage requires a quiescent managed indexed session");
  (void)maintenance_source();
  if(self.maintenance->coherence_storage || self.maintenance->coherence)
    fail(E::invalid_options,"coherence storage already configured");
  if(storage->retained_) {
    try {
      auto catalog=storage->retained_->snapshot();
      if(catalog.volume()==self.volume->volume_id && catalog.image()==self.volume->active_id)
        self.adopt_coherence(std::move(catalog),storage->retained_);
      else storage->retained_.reset();
    }catch(const std::exception&) {storage->retained_.reset();}
  }
  storage->session_=self.owner;
  self.maintenance->coherence_storage=std::move(storage);
}
NativeOptimizationInfo NativeFileSession::optimization_info() const noexcept {
  NativeOptimizationInfo out;
  if(!impl_||!impl_->volume)return out;
  const auto& self=*impl_;out.session_ready=!self.terminal&&!self.writer.poisoned();
  out.binding_attempts=self.optimization_bindings;
  out.owned_area_bytes=std::uint64_t(self.optimization_bindings)*kOptimizationAreaBytes;
  out.budgeted=self.budget_mode;out.loss_input_baseline=self.budget_loss_input;
  if(!self.optimization)return out;
  const auto& area=*self.optimization;
  out.phase=area.phase;out.read_code=area.read_code;out.reserved_bytes=kOptimizationAreaBytes;
  if(area.writer) {
    out.repair_pending=area.writer->budget_repair_pending();
    if(out.repair_pending)out.loss_input_baseline=area.writer->budget_repair_floor();
    out.settled_input_bytes=area.writer->budget_settlement();
    if(area.writer->poisoned()) {
      out.phase=NativeOptimizationPhase::terminal;out.read_code=OptimizationReadCode::io_failure;
    }else if(area.phase==NativeOptimizationPhase::ready) {
      out.sequence=area.writer->sequence();out.remaining_publications=area.writer->remaining_publications();
    }
  }
  return out;
}
NativeOptimizationInfo NativeFileSession::create_optimization_area(std::shared_ptr<StoreByteIo> area,unsigned limit) {
  return bind_optimization_area(std::move(area),0,limit,true,false);
}
NativeOptimizationInfo NativeFileSession::resume_optimization_area(std::shared_ptr<StoreByteIo> area,
    std::uint64_t sequence,unsigned limit) {
  return bind_optimization_area(std::move(area),sequence,limit,false,false);
}
NativeOptimizationInfo NativeFileSession::rebuild_optimization_area(std::shared_ptr<StoreByteIo> area,unsigned limit) {
  return bind_optimization_area(std::move(area),0,limit,true,true);
}
NativeOptimizationInfo NativeFileSession::bind_optimization_area(std::shared_ptr<StoreByteIo> area,
    std::uint64_t sequence,unsigned limit,bool create,bool rebuild,bool budgeted) {
  auto& self=ready();
  const bool failed=self.optimization&&
      (self.optimization->phase==NativeOptimizationPhase::unavailable||
       self.optimization->phase==NativeOptimizationPhase::terminal||
       (self.optimization->writer&&self.optimization->writer->poisoned()));
  if(!self.volume||!self.volume->head||self.recompression||
      (budgeted&&self.volume->pending)||
      (!budgeted&&self.budget_mode)||
      (budgeted&&rebuild&&self.optimization&&self.optimization->writer&&
          self.optimization->writer->budget_repair_pending())||
      !area||!limit||limit>4||(!create&&!sequence)||
      (rebuild?(!create||!failed||!self.optimization_bindings||
          self.optimization_bindings>=max_optimization_bindings):bool(self.optimization))||
      self.owns_optimization_backend(area)||
      area==self.volume->head_io||area==self.volume->active_io||area==self.volume->candidate_io)
    fail(E::invalid_options,"new distinct provider-reserved advisory binding required");
  std::optional<std::uint64_t> input,settled;
  if(budgeted) {
    const auto cached=self.writer.workload_info();
    if(!cached.enabled)fail(E::invalid_options,"budgeted advice needs native workload provenance");
    // Cached input can decline work, never fund it. Fresh checked input below
    // remains mandatory before any new record write.
    if(create&&self.budget_loss_input&&
        !(self.volume->budget_new&&!self.volume->budget_bootstrap_claimed)&&
        (cached.committed_input_bytes<*self.budget_loss_input||
         cached.committed_input_bytes-*self.budget_loss_input<2*kOptimizationAreaBytes))
      return optimization_info();
    const auto actual=self.writer.snapshot().workload_info();
    if(!actual.enabled)fail(E::invalid_options,"checked workload provenance unavailable");
    input=actual.committed_input_bytes;
    self.budget_mode=true;
    if(create) {
      const bool bootstrap=self.volume->budget_new&&!self.volume->budget_bootstrap_claimed;
      if(!self.budget_loss_input)self.budget_loss_input=*input;
      const auto baseline=*self.budget_loss_input;
      if(!bootstrap&&(*input<baseline||*input-baseline<2*kOptimizationAreaBytes))
        return optimization_info(); // No candidate admission or candidate backing I/O.
      settled=bootstrap?*input:baseline+2*kOptimizationAreaBytes;
    }
  }
  // Allocate before changing ownership. After admission retain the candidate,
  // every old backend and each poison-held lease, including failed creations.
  auto owner=std::make_unique<Impl::OptimizationOwner>();owner->backing=std::move(area);
  owner->budgeted=budgeted;
  if(rebuild)self.prior_optimization[self.optimization_bindings-1]=std::move(self.optimization);
  self.optimization=std::move(owner);++self.optimization_bindings;
  auto& bound=*self.optimization;
  if(budgeted){self.volume->budget_bootstrap_claimed=true;self.budget_loss_input=*input;}
  try {
    if(budgeted) {
      if(create)bound.writer.emplace(OptimizationStateWriter::create_budgeted(
          bound.backing,self.volume->volume_id,*settled,limit));
      else bound.writer.emplace(OptimizationStateWriter::resume_budgeted(
          bound.backing,self.volume->volume_id,sequence,*input,limit));
      if(!create)self.cold_coverage=bound.writer->cold_coverage_checkpoint();
      bound.read_code=OptimizationReadCode::available;
      bound.phase=bound.writer->budget_repair_pending()?
          NativeOptimizationPhase::unavailable:NativeOptimizationPhase::ready;
      self.budget_loss_input.reset();
      return optimization_info();
    }
    if(create)bound.writer.emplace(OptimizationStateWriter::create(bound.backing,self.volume->volume_id,limit));
    else {
      const auto observed=read_optimization_state(*bound.backing,self.volume->volume_id);
      bound.read_code=observed.code;
      if(observed.code!=OptimizationReadCode::available)return optimization_info();
      if(observed.sequence!=sequence){bound.read_code=OptimizationReadCode::invalid;return optimization_info();}
      if(observed.state.settled_input_bytes) {
        self.budget_mode=true;self.volume->budget_bootstrap_claimed=true;
        bound.read_code=OptimizationReadCode::unsupported;
        return optimization_info(); // No legacy repair or downgrade of funded state.
      }
      bound.writer.emplace(OptimizationStateWriter::resume(bound.backing,self.volume->volume_id,sequence,limit));
    }
    bound.phase=NativeOptimizationPhase::ready;bound.read_code=OptimizationReadCode::available;
  }catch(const std::bad_alloc&){bound.read_code=OptimizationReadCode::io_failure;throw;}
  catch(const std::exception&){bound.read_code=OptimizationReadCode::io_failure;}
  return optimization_info();
}
OptimizationSnapshot NativeFileSession::optimization_snapshot() const {
  const auto& self=ready();if(!self.optimization)return {};
  const auto& area=*self.optimization;
  if(area.phase==NativeOptimizationPhase::ready&&area.writer&&!area.writer->poisoned())
    return area.writer->snapshot();
  OptimizationSnapshot out;out.code=area.read_code==OptimizationReadCode::available?
      OptimizationReadCode::io_failure:area.read_code;
  if(area.writer&&area.writer->poisoned())out.code=OptimizationReadCode::io_failure;
  return out;
}
NativeOptimizationInfo NativeFileSession::publish_optimization_state(const OptimizationState& state) {
  auto& self=ready();
  if(!self.volume||!self.optimization||self.recompression||
      self.optimization->phase!=NativeOptimizationPhase::ready||!self.optimization->writer)
    fail(E::invalid_options,"no ready advisory owner or candidate work is active");
  auto& area=*self.optimization;
  try{area.writer->publish(state);}
  catch(...) {
    if(area.writer->poisoned()){
      area.phase=NativeOptimizationPhase::terminal;area.read_code=OptimizationReadCode::io_failure;
      if(area.budgeted)self.budget_loss_input.reset(); // Establish fresh checked input on the next opportunity.
    }
    throw; // Never fence data or retire file contexts for advisory failure.
  }
  return optimization_info(); // No allocation after successful publication.
}
NativeOptimizationInfo NativeFileSession::create_budgeted_optimization_area(
    std::shared_ptr<StoreByteIo> area,unsigned limit) {
  return bind_optimization_area(std::move(area),0,limit,true,false,true);
}
NativeOptimizationInfo NativeFileSession::resume_budgeted_optimization_area(
    std::shared_ptr<StoreByteIo> area,std::uint64_t sequence,unsigned limit) {
  return bind_optimization_area(std::move(area),sequence,limit,false,false,true);
}
bool NativeFileSession::prepare_budgeted_optimization_rebuild() {
  auto& self=ready();
  if(!self.volume||self.volume->pending||self.recompression||!self.budget_mode||!self.optimization||
      (self.optimization->writer&&self.optimization->writer->budget_repair_pending())||
      (self.optimization->phase!=NativeOptimizationPhase::terminal&&
       self.optimization->phase!=NativeOptimizationPhase::unavailable))
    fail(E::invalid_options,"no failed funded advisory owner");
  if(self.optimization_bindings>=max_optimization_bindings)return false;
  const auto cached=self.writer.workload_info();
  if(!cached.enabled)fail(E::invalid_options,"native input provenance unavailable");
  if(self.budget_loss_input&&(cached.committed_input_bytes<*self.budget_loss_input||
      cached.committed_input_bytes-*self.budget_loss_input<2*kOptimizationAreaBytes))return false;
  const auto actual=self.writer.snapshot().workload_info();
  if(!actual.enabled)fail(E::invalid_options,"checked native input provenance unavailable");
  if(!self.budget_loss_input)self.budget_loss_input=actual.committed_input_bytes;
  const auto baseline=*self.budget_loss_input;
  return actual.committed_input_bytes>=baseline&&
      actual.committed_input_bytes-baseline>=2*kOptimizationAreaBytes;
}
NativeOptimizationInfo NativeFileSession::rebuild_budgeted_optimization_area(
    std::shared_ptr<StoreByteIo> area,unsigned limit) {
  return bind_optimization_area(std::move(area),0,limit,true,true,true);
}
NativeOptimizationBudgetPublication NativeFileSession::publish_budgeted_optimization_state(const OptimizationState& state) {
  auto& self=ready();
  if(!self.volume||self.volume->pending||self.recompression||!self.optimization||!self.optimization->budgeted||
      self.optimization->phase!=NativeOptimizationPhase::ready||!self.optimization->writer)
    fail(E::invalid_options,"no ready budgeted advisory owner");
  auto& area=*self.optimization;
  if(!area.writer->remaining_publications())
    return {NativeOptimizationBudgetProgress::deferred,optimization_info()};
  const auto actual=self.writer.snapshot().workload_info();
  if(!actual.enabled)fail(E::invalid_options,"native input provenance unavailable");
  OptimizationStateWriter::BudgetResult result;
  try{result=area.writer->publish_budgeted(state,actual.committed_input_bytes,self.current_cold_coverage());}
  catch(...) {
    if(area.writer->poisoned()) {
      area.phase=NativeOptimizationPhase::terminal;area.read_code=OptimizationReadCode::io_failure;
      self.budget_loss_input=actual.committed_input_bytes;
    }
    throw;
  }
  const auto progress=result==OptimizationStateWriter::BudgetResult::published?
      NativeOptimizationBudgetProgress::published:result==OptimizationStateWriter::BudgetResult::unchanged?
      NativeOptimizationBudgetProgress::unchanged:NativeOptimizationBudgetProgress::deferred;
  return {progress,optimization_info()};
}
NativeOptimizationBudgetPublication NativeFileSession::repair_budgeted_optimization_area() {
  auto& self=ready();
  if(!self.volume||self.volume->pending||self.recompression||!self.optimization||!self.optimization->budgeted||
      !self.optimization->writer||!self.optimization->writer->budget_repair_pending())
    fail(E::invalid_options,"no pending funded advisory repair");
  auto& area=*self.optimization;
  const auto cached=self.writer.workload_info();
  const auto settlement=area.writer->budget_repair_floor();
  if(!area.writer->remaining_publications()||!cached.enabled||!settlement||
      cached.committed_input_bytes<*settlement||
      cached.committed_input_bytes-*settlement<kOptimizationAreaBytes)
    return {NativeOptimizationBudgetProgress::deferred,optimization_info()};
  const auto actual=self.writer.snapshot().workload_info();
  if(!actual.enabled)fail(E::invalid_options,"native input provenance unavailable");
  OptimizationStateWriter::BudgetResult result;
  try {
    result=area.writer->repair_budgeted(actual.committed_input_bytes);
    if(!area.writer->budget_repair_pending()) {
      area.phase=NativeOptimizationPhase::ready;area.read_code=OptimizationReadCode::available;
    }
  }catch(...) {
    if(area.writer->poisoned()) {
      area.phase=NativeOptimizationPhase::terminal;area.read_code=OptimizationReadCode::io_failure;
      self.budget_loss_input=actual.committed_input_bytes;
    }
    throw;
  }
  const auto progress=result==OptimizationStateWriter::BudgetResult::published?
      NativeOptimizationBudgetProgress::published:result==OptimizationStateWriter::BudgetResult::unchanged?
      NativeOptimizationBudgetProgress::unchanged:NativeOptimizationBudgetProgress::deferred;
  return {progress,optimization_info()};
}
NativeVolumeHeadInfo NativeFileSession::compact_volume(std::shared_ptr<StoreByteIo> target,
    StoreIdentity next,StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  return replace_volume(std::move(target),next,compression,checkpoints,false);
}
NativeVolumeHeadInfo NativeFileSession::recompress_volume(std::shared_ptr<StoreByteIo> target,
    StoreIdentity next,StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  return replace_volume(std::move(target),next,compression,checkpoints,true);
}
void NativeFileSession::admit_replacement(const Impl& self,
    const std::shared_ptr<StoreByteIo>& target,StoreIdentity next,bool owned_file_target) {
  if(self.volume_file_bound!=owned_file_target)
    fail(E::invalid_options,"volume file replacement requires its own image range");
  if(!self.volume || !self.volume->head)fail(E::invalid_options,"session has no volume authority");
  const auto& volume=*self.volume;
  if(self.recompression || volume.pending || next==StoreIdentity{} || next==volume.active_id ||
      next==volume.volume_id || next==volume.head->info().selection.parent_identity ||
      !target || target==volume.active_io || target==volume.head_io ||
      self.owns_optimization_backend(target))
    fail(E::invalid_options,"new distinct maintenance image required");
}

bool NativeFileSession::handoff_contexts_resolved(const Impl& self) noexcept {
  for(const auto& slot:self.slots)if(slot.token &&
      (slot.phase!=Impl::Phase::active || slot.requested || slot.cleanup_delete))return false;
  for(const auto latch:self.latched)if(latch)return false;
  return true;
}
void NativeFileSession::admit_handoff_contexts(const Impl& self) {
  if(!handoff_contexts_resolved(self))fail(E::delete_pending,"maintenance defers unresolved file contexts");
}
bool NativeFileSession::idle_compaction_context_ready() const noexcept {
  return !poisoned() && impl_->volume && impl_->volume->head &&
      impl_->volume->head->info().selection.storage.kind==NativeImageKind::indexed_paged &&
      handoff_contexts_resolved(*impl_);
}
bool NativeFileSession::idle_cold_compaction_pending() const noexcept {
  if(!idle_compaction_context_ready())return false;
  const auto& self=*impl_;
  if(self.recompression || self.volume->foreground.mode==StoreCompressionMode::raw || !store_zstd_available() ||
      self.writer.indexed_capacity_repair_generation() ||
      (self.evaluated_image==self.volume->active_id &&
       self.evaluated_generation==self.writer.recompression_generation()))return false;
  const auto* advice=self.ready_funded_advice();
  if(!advice)return false;
  const auto settled=advice->budget_settlement();
  const auto input=self.writer.workload_info();
  const auto end=self.writer.maintenance_physical_end();
  if(!settled || !input.enabled || !end)return false;
  const auto floor=(std::max)(*settled,self.cold_screen_input_floor);
  return input.committed_input_bytes>=floor &&
      input.committed_input_bytes-floor>=cold_compaction_input_cost(end);
}

void NativeFileSession::begin_handoff(Impl& self,NativeFilesystemSnapshot baseline,
    std::shared_ptr<StoreByteIo> target,StoreIdentity next) {
  auto& volume=*self.volume;
  if(baseline.needs_session_recovery())fail(E::delete_pending,"maintenance requires resolved native state");
  // Recovery owns both backings and the exact snapshot before retirement.
  volume.baseline.emplace(std::move(baseline));volume.candidate_io=std::move(target);
  volume.candidate_id=next;volume.pending=true;volume.recovery_attempted=false;
  self.terminal=true;
}

NativeVolumeHeadInfo NativeFileSession::finish_handoff(Impl& self) {
  auto& volume=*self.volume;
  auto candidate=NativeFilesystemSnapshot::open_image(volume.candidate_io,volume.baseline->image_storage());
  const auto published=volume.head->publish_compacted(*volume.baseline,candidate);
  self.writer=resume_image(volume.candidate_io,candidate,volume.foreground);
  adopt_handoff(self,published);
  return published;
}

void NativeFileSession::adopt_handoff(Impl& self,const NativeVolumeHeadInfo& published) noexcept {
  auto& volume=*self.volume;
  volume.active_io=std::move(volume.candidate_io);
  volume.active_id=published.selection.image_identity;
  self.cold_coverage.reset();
  if(self.maintenance) {
    self.maintenance->retire_coherence();
    self.maintenance->group_append_refusal={};
  }
  volume.epoch=published.selection.epoch;
  volume.candidate_id={};
  volume.baseline.reset();
  volume.pending=false;
  self.terminal=false;
}

NativeVolumeHeadInfo NativeFileSession::replace_volume(std::shared_ptr<StoreByteIo> target,
    StoreIdentity next,StoreCompressionOptions compression,StoreCheckpointOptions checkpoints,bool reencode) {
  auto& self=ready();
  admit_replacement(self,target,next);
  detail::validate_store_compression(compression);admit_empty(target);
  admit_handoff_contexts(self);
  auto baseline=self.writer.snapshot();
  if(baseline.image_storage().kind!=NativeImageKind::flat && checkpoints.compact_metadata)
    fail(E::invalid_options,"paged isolation derives from its backend contract");
  begin_handoff(self,std::move(baseline),target,next);
  retire(self.writer);
  {
    auto candidate=reencode?
        NativeFilesystemWriter::recompress(*self.volume->baseline,target,next,compression,checkpoints):
        NativeFilesystemWriter::compact(*self.volume->baseline,target,next,compression,checkpoints);
  }
  return finish_handoff(self);
}

void NativeFileSession::begin_recompression(std::shared_ptr<StoreByteIo> target,StoreIdentity next,
    StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  (void)stage_recompression(std::move(target),next,compression,checkpoints,false);
}

bool NativeFileSession::begin_idle_recompression(std::shared_ptr<StoreByteIo> target,StoreIdentity next,
    StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  return stage_recompression(std::move(target),next,compression,checkpoints,true);
}

bool NativeFileSession::stage_recompression(std::shared_ptr<StoreByteIo> target,StoreIdentity next,
    StoreCompressionOptions compression,StoreCheckpointOptions checkpoints,bool idle) {
  auto& self=ready();
  if(self.volume && self.volume->head->info().selection.storage.kind!=NativeImageKind::flat) {
    if(idle)return false;
    fail(E::invalid_options,"legacy staged recompression requires flat managed storage");
  }
  const auto generation=self.writer.recompression_generation();
  if(idle && self.volume && self.evaluated_image==self.volume->active_id &&
      self.evaluated_generation==generation && !self.recompression)
    return false;
  admit_replacement(self,target,next);
  detail::validate_store_compression(compression);
  admit_handoff_contexts(self);
  if(idle) {
    self.evaluated_image=self.volume->active_id;
    self.evaluated_generation=generation;
  }
  admit_empty(target);
  self.recompression=std::make_unique<Impl::StagedRecompression>(Impl::StagedRecompression{
      target,next,self.writer.prepare_recompression(target,next,compression,checkpoints),idle});
  return true;
}

bool NativeFileSession::begin_idle_compaction(std::shared_ptr<StoreByteIo> target,StoreIdentity next,PagedRewriteMode mode) {
  return begin_idle_compaction_impl(std::move(target),next,false,mode);
}
bool NativeFileSession::begin_idle_compaction_impl(std::shared_ptr<StoreByteIo> target,StoreIdentity next,
    bool owned_file_target,PagedRewriteMode mode) {
  auto& self=ready();
  if(!self.volume || self.volume->head->info().selection.storage.kind!=NativeImageKind::indexed_paged)
    fail(E::invalid_options,"idle compaction requires a managed indexed image");
  const auto generation=self.writer.recompression_generation();
  const bool screened=mode==PagedRewriteMode::cold_compression;
  if(screened && !idle_cold_compaction_pending())return false;
  const bool capacity_repair=mode==PagedRewriteMode::reconcile_foreground &&
      self.writer.indexed_capacity_repair_generation();
  if(!self.recompression && self.evaluated_image==self.volume->active_id &&
      self.evaluated_generation==generation && (!capacity_repair || self.evaluated_capacity_repair))return false;
  admit_replacement(self,target,next,owned_file_target);
  admit_handoff_contexts(self);
  admit_empty(target);
  const auto foreground=self.volume->foreground;
  std::optional<std::uint64_t> cold_after;
  if(mode!=PagedRewriteMode::exact_checkpoint && !capacity_repair &&
      foreground.mode!=StoreCompressionMode::raw && store_zstd_available()) {
    const auto saved=self.current_cold_coverage();
    cold_after=saved?saved->through_generation:0;
  }
  std::function<bool(std::uint64_t)> reserve;
  if(screened) {
    self.cold_screen_input_floor=self.writer.workload_info().committed_input_bytes;
    reserve=[owner=&self](std::uint64_t bytes){return owner->reserve_cold_compaction(bytes);};
  }
  auto rebuild=self.writer.prepare_paged_recompression_impl(target,next,foreground,false,mode,cold_after,std::move(reserve));
  self.recompression=std::make_unique<Impl::StagedRecompression>(
      Impl::StagedRecompression{std::move(target),next,std::move(rebuild),true});
  self.evaluated_image=self.volume->active_id;
  self.evaluated_generation=generation;
  self.evaluated_capacity_repair=capacity_repair;
  return true;
}

bool NativeFileSession::idle_compaction_preparation_pending() const noexcept {
  if(poisoned() || !impl_->recompression || !impl_->recompression->idle)return false;
  const auto* work=std::get_if<PagedObjectStoreWriter::Rewrite>(&impl_->recompression->work);
  if(!work || !impl_->writer.matches_paged_recompression(*work))return false;
  using Phase=PagedRewritePhase;
  const auto phase=work->progress().phase;
  return phase==Phase::verifying_source || phase==Phase::copying || phase==Phase::building_index ||
      phase==Phase::verifying_target || phase==Phase::verifying_publication ||
      phase==Phase::screening_compression ||
      phase==Phase::reconciling || phase==Phase::verifying_reconciliation ||
      (phase==Phase::ready && !impl_->writer.paged_recompression_ready(*work));
}

std::optional<StoreRecompressionInput> NativeFileSession::next_recompression_input() const {
  auto& self=ready();
  if(!self.recompression)return std::nullopt;
  const auto* flat=std::get_if<NativeFilesystemWriter::Recompression>(&self.recompression->work);
  if(!flat || !self.writer.matches_recompression(*flat))return std::nullopt;
  return NativeFilesystemWriter::next_recompression_input(*flat);
}

NativeFileSession::RecompressionProgress NativeFileSession::advance_recompression() {
  return advance_recompression(0,{});
}

NativeFileSession::RecompressionProgress NativeFileSession::advance_recompression(
    std::uint64_t object_id,std::span<const StoreMatchProposal> matches) {
  auto& self=ready();
  if(!self.recompression)fail(E::invalid_options,"no staged recompression");
  if(!self.recompression->matches(self.writer)) {
    self.recompression.reset();
    return RecompressionProgress::stale;
  }
  try {
    auto& work=self.recompression->work;
    if(auto* paged=std::get_if<PagedObjectStoreWriter::Rewrite>(&work)) {
      if(object_id || !matches.empty())fail(E::invalid_options,"indexed compaction preserves object encodings");
      if(self.writer.advance_paged_recompression(*paged))return RecompressionProgress::ready;
      if(paged->progress().phase==PagedRewritePhase::declined) {
        self.recompression.reset();
        return RecompressionProgress::declined;
      }
      return RecompressionProgress::copying;
    }
    auto& flat=std::get<NativeFilesystemWriter::Recompression>(work);
    return NativeFilesystemWriter::advance_recompression(flat,object_id,matches)?
        RecompressionProgress::ready:RecompressionProgress::copying;
  }catch(...){self.recompression.reset();throw;}
}

std::optional<StoreRecompressionFeedback> NativeFileSession::recompression_feedback() const noexcept {
  if(poisoned() || !impl_->recompression)return std::nullopt;
  const auto* flat=std::get_if<NativeFilesystemWriter::Recompression>(&impl_->recompression->work);
  if(!flat || !impl_->writer.matches_recompression(*flat))return std::nullopt;
  return NativeFilesystemWriter::recompression_feedback(*flat);
}

NativeVolumeHeadInfo NativeFileSession::publish_recompression() {
  return *publish_staged_recompression(false);
}

std::optional<NativeVolumeHeadInfo> NativeFileSession::publish_idle_recompression() {
  return publish_staged_recompression(true);
}

std::optional<NativeVolumeHeadInfo> NativeFileSession::publish_staged_recompression(bool idle,bool owned_file_target) {
  auto& self=ready();
  if(self.volume_file_bound!=owned_file_target)
    fail(E::invalid_options,"volume file publication requires its own image range");
  if(!self.recompression || self.recompression->idle!=idle)
    fail(E::invalid_options,"no matching staged recompression");
  if(!self.recompression->matches(self.writer)) {
    self.recompression.reset();
    fail(E::invalid_options,"staged recompression is stale");
  }
  admit_handoff_contexts(self);
  if(auto* paged=std::get_if<PagedObjectStoreWriter::Rewrite>(&self.recompression->work)) {
    if(!self.writer.paged_recompression_ready(*paged))
      fail(E::invalid_options,"staged indexed compaction has not verified current commits");
    const auto repaired=paged->verified_capacity_repair_generation();
    const auto cold_through=paged->cold_encoding_through();
    const bool permits_rotation=repaired && repaired==self.writer.indexed_capacity_repair_generation();
    auto staged=std::move(self.recompression);
    auto candidate=self.writer.finish_paged_recompression(*paged);
    auto verified=candidate.snapshot();
    if(verified.storage_info().physical_end>=self.writer.indexed_append_floor() && !permits_rotation)
      return std::nullopt;
    auto baseline=self.writer.snapshot();
    begin_handoff(self,std::move(baseline),std::move(staged->target),staged->id);
    auto& volume=*self.volume;
    NativeVolumeHeadInfo published;
    try {
      published=volume.head->publish_indexed_rebuild(*volume.baseline,verified);
    }catch(...) {
      if(!volume.head->poisoned()) {
        volume.baseline.reset();
        volume.candidate_io.reset();
        volume.candidate_id={};
        volume.pending=false;
        self.terminal=false;
      }
      throw;
    }
    self.writer=std::move(candidate);
    adopt_handoff(self,published);
    if(cold_through) {
      const auto& selection=published.selection;
      self.cold_coverage=OptimizationColdCoverage{selection.image_identity,selection.epoch,
          selection.minimum_checkpoint,cold_through,detail::kIndexedColdEncodingRevision,
          store_zstd_version(),static_cast<std::uint32_t>(volume.foreground.zstd_level)};
    }
    self.evaluated_image=volume.active_id;
    self.evaluated_generation=self.writer.recompression_generation();
    self.evaluated_capacity_repair=false;
    return published;
  }
  auto& flat=std::get<NativeFilesystemWriter::Recompression>(self.recompression->work);
  if(!flat.ready())fail(E::invalid_options,"staged recompression is incomplete");
  auto staged=std::move(self.recompression);
  if(idle && !flat.saves_space())return std::nullopt;
  auto baseline=self.writer.snapshot();
  // Seal and verify the candidate before fencing the foreground writer.
  NativeFilesystemWriter::finish_recompression(flat);
  begin_handoff(self,std::move(baseline),std::move(staged->target),staged->id);
  retire(self.writer);
  const auto published=finish_handoff(self);
  if(idle) {
    self.evaluated_image=self.volume->active_id;
    self.evaluated_generation=self.writer.recompression_generation();
  }
  return published;
}

void NativeFileSession::cancel_recompression() noexcept {
  if(impl_)impl_->recompression.reset();
}

NativeVolumeHeadInfo NativeFileSession::recover_volume() {
  if(!impl_ || !impl_->terminal || !impl_->volume)
    fail(E::invalid_options,"no failed maintenance transition to recover");
  auto& self=*impl_;auto& volume=*self.volume;
  if(!volume.pending || volume.recovery_attempted || !volume.baseline || !volume.candidate_io)
    fail(E::terminal,"maintenance recovery is unavailable or already attempted");
  volume.recovery_attempted=true;retire(self.writer);
  volume.head.reset(); // Confirmed destruction releases the exact previous lease.
  std::shared_ptr<StoreByteIo> chosen;
  auto selected=NativeVolumeHeadView::open(volume.head_io,volume.volume_id,[&](const auto& id) {
    if(id==volume.active_id)chosen=volume.active_io;
    else if(id==volume.candidate_id)chosen=volume.candidate_io;
    else fail(E::terminal,"head selected an unowned maintenance image");
    return chosen;
  });
  const auto observed=selected.info().selection;
  if(!((observed.epoch==volume.epoch && observed.image_identity==volume.active_id) ||
      (volume.epoch!=(std::numeric_limits<std::uint64_t>::max)() &&
       observed.epoch==volume.epoch+1 && observed.image_identity==volume.candidate_id &&
       observed.parent_identity==volume.active_id)))
    fail(E::terminal,"maintenance head is outside the owned transition");
  volume.baseline->verify_compacted_equivalent_for_volume_head(selected.filesystem());
  auto repaired=resume_image(chosen,selected.filesystem(),volume.foreground);
  auto checked=repaired.snapshot();retire(repaired);
  volume.baseline->verify_compacted_equivalent_for_volume_head(checked);
  volume.head.emplace(NativeVolumeHeadWriter::resume(volume.head_io,volume.volume_id,observed.epoch,checked));
  const auto restored=volume.head->info();
  self.writer=resume_image(chosen,checked,volume.foreground);
  if(self.maintenance && observed.image_identity!=volume.active_id)self.maintenance->retire_coherence();
  volume.active_io=std::move(chosen);volume.active_id=observed.image_identity;volume.epoch=observed.epoch;
  volume.candidate_io.reset();volume.candidate_id={};volume.baseline.reset();
  volume.pending=false;self.terminal=false;return restored;
}
NativeFileSession::Impl& NativeFileSession::ready() const {
  if(!impl_ || impl_->terminal || impl_->writer.poisoned())fail(E::terminal,"native file session is fenced");
  return *impl_;
}
std::size_t NativeFileSession::slot_for(const NativeFileHandle& handle,bool allow_provisional) const {
  auto& self=ready();const auto owner=handle.owner_.lock();
  if(!handle.token_ || !owner || owner!=self.owner)fail(E::invalid_handle,"foreign/expired native file context");
  for(std::size_t i=0;i<self.slots.size();++i)
    if(self.slots[i].token==handle.token_) {
      if(!allow_provisional && self.slots[i].phase!=Impl::Phase::active)
        fail(E::invalid_options,"native context is not adopted");
      return i;
    }
  fail(E::invalid_handle,"closed native file context");
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,NativeOpenKind kind) {
  return open(path,access,kind,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access) {
  return open(path,access,kind,basic_access,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access) {
  return open(path,access,kind,basic_access,security_access,{});
}
NativeFileHandle NativeFileSession::open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeOverwriteAccess overwrite_access) {
  return open_impl(path,access,kind,basic_access,security_access,overwrite_access,false,false,{});
}
NativeFileHandle NativeFileSession::prepare_overwrite(std::string_view path,NativeFileAccess access,
    NativeBasicAccess basic_access,NativeSecurityAccess security_access,NativeOverwriteAccess overwrite_access) {
  return prepare_overwrite(path,access,basic_access,security_access,overwrite_access,{});
}
NativeFileHandle NativeFileSession::prepare_overwrite(std::string_view path,NativeFileAccess access,
    NativeBasicAccess basic_access,NativeSecurityAccess security_access,NativeOverwriteAccess overwrite_access,
    NativeInitialDeleteAccess initial_delete) {
  return open_impl(path,access,NativeOpenKind::file,basic_access,security_access,overwrite_access,true,true,initial_delete);
}
NativeFileHandle NativeFileSession::prepare_open(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeInitialDeleteAccess initial_delete) {
  return open_impl(path,access,kind,basic_access,security_access,{},true,false,initial_delete);
}
NativeFileHandle NativeFileSession::open_impl(std::string_view path,NativeFileAccess access,
    NativeOpenKind kind,NativeBasicAccess basic_access,NativeSecurityAccess security_access,
    NativeOverwriteAccess overwrite_access,bool provisional,bool awaiting_overwrite,
    NativeInitialDeleteAccess initial_delete) {
  auto& self=ready();
  if(provisional && awaiting_overwrite && overwrite_access.overwrite==overwrite_access.supersede)
    fail(E::invalid_options,"prepared overwrite requires exactly one checked operation grant");
  if(kind!=NativeOpenKind::any && kind!=NativeOpenKind::file && kind!=NativeOpenKind::directory)
    fail(E::invalid_options,"unknown native open kind");
  std::size_t available=self.slots.size();
  for(std::size_t i=0;i<self.slots.size();++i)if(!self.slots[i].token){available=i;break;}
  if(available==self.slots.size() || self.next_token==(std::numeric_limits<std::uint64_t>::max)())
    fail(E::limit,"native file context budget exhausted");
  admit_path(self.writer,path,true);
  const auto state=self.writer.inspect_path(path);
  if(state.delete_pending || state.unlinked || self.volatile_initial(state.info.inode))
    fail(E::delete_pending,"native name is delete-pending");
  if(initial_delete.on_cleanup && state.info.inode==1)
    fail(E::invalid_options,"initial delete-on-close is forbidden on root");
  if(kind==NativeOpenKind::file && state.info.directory)
    throw NativeFsError(NativeFsErrorCode::is_directory,"native open requires regular file");
  if(kind==NativeOpenKind::directory && !state.info.directory)
    throw NativeFsError(NativeFsErrorCode::not_directory,"native open requires directory");
  NativeFileHandle result;result.owner_=self.owner;result.token_=self.next_token;
  self.slots[available]={self.next_token,state.info.inode,access,false,false,false,basic_access.write,{},security_access.read,security_access.replace,
      overwrite_access.overwrite,overwrite_access.supersede,
      provisional?(awaiting_overwrite?Impl::Phase::prepared:Impl::Phase::opened):Impl::Phase::active,
      initial_delete.on_cleanup};
  ++self.next_token;return result;
}
NativeSessionCreateCommit NativeFileSession::create_open(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,
    NativeSessionCreateOptions options) {
  auto stored=create_impl(path,directory,basic,descriptor,options,false,{});
  return {std::move(stored.transaction),stored.state.info,std::move(stored.handle)};
}
NativeSessionCreateCommit NativeFileSession::create_prepared(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,NativePreparedCreateOptions options) {
  auto stored=create_impl(path,directory,basic,descriptor,
      {options.access,options.basic_access,options.security_access,false},true,options.initial_delete);
  return {std::move(stored.transaction),stored.state.info,std::move(stored.handle)};
}
NativeSessionStorageCreateCommit NativeFileSession::create_prepared_storage(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,
    std::optional<std::uint64_t> reservation,NativePreparedCreateOptions options) {
  return create_impl(path,directory,basic,descriptor,
      {options.access,options.basic_access,options.security_access,false},true,options.initial_delete,reservation);
}
NativeSessionStorageCreateCommit NativeFileSession::create_impl(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor,NativeSessionCreateOptions options,
    bool provisional,NativeInitialDeleteAccess initial_delete,std::optional<std::uint64_t> reservation) {
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionCreateCommit>);
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionStorageCreateCommit>);
  static_assert(std::is_nothrow_move_assignable_v<NativeFsCommit>);
  static_assert(std::is_nothrow_copy_assignable_v<NativeInodeCommitInfo>);
  static_assert(std::is_nothrow_copy_assignable_v<NativeFileHandle>);
  auto& self=ready();
  if(options.delete_on_close && !options.access.erase)
    fail(E::access_denied,"delete-on-close creation requires authorized delete access");
  std::size_t available=self.slots.size();
  for(std::size_t i=0;i<self.slots.size();++i)if(!self.slots[i].token){available=i;break;}
  if(available==self.slots.size() || self.next_token==(std::numeric_limits<std::uint64_t>::max)())
    fail(E::limit,"native file context budget exhausted");
  admit_path(self.writer,path);self.admit_destination(path);
  // The session is single-control-thread. The local slot/token reservation
  // cannot race another request; do not expose/install it before publication.
  NativeSessionStorageCreateCommit result;
  result.handle.owner_=self.owner;result.handle.token_=self.next_token;
  auto created=self.writer.create_inode_storage_result(path,directory,basic,descriptor,reservation,options.delete_on_close);
  // ONLY statically nonthrowing scalar/optional/weak-owner/move operations
  // remain after durable success. Never call open(), inspect(), or snapshot().
  result.transaction=std::move(created.transaction);result.state=created.state;
  self.slots[available]={self.next_token,result.state.info.inode,options.access,false,false,
      options.delete_on_close,options.basic_access.write,{},
      options.security_access.read,options.security_access.replace,false,false,
      provisional?Impl::Phase::opened:Impl::Phase::active,initial_delete.on_cleanup};
  ++self.next_token;
  return result;
}
NativePathLookup NativeFileSession::lookup(std::string_view path) const {
  auto& self = ready();
  admit_path(self.writer,path,true);
  auto result = self.writer.inspect_path_lookup(path);
  auto& info = result.state.info;
  info.delete_pending = self.visible_delete_pending(info.inode, info.unlinked, info.delete_pending);
  return result;
}
NativeInodeState NativeFileSession::info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();return self.state(self.slots[index]);
}
std::optional<NativeBasicMetadata> NativeFileSession::basic_metadata(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  (void)self.state(slot);return self.writer.inspect_basic_metadata(slot.inode);
}
NativeFsCommit NativeFileSession::set_basic_metadata(const NativeFileHandle& handle,
    const NativeBasicMetadata& value) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.basic_write)fail(E::access_denied,"native context has no basic metadata write access");
  (void)self.state(slot);return self.writer.set_basic_metadata(slot.inode,value);
}
std::optional<NativeSecurityDescriptor> NativeFileSession::security_descriptor(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.security_read)fail(E::access_denied,"native context has no descriptor read grant");
  (void)self.state(slot);return self.writer.inspect_security_descriptor(slot.inode);
}
std::optional<NativeSecurityDescriptor> NativeFileSession::security_descriptor_for_update(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.security_replace)fail(E::access_denied,"native context has no full descriptor replacement grant");
  (void)self.state(slot);return self.writer.inspect_security_descriptor(slot.inode);
}
NativeFsCommit NativeFileSession::set_security_descriptor(const NativeFileHandle& handle,std::span<const std::byte> value) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.security_replace)fail(E::access_denied,"native context has no full descriptor replacement grant");
  if(!native_security_descriptor_framed(value))
    throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid native security descriptor framing");
  (void)self.state(slot);return self.writer.set_security_descriptor(slot.inode,value);
}
std::vector<NativeFileInfo> NativeFileSession::list(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.read)fail(E::access_denied,"native context has no list access");
  (void)self.state(slot);return self.writer.inspect_directory(slot.inode);
}
NativeDirectoryRecordPage NativeFileSession::list_page(const NativeFileHandle& handle,
    std::string_view after,std::size_t maximum) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.read)fail(E::access_denied,"native context has no list access");
  (void)self.state(slot);
  auto page=self.writer.inspect_directory_page(slot.inode,after,maximum);
  for(auto& entry:page.entries) {
    auto& info=entry.state.info;
    info.delete_pending=self.visible_delete_pending(info.inode,info.unlinked,info.delete_pending);
  }
  return page;
}
std::vector<std::byte> NativeFileSession::read(const NativeFileHandle& handle,
    std::uint64_t offset,std::uint32_t bytes) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.read)fail(E::access_denied,"native context has no read access");
  return self.writer.read_inode(slot.inode,offset,bytes);
}
NativeInodeWriteCommit NativeFileSession::write(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode(slot.inode,offset,bytes,options);
}
NativeInodeWriteCommit NativeFileSession::write_with_metadata(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_with_metadata(slot.inode,offset,bytes,options,effect);
}
NativeFsCommit NativeFileSession::truncate_with_metadata(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_with_metadata(slot.inode,bytes,effect);
}
NativeInodeMutationCommit NativeFileSession::write_result(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_result(slot.inode,offset,bytes,options,effect);
}
NativeInodeMutationCommit NativeFileSession::truncate_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_result(slot.inode,bytes,effect);
}
NativeInodeAllocationInfo NativeFileSession::allocation_info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle,true);auto& self=ready();const auto& slot=self.slots[index];
  const auto state=self.state(slot);auto result=self.writer.inspect_allocation(slot.inode);
  result.info.delete_pending=state.delete_pending;return result;
}
NativeInodeAllocationInfo NativeFileSession::mutation_info(const NativeFileHandle& handle) const {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no data mutation access");
  const auto state=self.state(slot);
  if(state.info.directory)throw NativeFsError(NativeFsErrorCode::is_directory,"native data mutation requires a file");
  auto result=self.writer.inspect_allocation(slot.inode);result.info.delete_pending=state.delete_pending;return result;
}
NativeAllocationUsage NativeFileSession::allocation_usage() const {return ready().writer.inspect_allocation_usage();}
NativeInodeAllocationCommit NativeFileSession::reserve_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no reservation write access");
  return self.writer.reserve_inode_result(slot.inode,bytes,effect);
}
NativeFsCommit NativeFileSession::truncate(const NativeFileHandle& handle,std::uint64_t bytes) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode(slot.inode,bytes);
}
NativeInodeMutationCommit NativeFileSession::overwrite_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect) {
  auto stored=overwrite_impl(handle,effect,false);
  return {std::move(stored.transaction),stored.state.info,0,0};
}
NativeInodeMutationCommit NativeFileSession::overwrite_prepared_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect) {
  auto stored=overwrite_impl(handle,effect,true);
  return {std::move(stored.transaction),stored.state.info,0,0};
}
NativeInodeAllocationCommit NativeFileSession::overwrite_storage_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,std::optional<std::uint64_t> reservation) {
  return overwrite_impl(handle,effect,false,reservation);
}
NativeInodeAllocationCommit NativeFileSession::overwrite_prepared_storage_result(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,std::optional<std::uint64_t> reservation) {
  return overwrite_impl(handle,effect,true,reservation);
}
NativeInodeStorageMutationCommit NativeFileSession::write_storage_result(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,NativeInodeWriteOptions options,
    const NativeAutomaticMetadata& effect) {
  return write_storage_with_lz4_result(handle,offset,bytes,{},options,effect);
}
NativeInodeStorageMutationCommit NativeFileSession::write_storage_with_lz4_result(const NativeFileHandle& handle,
    std::uint64_t offset,std::span<const std::byte> bytes,std::span<const NativeLz4ExtentCandidate> candidates,
    NativeInodeWriteOptions options,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no write access");
  return self.writer.write_inode_storage_with_lz4(slot.inode,offset,bytes,candidates,options,effect);
}
NativeInodeStorageMutationCommit NativeFileSession::truncate_storage_result(const NativeFileHandle& handle,
    std::uint64_t bytes,const NativeAutomaticMetadata& effect) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.write)fail(E::access_denied,"native context has no truncate access");
  return self.writer.truncate_inode_storage_result(slot.inode,bytes,effect);
}
void NativeFileSession::adopt_prepared(const NativeFileHandle& handle) {
  const auto index=slot_for(handle,true);auto& slot=ready().slots[index];
  if(slot.phase!=Impl::Phase::overwritten && slot.phase!=Impl::Phase::opened)
    fail(E::invalid_options,"native prepared context is not ready for adoption");
  slot.phase=Impl::Phase::active;
}
NativeInodeAllocationCommit NativeFileSession::overwrite_impl(const NativeFileHandle& handle,
    const NativeOverwriteMetadata& effect,bool provisional,std::optional<std::uint64_t> reservation) {
  static_assert(std::is_nothrow_move_constructible_v<NativeInodeMutationCommit>);
  const auto index=slot_for(handle,provisional);auto& self=ready();auto& slot=self.slots[index];
  if(provisional && slot.phase!=Impl::Phase::prepared)
    fail(E::invalid_options,"native context is not awaiting overwrite");
  if(effect.replace_attributes?!slot.supersede:!slot.overwrite)
    fail(E::access_denied,"native context has no checked overwrite operation grant");
  if(slot.cleaned)fail(E::invalid_options,"cannot overwrite a cleaned native context");
  if(!native_overwrite_metadata_valid(effect))
    fail(E::invalid_options,"invalid canonical native overwrite metadata");
  const auto state=self.state(slot);
  if(state.delete_pending || state.unlinked)
    fail(E::delete_pending,"cannot overwrite deleting native inode");
  auto result=self.writer.overwrite_inode_storage_result(slot.inode,effect,reservation);
  // No I/O/allocation/fallible operation after publication. Waiting for
  // adoption permits a healthy cancellation Close without fabricated Cleanup.
  if(provisional)slot.phase=Impl::Phase::overwritten;
  return result;
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory) {
  auto& self=ready();admit_path(self.writer,path);self.admit_destination(path);NativeFileMutation op;
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  op.path=path;return self.writer.apply({&op,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& value) {
  auto& self=ready();admit_path(self.writer,path);self.admit_destination(path);
  NativeFileMutation op;op.path=std::string(path);
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  const NativeBasicMutation basic{0,1,value};
  return self.writer.apply_with_basic({&op,1},{&basic,1});
}
NativeFsCommit NativeFileSession::create_entry(std::string_view path,bool directory,
    const NativeBasicMetadata& basic,std::span<const std::byte> descriptor) {
  auto& self=ready();admit_path(self.writer,path);self.admit_destination(path);
  if(!native_security_descriptor_framed(descriptor))
    throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid native creation security descriptor");
  NativeFileMutation op;op.path=std::string(path);
  op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;
  const NativeBasicMutation metadata{0,1,basic};
  const NativeSecurityMutation security{0,1,NativeSecurityDescriptor(descriptor.begin(),descriptor.end())};
  return self.writer.apply_with_metadata({&op,1},{&metadata,1},{&security,1});
}
NativeFsCommit NativeFileSession::rename(const NativeFileHandle& handle,std::string_view target,bool replace) {
  const auto index=slot_for(handle);auto& self=ready();const auto& slot=self.slots[index];
  if(!slot.access.erase)fail(E::access_denied,"native context has no rename access");
  if(slot.cleaned)fail(E::invalid_options,"cannot rename cleaned native context");
  admit_path(self.writer,target);self.admit_destination(target);
  const auto source=self.state(slot);
  if(source.delete_pending || source.unlinked)fail(E::delete_pending,"cannot rename deleting native inode");
  try {
    const auto destination=self.writer.inspect_path(target);
    if(replace && destination.info.inode!=slot.inode &&
        (self.contexts(destination.info.inode) || self.volatile_initial(destination.info.inode)))
      fail(E::sharing_violation,"replacement inode still has owned file contexts");
  }catch(const NativeFsError& error){if(error.code()!=NativeFsErrorCode::not_found)throw;}
  NativeFileMutation op;op.operation=NativeFileOperation::rename;op.path=source.info.path;
  op.target=target;op.replace=replace;return self.writer.apply({&op,1});
}
void NativeFileSession::set_delete(const NativeFileHandle& handle,bool requested) {
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!slot.access.erase)fail(E::access_denied,"native context has no delete access");
  if(slot.cleaned)fail(E::invalid_options,"cannot change disposition after Cleanup");
  if(requested && self.provisional_inode(slot.inode))
    fail(E::sharing_violation,"native inode has an unresolved provisional open");
  const auto state=self.state(slot);
  if(state.unlinked)fail(E::delete_pending,"detached inode cannot be resurrected");
  const bool global=self.disposition_policy==NativeDispositionPolicy::windows_global;
  const auto desired=global?requested:(requested || self.explicit_intent(slot.inode,slot.token));
  self.writer.set_delete_pending(slot.inode,desired);
  // FSD SetDisposition(false) clears current initial pending state, but not
  // any descriptor's still-unprocessed initial option. Preserve independent
  // explicit requests/latches. No latch changes precede durable success.
  if(!requested) {
    if(global) {
      // Selected FSD clears the NODE's pending state, not just one open's
      // request. Preserve still-unprocessed initial per-FD options to re-arm
      // at their actual Cleanup. All updates follow durable success.
      for(auto& other:self.slots)if(other.token && other.inode==slot.inode)other.requested=false;
      self.clear_latch(slot.inode);
    }else self.cancel_initial_latch(slot.inode);
  }
  slot.requested=requested; // Only after durable success; assignment cannot throw.
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file) {
  cleanup(handle,delete_file,{});
}
void NativeFileSession::cleanup(const NativeFileHandle& handle,bool delete_file,
    const NativeCleanupMetadata& effect) {
  cleanup_event(handle,delete_file,{effect,false});
}
void NativeFileSession::cleanup_event(const NativeFileHandle& handle,bool delete_file,
    const NativeCleanupEvent& event) {
  const auto& effect=event.metadata;
  const auto index=slot_for(handle);auto& self=ready();auto& slot=self.slots[index];
  if(!native_cleanup_metadata_valid(effect))
    fail(E::invalid_options,"invalid automatic Cleanup metadata clock");
  if(slot.cleaned) {
    if(slot.cleanup_delete!=delete_file || slot.cleanup_trim!=event.trim_allocation ||
        !slot.cleanup_metadata.same_requests(effect))
      fail(E::invalid_options,"conflicting duplicate native Cleanup");
    return;
  }
  const bool initial=slot.initial_delete || self.volatile_initial(slot.inode);
  if((delete_file || initial) && self.provisional_inode(slot.inode))
    fail(E::sharing_violation,"cannot arm deletion with a provisional open");
  const auto state=self.state(slot);
  if(delete_file && !initial && !state.delete_pending && !state.unlinked) {
    self.terminal=true;fail(E::terminal,"Cleanup deletion has no authorized intent");
  }
  std::size_t latch=self.latched.size();
  for(std::size_t i=0;i<self.latched.size();++i)
    if(self.latched[i]==slot.inode){latch=i;break;}
  if(!delete_file && !state.unlinked && (slot.requested || initial) && latch==self.latched.size()) {
    for(std::size_t i=0;i<self.latched.size();++i)if(!self.latched[i]){latch=i;break;}
    if(latch==self.latched.size()){self.terminal=true;fail(E::terminal,"native delete latch invariant exhausted");}
  }
  if(initial) {
    // All latch capacity is reserved BEFORE publication. Initial intent is
    // armed at real Cleanup, never by Open/creation/adoption. Capture the
    // effective directory policy before commit; no post-commit query.
    auto committed=self.writer.cleanup_inode_storage_result(slot.inode,delete_file,true,event);
    if(!delete_file && !committed.state.info.unlinked) {
      self.latched[latch]=slot.inode;
      self.latch_flags[latch]|=Impl::kInitial;
      if(slot.requested)self.latch_flags[latch]|=Impl::kExplicit;
      if(!committed.state.info.delete_pending)self.latch_flags[latch]|=Impl::kVolatile;
      else self.latch_flags[latch]=static_cast<std::uint8_t>(self.latch_flags[latch]&~Impl::kVolatile);
    } else self.clear_latch(slot.inode);
  } else {
    if(effect.any() || event.trim_allocation)
      self.writer.cleanup_inode_storage_result(slot.inode,delete_file,false,event);
    else if(delete_file)self.writer.unlink_inode(slot.inode);
    if(delete_file)self.clear_latch(slot.inode);
    else if(latch!=self.latched.size()) {
      self.latched[latch]=slot.inode;
      if(slot.requested)self.latch_flags[latch]|=Impl::kExplicit;
    }
  }
  slot.cleanup_metadata=effect;slot.cleanup_trim=event.trim_allocation;
  slot.cleaned=true;slot.cleanup_delete=delete_file; // Paging stays owned until Close.
}
void NativeFileSession::fence() noexcept {
  if(impl_) {
    impl_->terminal=true;
    impl_->recompression.reset();
  }
}
void NativeFileSession::close(const NativeFileHandle& handle) {
  const auto index=slot_for(handle,true);auto& self=ready();auto& slot=self.slots[index];
  if(slot.phase==Impl::Phase::prepared || slot.phase==Impl::Phase::overwritten || slot.phase==Impl::Phase::opened) {
    // Preparation owns no persisted delete intent, and peer deletion was
    // excluded while provisional. Release ONLY this healthy context; do not
    // query storage, synthesize Cleanup, undo overwrite or remove its inode.
    if(slot.cleaned || slot.requested || slot.cleanup_delete || self.intent(slot.inode,slot.token) ||
        self.volatile_initial(slot.inode)) {
      self.terminal=true;fail(E::terminal,"provisional native context lifetime diverged");
    }
    slot=Impl::Slot{};return;
  }
  if(slot.phase!=Impl::Phase::active) {
    self.terminal=true;fail(E::terminal,"invalid native open phase");
  }
  if(!slot.cleaned)fail(E::invalid_options,"native Close requires prior Cleanup");
  const auto state=self.state(slot);
  const auto last=self.contexts(slot.inode,slot.token)==0;
  if(last && state.delete_pending && !state.unlinked) {
    self.terminal=true;fail(E::terminal,"last Close arrived without required deletion Cleanup");
  }
  if(last && state.unlinked)self.writer.retire_inode(slot.inode);
  // All fallible work is complete. Never release a context before retirement
  // succeeds, or allocate/throw after a durable retirement.
  if(last)self.clear_latch(slot.inode);
  slot=Impl::Slot{};
}
NativeFsCommit NativeFileSession::flush() {
  return ready().writer.flush();
}
NativeFsCommit NativeFileSession::flush(const NativeFileHandle& handle) {
  const auto index=slot_for(handle);auto& self=ready();
  (void)self.state(self.slots[index]);
  return self.writer.flush();
}
bool NativeFileSession::retirement_pending() const noexcept {
  return !poisoned() && impl_->writer.retirement_pending();
}
bool NativeFileSession::reclaim_retired_inode() {
  return ready().writer.reclaim_retired_inode();
}
bool NativeFileSession::directory_backfill_pending() const noexcept {
  return !poisoned() && impl_->writer.directory_backfill_pending();
}
NativeDirectoryBackfillStep NativeFileSession::advance_directory_backfill() {
  return ready().writer.advance_directory_backfill();
}
void NativeFileSession::cancel_directory_backfill() noexcept {
  if(impl_)impl_->writer.cancel_directory_backfill();
}
NativeFilesystemSnapshot NativeFileSession::snapshot() const {
  return ready().writer.snapshot();
}
NativeSessionStats NativeFileSession::stats() const noexcept {
  NativeSessionStats result;
  if(!impl_)return result;
  result.fenced=impl_->terminal || impl_->writer.poisoned();
  for(const auto& slot:impl_->slots)if(slot.token) {
    ++result.contexts;if(slot.cleaned)++result.cleaned;if(slot.requested)++result.delete_requests;
  }
  for(const auto id:impl_->latched)if(id)++result.latched_inodes;
  return result;
}
std::uint64_t NativeFileSession::namespace_sequence() const noexcept {
  return poisoned()?0:impl_->writer.maintenance_sequence();
}
bool NativeFileSession::poisoned() const noexcept{return !impl_ || impl_->terminal || impl_->writer.poisoned();}
}
