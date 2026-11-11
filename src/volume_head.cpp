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
  if(!s.epoch||zero(s.volume_identity)||zero(s.image_identity)||s.volume_identity==s.image_identity||
      !s.minimum_checkpoint||!s.namespace_sequence||s.namespace_sequence>s.minimum_checkpoint)
    fail(code,"invalid volume head identity/checkpoint");
  if(s.epoch==1) {
    if(!zero(s.parent_identity)||s.parent_checkpoint)fail(code,"initial volume head has parent");
  }else if(zero(s.parent_identity)||s.parent_identity==s.image_identity||
      s.parent_identity==s.volume_identity||!s.parent_checkpoint||s.parent_checkpoint>=s.minimum_checkpoint)
    fail(code,"invalid compacted volume head parent");
}
void contract(const StoreByteIo& io) {
  const auto c=io.contract();const auto u=c.write_failure_unit_bytes;
  if(!u||u>kStoreCommitUnit||(u&(u-1))||kStoreCommitUnit%u||!c.successful_flush_persists)
    fail(Code::invalid_operation,"volume head requires qualified disjoint failure units and persistence");
}
Bytes encoded(const NativeVolumeSelection& s,unsigned slot) {
  valid(s,Code::invalid_operation);Bytes b(kStoreCommitUnit);
  std::memcpy(b.data(),magic,8);put(b,8,1,4);put(b,12,slot,4);put(b,16,s.epoch);
  std::copy(s.volume_identity.begin(),s.volume_identity.end(),b.begin()+24);
  std::copy(s.image_identity.begin(),s.image_identity.end(),b.begin()+40);
  std::copy(s.parent_identity.begin(),s.parent_identity.end(),b.begin()+56);
  put(b,72,s.minimum_checkpoint);put(b,80,s.namespace_sequence);put(b,88,s.parent_checkpoint);
  const auto digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));
  std::copy(digest.begin(),digest.end(),b.begin()+digest_at);return b;
}
std::optional<NativeVolumeSelection> decoded(std::span<const std::byte> b,unsigned slot,StoreIdentity expected) {
  const auto digest=detail::record_digest(b.first(digest_at));
  if(!std::equal(digest.begin(),digest.end(),b.begin()+digest_at))return {};
  // Intact unsupported/contradictory metadata is NEVER an excuse to roll back.
  if(std::memcmp(b.data(),magic,8)||get(b,8,4)!=1||get(b,12,4)!=slot||
      !std::all_of(b.begin()+96,b.begin()+digest_at,[](auto x){return x==std::byte{};}))
    fail(Code::corrupt,"intact volume head framing unsupported");
  NativeVolumeSelection s;s.epoch=get(b,16);
  std::copy_n(b.begin()+24,16,s.volume_identity.begin());std::copy_n(b.begin()+40,16,s.image_identity.begin());
  std::copy_n(b.begin()+56,16,s.parent_identity.begin());
  s.minimum_checkpoint=get(b,72);s.namespace_sequence=get(b,80);s.parent_checkpoint=get(b,88);
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
  if(n.epoch-o.epoch!=1||n.parent_identity!=o.image_identity||
      n.parent_checkpoint<o.minimum_checkpoint||n.namespace_sequence<o.namespace_sequence)
    fail(Code::corrupt,"volume head transition is not a bound successor");
  return {{n,true},newer};
}
void bind(const NativeVolumeSelection& selected,const NativeFilesystemSnapshot& image,bool writable) {
  const auto info=image.storage_info();
  if(image.identity()!=selected.image_identity||image.sequence()<selected.namespace_sequence||
      !info.replicated_checkpoint||info.checkpoint_generation<selected.minimum_checkpoint||
      info.generation<selected.minimum_checkpoint||image.needs_session_recovery())
    fail(Code::corrupt,"selected volume image is absent, stale or unresolved");
  if(writable&&(info.degraded_checkpoint||info.degraded_root))
    fail(Code::invalid_operation,"repair selected image metadata independently before head admission");
  image.verify_all();
}
NativeVolumeSelection initial(StoreIdentity volume,const NativeFilesystemSnapshot& source) {
  NativeVolumeSelection s;s.volume_identity=volume;s.image_identity=source.identity();s.epoch=1;
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
  if(!head||!resolve)fail(Code::invalid_operation,"missing volume head/image resolver");
  try {
    const auto selected=load(*head,volume).info;auto image=resolve(selected.selection.image_identity);
    if(!image||image==head)fail(Code::corrupt,"missing or aliased selected image");
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(std::move(image)));
    bind(selected.selection,view,false);
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
    return image.objects_for_volume_head().backing_for_volume_head();
  }
  static NativeFilesystemSnapshot fresh(std::shared_ptr<StoreByteIo> io) {
    return NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(std::move(io)));
  }
  static void exact(const NativeFilesystemSnapshot& a,const NativeFilesystemSnapshot& b) {
    const auto x=a.storage_info(),y=b.storage_info();
    if(a.identity()!=b.identity()||a.sequence()!=b.sequence()||x.generation!=y.generation||
        x.checkpoint_generation!=y.checkpoint_generation||x.physical_end!=y.physical_end||
        x.degraded_root!=y.degraded_root||x.degraded_checkpoint!=y.degraded_checkpoint)
      fail(Code::invalid_operation,"handoff snapshot is not latest");
    a.verify_compacted_equivalent_for_volume_head(b);
  }
  void check() const {
    if(poisoned)fail(Code::storage_failure,"volume head writer is terminal");
    const auto now=load(*owner.io,selected.volume_identity);
    if(now.info.degraded||now.info.selection!=selected)fail(Code::corrupt,"volume head changed outside owner");
  }
  void pair(const NativeVolumeSelection& next,unsigned first=0) {
    owner.io->admit_write(kVolumeHeadBytes);
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
  bind(next,source,true);auto before=Impl::fresh(image);Impl::exact(source,before);
  image->flush();auto after=Impl::fresh(image);Impl::exact(source,after);
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
  Impl::Lease image_lease(backing);contract(*backing);bind(current.info.selection,image,true);
  auto before=Impl::fresh(backing);Impl::exact(image,before);
  backing->flush();auto after=Impl::fresh(backing);Impl::exact(image,after);
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
  if(source_io==target_io||source_io==p.owner.io||target_io==p.owner.io||
      source.identity()!=p.selected.image_identity||target.identity()==source.identity()||
      target.identity()==p.selected.volume_identity||target.identity()==p.selected.parent_identity)
    fail(Code::invalid_operation,"invalid volume handoff identities/backends");
  if(p.selected.epoch==(std::numeric_limits<std::uint64_t>::max)())
    fail(Code::limit,"volume head epoch exhausted");
  Impl::Lease source_lease(source_io),target_lease(target_io);
  bool mutation=false;
  try {
    p.check();contract(*source_io);contract(*target_io);p.owner.io->admit_write(kVolumeHeadBytes);
    bind(p.selected,source,true);
    const auto target_info=target.storage_info();
    if(!target_info.replicated_checkpoint||target_info.degraded_checkpoint||target_info.degraded_root)
      fail(Code::invalid_operation,"compacted target needs complete replicated metadata");
    auto old=Impl::fresh(source_io),candidate=Impl::fresh(target_io);
    Impl::exact(source,old);Impl::exact(target,candidate);
    source.verify_compacted_equivalent_for_volume_head(target);
    NativeVolumeSelection next;next.volume_identity=p.selected.volume_identity;next.epoch=p.selected.epoch+1;
    next.image_identity=target.identity();next.minimum_checkpoint=target.storage_info().checkpoint_generation;
    next.namespace_sequence=target.sequence();next.parent_identity=source.identity();
    next.parent_checkpoint=source.storage_info().checkpoint_generation;valid(next,Code::invalid_operation);
    mutation=true;target_io->flush();auto checked=Impl::fresh(target_io);Impl::exact(target,checked);
    // Source remains unchanged and quiescent through candidate fencing.
    auto stable_source=Impl::fresh(source_io);Impl::exact(source,stable_source);
    p.pair(next);return {p.selected,false};
  }catch(const std::bad_alloc&){if(mutation)p.poisoned=true;fail(Code::limit,"volume handoff allocation failure");}
  catch(const NativeFsError& e){if(mutation||e.code()==Code::corrupt||e.code()==Code::storage_failure)p.poisoned=true;throw;}
  catch(const std::exception&){p.poisoned=true;fail(Code::storage_failure,"volume handoff failed; retain all images");}
}
NativeVolumeHeadInfo NativeVolumeHeadWriter::info() const noexcept {
  return impl_&&!impl_->poisoned?NativeVolumeHeadInfo{impl_->selected,false}:NativeVolumeHeadInfo{{},true};
}
bool NativeVolumeHeadWriter::poisoned() const noexcept{return !impl_||impl_->poisoned;}
} // namespace flitzfs
