#include "flitzfs/volume_head.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
using Code=NativeFsErrorCode;
[[noreturn]] void fail(Code code,const char* message){throw NativeFsError(code,message);}
constexpr std::size_t digest_at=kStoreCommitUnit-32;
constexpr char magic[]="FLTVH001";
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
  if(!((s.storage.kind==NativeImageKind::flat && !s.storage.maximum_physical_bytes) ||
      ((s.storage.kind==NativeImageKind::paged || indexed) && s.storage.maximum_physical_bytes)))
    fail(code,"invalid volume image storage descriptor");
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
void contract(const StoreByteIo& io) {
  const auto c=io.contract();const auto u=c.write_failure_unit_bytes;
  if(!u||u>kStoreCommitUnit||(u&(u-1))||kStoreCommitUnit%u||!c.successful_flush_persists)
    fail(Code::invalid_operation,"volume head requires qualified disjoint failure units and persistence");
}
Bytes encoded(const NativeVolumeSelection& s,unsigned slot) {
  valid(s,Code::invalid_operation);Bytes b(kStoreCommitUnit);
  const auto kind=static_cast<std::uint32_t>(s.storage.kind);
  std::memcpy(b.data(),magic,8);put(b,8,kind+1,4);put(b,12,slot,4);put(b,16,s.epoch);
  std::copy(s.volume_identity.begin(),s.volume_identity.end(),b.begin()+24);
  std::copy(s.image_identity.begin(),s.image_identity.end(),b.begin()+40);
  std::copy(s.parent_identity.begin(),s.parent_identity.end(),b.begin()+56);
  put(b,72,s.minimum_checkpoint);put(b,80,s.namespace_sequence);put(b,88,s.parent_checkpoint);
  if(kind){put(b,96,kind,4);put(b,104,s.storage.maximum_physical_bytes);}
  const auto digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));
  std::copy(digest.begin(),digest.end(),b.begin()+digest_at);return b;
}
std::optional<NativeVolumeSelection> decoded(std::span<const std::byte> b,unsigned slot,StoreIdentity expected) {
  const auto digest=detail::record_digest(b.first(digest_at));
  if(!std::equal(digest.begin(),digest.end(),b.begin()+digest_at))return {};
  // Intact unsupported/contradictory metadata is NEVER an excuse to roll back.
  const auto capability=get(b,8,4);
  if(std::memcmp(b.data(),magic,8)||capability<1||capability>3||get(b,12,4)!=slot||
      (capability>1 && (get(b,96,4)!=capability-1 || get(b,100,4)!=0)) ||
      !std::all_of(b.begin()+(capability>1?112:96),b.begin()+digest_at,[](auto x){return x==std::byte{};}))
    fail(Code::corrupt,"intact volume head framing unsupported");
  NativeVolumeSelection s;s.epoch=get(b,16);
  std::copy_n(b.begin()+24,16,s.volume_identity.begin());std::copy_n(b.begin()+40,16,s.image_identity.begin());
  std::copy_n(b.begin()+56,16,s.parent_identity.begin());
  s.minimum_checkpoint=get(b,72);s.namespace_sequence=get(b,80);s.parent_checkpoint=get(b,88);
  if(capability>1)s.storage={static_cast<NativeImageKind>(capability-1),get(b,104)};
  valid(s,Code::corrupt);
  if(s.volume_identity!=expected)fail(Code::corrupt,"volume head identity mismatch");
  return s;
}
struct Loaded {NativeVolumeHeadInfo info;unsigned selected_slot=0;};
Loaded load(const StoreByteIo& io,StoreIdentity expected) {
  if(zero(expected))fail(Code::invalid_operation,"missing expected volume identity");
  if(io.extent()!=kVolumeHeadBytes)fail(Code::corrupt,"volume head must have its exact fixed extent");
  Bytes scratch(kStoreCommitUnit);std::optional<NativeVolumeSelection> slots[2];
  for(unsigned i=0;i<2;++i){io.read_exact(i*kStoreCommitUnit,scratch);slots[i]=decoded(scratch,i,expected);}
  if(!slots[0]&&!slots[1])fail(Code::corrupt,"no intact volume head");
  if(!slots[0])return {{*slots[1],true},1};
  if(!slots[1])return {{*slots[0],true},0};
  if(slots[0]->epoch==slots[1]->epoch) {
    if(*slots[0]!=*slots[1])fail(Code::corrupt,"same-epoch volume heads disagree");
    return {{*slots[1],false},1};
  }
  const unsigned newer=slots[0]->epoch>slots[1]->epoch?0:1,older=1-newer;
  const auto& n=*slots[newer];const auto& o=*slots[older];
  if(n.storage!=o.storage || n.epoch-o.epoch!=1||n.parent_identity!=o.image_identity||
      n.parent_checkpoint<o.minimum_checkpoint||n.namespace_sequence<o.namespace_sequence)
    fail(Code::corrupt,"volume head transition is not a bound successor");
  return {{n,true},newer};
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
NativeVolumeSelection initial(StoreIdentity volume,const NativeFilesystemSnapshot& source) {
  NativeVolumeSelection s;s.volume_identity=volume;s.image_identity=source.identity();s.epoch=1;
  s.storage=source.image_storage();
  s.minimum_checkpoint=source.storage_info().checkpoint_generation;s.namespace_sequence=source.sequence();
  valid(s,Code::invalid_operation);return s;
}
void write_slot(StoreByteIo& io,const NativeVolumeSelection& s,unsigned slot) {
  const auto bytes=encoded(s,slot);io.write_exact(slot*kStoreCommitUnit,bytes);io.flush();
}
}
struct NativeVolumeHeadView::Impl {
  NativeVolumeHeadInfo info;NativeFilesystemSnapshot filesystem;
  Impl(NativeVolumeHeadInfo i,NativeFilesystemSnapshot f):info(i),filesystem(std::move(f)){}
};
NativeVolumeHeadView::NativeVolumeHeadView(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeVolumeHeadView::NativeVolumeHeadView(NativeVolumeHeadView&&) noexcept=default;
NativeVolumeHeadView& NativeVolumeHeadView::operator=(NativeVolumeHeadView&&) noexcept=default;
NativeVolumeHeadView::~NativeVolumeHeadView()=default;
NativeVolumeHeadView NativeVolumeHeadView::open(std::shared_ptr<StoreByteIo> head,StoreIdentity volume,
    const NativeVolumeImageResolver& resolve) {
  return open_impl(std::move(head),volume,resolve,false);
}
NativeVolumeHeadView NativeVolumeHeadView::open_indexed_recovery(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,const NativeVolumeImageResolver& resolve) {
  return open_impl(std::move(head),volume,resolve,true);
}
NativeVolumeHeadView NativeVolumeHeadView::open_impl(std::shared_ptr<StoreByteIo> head,StoreIdentity volume,
    const NativeVolumeImageResolver& resolve,bool indexed_recovery) {
  if(!head||!resolve)fail(Code::invalid_operation,"missing volume head/image resolver");
  try {
    const auto selected=load(*head,volume).info;
    if(indexed_recovery && selected.selection.storage.kind!=NativeImageKind::indexed_paged)
      fail(Code::invalid_operation,"indexed recovery requires the indexed volume head kind");
    auto image=resolve(selected.selection.image_identity);
    if(!image||image==head)fail(Code::corrupt,"missing or aliased selected image");
    auto view=NativeFilesystemSnapshot::open_image(std::move(image),selected.selection.storage);
    bind(selected.selection,view,false,indexed_recovery);
    return NativeVolumeHeadView(std::make_unique<Impl>(selected,std::move(view)));
  }catch(const std::bad_alloc&){fail(Code::limit,"volume head reader allocation");}
  catch(const NativeFsError&){throw;}
  catch(const std::exception&){fail(Code::storage_failure,"volume head/image read failed; no fallback");}
}
NativeVolumeHeadInfo NativeVolumeHeadView::info() const noexcept{return impl_?impl_->info:NativeVolumeHeadInfo{};}
const NativeFilesystemSnapshot& NativeVolumeHeadView::filesystem() const {
  if(!impl_)fail(Code::invalid_operation,"moved-from volume view");return impl_->filesystem;
}
struct NativeVolumeHeadWriter::Impl {
  struct Lease {
    std::shared_ptr<StoreByteIo> io;
    explicit Lease(std::shared_ptr<StoreByteIo> p):io(std::move(p)) {
      if(!io||!io->acquire_writer())fail(Code::invalid_operation,"volume backend already has a live writer");
    }
    ~Lease(){io->release_writer();}
    Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
  };
  Lease owner;NativeVolumeSelection selected{};bool poisoned=false;
  explicit Impl(std::shared_ptr<StoreByteIo> io):owner(std::move(io)){}
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
    valid(next,Code::invalid_operation);
    return next;
  }
  void check() const {
    if(poisoned)fail(Code::storage_failure,"volume head writer is terminal");
    const auto now=load(*owner.io,selected.volume_identity);
    if(now.info.degraded||now.info.selection!=selected)fail(Code::corrupt,"volume head changed outside owner");
  }
  void pair(const NativeVolumeSelection& next,unsigned first=0) {
    owner.io->admit_write(kVolumeHeadBytes);
    publish_pair(next,first);
  }
  void publish_pair(const NativeVolumeSelection& next,unsigned first=0) {
    write_slot(*owner.io,next,first);write_slot(*owner.io,next,1-first);
    const auto observed=load(*owner.io,next.volume_identity);
    if(observed.info.degraded||observed.info.selection!=next)fail(Code::corrupt,"volume head publication readback");
    selected=next;
  }
};
NativeVolumeHeadWriter::NativeVolumeHeadWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
NativeVolumeHeadWriter::NativeVolumeHeadWriter(NativeVolumeHeadWriter&&) noexcept=default;
NativeVolumeHeadWriter& NativeVolumeHeadWriter::operator=(NativeVolumeHeadWriter&&) noexcept=default;
NativeVolumeHeadWriter::~NativeVolumeHeadWriter()=default;
NativeVolumeHeadWriter NativeVolumeHeadWriter::create(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,const NativeFilesystemSnapshot& source) {
  const auto next=initial(volume,source);auto image=Impl::backing(source);
  if(head==image)fail(Code::invalid_operation,"volume head aliases image");
  auto p=std::make_unique<Impl>(std::move(head));Impl::Lease image_lease(image);
  contract(*p->owner.io);contract(*image);
  if(p->owner.io->extent())fail(Code::invalid_operation,"volume head requires a new empty backend");
  p->owner.io->admit_write(kVolumeHeadBytes);
  bind(next,source,true);auto before=Impl::fresh(image,next.storage);Impl::exact(source,before);
  image->flush();auto after=Impl::fresh(image,next.storage);Impl::exact(source,after);
  // Establish complete zero extent before the first self-contained selector.
  Bytes zeroes(kStoreCommitUnit);
  p->owner.io->write_exact(0,zeroes);p->owner.io->write_exact(kStoreCommitUnit,zeroes);
  p->pair(next);
  return NativeVolumeHeadWriter(std::move(p));
}
NativeVolumeHeadWriter NativeVolumeHeadWriter::resume(std::shared_ptr<StoreByteIo> head,
    StoreIdentity volume,std::uint64_t epoch,const NativeFilesystemSnapshot& image) {
  auto p=std::make_unique<Impl>(std::move(head));contract(*p->owner.io);
  const auto current=load(*p->owner.io,volume);
  if(!epoch||epoch!=current.info.selection.epoch)fail(Code::invalid_operation,"stale volume head epoch");
  auto backing=Impl::backing(image);
  if(backing==p->owner.io)fail(Code::invalid_operation,"volume head aliases image");
  Impl::Lease image_lease(backing);contract(*backing);
  // A paged head may be resumed over independently CPU-readable degraded
  // data solely to select a verified repaired copy. Paged writable resume
  // still refuses lost payload replicas; no damaged source is repaired here.
  bind(current.info.selection,image,current.info.selection.storage.kind==NativeImageKind::flat);
  auto before=Impl::fresh(backing,current.info.selection.storage);Impl::exact(image,before);
  backing->flush();auto after=Impl::fresh(backing,current.info.selection.storage);Impl::exact(image,after);
  p->selected=current.info.selection;p->owner.io->admit_write(kVolumeHeadBytes);
  // Repair peer FIRST, preserving the selected durable survivor.
  if(current.info.degraded)p->pair(p->selected,1-current.selected_slot);
  else {p->owner.io->flush();p->check();}
  return NativeVolumeHeadWriter(std::move(p));
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::publish_compacted(
    const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& target) {
  if(!impl_||impl_->poisoned)fail(Code::storage_failure,"volume head writer is terminal");
  auto& p=*impl_;const auto source_io=Impl::backing(source),target_io=Impl::backing(target);
  const auto next=p.successor(source,target);
  Impl::Lease source_lease(source_io),target_lease(target_io);
  bool mutation=false;
  try {
    p.check();contract(*source_io);contract(*target_io);p.owner.io->admit_write(kVolumeHeadBytes);
    bind(p.selected,source,p.selected.storage.kind==NativeImageKind::flat);
    const auto target_info=target.storage_info();
    if(!target_info.replicated_checkpoint||target_info.degraded_checkpoint||target_info.degraded_root)
      fail(Code::invalid_operation,"compacted target needs complete replicated metadata");
    auto old=Impl::fresh(source_io,source.image_storage()),candidate=Impl::fresh(target_io,target.image_storage());
    Impl::exact(source,old);Impl::exact(target,candidate);
    if(candidate.storage_info().degraded_checkpoint || candidate.storage_info().degraded_root)
      fail(Code::invalid_operation,"handoff target replicas are degraded");
    source.verify_compacted_equivalent_for_volume_head(target);
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
    const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& target) {
  if(!impl_ || impl_->poisoned)fail(Code::storage_failure,"volume head writer is terminal");
  auto& p=*impl_;
  if(p.selected.storage.kind!=NativeImageKind::indexed_paged || source.sequence()!=target.sequence())
    fail(Code::invalid_operation,"staged selection requires an equivalent indexed image");
  const auto next=p.successor(source,target);
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
    p.owner.io->admit_write(kVolumeHeadBytes);
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
