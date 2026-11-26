#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/indexed_coherence.hpp"
#include "dense_volume_backing.hpp"
#include "store_file.hpp"
#include "native_volume_file_reclaimer.hpp"
#include "protected_stripe_image.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <utility>

namespace flitzfs {
namespace {
constexpr auto image_offset=kNativeVolumeFileImageOffset;
constexpr auto maximum_file_bytes=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
constexpr std::uint64_t retirement_quantum=4*1024*1024;
static_assert(image_offset%kStoreCommitUnit==0);
using Error=NativeFsErrorCode;
[[noreturn]] void fail(Error code,const char* message){throw NativeFsError(code,message);}
void check_image_capacity(std::uint64_t bytes,Error code) {
  if(bytes<kVolumeHeadBytes || bytes>maximum_file_bytes-image_offset)
    fail(code,"invalid indexed volume-file capacity");
}
struct PairedFileGeometry {
  std::uint64_t images_end,catalog_capacity,file_end;
};
PairedFileGeometry paired_file_geometry(std::uint64_t bytes,bool catalog,Error code) {
  if(bytes<kVolumeHeadBytes || bytes%kStoreCommitUnit || bytes>(maximum_file_bytes-image_offset)/2)
    fail(code,"invalid paired image range capacity");
  const auto images_end=image_offset+2*bytes;
  // Capability 5 derives these offsets from the image capacity. Its
  // divisor is independent of the tunable advisory write budget.
  constexpr std::uint64_t catalog_capacity_divisor=32;
  const auto available=(std::min)(bytes/catalog_capacity_divisor,(maximum_file_bytes-images_end)/2);
  const auto aligned=available-available%kStoreCommitUnit;
  // Two root slots, one initial and one incremental allocation. Each
  // allocation includes a payload unit and a pair of metadata units.
  constexpr auto minimum=8*kStoreCommitUnit;
  const auto capacity=catalog && aligned>=minimum?aligned:0;
  return {images_end,capacity,images_end+2*capacity};
}
std::uint64_t image_start(const NativeVolumeSelection& selected) noexcept {
  return image_offset+((selected.epoch%2==0)?selected.storage.maximum_physical_bytes:0);
}
}

std::optional<PagedStoreLimits> NativeVolumeFile::paired_image_limits(std::uint64_t backing_bytes,bool packed) {
  backing_bytes=(std::min)(backing_bytes,maximum_file_bytes);
  const auto usable=packed?detail::DenseVolumeBacking::payload_capacity(backing_bytes):backing_bytes;
  constexpr auto minimum=(kVolumeHeadBytes+2*kStoreCommitUnit)/kStoreCommitUnit;
  if(usable<image_offset+2*minimum*kStoreCommitUnit)return std::nullopt;
  auto low=minimum,high=(usable-image_offset)/(2*kStoreCommitUnit);
  // Catalog slots are derived from the image capacity. Use the same layout
  // as creation and reopen so reserve rounding cannot overstate capacity.
  while(low<high) {
    const auto middle=low+(high-low+1)/2;
    if(paired_file_geometry(middle*kStoreCommitUnit,true,Error::invalid_operation).file_end<=usable)
      low=middle;
    else
      high=middle-1;
  }
  return PagedStoreLimits{low*kStoreCommitUnit};
}

struct NativeVolumeFile::Owner {
  const std::shared_ptr<StoreByteIo> physical;
  std::shared_ptr<StoreByteIo> backing;
  const bool writable;
  NativeVolumeRangeIo* ranges=nullptr;
  detail::DenseVolumeBacking* dense_file=nullptr;
  std::atomic<std::weak_ptr<detail::NativeVolumeRangeWake>> reclamation_wake;
  bool leased=false;

  explicit Owner(std::shared_ptr<StoreByteIo> file,bool write):physical(std::move(file)),
      backing(physical),writable(write) {
    if(!physical)fail(Error::invalid_operation,"missing volume-file backing");
    if(writable) {
      if(!valid_store_io_contract(physical->contract()) || !physical->acquire_writer())
        fail(Error::invalid_operation,"volume file requires an exclusive qualified writer");
      leased=true;
      install_ranges();
    }
  }
  void install_ranges() noexcept {
    auto* candidate=dynamic_cast<NativeVolumeRangeIo*>(backing.get());
    ranges=writable && candidate && backing->contract().isolated_range_reclamation &&
        candidate->supports_reclaimable_ranges()?candidate:nullptr;
  }
  void install_dense(std::shared_ptr<detail::DenseVolumeBacking> dense) {
    dense_file=dense.get();
    backing=std::move(dense);
    install_ranges();
  }
  void pin_reader() {
    if(!leased) {
      leased=physical->acquire_exclusive_use();
      if(!leased)fail(Error::invalid_operation,"paired volume inspection requires a retired backing owner");
    }
  }
  ~Owner(){if(leased)physical->release_writer();}
};

struct NativeVolumeFile::Region final:StoreByteIo {
  const std::shared_ptr<Owner> owner;
  const std::uint64_t offset,capacity;
  const bool tail;
  std::atomic<std::uint64_t> fixed_extent;
  const std::shared_ptr<StoreByteIo> image_pin;

  Region(std::shared_ptr<Owner> file,std::uint64_t base,std::uint64_t limit,
      std::uint64_t initialized,bool follows_end=false,std::shared_ptr<StoreByteIo> pin={})
      :owner(std::move(file)),offset(base),capacity(limit),tail(follows_end),fixed_extent(initialized),
       image_pin(std::move(pin)){}
  static std::shared_ptr<Region> slice(const std::shared_ptr<Region>& range,std::uint64_t at,
      std::uint64_t limit,std::uint64_t extent,std::shared_ptr<StoreByteIo> pin={}) {
    if(at>range->capacity || !limit || limit>range->capacity-at || extent>limit)
      fail(Error::corrupt,"volume image component exceeds its owned range");
    return std::make_shared<Region>(range->owner,range->offset+at,limit,extent,false,std::move(pin));
  }
  // All copies of a selected or staged region share this pin. The reclaimer
  // retains the underlying Region separately after a selector switch.
  static std::shared_ptr<StoreByteIo> pin_image(const std::shared_ptr<Region>& region) {
    struct Pin {
      explicit Pin(const std::shared_ptr<Region>& image):image(image){}
      ~Pin() {
        const auto file=image->owner;
        image.reset();
        if(const auto wake=file->reclamation_wake.load(std::memory_order_acquire).lock())wake->signal();
      }
      std::shared_ptr<Region> image;
    };
    return {std::make_shared<Pin>(region),region.get()};
  }
  std::uint64_t extent() const override {
    if(!tail)return fixed_extent.load(std::memory_order_acquire);
    const auto end=owner->backing->extent();
    if(end>offset+capacity)fail(Error::corrupt,"volume file exceeds its image extent limit");
    return end<=offset?0:end-offset;
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    const auto end=extent();
    if(at>end || out.size()>end-at)fail(Error::corrupt,"volume-file read exceeds its region");
    if(!out.empty())owner->backing->read_exact(offset+at,out);
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    if(at>capacity || bytes.size()>capacity-at)fail(Error::limit,"volume-file write exceeds its region");
    if(bytes.empty())return;
    owner->backing->write_exact(offset+at,bytes);
    if(!tail)fixed_extent.store((std::max)(fixed_extent.load(std::memory_order_relaxed),
        at+static_cast<std::uint64_t>(bytes.size())),std::memory_order_release);
  }
  void admit_write(std::uint64_t projected) override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    const auto current=extent();
    if(projected<current || projected>capacity)fail(Error::limit,"volume-file admission exceeds its region");
    const auto end=(std::max)(owner->backing->extent(),offset+projected);
    if(owner->ranges)owner->ranges->admit_contiguous_growth(end,offset+current,offset+projected);
    else owner->backing->admit_write(end);
  }
  void flush() override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    owner->backing->flush();
  }
  StoreIoContract contract() const noexcept override {return owner->backing->contract();}
  bool concurrent_reads_supported() const noexcept override {
    return owner->backing->concurrent_reads_supported();
  }
  std::uint64_t allocation_extent() const {
    const auto end=owner->ranges->allocated_extent(offset,capacity);
    if(end>capacity || end%kStoreCommitUnit)fail(Error::storage_failure,"invalid volume allocation bound");
    return end;
  }
  void reclaim_tail(bool inactive_image=false) {
    const auto before=allocation_extent();
    if(!before){fixed_extent.store(0,std::memory_order_release);return;}
    auto start=(before-1)/retirement_quantum*retirement_quantum;
    // Deleting a contiguous mapping touches one packed directory even if
    // the run owns many units. Leave sparse, scratch and advice work bounded
    // by the ordinary byte quantum.
    if(inactive_image && owner->dense_file)
      start=(std::min)(start,owner->dense_file->trailing_run_start(offset,offset+before)-offset);
    owner->ranges->reclaim_range(offset+start,before-start);
    flush();
    const auto after=allocation_extent();
    fixed_extent.store(after,std::memory_order_release);
    if(after>=before)fail(Error::storage_failure,"volume range allocation did not decrease");
  }
 protected:
  bool allows_writer() const noexcept override {return owner->writable;}
};

struct NativeVolumeFile::CatalogRanges {
  struct Slot {
    std::shared_ptr<Region> range;
    bool needs_review=false;
  };
  const std::shared_ptr<StoreByteIo> advice;
  const StoreIdentity volume;
  std::array<Slot,2> slots;
  std::uint64_t retirement_failure_sequence=0;

  CatalogRanges(const std::shared_ptr<Owner>& owner,std::shared_ptr<StoreByteIo> area,StoreIdentity volume_id,
      PairedFileGeometry geometry,std::uint64_t visible_end):advice(std::move(area)),volume(volume_id) {
    for(std::size_t i=0;i<slots.size();++i) {
      const auto offset=geometry.images_end+i*geometry.catalog_capacity;
      slots[i]={std::make_shared<Region>(owner,offset,geometry.catalog_capacity,0),visible_end>offset};
    }
  }
  static std::size_t slot_of(StoreIdentity identity) noexcept {
    return std::to_integer<unsigned>(identity.back())&1u;
  }
  std::optional<std::size_t> available(const std::optional<OptimizationCatalogSelection>& selected,
      bool needs_review) const noexcept {
    for(std::size_t i=0;i<slots.size();++i)
      if((!selected || slot_of(selected->backing)!=i) &&
          slots[i].needs_review==needs_review && slots[i].range.use_count()==1)return i;
    return {};
  }
  OptimizationSnapshot authority() const {return read_optimization_state(*advice,volume);}
  static bool paired(const OptimizationSnapshot& state) noexcept {
    return state.code==OptimizationReadCode::available && !state.degraded && state.state.settled_input_bytes;
  }
  IndexedCoherenceStorage::Reservation reserve() {
    const auto state=authority();
    if(!paired(state) || state.sequence>(std::numeric_limits<std::uint64_t>::max)()-2)return {};
    const auto selected=state.state.catalog_selection;
    const auto open=available(selected,false);
    if(!open)return {};
    // The first catalog write follows a paid journal publication. A
    // materialized ID therefore cannot be reused at the same sequence.
    auto identity=volume;
    for(unsigned i=0;i<8;++i)identity[i]=static_cast<std::byte>(state.sequence>>(i*8));
    identity.back()=(identity.back()&std::byte{0xfe})|static_cast<std::byte>(*open);
    if(identity==volume || (selected && identity==selected->image))identity[8]^=std::byte{0x80};
    auto& slot=slots[*open];
    auto pin=Region::pin_image(slot.range);
    slot.needs_review=true;
    return {identity,std::move(pin)};
  }
  std::shared_ptr<StoreByteIo> resolve(StoreIdentity identity) {
    auto& slot=slots[slot_of(identity)];
    if(slot.range.use_count()!=1)return {};
    slot.range->fixed_extent.store(slot.range->allocation_extent(),std::memory_order_release);
    return Region::pin_image(slot.range);
  }
  bool pending(std::uint64_t sequence,const std::optional<OptimizationCatalogSelection>& selected) const noexcept {
    return sequence && sequence!=retirement_failure_sequence && available(selected,true).has_value();
  }
  NativeVolumeReclaimProgress retire(std::uint64_t sequence,const std::optional<OptimizationCatalogSelection>& selected) {
    const auto inactive=available(selected,true);
    if(!inactive)return NativeVolumeReclaimProgress::idle;
    try {
      const auto state=authority();
      if(!paired(state) || state.sequence!=sequence || state.state.catalog_selection!=selected)
        fail(Error::corrupt,"unconfirmed catalog selection prevents range reclamation");
      auto& slot=slots[*inactive];
      slot.range->reclaim_tail();
      slot.needs_review=slot.range->extent()!=0;
      return NativeVolumeReclaimProgress::retiring;
    }catch(const std::exception&) {
      retirement_failure_sequence=sequence;
      return NativeVolumeReclaimProgress::failed;
    }
  }
};

namespace detail {
using Progress=NativeVolumeReclaimProgress;
NativeVolumeFileReclaimer::NativeVolumeFileReclaimer(std::shared_ptr<StoreByteIo> head,
    std::shared_ptr<NativeVolumeFile::Region> active,std::shared_ptr<NativeVolumeFile::Region> spare,
    bool recover_spare,std::shared_ptr<NativeVolumeFile::CatalogRanges> catalog):
    head_(std::move(head)),active_(std::move(active)),spare_(std::move(spare)),catalog_(std::move(catalog)),
    phase_(recover_spare?Phase::retiring:Phase::idle) {}

bool NativeVolumeFileReclaimer::pending(const NativeFileSession& session,NativeVolumeReclaimMode mode,
    std::uint64_t private_workspace_bytes) const {
  if(mode==NativeVolumeReclaimMode::shorten_packed_tail)return dense_work_pending(session,mode);
  if(image_work_pending(session,mode==NativeVolumeReclaimMode::include_cold,private_workspace_bytes) ||
      dense_work_pending(session,mode))return true;
  if(!catalog_)return false;
  std::optional<OptimizationCatalogSelection> selected;
  const auto sequence=session.catalog_retirement_selection(selected);
  return catalog_->pending(sequence,selected);
}
std::optional<NativeDenseFileSpace> NativeVolumeFileReclaimer::dense_file_space() const {
  const auto* dense=active_->owner->dense_file;
  return dense?std::optional{dense->file_space()}:std::nullopt;
}
bool NativeVolumeFileReclaimer::dense_work_pending(const NativeFileSession& session,NativeVolumeReclaimMode mode) const {
  const auto* dense=active_->owner->dense_file;
  if(mode==NativeVolumeReclaimMode::logical_only || mode==NativeVolumeReclaimMode::pack_short_inodes ||
      !dense || phase_!=Phase::idle || !session.idle_compaction_context_ready())return false;
  if(mode==NativeVolumeReclaimMode::shorten_packed_tail)
    return dense->tail_shorten_pending(retirement_quantum) && !image_work_pending(session,false);
  return dense->tail_work_pending(retirement_quantum);
}
bool NativeVolumeFileReclaimer::image_work_pending(const NativeFileSession& session,bool allow_cold,
    std::uint64_t private_workspace_bytes) const {
  if(phase_==Phase::retiring)
    return scratch_ || spare_.use_count()==1 ||
        active_->owner->reclamation_wake.load(std::memory_order_acquire).expired();
  if(phase_==Phase::uncertain || phase_==Phase::failed)return false;
  if(seal_)return true;
  if(!session.idle_compaction_context_ready())return false;
  if(phase_!=Phase::idle)return true;
  if(const auto repair=session.indexed_capacity_repair_generation())return repair!=attempted_repair_;
  const auto space=session.indexed_volume_space();
  const auto obsolete=space.obsolete_bytes;
  return (obsolete>attempted_obsolete_ && obsolete-attempted_obsolete_>=abandoned_bytes_ &&
      space.reclamation_worthwhile()) ||
      (allow_cold && (session.idle_cold_compaction_pending() || funded_seal_plans(
          session,private_workspace_bytes).available()));
}
bool NativeVolumeFileReclaimer::seal_preparation_pending(const NativeFileSession& session) const noexcept {
  return seal_ && seal_->source_current(session) && seal_->can_continue_preparation();
}
std::uint64_t NativeVolumeFileReclaimer::projected_seal_writes(
    const NativeVolumeSelection& selected,const NativeFilesystemWriter& writer) const noexcept {
  if(!cold_target_available())return 0;
  const auto plan=seal_plan(selected,writer.packed_folded_end_bound(spare_->contract().write_failure_unit_bytes));
  return plan?plan->writes:0;
}
std::optional<NativeVolumeFileReclaimer::SealPlan> NativeVolumeFileReclaimer::seal_plan(
    const NativeVolumeSelection& selected,std::uint64_t packed_end_bound,std::uint64_t private_workspace_bytes) const noexcept {
  try {
    if(selected.sealed_range)return {};
    const auto contract=spare_->contract();
    const auto allowance=private_workspace_bytes/kStoreCommitUnit*kStoreCommitUnit;
    if(private_workspace_bytes && !allowance)return {};
    const auto fitted=NativeHybridBacking::workspace_capacity(
        selected.volume_identity,contract,packed_end_bound,spare_->capacity,allowance);
    if(fitted<=kVolumeHeadBytes)return {};
    // Reserve the whole unit that contains the last fitted workspace byte.
    const auto workspace=(1+(fitted-1)/kStoreCommitUnit)*kStoreCommitUnit;
    constexpr auto root_unit=static_cast<std::uint32_t>(detail::kPagedStoreCheckpointWriteBytes/2);
    const auto tail_prefix=4ULL*(std::max)(root_unit,contract.write_failure_unit_bytes);
    const auto base=ProtectedStripeImage::projected_occupied(selected.volume_identity,workspace,
        contract.write_failure_unit_bytes,spare_->capacity,ProtectedStripeFormat::compact_roots);
    const auto staging=workspace+base+tail_prefix;
    const auto backing=allowance?base+tail_prefix:staging;
    if(backing>spare_->capacity)return {};
    // Include overwritten roots and binding records in the selected charge.
    return SealPlan{workspace,staging,backing,backing+kVolumeHeadBytes};
  }catch(const std::exception&){return {};}
}
NativeVolumeFileReclaimer::SealPlans NativeVolumeFileReclaimer::funded_seal_plans(
    const NativeFileSession& session,std::uint64_t private_workspace_bytes) const {
  SealPlans plans;
  if(!cold_target_available())return plans;
  const auto selection=session.volume_info().selection;
  const auto bound=session.packed_folded_end_bound(spare_->contract().write_failure_unit_bytes);
  plans.file=seal_plan(selection,bound);
  if(plans.file && !session.idle_seal_funded(plans.file->writes))plans.file.reset();
  if(private_workspace_bytes) {
    plans.private_memory=seal_plan(selection,bound,private_workspace_bytes);
    if(plans.private_memory && (!session.idle_seal_funded(plans.private_memory->writes) ||
        (plans.file && plans.private_memory->workspace<plans.file->workspace)))
      plans.private_memory.reset();
  }
  return plans;
}
void NativeVolumeFileReclaimer::bind_wake(const std::shared_ptr<NativeVolumeRangeWake>& wake) noexcept {
  active_->owner->reclamation_wake.store(wake,std::memory_order_release);
}

void NativeVolumeFileReclaimer::check_inactive(const NativeVolumeHeadInfo& current) const {
  // Revalidate both replicas immediately before changing any allocation.
  const auto durable=NativeVolumeHeadView::inspect_selector(*head_,current.selection.volume_identity);
  if(current.degraded || durable.degraded || durable.selection!=current.selection ||
      !paired_volume_file(durable.selection.placement) ||
      image_start(durable.selection)!=active_->offset || active_->offset==spare_->offset)
    fail(Error::corrupt,"unconfirmed volume selection prevents range reclamation");
}
Progress NativeVolumeFileReclaimer::retire(const NativeVolumeHeadInfo& current) {
  if(spare_.use_count()!=1)return Progress::pinned;
  check_inactive(current);
  auto& range=*spare_;
  range.reclaim_tail(true);
  if(!range.extent())phase_=Phase::idle;
  return Progress::retiring;
}

NativeStartupRepairImage NativeVolumeFileReclaimer::startup_repair_image(const NativeVolumeHeadInfo& authority,
    const std::function<StoreIdentity()>& identity) {
  // The closed repair owner has verified every current byte and fenced both
  // selector peers. Any incomplete older candidate is outside that selection.
  if(authority.degraded || image_start(authority.selection)!=active_->offset || spare_.use_count()!=1)
    fail(Error::invalid_operation,"startup repair spare range is unavailable");
  while(spare_->allocation_extent()) {
    if(retire(authority)==Progress::pinned)
      fail(Error::invalid_operation,"startup repair spare range is pinned");
  }
  return {NativeVolumeFile::Region::pin_image(spare_),identity()};
}
void NativeVolumeFileReclaimer::adopt_startup_repair(const NativeVolumeSelection& original,
    const NativeVolumeSelection& repaired) {
  if(repaired==original)return;
  if(repaired.epoch!=original.epoch+1 || repaired.parent_identity!=original.image_identity ||
      !paired_volume_file(repaired.placement) || repaired.placement!=original.placement ||
      image_start(repaired)!=spare_->offset)
    fail(Error::storage_failure,"startup repair selected an unexpected range");
  adopt_selection();
}
void NativeVolumeFileReclaimer::settle_recovery(std::uint64_t previous_epoch,const NativeVolumeSelection& selected) {
  if(phase_!=Phase::uncertain || !paired_volume_file(selected.placement))
    fail(Error::storage_failure,"recovered selector is outside the owned range transition");
  const auto offset=image_start(selected);
  if(selected.epoch==previous_epoch && offset==active_->offset) {
    phase_=Phase::retiring;
    return;
  }
  if(selected.epoch>previous_epoch && selected.epoch-previous_epoch==1 && offset==spare_->offset) {
    adopt_selection();
    return;
  }
  fail(Error::storage_failure,"recovered selector chose an unexpected range");
}
void NativeVolumeFileReclaimer::adopt_selection() noexcept {
  active_.swap(spare_);
  attempted_obsolete_=0;
  abandoned_bytes_=0;
  attempted_repair_=0;
  packed_copy_attempted_=false;
  phase_=Phase::retiring;
}
void NativeVolumeFileReclaimer::discard_seal() noexcept {
  seal_.reset();
  workspace_.reset();
  base_bytes_=0;
}
void NativeVolumeFileReclaimer::retire_scratch() {
  try {
    if(!scratch_->retire_step())return;
  }catch(...) {
    scratch_.reset();
    scratch_bytes_=0;
    throw;
  }
  scratch_.reset();
  scratch_bytes_=0;
}
bool NativeVolumeFileReclaimer::shed_scratch(std::uint64_t retained_bytes) noexcept {
  if(!scratch_ || (phase_!=Phase::retiring && scratch_bytes_<=retained_bytes))return false;
  if(seal_ && seal_->phase()!=NativeHybridBacking::BuildPhase::awaiting_tail) {
    discard_seal();
    phase_=Phase::retiring;
  }
  try{retire_scratch();}
  catch(...) {
    discard_seal();
    phase_=Phase::retiring;
  }
  return true;
}
Progress NativeVolumeFileReclaimer::begin_seal(NativeFileSession& session,const std::function<StoreIdentity()>& next_identity,
    const SealPlans& plans,NativeSealScratchOffer offer,StoreCompressionOptions compression) {
  const auto current=session.volume_info();
  check_inactive(current);
  const auto destination=spare_->contract();
  auto scratch=plans.private_memory?offer.create(plans.private_memory->workspace,destination):nullptr;
  if(scratch) {
    constexpr auto root_unit=static_cast<std::uint32_t>(detail::kPagedStoreCheckpointWriteBytes/2);
    const auto& store=scratch->store();
    const auto contract=store?store->contract():StoreIoContract{};
    if(!valid_store_io_contract(contract) || (std::max)(root_unit,contract.write_failure_unit_bytes)!=
        (std::max)(root_unit,destination.write_failure_unit_bytes))scratch.reset();
  }
  const auto& plan=scratch?plans.private_memory:plans.file;
  if(!plan)return Progress::idle;
  const auto identity=next_identity();
  const auto& file=*spare_->owner;
  const auto end=scratch?plan->backing:spare_->capacity;
  file.ranges->admit_range_write((std::max)(file.backing->extent(),spare_->offset+end),plan->backing);
  if(!session.reserve_idle_seal(plan->writes))return Progress::idle;
  phase_=Phase::copying;
  if(scratch) {
    scratch_=std::move(scratch);
    scratch_bytes_=plan->workspace;
  }else workspace_=NativeVolumeFile::Region::slice(spare_,spare_->capacity-plan->workspace,plan->workspace,0);
  seal_.emplace(NativeHybridBacking::prepare_impl(session,scratch_?scratch_->store():workspace_,destination,identity,
      spare_->capacity,plan->staging,compression,true,NativeHybridControls::compact));
  return Progress::copying;
}
Progress NativeVolumeFileReclaimer::advance_seal(NativeFileSession& session) {
  using Stage=NativeHybridBacking::BuildPhase;
  using Region=NativeVolumeFile::Region;
  if(!seal_->source_current(session) || session.indexed_capacity_repair_generation()) {
    discard_seal();
    phase_=Phase::retiring;
    return Progress::retiring;
  }
  const auto stage=seal_->phase();
  if(stage==Stage::awaiting_base) {
    base_bytes_=seal_->required_store_bytes();
    if(workspace_ && base_bytes_>workspace_->offset-spare_->offset)
      fail(Error::storage_failure,"sealed base overlaps its private workspace");
    seal_->provide_store(session,Region::slice(spare_,0,base_bytes_,0,Region::pin_image(spare_)));
    return Progress::copying;
  }
  if(stage==Stage::awaiting_tail) {
    if(scratch_) {
      if(scratch_->store().use_count()!=1)return Progress::pinned;
      retire_scratch();
      return Progress::copying;
    }
    if(workspace_) {
      if(workspace_.use_count()!=1)return Progress::pinned;
      check_inactive(session.volume_info());
      workspace_->reclaim_tail();
      if(!workspace_->extent())workspace_.reset();
      return Progress::copying;
    }
    seal_->provide_store(session,Region::slice(spare_,base_bytes_,seal_->required_store_bytes(),0,
        Region::pin_image(spare_)));
    return Progress::copying;
  }
  if(stage!=Stage::ready) {
    if(!seal_->advance(session))return Progress::copying;
    phase_=Phase::ready;
    return Progress::ready;
  }
  phase_=Phase::uncertain;
  std::shared_ptr<NativeHybridBacking> replacement;
  const bool published=seal_->publish(session,replacement);
  discard_seal();
  if(!published) {
    phase_=Phase::retiring;
    return Progress::retiring;
  }
  adopt_selection();
  return Progress::published;
}

Progress NativeVolumeFileReclaimer::advance_dense(const NativeVolumeHeadInfo& current) {
  try {
    check_inactive(current);
    return active_->owner->dense_file->advance_tail(retirement_quantum)?Progress::retiring:Progress::idle;
  }catch(const std::exception&){phase_=Phase::failed;return Progress::failed;}
}

Progress NativeVolumeFileReclaimer::advance(NativeFileSession& session,const std::function<StoreIdentity()>& identity,
    NativeVolumeReclaimMode mode,NativeSealScratchOffer scratch) {
  if(mode==NativeVolumeReclaimMode::shorten_packed_tail)
    return dense_work_pending(session,mode)?advance_dense(session.volume_info()):Progress::idle;
  const auto allow_cold=mode==NativeVolumeReclaimMode::include_cold;
  const auto workspace=scratch.create?scratch.bytes:0;
  const bool image_pending=image_work_pending(session,allow_cold,workspace);
  if(catalog_ && (!image_pending || (phase_==Phase::idle && !identity) ||
      (phase_==Phase::retiring && spare_.use_count()!=1))) {
    std::optional<OptimizationCatalogSelection> selected;
    const auto sequence=session.catalog_retirement_selection(selected);
    if(catalog_->pending(sequence,selected))return catalog_->retire(sequence,selected);
  }
  if(phase_==Phase::retiring) {
    try {
      if(scratch_) {
        retire_scratch();
        return Progress::retiring;
      }
      return retire(session.volume_info());
    }
    catch(const std::exception&){phase_=Phase::failed;return Progress::failed;}
  }
  if(dense_work_pending(session,mode) && !image_work_pending(session,false))
    return advance_dense(session.volume_info());
  if(phase_==Phase::idle && !identity)return Progress::idle;
  if(!image_pending)return Progress::idle;
  try {
    if(seal_)return advance_seal(session);
    if(phase_==Phase::idle) {
      const bool reclaim=image_work_pending(session,false);
      if(reclaim) {
        attempted_repair_=session.indexed_capacity_repair_generation();
        attempted_obsolete_=session.indexed_volume_space().obsolete_bytes;
        abandoned_bytes_=0;
      }
      if(spare_.use_count()!=1 || spare_->allocation_extent())
        fail(Error::invalid_operation,"inactive volume range is unavailable");
      if(!reclaim && allow_cold) {
        const auto cold_pending=session.idle_cold_compaction_pending();
        const auto foreground=session.managed_foreground();
        const bool fuse=foreground && foreground->compression.experimental_cold_byte_planes;
        if(fuse || !cold_pending) {
          const auto plans=funded_seal_plans(session,workspace);
          const auto sealed=begin_seal(session,identity,plans,scratch,
              fuse?foreground->compression:StoreCompressionOptions{});
          if(sealed!=Progress::idle || !cold_pending)return sealed;
        }
      }
      auto rewrite=reclaim?PagedRewriteMode::reconcile_foreground:PagedRewriteMode::cold_compression;
      if(reclaim && mode==NativeVolumeReclaimMode::pack_short_inodes && !packed_copy_attempted_ &&
          !attempted_repair_ && !session.volume_info().selection.sealed_range) {
        const auto foreground=session.managed_foreground();
        if(foreground && foreground->data_layout==NativeIndexedDataLayout::inline_small_files) {
          // Packing binds an exact checkpoint. Use ordinary reconciliation
          // after the first interrupted or refused attempt on this image.
          packed_copy_attempted_=true;
          rewrite=PagedRewriteMode::pack_short_inodes;
        }
      }
      if(!session.begin_idle_compaction_impl(NativeVolumeFile::Region::pin_image(spare_),identity(),true,rewrite))
        return Progress::idle;
      phase_=Phase::copying;
      return Progress::copying;
    }
    if(phase_==Phase::copying) {
      switch(session.advance_recompression()) {
        case NativeFileSession::RecompressionProgress::copying:return Progress::copying;
        case NativeFileSession::RecompressionProgress::ready:phase_=Phase::ready;return Progress::ready;
        case NativeFileSession::RecompressionProgress::declined:
          phase_=Phase::idle;return Progress::idle; // The screen wrote no image bytes.
        case NativeFileSession::RecompressionProgress::stale:
          abandoned_bytes_=spare_->extent();phase_=Phase::retiring;return Progress::retiring;
      }
    }
    if(session.idle_compaction_preparation_pending()) {
      phase_=Phase::copying;
      return Progress::copying;
    }
    phase_=Phase::uncertain;
    if(!session.publish_staged_recompression(true,true)) {
      abandoned_bytes_=spare_->extent();phase_=Phase::retiring;return Progress::retiring;
    }
    adopt_selection();
    return Progress::published;
  }catch(const std::exception&) {
    if(session.poisoned()){phase_=Phase::uncertain;throw;}
    abandoned_bytes_=spare_->extent();
    abandon(session);
    return Progress::failed;
  }
}

void NativeVolumeFileReclaimer::abandon(NativeFileSession& session) noexcept {
  session.cancel_recompression();
  discard_seal();
  if(phase_==Phase::copying || phase_==Phase::ready || (phase_==Phase::uncertain && !session.poisoned()))
    phase_=Phase::retiring;
}
void NativeVolumeFileReclaimer::cancel(NativeFileSession& session) noexcept {
  abandon(session);
  scratch_.reset();
  scratch_bytes_=0;
}
} // namespace detail

NativeVolumeImageResolver NativeVolumeFile::Regions::resolver() const {
  return [retained=image,expected=selection.image_identity](const StoreIdentity& requested) {
    return requested==expected?retained:nullptr;
  };
}
std::uint64_t NativeVolumeFile::Regions::advice_sequence() const {
  const auto observed=read_optimization_state(*advice,selection.volume_identity);
  return observed.code==OptimizationReadCode::available && observed.state.settled_input_bytes?observed.sequence:0;
}
void NativeVolumeFile::Regions::configure_catalog(NativeFileSession& session) const noexcept {
  if(!reclaimer || !reclaimer->catalog_)return;
  try {
    const auto& catalog=reclaimer->catalog_;
    session.configure_indexed_coherence(std::make_shared<IndexedCoherenceStorage>(
        PagedStoreLimits{catalog->slots.front().range->capacity},
        [catalog]{return catalog->reserve();},[catalog](StoreIdentity id){return catalog->resolve(id);}));
  }catch(const std::exception&) {
    // Optional learning must leave the selected file data available.
  }
}
NativeVolumeFile::Regions NativeVolumeFile::new_regions(std::shared_ptr<StoreByteIo> file,
    StoreIdentity volume,PagedStoreLimits limits,std::optional<std::uint64_t> dense_physical_limit) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  auto owner=std::make_shared<Owner>(std::move(file),true);
  if(owner->backing->extent())fail(Error::invalid_operation,"volume-file creation requires empty backing");
  const auto bytes=limits.maximum_physical_bytes;
  if(dense_physical_limit) {
    const auto geometry=paired_file_geometry(bytes,true,Error::invalid_operation);
    owner->install_dense(detail::DenseVolumeBacking::create(owner->physical,volume,geometry.file_end,
        *dense_physical_limit));
  }
  auto head=std::make_shared<Region>(owner,0,kVolumeHeadBytes,0);
  auto advice=std::make_shared<Region>(owner,kVolumeHeadBytes,kOptimizationAreaBytes,0);
  auto image=std::make_shared<Region>(owner,image_offset,bytes,0,!owner->ranges);
  Regions regions{head,image,advice,{},{}};
  if(owner->ranges) {
    const auto geometry=paired_file_geometry(bytes,true,Error::invalid_operation);
    auto catalog=geometry.catalog_capacity?
        std::make_shared<CatalogRanges>(owner,advice,volume,geometry,0):nullptr;
    regions.selection.placement=catalog?NativeVolumePlacement::paired_file_catalog_ranges:
        NativeVolumePlacement::paired_file_ranges;
    auto spare=std::make_shared<Region>(owner,image_offset+bytes,bytes,0);
    regions.reclaimer=std::make_shared<detail::NativeVolumeFileReclaimer>(head,image,std::move(spare),false,std::move(catalog));
    regions.image=Region::pin_image(image);
  }
  return regions;
}
NativeVolumeFile::Regions NativeVolumeFile::open_regions(std::shared_ptr<StoreByteIo> file,bool writable,
    std::optional<StoreIdentity> expected) {
  auto owner=std::make_shared<Owner>(std::move(file),writable);
  std::shared_ptr<detail::DenseVolumeBacking> dense;
  if(detail::DenseVolumeBacking::recognizes(*owner->physical)) {
    owner->pin_reader();
    dense=detail::DenseVolumeBacking::open(owner->physical,writable);
    if(expected && *expected!=dense->volume_identity())
      fail(Error::corrupt,"dense volume-file identity mismatch");
    expected=dense->volume_identity();
    owner->install_dense(dense);
  }
  const auto end=owner->backing->extent();
  if(end<image_offset+kVolumeHeadBytes || end>maximum_file_bytes)
    fail(Error::corrupt,"truncated or oversized volume file");
  auto head=std::make_shared<Region>(owner,0,kVolumeHeadBytes,kVolumeHeadBytes);
  const auto selected=NativeVolumeHeadView::inspect_selector(*head,expected).selection;
  if(selected.storage.kind!=NativeImageKind::indexed_paged)
    fail(Error::corrupt,"head does not select an indexed volume-file image");
  const auto bytes=selected.storage.maximum_physical_bytes;
  check_image_capacity(bytes,Error::corrupt);
  auto advice=std::make_shared<Region>(owner,kVolumeHeadBytes,kOptimizationAreaBytes,kOptimizationAreaBytes);
  if(selected.placement==NativeVolumePlacement::external) {
    if(selected.epoch!=1 || end-image_offset>bytes || owner->ranges || dense)
      fail(Error::corrupt,"invalid linear volume-file extent or selection");
    return {std::move(head),std::make_shared<Region>(owner,image_offset,bytes,0,true),std::move(advice),selected,{}};
  }
  const bool has_catalog=selected.placement==NativeVolumePlacement::paired_file_catalog_ranges;
  const auto geometry=paired_file_geometry(bytes,has_catalog,Error::corrupt);
  if(end>geometry.file_end || (has_catalog && !geometry.catalog_capacity) ||
      (dense && dense->logical_limit()!=geometry.file_end))
    fail(Error::corrupt,"volume file exceeds its paired range geometry");
  if(writable && !owner->ranges)fail(Error::invalid_operation,"paired volume file requires a qualified range provider");
  const auto offset=image_start(selected);
  const auto visible=end>offset?(std::min)(bytes,end-offset):0;
  auto image=std::make_shared<Region>(owner,offset,bytes,visible);
  Regions regions{head,image,advice,selected,{}};
  if(writable) {
    image->fixed_extent.store(image->allocation_extent(),std::memory_order_release);
    auto spare=std::make_shared<Region>(owner,offset==image_offset?image_offset+bytes:image_offset,bytes,0);
    const auto recover_spare=spare->allocation_extent()!=0;
    auto catalog=has_catalog?
        std::make_shared<CatalogRanges>(owner,advice,selected.volume_identity,geometry,end):nullptr;
    regions.reclaimer=std::make_shared<detail::NativeVolumeFileReclaimer>(head,image,std::move(spare),recover_spare,
        std::move(catalog));
  } else {
    owner->pin_reader();
  }
  if(!selected.sealed_range) {
    if(writable)regions.image=Region::pin_image(image);
    return regions;
  }
  const auto& sealed=*selected.sealed_range;
  const auto base_bytes=sealed.base_bytes,indexed_end=sealed.indexed_end,tail_prefix=sealed.tail_prefix_bytes();
  const auto present=(std::min)(visible,image->extent());
  if(present<base_bytes+tail_prefix)
    fail(Error::corrupt,"sealed image components are outside the selected file range");
  const auto contract=image->contract();
  if(writable && (!valid_store_io_contract(contract) || base_bytes%contract.write_failure_unit_bytes))
    fail(Error::invalid_operation,"sealed component alignment exceeds the current backing contract");
  const auto tail_capacity=(std::min)(bytes-base_bytes,tail_prefix+(bytes-indexed_end));
  auto pin=Region::pin_image(image);
  auto base=Region::slice(image,0,base_bytes,base_bytes,pin);
  auto tail=Region::slice(image,base_bytes,tail_capacity,(std::min)(tail_capacity,present-base_bytes),std::move(pin));
  regions.image=NativeHybridBacking::open(std::move(base),std::move(tail),selected.image_identity,bytes,bytes);
  return regions;
}
NativeFileSession NativeVolumeFile::create(std::shared_ptr<StoreByteIo> empty,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  return create_regions(new_regions(std::move(empty),volume,limits),volume,image,limits,
      foreground,disposition,std::move(root));
}
NativeFileSession NativeVolumeFile::create_dense(std::shared_ptr<StoreByteIo> empty,
    std::uint64_t physical_limit,StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits,
    StoreCompressionOptions foreground,NativeDispositionPolicy disposition,
    std::optional<NativeVolumeRootMetadata> root) {
  return create_regions(new_regions(std::move(empty),volume,limits,physical_limit),volume,image,limits,
      foreground,disposition,std::move(root));
}
NativeFileSession NativeVolumeFile::create_regions(Regions region,StoreIdentity volume,StoreIdentity image,
    PagedStoreLimits limits,StoreCompressionOptions foreground,NativeDispositionPolicy disposition,
    std::optional<NativeVolumeRootMetadata> root) {
  auto session=NativeFileSession::create_indexed_volume(std::move(region.head),volume,std::move(region.image),
      image,limits,foreground,disposition,std::move(root),region.selection.placement,
      NativeIndexedDataLayout::inline_small_files);
  session.create_budgeted_optimization_area(std::move(region.advice));
  region.configure_catalog(session);
  session.bind_persistent_reclamation(std::move(region.reclaimer));
  return session;
}
NativeFileSession NativeVolumeFile::resume(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground,NativeDispositionPolicy disposition,
    const std::function<StoreIdentity()>& startup_repair_identity) {
  return resume_regions(open_regions(std::move(existing),true,expected),foreground,disposition,startup_repair_identity);
}
NativeFileSession NativeVolumeFile::resume_regions(Regions region,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,const std::function<StoreIdentity()>& startup_repair_identity,
    const detail::IndexedReopenAdmission& admission,const std::function<void(StoreIdentity)>& retain_verified) {
  const auto& selected=region.selection;
  NativeStartupRepairProvider target;
  if(region.reclaimer && startup_repair_identity)
    target=[&](const auto& authority){return region.reclaimer->startup_repair_image(authority,startup_repair_identity);};
  auto session=NativeFileSession::resume_volume_impl(std::move(region.head),selected.volume_identity,
      selected.epoch,region.resolver(),foreground,disposition,true,target,admission,retain_verified);
  if(region.reclaimer)region.reclaimer->adopt_startup_repair(selected,session.volume_info().selection);
  if(const auto sequence=region.advice_sequence())
    session.resume_budgeted_optimization_area(std::move(region.advice),sequence);
  region.configure_catalog(session);
  session.bind_persistent_reclamation(std::move(region.reclaimer));
  return session;
}
NativeVolumeHeadView NativeVolumeFile::inspect(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected) {
  auto region=open_regions(std::move(existing),false,expected);
  return NativeVolumeHeadView::open_indexed_recovery(std::move(region.head),region.selection.volume_identity,region.resolver());
}
#ifdef _WIN32
std::shared_ptr<StoreByteIo> NativeVolumeFile::create_backing(const std::filesystem::path& path,StoreIoContract contract,
    PagedStoreLimits limits,std::wstring_view expected_volume,std::optional<std::uint64_t> dense_physical_limit) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  if(dense_physical_limit) {
    if(!valid_store_io_contract(contract) || contract.isolated_range_reclamation ||
        *dense_physical_limit<5*kStoreCommitUnit || *dense_physical_limit>maximum_file_bytes)
      fail(Error::invalid_operation,"dense creation requires a qualified ordinary file and physical limit");
    (void)paired_file_geometry(limits.maximum_physical_bytes,true,Error::invalid_operation);
    return detail::new_volume_file(path,contract,*dense_physical_limit,expected_volume);
  }
  const auto bytes=limits.maximum_physical_bytes;
  const auto limit=contract.isolated_range_reclamation?
      paired_file_geometry(bytes,true,Error::invalid_operation).file_end:image_offset+bytes;
  return detail::new_volume_file(path,contract,limit,expected_volume);
}
std::shared_ptr<StoreByteIo> NativeVolumeFile::reopen_backing(const std::filesystem::path& path,StoreIoContract contract,
    std::wstring_view expected_volume) {
  return detail::reopen_volume_file(path,contract,maximum_file_bytes,expected_volume);
}
NativeFileSession NativeVolumeFile::create(const std::filesystem::path& path,StoreIoContract contract,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  return create(create_backing(path,contract,limits),volume,image,limits,foreground,disposition,std::move(root));
}
NativeFileSession NativeVolumeFile::create_dense(const std::filesystem::path& path,StoreIoContract contract,
    std::uint64_t physical_limit,StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits,
    StoreCompressionOptions foreground,NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  return create_dense(create_backing(path,contract,limits,{},physical_limit),physical_limit,volume,image,limits,
      foreground,disposition,std::move(root));
}
NativeFileSession NativeVolumeFile::resume(const std::filesystem::path& path,StoreIoContract contract,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground,NativeDispositionPolicy disposition) {
  return resume(reopen_backing(path,contract),expected,foreground,disposition);
}
NativeVolumeHeadView NativeVolumeFile::inspect(const std::filesystem::path& path,std::optional<StoreIdentity> expected) {
  return inspect(detail::read_store_file(path,maximum_file_bytes),expected);
}
#endif
} // namespace flitzfs
