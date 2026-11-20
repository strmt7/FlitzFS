#include "flitzfs/native_volume_file.hpp"
#include "store_file.hpp"
#include "native_volume_file_reclaimer.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace flitzfs {
namespace {
constexpr auto image_offset=kNativeVolumeFileImageOffset;
constexpr auto maximum_file_bytes=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
static_assert(image_offset%kStoreCommitUnit==0);
using Error=NativeFsErrorCode;
[[noreturn]] void fail(Error code,const char* message){throw NativeFsError(code,message);}
void check_image_capacity(std::uint64_t bytes,Error code) {
  if(bytes<kVolumeHeadBytes || bytes>maximum_file_bytes-image_offset)
    fail(code,"invalid indexed volume-file capacity");
}
std::uint64_t paired_file_limit(std::uint64_t bytes,Error code) {
  if(bytes<kVolumeHeadBytes || bytes%kStoreCommitUnit || bytes>(maximum_file_bytes-image_offset)/2)
    fail(code,"invalid paired image range capacity");
  return image_offset+2*bytes;
}
std::uint64_t image_start(const NativeVolumeSelection& selected) noexcept {
  return image_offset+((selected.epoch%2==0)?selected.storage.maximum_physical_bytes:0);
}
}
struct NativeVolumeFile::Owner {
  const std::shared_ptr<StoreByteIo> backing;
  const bool writable;
  NativeVolumeRangeIo* ranges=nullptr;
  bool leased=false;

  explicit Owner(std::shared_ptr<StoreByteIo> file,bool write):backing(std::move(file)),writable(write) {
    if(!backing)fail(Error::invalid_operation,"missing volume-file backing");
    if(writable) {
      if(!valid_store_io_contract(backing->contract()) || !backing->acquire_writer())
        fail(Error::invalid_operation,"volume file requires an exclusive qualified writer");
      leased=true;
      auto* candidate=dynamic_cast<NativeVolumeRangeIo*>(backing.get());
      if(candidate && backing->contract().isolated_range_reclamation && candidate->supports_reclaimable_ranges())
        ranges=candidate;
    }
  }
  void pin_reader() {
    if(!leased) {
      leased=backing->acquire_exclusive_use();
      if(!leased)fail(Error::invalid_operation,"paired volume inspection requires a retired backing owner");
    }
  }
  ~Owner(){if(leased)backing->release_writer();}
};

struct NativeVolumeFile::Region final:StoreByteIo {
  const std::shared_ptr<Owner> owner;
  const std::uint64_t offset,capacity;
  const bool tail;
  std::uint64_t fixed_extent;

  Region(std::shared_ptr<Owner> file,std::uint64_t base,std::uint64_t limit,
      std::uint64_t initialized,bool follows_end=false)
      :owner(std::move(file)),offset(base),capacity(limit),tail(follows_end),fixed_extent(initialized){}
  std::uint64_t extent() const override {
    if(!tail)return fixed_extent;
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
    if(!tail)fixed_extent=(std::max)(fixed_extent,at+static_cast<std::uint64_t>(bytes.size()));
  }
  void admit_write(std::uint64_t projected) override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    const auto current=extent();
    if(projected<current || projected>capacity)fail(Error::limit,"volume-file admission exceeds its region");
    const auto end=(std::max)(owner->backing->extent(),offset+projected);
    if(owner->ranges)owner->ranges->admit_range_write(end,projected-current);
    else owner->backing->admit_write(end);
  }
  void flush() override {
    if(!owner->writable)fail(Error::invalid_operation,"read-only volume file");
    owner->backing->flush();
  }
  StoreIoContract contract() const noexcept override {return owner->backing->contract();}
  std::uint64_t allocation_extent() const {
    const auto end=owner->ranges->allocated_extent(offset,capacity);
    if(end>capacity || end%kStoreCommitUnit)fail(Error::storage_failure,"invalid volume allocation bound");
    return end;
  }
 protected:
  bool allows_writer() const noexcept override {return owner->writable;}
};

namespace detail {
using Progress=NativeVolumeReclaimProgress;
NativeVolumeFileReclaimer::NativeVolumeFileReclaimer(std::shared_ptr<StoreByteIo> head,
    std::shared_ptr<NativeVolumeFile::Region> active,std::shared_ptr<NativeVolumeFile::Region> spare,
    bool recover_spare):
    head_(std::move(head)),active_(std::move(active)),spare_(std::move(spare)),
    phase_(recover_spare?Phase::retiring:Phase::idle) {}

bool NativeVolumeFileReclaimer::pending(const NativeFileSession& session) const {
  if(phase_==Phase::retiring)return true;
  if(phase_==Phase::uncertain || phase_==Phase::failed || !session.idle_compaction_context_ready())return false;
  if(phase_!=Phase::idle)return true;
  if(const auto repair=session.indexed_capacity_repair_generation())return repair!=attempted_repair_;
  const auto space=session.indexed_volume_space();
  const auto obsolete=space.obsolete_bytes;
  if(obsolete<=attempted_obsolete_ || obsolete-attempted_obsolete_<abandoned_bytes_)return false;
  return space.reclamation_worthwhile();
}

Progress NativeVolumeFileReclaimer::retire(NativeFileSession& session) {
  if(spare_.use_count()!=1)return Progress::pinned;
  // Revalidate both replicas immediately before changing any allocation.
  const auto current=session.volume_info();
  const auto durable=NativeVolumeHeadView::inspect_selector(*head_,current.selection.volume_identity);
  if(current.degraded || durable.degraded || durable.selection!=current.selection ||
      durable.selection.placement!=NativeVolumePlacement::paired_file_ranges ||
      image_start(durable.selection)!=active_->offset || active_->offset==spare_->offset)
    fail(Error::corrupt,"unconfirmed volume selection prevents range reclamation");
  auto& range=*spare_;
  const auto before=range.allocation_extent();
  if(before) {
    constexpr std::uint64_t quantum=4*1024*1024;
    const auto start=(before-1)/quantum*quantum;
    range.owner->ranges->reclaim_range(range.offset+start,before-start);
    range.flush();
    range.fixed_extent=range.allocation_extent();
    if(range.fixed_extent>=before)fail(Error::storage_failure,"volume range allocation did not decrease");
  } else {
    range.fixed_extent=0;
  }
  if(!range.fixed_extent)phase_=Phase::idle;
  return Progress::retiring;
}

Progress NativeVolumeFileReclaimer::advance(NativeFileSession& session,const std::function<StoreIdentity()>& identity) {
  if(phase_==Phase::retiring) {
    try{return retire(session);}
    catch(const std::exception&){phase_=Phase::failed;return Progress::failed;}
  }
  if(!pending(session))return Progress::idle;
  try {
    if(phase_==Phase::idle) {
      attempted_repair_=session.indexed_capacity_repair_generation();
      attempted_obsolete_=session.indexed_volume_space().obsolete_bytes;
      abandoned_bytes_=0;
      if(!identity || spare_.use_count()!=1 || spare_->allocation_extent())
        fail(Error::invalid_operation,"inactive volume range is unavailable");
      if(!session.begin_idle_compaction_impl(spare_,identity(),true,PagedRewriteMode::reconcile_foreground))
        return Progress::idle;
      phase_=Phase::copying;
      return Progress::copying;
    }
    if(phase_==Phase::copying) {
      switch(session.advance_recompression()) {
        case NativeFileSession::RecompressionProgress::copying:return Progress::copying;
        case NativeFileSession::RecompressionProgress::ready:phase_=Phase::ready;return Progress::ready;
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
    active_.swap(spare_);
    attempted_obsolete_=0;
    abandoned_bytes_=0;
    attempted_repair_=0;
    phase_=Phase::retiring;
    return Progress::published;
  }catch(const std::exception&) {
    if(session.poisoned()){phase_=Phase::uncertain;throw;}
    abandoned_bytes_=spare_->extent();
    cancel(session);
    return Progress::failed;
  }
}

void NativeVolumeFileReclaimer::cancel(NativeFileSession& session) noexcept {
  session.cancel_recompression();
  if(phase_==Phase::copying || phase_==Phase::ready || (phase_==Phase::uncertain && !session.poisoned()))
    phase_=Phase::retiring;
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
NativeVolumeFile::Regions NativeVolumeFile::new_regions(std::shared_ptr<StoreByteIo> file,PagedStoreLimits limits) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  auto owner=std::make_shared<Owner>(std::move(file),true);
  if(owner->backing->extent())fail(Error::invalid_operation,"volume-file creation requires empty backing");
  const auto bytes=limits.maximum_physical_bytes;
  auto head=std::make_shared<Region>(owner,0,kVolumeHeadBytes,0);
  auto advice=std::make_shared<Region>(owner,kVolumeHeadBytes,kOptimizationAreaBytes,0);
  auto image=std::make_shared<Region>(owner,image_offset,bytes,0,!owner->ranges);
  Regions regions{head,image,advice,{},{}};
  if(owner->ranges) {
    (void)paired_file_limit(bytes,Error::invalid_operation);
    regions.selection.placement=NativeVolumePlacement::paired_file_ranges;
    auto spare=std::make_shared<Region>(owner,image_offset+bytes,bytes,0);
    regions.reclaimer=std::make_shared<detail::NativeVolumeFileReclaimer>(head,image,std::move(spare),false);
  }
  return regions;
}
NativeVolumeFile::Regions NativeVolumeFile::open_regions(std::shared_ptr<StoreByteIo> file,bool writable,
    std::optional<StoreIdentity> expected) {
  auto owner=std::make_shared<Owner>(std::move(file),writable);
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
    if(selected.epoch!=1 || end-image_offset>bytes || owner->ranges)
      fail(Error::corrupt,"invalid linear volume-file extent or selection");
    return {std::move(head),std::make_shared<Region>(owner,image_offset,bytes,0,true),std::move(advice),selected,{}};
  }
  if(end>paired_file_limit(bytes,Error::corrupt) || (writable && !owner->ranges))
    fail(Error::invalid_operation,"paired volume file requires a qualified range provider");
  const auto offset=image_start(selected);
  const auto visible=end>offset?(std::min)(bytes,end-offset):0;
  auto image=std::make_shared<Region>(owner,offset,bytes,visible);
  Regions regions{head,image,advice,selected,{}};
  if(writable) {
    image->fixed_extent=image->allocation_extent();
    auto spare=std::make_shared<Region>(owner,offset==image_offset?image_offset+bytes:image_offset,bytes,0);
    const auto recover_spare=spare->allocation_extent()!=0;
    regions.reclaimer=std::make_shared<detail::NativeVolumeFileReclaimer>(head,image,std::move(spare),recover_spare);
  } else {
    owner->pin_reader();
  }
  return regions;
}
NativeFileSession NativeVolumeFile::create(std::shared_ptr<StoreByteIo> empty,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  auto region=new_regions(std::move(empty),limits);
  auto session=NativeFileSession::create_indexed_volume(std::move(region.head),volume,std::move(region.image),
      image,limits,foreground,disposition,std::move(root),region.selection.placement);
  session.create_budgeted_optimization_area(std::move(region.advice));
  session.bind_persistent_reclamation(std::move(region.reclaimer));
  return session;
}
NativeFileSession NativeVolumeFile::resume(std::shared_ptr<StoreByteIo> existing,
    std::optional<StoreIdentity> expected,StoreCompressionOptions foreground,NativeDispositionPolicy disposition) {
  auto region=open_regions(std::move(existing),true,expected);
  const auto& selected=region.selection;
  auto session=NativeFileSession::resume_closed_indexed_volume(std::move(region.head),selected.volume_identity,
      selected.epoch,region.resolver(),foreground,disposition);
  if(const auto sequence=region.advice_sequence())
    session.resume_budgeted_optimization_area(std::move(region.advice),sequence);
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
    PagedStoreLimits limits) {
  check_image_capacity(limits.maximum_physical_bytes,Error::invalid_operation);
  const auto bytes=limits.maximum_physical_bytes;
  const auto limit=contract.isolated_range_reclamation?paired_file_limit(bytes,Error::invalid_operation):image_offset+bytes;
  return detail::new_volume_file(path,contract,limit);
}
std::shared_ptr<StoreByteIo> NativeVolumeFile::reopen_backing(const std::filesystem::path& path,StoreIoContract contract) {
  return detail::reopen_volume_file(path,contract,maximum_file_bytes);
}
NativeFileSession NativeVolumeFile::create(const std::filesystem::path& path,StoreIoContract contract,StoreIdentity volume,
    StoreIdentity image,PagedStoreLimits limits,StoreCompressionOptions foreground,
    NativeDispositionPolicy disposition,std::optional<NativeVolumeRootMetadata> root) {
  return create(create_backing(path,contract,limits),volume,image,limits,foreground,disposition,std::move(root));
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
