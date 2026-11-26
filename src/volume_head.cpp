#include "flitzfs/volume_head.hpp"
#include "flitzfs/native_hybrid_backing.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
using Code=NativeFsErrorCode;
[[noreturn]] void fail(Code code,const char* message){throw NativeFsError(code,message);}
constexpr std::size_t minimum_slot_bytes=4096;
constexpr std::size_t digest_bytes=32;
constexpr char magic[]="FLTVH001",compact_magic[]="FLTVH002";
const char* slot_magic(std::size_t bytes) noexcept {
  return bytes==kStoreCommitUnit?magic:compact_magic;
}
constexpr auto paired_file_capability=4U,paired_catalog_capability=5U;
constexpr auto sealed_file_capability=6U,sealed_catalog_capability=7U;
constexpr auto compact_sealed_file_capability=8U,compact_sealed_catalog_capability=9U;
void put(Bytes& b,std::size_t at,std::uint64_t v,unsigned n=8) {
  for(unsigned i=0;i<n;++i)b[at+i]=static_cast<std::byte>(v>>(8*i));
}
std::uint64_t get(std::span<const std::byte> b,std::size_t at,unsigned n=8) {
  std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(std::to_integer<unsigned>(b[at+i]))<<(8*i);
  return v;
}
bool zero(StoreIdentity id){return id==StoreIdentity{};}
void valid(const NativeVolumeSelection& s,Code code) {
  const bool indexed=s.storage.kind==NativeImageKind::indexed_paged;
  if(s.placement!=NativeVolumePlacement::external &&
      (!paired_volume_file(s.placement) || !indexed ||
       s.storage.maximum_physical_bytes%kStoreCommitUnit))
    fail(code,"invalid volume image placement");
  if(!((s.storage.kind==NativeImageKind::flat && !s.storage.maximum_physical_bytes) ||
      ((s.storage.kind==NativeImageKind::paged || indexed) && s.storage.maximum_physical_bytes)))
    fail(code,"invalid volume image storage descriptor");
  if(s.sealed_range) {
    const auto& range=*s.sealed_range;
    const auto base=range.base_bytes,end=range.indexed_end,prefix=range.tail_prefix_bytes();
    if(!paired_volume_file(s.placement) || s.epoch<2 || end>s.storage.maximum_physical_bytes ||
        range.tail_slot_bytes<minimum_slot_bytes || range.tail_slot_bytes>kStoreCommitUnit ||
        !std::has_single_bit(range.tail_slot_bytes) || end<=prefix || base>=end-prefix ||
        base<=kVolumeHeadBytes || base%minimum_slot_bytes || end%minimum_slot_bytes)
      fail(code,"invalid paired sealed image geometry");
  }
  if(!s.epoch||zero(s.volume_identity)||zero(s.image_identity)||s.volume_identity==s.image_identity||
      !s.minimum_checkpoint||!s.namespace_sequence||(!indexed && s.namespace_sequence>s.minimum_checkpoint))
    fail(code,"invalid volume head identity/checkpoint");
  if(s.epoch==1) {
    if(!zero(s.parent_identity)||s.parent_checkpoint)fail(code,"initial volume head has parent");
  }else if(zero(s.parent_identity)||s.parent_identity==s.image_identity||
      s.parent_identity==s.volume_identity||!s.parent_checkpoint||
      (!indexed && s.parent_checkpoint>=s.minimum_checkpoint))
    fail(code,"invalid compacted volume head parent");
}
void contract(const StoreByteIo& io,std::size_t slot_bytes=kStoreCommitUnit) {
  const auto c=io.contract();const auto u=c.write_failure_unit_bytes;
  if(!u||u>slot_bytes||(u&(u-1))||slot_bytes%u||!c.successful_flush_persists)
    fail(Code::invalid_operation,"volume head requires qualified disjoint failure units and persistence");
}
Bytes encoded(const NativeVolumeSelection& s,unsigned slot,std::size_t slot_bytes) {
  valid(s,Code::invalid_operation);
  Bytes b(slot_bytes);
  const auto digest_at=slot_bytes-digest_bytes;
  const auto kind=static_cast<std::uint32_t>(s.storage.kind);
  auto capability=kind+1;
  const bool compact_sealed=s.sealed_range && s.sealed_range->tail_slot_bytes!=kStoreCommitUnit;
  if(s.placement==NativeVolumePlacement::paired_file_catalog_ranges)
    capability=compact_sealed?compact_sealed_catalog_capability:
        s.sealed_range?sealed_catalog_capability:paired_catalog_capability;
  else if(s.placement==NativeVolumePlacement::paired_file_ranges)
    capability=compact_sealed?compact_sealed_file_capability:
        s.sealed_range?sealed_file_capability:paired_file_capability;
  std::memcpy(b.data(),slot_magic(slot_bytes),8);put(b,8,capability,4);put(b,12,slot,4);put(b,16,s.epoch);
  std::copy(s.volume_identity.begin(),s.volume_identity.end(),b.begin()+24);
  std::copy(s.image_identity.begin(),s.image_identity.end(),b.begin()+40);
  std::copy(s.parent_identity.begin(),s.parent_identity.end(),b.begin()+56);
  put(b,72,s.minimum_checkpoint);put(b,80,s.namespace_sequence);put(b,88,s.parent_checkpoint);
  if(kind){put(b,96,kind,4);put(b,104,s.storage.maximum_physical_bytes);}
  if(s.sealed_range){put(b,112,s.sealed_range->base_bytes);put(b,120,s.sealed_range->indexed_end);}
  if(compact_sealed)put(b,128,s.sealed_range->tail_slot_bytes,4);
  const auto digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));
  std::copy(digest.begin(),digest.end(),b.begin()+digest_at);return b;
}
std::optional<NativeVolumeSelection> decoded(std::span<const std::byte> b,unsigned slot,
    const std::optional<StoreIdentity>& expected) {
  const auto digest_at=b.size()-digest_bytes;
  const auto digest=detail::record_digest(b.first(digest_at));
  if(!std::equal(digest.begin(),digest.end(),b.begin()+digest_at))return {};
  // Intact unsupported/contradictory metadata is NEVER an excuse to roll back.
  const auto capability=get(b,8,4);
  const bool paired=capability>=paired_file_capability;
  const bool sealed=capability>=sealed_file_capability;
  const bool compact_sealed=capability>=compact_sealed_file_capability;
  const auto kind=paired?static_cast<std::uint64_t>(NativeImageKind::indexed_paged):capability-1;
  const auto reserved_at=compact_sealed?132:sealed?128:(capability>1?112:96);
  if(std::memcmp(b.data(),slot_magic(b.size()),8)||capability<1||capability>compact_sealed_catalog_capability||
      (paired && b.size()!=kStoreCommitUnit)||get(b,12,4)!=slot||
      (compact_sealed && get(b,128,4)>=kStoreCommitUnit) ||
      (capability>1 && (get(b,96,4)!=kind || get(b,100,4)!=0)) ||
      !std::all_of(b.begin()+reserved_at,b.begin()+digest_at,[](auto x){return x==std::byte{};}))
    fail(Code::corrupt,"intact volume head framing unsupported");
  NativeVolumeSelection s;s.epoch=get(b,16);
  std::copy_n(b.begin()+24,16,s.volume_identity.begin());std::copy_n(b.begin()+40,16,s.image_identity.begin());
  std::copy_n(b.begin()+56,16,s.parent_identity.begin());
  s.minimum_checkpoint=get(b,72);s.namespace_sequence=get(b,80);s.parent_checkpoint=get(b,88);
  if(capability>1)s.storage={static_cast<NativeImageKind>(kind),get(b,104)};
  if(paired)s.placement=(capability==paired_catalog_capability || capability==sealed_catalog_capability ||
      capability==compact_sealed_catalog_capability)?
      NativeVolumePlacement::paired_file_catalog_ranges:NativeVolumePlacement::paired_file_ranges;
  if(sealed)s.sealed_range=NativePairedSealedRange{get(b,112),get(b,120),
      compact_sealed?static_cast<std::uint32_t>(get(b,128,4)):static_cast<std::uint32_t>(kStoreCommitUnit)};
  valid(s,Code::corrupt);
  if(expected && s.volume_identity!=*expected)fail(Code::corrupt,"volume head identity mismatch");
  return s;
}
struct Loaded {NativeVolumeHeadInfo info;std::size_t slot_bytes;unsigned selected_slot;};
Loaded load(const StoreByteIo& io,std::optional<StoreIdentity> expected) {
  if(expected && zero(*expected))fail(Code::invalid_operation,"missing expected volume identity");
  const auto extent=io.extent();
  if(extent<2*minimum_slot_bytes || extent>kVolumeHeadBytes || (extent&(extent-1)))
    fail(Code::corrupt,"unsupported volume head extent");
  const auto slot_bytes=static_cast<std::size_t>(extent/2);
  Bytes scratch(slot_bytes);std::optional<NativeVolumeSelection> slots[2];
  for(unsigned i=0;i<2;++i){io.read_exact(i*slot_bytes,scratch);slots[i]=decoded(scratch,i,expected);}
  if(!slots[0]&&!slots[1])fail(Code::corrupt,"no intact volume head");
  if(!slots[0])return {{*slots[1],true},slot_bytes,1};
  if(!slots[1])return {{*slots[0],true},slot_bytes,0};
  if(slots[0]->epoch==slots[1]->epoch) {
    if(*slots[0]!=*slots[1])fail(Code::corrupt,"same-epoch volume heads disagree");
    return {{*slots[1],false},slot_bytes,1};
  }
  const unsigned newer=slots[0]->epoch>slots[1]->epoch?0:1,older=1-newer;
  const auto& n=*slots[newer];const auto& o=*slots[older];
  if(n.volume_identity!=o.volume_identity || n.storage!=o.storage || n.placement!=o.placement ||
      n.epoch-o.epoch!=1||n.parent_identity!=o.image_identity||
      n.parent_checkpoint<o.minimum_checkpoint||n.namespace_sequence<o.namespace_sequence)
    fail(Code::corrupt,"volume head transition is not a bound successor");
  return {{n,true},slot_bytes,newer};
}
void admit_binding(const NativeVolumeSelection& selected,const NativeFilesystemSnapshot& image,
    bool writable,bool indexed_recovery) {
  const auto info=image.storage_info();
  if(image.image_storage()!=selected.storage || image.identity()!=selected.image_identity||image.sequence()<selected.namespace_sequence||
      !info.replicated_checkpoint||info.checkpoint_generation<selected.minimum_checkpoint||
      info.generation<selected.minimum_checkpoint||(!indexed_recovery && image.needs_session_recovery()))
    fail(Code::corrupt,"selected volume image is absent, stale or unresolved");
  if(writable&&(info.degraded_checkpoint||info.degraded_root))
    fail(Code::invalid_operation,"repair selected image metadata independently before head admission");
}
void bind(const NativeVolumeSelection& selected,const NativeFilesystemSnapshot& image,bool writable,
    bool indexed_recovery=false) {
  admit_binding(selected,image,writable,indexed_recovery);
  image.verify_all();
  const auto verified=image.storage_info();
  if(writable && (verified.degraded_checkpoint || verified.degraded_root))
    fail(Code::invalid_operation,"image scrub discovered degraded protection");
}
NativeVolumeSelection initial(StoreIdentity volume,const NativeFilesystemSnapshot& source,NativeVolumePlacement placement) {
  NativeVolumeSelection s;s.volume_identity=volume;s.image_identity=source.identity();s.epoch=1;
  s.placement=placement;
  s.storage=source.image_storage();
  s.minimum_checkpoint=source.storage_info().checkpoint_generation;s.namespace_sequence=source.sequence();
  valid(s,Code::invalid_operation);return s;
}
void write_slot(StoreByteIo& io,const NativeVolumeSelection& s,unsigned slot,std::size_t slot_bytes) {
  const auto bytes=encoded(s,slot,slot_bytes);
  io.write_exact(slot*slot_bytes,bytes);
  io.flush();
}
}
struct NativeVolumeHeadView::Impl {
  NativeVolumeHeadInfo info;NativeFilesystemSnapshot filesystem;
  std::size_t slot_bytes;
  Impl(const Loaded& selected,NativeFilesystemSnapshot f)
      :info(selected.info),filesystem(std::move(f)),slot_bytes(selected.slot_bytes){}
};
NativeVolumeHeadView::NativeVolumeHeadView(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeVolumeHeadView::NativeVolumeHeadView(NativeVolumeHeadView&&) noexcept=default;
NativeVolumeHeadView& NativeVolumeHeadView::operator=(NativeVolumeHeadView&&) noexcept=default;
NativeVolumeHeadView::~NativeVolumeHeadView()=default;
NativeVolumeHeadInfo NativeVolumeHeadView::inspect_selector(const StoreByteIo& head,
    std::optional<StoreIdentity> expected) {
  try{return load(head,expected).info;}
  catch(const std::bad_alloc&){fail(Code::limit,"volume selector allocation");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"volume selector read failed");}
}
NativeVolumeHeadView NativeVolumeHeadView::open(std::shared_ptr<StoreByteIo> head,StoreIdentity volume,
    const NativeVolumeImageResolver& resolve) {
  return open_impl(std::move(head),volume,resolve,false);
}
NativeVolumeHeadView NativeVolumeHeadView::open_indexed_recovery(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,const NativeVolumeImageResolver& resolve) {
  return open_impl(std::move(head),volume,resolve,true);
}
NativeVolumeHeadView NativeVolumeHeadView::open_impl(std::shared_ptr<StoreByteIo> head,StoreIdentity volume,
    const NativeVolumeImageResolver& resolve,bool indexed_recovery,const detail::IndexedReopenAdmission& admission) {
  if(!head||!resolve)fail(Code::invalid_operation,"missing volume head/image resolver");
  try {
    const auto loaded=load(*head,volume);
    const auto& selected=loaded.info;
    if(indexed_recovery && selected.selection.storage.kind!=NativeImageKind::indexed_paged)
      fail(Code::invalid_operation,"indexed recovery requires the indexed volume head kind");
    auto image=resolve(selected.selection.image_identity);
    if(!image||image==head)fail(Code::corrupt,"missing or aliased selected image");
    if(selected.selection.sealed_range) {
      const auto* hybrid=dynamic_cast<const NativeHybridBacking*>(image.get());
      if(!hybrid || hybrid->sealed_range()!=*selected.selection.sealed_range)
        fail(Code::corrupt,"selected sealed image differs from the volume head");
    }
    auto view=NativeFilesystemSnapshot::open_image(std::move(image),selected.selection.storage);
    admit_binding(selected.selection,view,false,indexed_recovery);
    view.verify_all(admission);
    return NativeVolumeHeadView(std::make_unique<Impl>(loaded,std::move(view)));
  }catch(const std::bad_alloc&){fail(Code::limit,"volume head reader allocation");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"volume head/image read failed; no fallback");}
}
NativeVolumeHeadInfo NativeVolumeHeadView::info() const noexcept{return impl_?impl_->info:NativeVolumeHeadInfo{};}
void NativeVolumeHeadView::admit_writable_provider(const StoreByteIo& head) const {
  if(!impl_)fail(Code::invalid_operation,"moved-from volume view");
  contract(head,impl_->slot_bytes);
}
const NativeFilesystemSnapshot& NativeVolumeHeadView::filesystem() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from volume view");return impl_->filesystem;
}
struct NativeVolumeHeadWriter::Impl {
  struct Lease {
    std::shared_ptr<StoreByteIo> io;
    explicit Lease(std::shared_ptr<StoreByteIo> p,bool writable=true):io(std::move(p)) {
      if(!io || !(writable?io->acquire_writer():io->acquire_exclusive_use()))
        fail(Code::invalid_operation,"volume backend cannot acquire its required lease");
    }
    ~Lease(){io->release_writer();}
    Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
  };
  Lease owner;NativeVolumeSelection selected{};bool poisoned=false;
  std::size_t slot_bytes=kStoreCommitUnit;
  explicit Impl(std::shared_ptr<StoreByteIo> io):owner(std::move(io)){}
  std::uint64_t occupied_bytes() const noexcept {return 2*slot_bytes;}
  static std::shared_ptr<StoreByteIo> backing(const NativeFilesystemSnapshot& image) {
    return image.backing_for_volume_head();
  }
  static NativeFilesystemSnapshot fresh(std::shared_ptr<StoreByteIo> io,NativeImageStorage storage) {
    auto view=NativeFilesystemSnapshot::open_image(std::move(io),storage);
    // A surviving primary alone does not expose a damaged payload peer.
    // Scrub both before comparing health or admitting a new selected image.
    if(storage.kind!=NativeImageKind::flat)view.verify_all();
    return view;
  }
  static void exact(const NativeFilesystemSnapshot& a,const NativeFilesystemSnapshot& b) {
    const auto x=a.storage_info(),y=b.storage_info();
    if(a.identity()!=b.identity()||a.sequence()!=b.sequence()||x.generation!=y.generation||
        x.checkpoint_generation!=y.checkpoint_generation||x.physical_end!=y.physical_end||
        x.degraded_root!=y.degraded_root||x.degraded_checkpoint!=y.degraded_checkpoint)
      fail(Code::invalid_operation,"handoff snapshot is not latest");
    if(a.image_storage().kind==NativeImageKind::indexed_paged) {
      if(!a.matches_indexed_checkpoint(b))
        fail(Code::invalid_operation,"indexed handoff snapshot root differs");
    }else a.verify_compacted_equivalent_for_volume_head(b);
  }
  NativeVolumeSelection successor(const NativeFilesystemSnapshot& source,
      const NativeFilesystemSnapshot& target) const {
    const auto source_io=backing(source),target_io=backing(target);
    if(source_io==target_io || source_io==owner.io || target_io==owner.io ||
        source.image_storage()!=selected.storage || target.image_storage()!=selected.storage ||
        source.identity()!=selected.image_identity || target.identity()==source.identity() ||
        target.identity()==selected.volume_identity || target.identity()==selected.parent_identity)
      fail(Code::invalid_operation,"invalid volume handoff identities/backends");
    if(selected.epoch==(std::numeric_limits<std::uint64_t>::max)())
      fail(Code::limit,"volume head epoch exhausted");
    NativeVolumeSelection next=selected;
    next.epoch++;
    next.image_identity=target.identity();
    next.minimum_checkpoint=target.storage_info().checkpoint_generation;
    next.namespace_sequence=target.sequence();
    next.parent_identity=source.identity();
    next.parent_checkpoint=source.storage_info().checkpoint_generation;
    next.sealed_range.reset();
    valid(next,Code::invalid_operation);
    return next;
  }
  void check() const {
    if(poisoned)fail(Code::storage_failure,"volume head writer is terminal");
    contract(*owner.io,slot_bytes);
    const auto now=load(*owner.io,selected.volume_identity);
    if(now.slot_bytes!=slot_bytes || now.info.degraded || now.info.selection!=selected)
      fail(Code::corrupt,"volume head changed outside owner");
  }
  void pair(const NativeVolumeSelection& next,unsigned first=0) {
    owner.io->admit_write(occupied_bytes());
    publish_pair(next,first);
  }
  void publish_pair(const NativeVolumeSelection& next,unsigned first=0) {
    contract(*owner.io,slot_bytes);
    write_slot(*owner.io,next,first,slot_bytes);
    write_slot(*owner.io,next,1-first,slot_bytes);
    const auto observed=load(*owner.io,next.volume_identity);
    if(observed.slot_bytes!=slot_bytes || observed.info.degraded || observed.info.selection!=next)
      fail(Code::corrupt,"volume head publication readback");
    selected=next;
  }
};
NativeVolumeHeadWriter::NativeVolumeHeadWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeVolumeHeadWriter::NativeVolumeHeadWriter(NativeVolumeHeadWriter&&) noexcept=default;
NativeVolumeHeadWriter& NativeVolumeHeadWriter::operator=(NativeVolumeHeadWriter&&) noexcept=default;
NativeVolumeHeadWriter::~NativeVolumeHeadWriter()=default;
std::uint64_t NativeVolumeHeadWriter::occupied_bytes() const noexcept {
  return impl_?impl_->occupied_bytes():0;
}
NativeVolumeHeadWriter NativeVolumeHeadWriter::create(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,const NativeFilesystemSnapshot& source,NativeVolumePlacement placement,
    NativeVolumeHeadLayout layout) {
  if(layout!=NativeVolumeHeadLayout::legacy &&
      (layout!=NativeVolumeHeadLayout::compact || placement!=NativeVolumePlacement::external))
    fail(Code::invalid_operation,"unsupported volume head layout");
  const auto next=initial(volume,source,placement);auto image=Impl::backing(source);
  if(head==image)fail(Code::invalid_operation,"volume head aliases image");
  auto p=std::make_unique<Impl>(std::move(head));Impl::Lease image_lease(image);
  contract(*p->owner.io);contract(*image);
  if(p->owner.io->extent())fail(Code::invalid_operation,"volume head requires a new empty backend");
  if(layout==NativeVolumeHeadLayout::compact)
    p->slot_bytes=(std::max)(minimum_slot_bytes,static_cast<std::size_t>(p->owner.io->contract().write_failure_unit_bytes));
  p->owner.io->admit_write(p->occupied_bytes());
  bind(next,source,true);auto before=Impl::fresh(image,next.storage);Impl::exact(source,before);
  image->flush();auto after=Impl::fresh(image,next.storage);Impl::exact(source,after);
  // Establish complete zero extent before the first self-contained selector.
  Bytes zeroes(p->slot_bytes);
  p->owner.io->write_exact(0,zeroes);p->owner.io->write_exact(p->slot_bytes,zeroes);
  p->pair(next);
  return NativeVolumeHeadWriter(std::move(p));
}
NativeVolumeHeadWriter NativeVolumeHeadWriter::resume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeFilesystemSnapshot& image) {
  return resume_impl(std::move(head),volume,epoch,image,false);
}
NativeVolumeHeadWriter NativeVolumeHeadWriter::resume_impl(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeFilesystemSnapshot& image,bool closed_indexed) {
  auto p=std::make_unique<Impl>(std::move(head));
  const auto current=load(*p->owner.io,volume);
  p->slot_bytes=current.slot_bytes;
  contract(*p->owner.io,p->slot_bytes);
  if(!epoch||epoch!=current.info.selection.epoch)fail(Code::invalid_operation,"stale volume head epoch");
  auto backing=Impl::backing(image);
  if(backing==p->owner.io)fail(Code::invalid_operation,"volume head aliases image");
  // Scoped to a closed paired file. Its verified degraded image stays
  // immutable and the head flush reaches the same qualified backing file.
  const bool sealed_repair=closed_indexed && current.info.selection.sealed_range && backing->backing_degraded();
  Impl::Lease image_lease(backing,!sealed_repair);contract(*backing);
  // A paged head may be resumed over independently CPU-readable degraded
  // data solely to select a verified repaired copy. Paged writable resume
  // still refuses lost payload replicas; no damaged source is repaired here.
  bind(current.info.selection,image,current.info.selection.storage.kind==NativeImageKind::flat,closed_indexed);
  auto before=Impl::fresh(backing,current.info.selection.storage);Impl::exact(image,before);
  if(sealed_repair)p->owner.io->flush();
  else backing->flush();
  auto after=Impl::fresh(backing,current.info.selection.storage);Impl::exact(image,after);
  p->selected=current.info.selection;p->owner.io->admit_write(p->occupied_bytes());
  // Repair peer FIRST, preserving the selected durable survivor.
  if(current.info.degraded)p->pair(p->selected,1-current.selected_slot);
  else {p->owner.io->flush();p->check();}
  return NativeVolumeHeadWriter(std::move(p));
}
std::optional<NativeVolumeHeadWriter::IndexedReopen> NativeVolumeHeadWriter::retain_indexed_reopen(
    std::shared_ptr<StoreByteIo> head,const NativeVolumeHeadView& audited,StoreCompressionOptions foreground) {
  const auto selected=audited.info().selection;
  const auto& verified=audited.filesystem();
  auto p=std::make_unique<Impl>(std::move(head));
  const auto current=load(*p->owner.io,selected.volume_identity);
  p->slot_bytes=current.slot_bytes;
  contract(*p->owner.io,p->slot_bytes);
  if(current.info.selection!=selected)fail(Code::invalid_operation,"indexed reopen selector changed");
  if(current.info.degraded)return std::nullopt;

  const auto backing=Impl::backing(verified);
  if(selected.storage.kind!=NativeImageKind::indexed_paged || backing==p->owner.io)
    fail(Code::invalid_operation,"indexed reopen requires a distinct indexed image");
  contract(*backing);
  admit_binding(selected,verified,true,false);
  const auto extent=backing->extent();
  p->selected=selected;
  p->owner.io->admit_write(p->occupied_bytes());
  // The image lease checks the exact verified checkpoint, flushes the image,
  // and rechecks the root before the selector fence. Both leases are held.
  auto writer=NativeFilesystemWriter::lease_verified_indexed_reopen(verified,foreground);
  if(backing->extent()!=extent)fail(Code::invalid_operation,"indexed reopen extent changed");
  p->owner.io->flush();
  p->check();
  if(backing->extent()!=extent)fail(Code::invalid_operation,"indexed reopen extent changed");
  auto after=NativeFilesystemSnapshot::open_image(backing,selected.storage);
  Impl::exact(verified,after);
  admit_binding(selected,after,true,false);
  after.recheck_verified_indexed_mirrors(verified);
  const auto health=after.storage_info();
  if(health.degraded_checkpoint || health.degraded_root)
    fail(Code::invalid_operation,"image scrub discovered degraded protection");
  p->check();
  return IndexedReopen{NativeVolumeHeadWriter(std::move(p)),std::move(writer)};
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::publish_compacted(
    const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& target) {
  return publish_compacted_impl(source,target,false);
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::publish_compacted_impl(
    const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& target,bool closed_indexed) {
  if(!impl_||impl_->poisoned)fail(Code::storage_failure,"volume head writer is terminal");
  auto& p=*impl_;const auto source_io=Impl::backing(source),target_io=Impl::backing(target);
  const auto next=p.successor(source,target);
  const bool sealed_repair=closed_indexed && p.selected.sealed_range && source_io->backing_degraded();
  Impl::Lease source_lease(source_io,!sealed_repair),target_lease(target_io);
  bool mutation=false;
  try {
    p.check();contract(*source_io);contract(*target_io);p.owner.io->admit_write(p.occupied_bytes());
    bind(p.selected,source,p.selected.storage.kind==NativeImageKind::flat,closed_indexed);
    const auto target_info=target.storage_info();
    if(!target_info.replicated_checkpoint||target_info.degraded_checkpoint||target_info.degraded_root)
      fail(Code::invalid_operation,"compacted target needs complete replicated metadata");
    auto old=Impl::fresh(source_io,source.image_storage()),candidate=Impl::fresh(target_io,target.image_storage());
    Impl::exact(source,old);Impl::exact(target,candidate);
    if(candidate.storage_info().degraded_checkpoint || candidate.storage_info().degraded_root)
      fail(Code::invalid_operation,"handoff target replicas are degraded");
    source.verify_compacted_equivalent_for_volume_head(target,closed_indexed);
    mutation=true;target_io->flush();auto checked=Impl::fresh(target_io,next.storage);Impl::exact(target,checked);
    if(checked.storage_info().degraded_checkpoint || checked.storage_info().degraded_root)
      fail(Code::corrupt,"handoff target lost protection after flush");
    // Source remains unchanged and quiescent through candidate fencing.
    auto stable_source=Impl::fresh(source_io,source.image_storage());Impl::exact(source,stable_source);
    p.pair(next);return {p.selected,false};
  }catch(const std::bad_alloc&){if(mutation)p.poisoned=true;fail(Code::limit,"volume handoff allocation failure");}
  catch(const NativeFsError& e){if(mutation||e.code()==Code::corrupt||e.code()==Code::storage_failure)p.poisoned=true;throw;}
  catch(const std::exception&){p.poisoned=true;fail(Code::storage_failure,"volume handoff failed; retain all images");}
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::publish_indexed_rebuild(
    const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& target,
    std::optional<NativePairedSealedRange> sealed_range) {
  if(!impl_ || impl_->poisoned)fail(Code::storage_failure,"volume head writer is terminal");
  auto& p=*impl_;
  if(p.selected.storage.kind!=NativeImageKind::indexed_paged || source.sequence()!=target.sequence())
    fail(Code::invalid_operation,"staged selection requires an equivalent indexed image");
  auto next=p.successor(source,target);
  next.sealed_range=sealed_range;
  valid(next,Code::invalid_operation);
  const auto source_io=Impl::backing(source),target_io=Impl::backing(target);
  // Rewrite already flushed and independently compared every selected object
  // and replica after publishing the private target. Recheck the exact durable
  // checkpoints before the selector writes while both owners are still held.
  try {
    p.check();
    contract(*source_io);
    auto old=NativeFilesystemSnapshot::open_image(source_io,p.selected.storage);
    Impl::exact(source,old);
    admit_binding(p.selected,old,true,false);
  }catch(const std::bad_alloc&){fail(Code::limit,"volume handoff allocation failure");}
  catch(const NativeFsError&){p.poisoned=true;throw;}
  catch(const std::exception&){p.poisoned=true;fail(Code::storage_failure,"indexed handoff source check failed");}
  try {
    contract(*target_io);
    auto candidate=NativeFilesystemSnapshot::open_image(target_io,next.storage);
    Impl::exact(target,candidate);
    admit_binding(next,candidate,true,false);
    p.owner.io->admit_write(p.occupied_bytes());
  }catch(const std::bad_alloc&){fail(Code::limit,"volume handoff allocation failure");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"indexed handoff target or resource check failed");}
  try {
    p.publish_pair(next);
    return {p.selected,false};
  }catch(const NativeFsError&){p.poisoned=true;throw;}
  catch(const std::exception&){p.poisoned=true;fail(Code::storage_failure,"staged volume head failed; retain both images");}
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::info() const noexcept {
  return impl_&&!impl_->poisoned?NativeVolumeHeadInfo{impl_->selected,false}:NativeVolumeHeadInfo{{},true};
}
bool NativeVolumeHeadWriter::poisoned() const noexcept{return !impl_||impl_->poisoned;}
} // namespace flitzfs
