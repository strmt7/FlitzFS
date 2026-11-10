#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include "store_file.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
using detail::Digest;
constexpr std::uint64_t kDataStart=2*kStoreCommitUnit;
constexpr std::size_t kRootBytes=4096,kDataHeader=128,kManifestHeader=64,kEntryBytes=64;
constexpr std::array<std::byte,8> kRootMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'R'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
constexpr std::array<std::byte,8> kDataMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'D'},std::byte{'A'},std::byte{'T'},std::byte{'1'}};
constexpr std::array<std::byte,8> kIndexMagic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'I'},std::byte{'D'},std::byte{'X'},std::byte{'1'}};
[[noreturn]] void fail(const char* reason) { throw std::runtime_error(std::string("FlitzFS object store: ")+reason); }
template<std::size_t N> bool nonzero(const std::array<std::byte,N>& a) {
  return std::any_of(a.begin(),a.end(),[](auto b){return b!=std::byte{0};});
}
void admit_identity(StoreIdentity id) { if(!nonzero(id)) fail("nonzero store identity required"); }
void admit_contract(StoreIoContract c) {
  const auto n=c.write_failure_unit_bytes;
  if(!c.successful_flush_persists || n<512 || n>kStoreCommitUnit || (n&(n-1)) || kStoreCommitUnit%n)
    fail("unknown or unsupported durability contract");
}
void put32(std::span<std::byte> b,std::size_t at,std::uint32_t n) {
  for(unsigned i=0;i<4;++i) b[at+i]=static_cast<std::byte>(n>>(8*i));
}
void put64(std::span<std::byte> b,std::size_t at,std::uint64_t n) {
  for(unsigned i=0;i<8;++i) b[at+i]=static_cast<std::byte>(n>>(8*i));
}
std::uint32_t get32(std::span<const std::byte> b,std::size_t at) {
  std::uint32_t n=0;for(unsigned i=0;i<4;++i) n|=std::uint32_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
std::uint64_t get64(std::span<const std::byte> b,std::size_t at) {
  std::uint64_t n=0;for(unsigned i=0;i<8;++i) n|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
bool zero(std::span<const std::byte> b) { return std::all_of(b.begin(),b.end(),[](auto x){return x==std::byte{0};}); }
template<std::size_t N> std::array<std::byte,N> field(std::span<const std::byte> b,std::size_t at) {
  std::array<std::byte,N> a{};std::copy_n(b.begin()+static_cast<std::ptrdiff_t>(at),N,a.begin());return a;
}
template<std::size_t N> void copy_field(std::span<std::byte> b,std::size_t at,const std::array<std::byte,N>& a) {
  std::copy(a.begin(),a.end(),b.begin()+static_cast<std::ptrdiff_t>(at));
}
bool magic(std::span<const std::byte> b,const std::array<std::byte,8>& expected) {
  return b.size()>=8 && std::equal(expected.begin(),expected.end(),b.begin());
}
std::uint64_t aligned(std::uint64_t n,std::uint64_t unit=kStoreCommitUnit) {
  return (n+unit-1)/unit*unit;
}
bool metadata_unit_valid(std::uint32_t unit) {
  return unit>=kRootBytes && unit<=kStoreCommitUnit && !(unit&(unit-1)) && kStoreCommitUnit%unit==0;
}
std::uint32_t compact_metadata_unit(StoreIoContract contract) {
  admit_contract(contract);
  return (std::max)(static_cast<std::uint32_t>(kRootBytes),contract.write_failure_unit_bytes);
}

struct Entry {
  std::uint64_t id=0,generation=0,offset=0;
  std::uint32_t bytes=0,stored=0;
  Digest digest{};
};
struct State {
  std::shared_ptr<StoreByteIo> image;
  StoreIdentity id{};
  std::uint64_t generation=0,end=0;
  std::vector<Entry> entries;
  bool degraded=false;
  std::shared_ptr<detail::StoreCodecDecoder> decoder=std::make_shared<detail::StoreCodecDecoder>();
  std::uint64_t index_offset=0;
  unsigned selected_slot=0;
  bool replicated=false,degraded_checkpoint=false;
  std::uint64_t peer_index_offset=0,checkpoint_generation=0;
  bool compact_metadata=false;
  std::uint32_t metadata_unit=kStoreCommitUnit;
};
void admit_state_contract(const State& state) {
  const auto contract=state.image->contract();admit_contract(contract);
  if(state.compact_metadata && contract.write_failure_unit_bytes>state.metadata_unit)
    fail("backend failure unit exceeds persisted metadata isolation");
}
StoreCommitInfo state_info(const State& s) {
  std::uint64_t total=0;for(const auto& e:s.entries) total+=e.bytes;
  std::uint64_t stored=0;for(const auto& e:s.entries) stored+=e.stored;
  return {s.generation,s.end,total,s.entries.size(),s.degraded,stored,
          s.replicated?s.checkpoint_generation:s.generation,s.replicated,s.degraded_checkpoint};
}
Bytes manifest(const State& s) {
  Bytes b(kManifestHeader+s.entries.size()*kEntryBytes);
  copy_field(b,0,kIndexMagic);put32(b,8,1);put32(b,12,static_cast<std::uint32_t>(s.entries.size()));
  put64(b,16,s.generation);put64(b,24,state_info(s).logical_bytes);copy_field(b,32,s.id);
  for(std::size_t i=0;i<s.entries.size();++i) {
    const auto at=kManifestHeader+i*kEntryBytes;const auto& e=s.entries[i];
    put64(b,at,e.id);put64(b,at+8,e.generation);put64(b,at+16,e.offset);
    put32(b,at+24,e.bytes);put32(b,at+28,static_cast<std::uint32_t>(kDataHeader)+e.stored);
    copy_field(b,at+32,e.digest);
  }
  return b;
}
Bytes root(const State& s,unsigned slot,std::uint64_t index_offset,const Bytes& index) {
  Bytes b(kRootBytes);
  copy_field(b,0,kRootMagic);put32(b,8,s.replicated?(s.compact_metadata?3U:2U):1U);put32(b,12,slot);put64(b,16,s.generation);
  put64(b,24,s.end);put64(b,32,index_offset);put32(b,40,static_cast<std::uint32_t>(index.size()));
  put32(b,44,static_cast<std::uint32_t>(s.entries.size()));copy_field(b,48,s.id);
  copy_field(b,64,detail::record_digest(index));
  if(s.replicated){put64(b,96,s.peer_index_offset);put64(b,104,s.checkpoint_generation);}
  if(s.compact_metadata)put32(b,112,s.metadata_unit);
  copy_field(b,kRootBytes-32,detail::record_digest(std::span<const std::byte>(b).first(kRootBytes-32)));
  return b;
}
struct Root {
  StoreIdentity id{};
  std::uint64_t generation=0,end=0,index=0;
  std::uint32_t index_bytes=0,count=0;
  Digest digest{};
  bool replicated=false;
  std::uint64_t peer_index=0,checkpoint_generation=0;
  bool compact_metadata=false;
  std::uint32_t metadata_unit=kStoreCommitUnit;
};
std::optional<Root> read_root(StoreByteIo& image,unsigned slot,std::uint64_t extent) {
  Bytes b(kRootBytes);image.read_exact(slot*kStoreCommitUnit,b);
  if(field<32>(b,kRootBytes-32)!=detail::record_digest(std::span<const std::byte>(b).first(kRootBytes-32))) return {};
  // A valid digest with unsupported/invalid semantics is not a torn candidate.
  // Refuse it explicitly rather than silently rolling back to an older root.
  const auto format=get32(b,8);
  if(!magic(b,kRootMagic) || (format!=1 && format!=2 && format!=3)) fail("unsupported intact root format");
  const auto reserved=format==3?116U:format==2?112U:96U;
  if(get32(b,12)!=slot || !zero(std::span<const std::byte>(b).subspan(reserved,kRootBytes-32-reserved)))
    fail("intact root identity/reserved fields invalid");
  Root r{field<16>(b,48),get64(b,16),get64(b,24),get64(b,32),get32(b,40),get32(b,44),field<32>(b,64)};
  r.replicated=format>=2;r.compact_metadata=format==3;
  if(r.replicated){r.peer_index=get64(b,96);r.checkpoint_generation=get64(b,104);}
  if(r.compact_metadata) {
    r.metadata_unit=get32(b,112);
    if(!metadata_unit_valid(r.metadata_unit))fail("intact compact metadata unit invalid");
  }
  if(!nonzero(r.id) || (r.generation && r.generation%2!=slot) ||
      r.end<kDataStart+(r.compact_metadata?2*r.metadata_unit:kStoreCommitUnit) ||
      r.end>extent || r.end>kStoreMaxPhysicalBytes ||
      r.end%r.metadata_unit || r.count>kStoreMaxObjects ||
      r.index_bytes!=kManifestHeader+r.count*kEntryBytes || r.index<kDataStart ||
      r.index>r.end || r.index_bytes>r.end-r.index) fail("intact root fields outside resource/format limits");
  if(r.replicated) {
    const auto low=(std::min)(r.index,r.peer_index),high=(std::max)(r.index,r.peer_index);
    const auto span=r.compact_metadata?aligned(r.index_bytes,r.metadata_unit):kStoreCommitUnit;
    if(!r.checkpoint_generation || r.checkpoint_generation==(std::numeric_limits<std::uint64_t>::max)() ||
        (r.generation!=r.checkpoint_generation && r.generation!=r.checkpoint_generation+1) ||
        low<kDataStart || r.peer_index>r.end || r.index_bytes>r.end-r.peer_index ||
        low%r.metadata_unit || high%r.metadata_unit ||
        high-low!=span || high+span!=r.end ||
        r.index_bytes>span || span>kStoreCommitUnit ||
        (r.generation==r.checkpoint_generation ? r.index!=low : r.index!=high))
      fail("intact replicated root layout/generation invalid");
  }
  return r;
}
State load(std::shared_ptr<StoreByteIo> image) {
  if(!image) fail("missing backing image");
  const auto extent=image->extent();
  if(extent<kDataStart+2*kRootBytes || extent>kStoreMaxPhysicalBytes) fail("physical image budget or truncation");
  const auto a=read_root(*image,0,extent),b=read_root(*image,1,extent);
  if(!a && !b) fail("no valid recovery root");
  if(a && b) {
    if(a->id!=b->id) fail("conflicting store identities");
    const auto high=(std::max)(a->generation,b->generation),low=(std::min)(a->generation,b->generation);
    if(high-low>1 || (high==low && high!=0)) fail("inconsistent root generations");
    if(high==0 && (a->index!=b->index || a->end!=b->end || a->digest!=b->digest)) fail("conflicting initial roots");
  }
  const bool choose_a=a && (!b || a->generation>=b->generation);
  const auto& r=choose_a ? *a : *b;
  State s{std::move(image),r.id,r.generation,r.end,{},!(a && b)};
  s.index_offset=r.index;s.selected_slot=choose_a?0U:1U;
  s.replicated=r.replicated;s.peer_index_offset=r.peer_index;s.checkpoint_generation=r.checkpoint_generation;
  s.compact_metadata=r.compact_metadata;s.metadata_unit=r.metadata_unit;
  const auto& other=choose_a?b:a;
  if(!r.replicated && other && other->replicated)fail("intact checkpoint format downgrade");
  if(other && ((!r.compact_metadata && other->compact_metadata) ||
      (r.compact_metadata && other->compact_metadata && r.metadata_unit!=other->metadata_unit)))
    fail("intact compact metadata downgrade/unit conflict");
  auto candidate=[&](std::uint64_t at,std::uint64_t generation)->std::optional<Bytes> {
    Bytes bytes(r.index_bytes);
    try{s.image->read_exact(at,bytes);}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){return {};}
    if(get64(bytes,16)!=generation)return {};
    put64(bytes,16,r.generation);
    if(detail::record_digest(bytes)!=r.digest)return {};
    return bytes;
  };
  Bytes index;
  if(!r.replicated) {
    index.resize(r.index_bytes);s.image->read_exact(r.index,index);
    if(detail::record_digest(index)!=r.digest)fail("latest root has corrupt manifest; refusing silent rollback");
  } else {
    const auto peer_generation=r.generation==r.checkpoint_generation?r.checkpoint_generation+1:r.checkpoint_generation;
    auto own=candidate(r.index,r.generation),peer=candidate(r.peer_index,peer_generation);
    if(!own && !peer)fail("both current manifest replicas invalid; refusing older checkpoint");
    index=own?std::move(*own):std::move(*peer);
    const bool paired=other && other->replicated &&
        other->compact_metadata==r.compact_metadata && other->metadata_unit==r.metadata_unit &&
        other->checkpoint_generation==r.checkpoint_generation &&
        other->index==r.peer_index && other->peer_index==r.index &&
        other->end==r.end && other->count==r.count && other->index_bytes==r.index_bytes;
    if(other && r.generation!=r.checkpoint_generation && !paired)
      fail("conflicting intact replicated root pair");
    if(paired) {
      auto matching=index;put64(matching,16,other->generation);
      if(detail::record_digest(matching)!=other->digest)fail("replicated roots name different object sets");
    }
    s.degraded_checkpoint=!paired || !own || !peer;
  }
  if(!magic(index,kIndexMagic) || get32(index,8)!=1 || get32(index,12)!=r.count ||
      get64(index,16)!=r.generation || field<16>(index,32)!=r.id ||
      !zero(std::span<const std::byte>(index).subspan(48,16))) fail("manifest identity/framing");
  s.entries.reserve(r.count);
  std::uint64_t total=0,previous=0;
  const auto data_limit=r.replicated?(std::min)(r.index,r.peer_index):r.index;
  for(std::size_t i=0;i<r.count;++i) {
    const auto at=kManifestHeader+i*kEntryBytes;
    Entry e{get64(index,at),get64(index,at+8),get64(index,at+16),get32(index,at+24),0,field<32>(index,at+32)};
    const auto record_bytes=get32(index,at+28);
    if(record_bytes<kDataHeader || record_bytes-kDataHeader>e.bytes) fail("manifest stored-byte budget");
    e.stored=record_bytes-static_cast<std::uint32_t>(kDataHeader);
    if(!e.id || e.id<=previous || !e.generation || e.generation>r.generation ||
        e.bytes>kStoreMaxObjectBytes ||
        e.offset<kDataStart || e.offset>data_limit || kDataHeader+e.stored>data_limit-e.offset)
      fail("manifest entry extent or ordering");
    total+=e.bytes;previous=e.id;s.entries.push_back(e);
  }
  if(total!=get64(index,24) || total>kStoreMaxLogicalBytes) fail("manifest logical budget");
  std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;ranges.reserve(s.entries.size());
  for(const auto& e:s.entries)ranges.emplace_back(e.offset,e.offset+kDataHeader+e.stored);
  std::sort(ranges.begin(),ranges.end());
  for(std::size_t i=1;i<ranges.size();++i)
    if(ranges[i].first<ranges[i-1].second)fail("overlapping object records");
  return s;
}
struct StoredRecord {std::array<std::byte,kDataHeader> header{};Bytes payload;};
StoredRecord read_record(const State& s,const Entry& e) {
  StoredRecord record;auto& h=record.header;s.image->read_exact(e.offset,h);
  const auto codec=get32(h,12);
  if(!magic(h,kDataMagic) || get32(h,8)!=1 || get64(h,16)!=e.id ||
      get64(h,24)!=e.generation || get32(h,32)!=e.bytes || get32(h,36)!=e.stored ||
      field<16>(h,40)!=s.id || !detail::store_codec_framing(codec,e.bytes,e.stored) ||
      !zero(std::span<const std::byte>(h).subspan(codec==detail::kStoreRaw?56:88,codec==detail::kStoreRaw?40:8)) ||
      field<32>(h,96)!=e.digest) fail("object identity, codec or framing");
  record.payload.resize(e.stored);
  if(!record.payload.empty())s.image->read_exact(e.offset+kDataHeader,record.payload);
  if(detail::record_digest(std::span<const std::byte>(h).first(96),record.payload)!=e.digest) fail("object stored digest mismatch");
  return record;
}
Bytes read_object(const State& s,const Entry& e) {
  auto record=read_record(s,e);const auto codec=get32(record.header,12);
  if(codec==detail::kStoreRaw)return std::move(record.payload); // Historical raw bytes unchanged.
  auto decoded=s.decoder->decode(codec,record.payload,e.bytes);
  if(detail::record_digest(decoded)!=field<32>(record.header,56)) fail("object decoded digest mismatch");
  return decoded;
}
bool same_state(const State& a,const State& b) {
  if(a.id!=b.id || a.generation!=b.generation || a.end!=b.end ||
      a.index_offset!=b.index_offset || a.selected_slot!=b.selected_slot || a.degraded!=b.degraded ||
      a.replicated!=b.replicated || a.degraded_checkpoint!=b.degraded_checkpoint ||
      a.peer_index_offset!=b.peer_index_offset || a.checkpoint_generation!=b.checkpoint_generation ||
      a.compact_metadata!=b.compact_metadata || a.metadata_unit!=b.metadata_unit ||
      a.entries.size()!=b.entries.size())return false;
  for(std::size_t i=0;i<a.entries.size();++i) {
    const auto& x=a.entries[i];const auto& y=b.entries[i];
    if(x.id!=y.id || x.generation!=y.generation || x.offset!=y.offset ||
        x.bytes!=y.bytes || x.stored!=y.stored || x.digest!=y.digest)return false;
  }
  return true;
}
void verify(const State& s) { for(const auto& e:s.entries) (void)read_object(s,e); }
void write_zeros(StoreByteIo& image,std::uint64_t offset,std::uint64_t end) {
  const std::array<std::byte,4096> zeroes{};
  while(offset<end) {
    const auto count=static_cast<std::size_t>((std::min)(end-offset,std::uint64_t(zeroes.size())));
    image.write_exact(offset,std::span<const std::byte>(zeroes).first(count));offset+=count;
  }
}

struct ReplicatedPlan {
  State first,last;
  Bytes first_index,last_index,first_root,last_root;
  std::uint64_t data_end=0;
};
ReplicatedPlan replicated_plan(State first,std::uint64_t data_end) {
  if(first.generation==0 || first.generation==(std::numeric_limits<std::uint64_t>::max)())
    fail("replicated checkpoint generation exhausted");
  first.replicated=true;first.degraded=false;first.degraded_checkpoint=false;
  first.checkpoint_generation=first.generation;
  if(!metadata_unit_valid(first.metadata_unit))fail("invalid planned metadata unit");
  const auto index_bytes=kManifestHeader+first.entries.size()*kEntryBytes;
  const auto span=first.compact_metadata?aligned(index_bytes,first.metadata_unit):kStoreCommitUnit;
  first.index_offset=aligned(data_end,first.metadata_unit);first.peer_index_offset=first.index_offset+span;
  first.end=first.peer_index_offset+span;
  if(first.end>kStoreMaxPhysicalBytes)fail("replicated checkpoint physical history budget");
  first.selected_slot=static_cast<unsigned>(first.generation%2);
  State last=first;++last.generation;std::swap(last.index_offset,last.peer_index_offset);
  last.selected_slot=static_cast<unsigned>(last.generation%2);
  auto a=manifest(first),b=manifest(last);
  auto x=root(first,first.selected_slot,first.index_offset,a),y=root(last,last.selected_slot,last.index_offset,b);
  return {std::move(first),std::move(last),std::move(a),std::move(b),std::move(x),std::move(y),data_end};
}
void publish_replicated(const ReplicatedPlan& plan,StoreByteIo& image) {
  write_zeros(image,plan.data_end,plan.first.index_offset);
  image.write_exact(plan.first.index_offset,plan.first_index);
  write_zeros(image,plan.first.index_offset+plan.first_index.size(),plan.last.index_offset);
  image.write_exact(plan.last.index_offset,plan.last_index);
  write_zeros(image,plan.last.index_offset+plan.last_index.size(),plan.last.end);
  image.flush();
  image.write_exact(plan.first.selected_slot*kStoreCommitUnit,plan.first_root);
  image.flush();
  image.write_exact(plan.last.selected_slot*kStoreCommitUnit,plan.last_root);
  image.flush();
}
}

struct ObjectStoreSnapshot::Impl { State state; };
struct ObjectStoreWriter::Impl {
  struct Lease {
    std::shared_ptr<StoreByteIo> image;
    explicit Lease(std::shared_ptr<StoreByteIo> backing):image(std::move(backing)) {
      if(!image || !image->acquire_writer()) fail("backend already has a live writer or is missing");
    }
    ~Lease(){image->release_writer();}
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
  };
  Lease lease;
  State state;bool poisoned=false;detail::StoreCodecEncoder encoder;
  std::uint64_t append_floor=0;
  bool prefer_compact_metadata=false;
  std::uint32_t preferred_metadata_unit=kStoreCommitUnit;
  Impl(StoreCompressionOptions options,std::shared_ptr<StoreByteIo> image):lease(std::move(image)),encoder(options){}
};
ObjectStoreSnapshot::ObjectStoreSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)) {}
ObjectStoreSnapshot::ObjectStoreSnapshot(ObjectStoreSnapshot&&) noexcept=default;
ObjectStoreSnapshot& ObjectStoreSnapshot::operator=(ObjectStoreSnapshot&&) noexcept=default;
ObjectStoreSnapshot::~ObjectStoreSnapshot()=default;
ObjectStoreWriter::ObjectStoreWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)) {}
ObjectStoreWriter::ObjectStoreWriter(ObjectStoreWriter&&) noexcept=default;
ObjectStoreWriter& ObjectStoreWriter::operator=(ObjectStoreWriter&&) noexcept=default;
ObjectStoreWriter::~ObjectStoreWriter()=default;
ObjectStoreSnapshot ObjectStoreSnapshot::open(std::shared_ptr<StoreByteIo> image) {
  auto p=std::make_unique<Impl>();p->state=load(std::move(image));verify(p->state);
  return ObjectStoreSnapshot(std::move(p));
}
ObjectStoreSnapshot ObjectStoreSnapshot::open_file(const std::filesystem::path& path) {
  return open(detail::read_store_file(path));
}
StoreIdentity ObjectStoreSnapshot::identity() const noexcept { return impl_ ? impl_->state.id : StoreIdentity{}; }
StoreCommitInfo ObjectStoreSnapshot::info() const noexcept { return impl_ ? state_info(impl_->state) : StoreCommitInfo{}; }
std::uint64_t ObjectStoreSnapshot::source_bytes() const {
  if(!impl_)fail("moved-from snapshot");
  const auto bytes=impl_->state.image->extent();
  if(bytes<impl_->state.end || bytes>kStoreMaxPhysicalBytes)fail("snapshot backing extent changed/out of bounds");
  return bytes;
}

std::vector<StoreObjectInfo> ObjectStoreSnapshot::list() const {
  if(!impl_) fail("moved-from snapshot");
  std::vector<StoreObjectInfo> result;result.reserve(impl_->state.entries.size());
  for(const auto& e:impl_->state.entries) result.push_back({e.id,e.generation,e.bytes,e.stored});
  return result;
}
Bytes ObjectStoreSnapshot::read(std::uint64_t id) const {
  if(!impl_) fail("moved-from snapshot");
  const auto& s=impl_->state;
  const auto it=std::lower_bound(s.entries.begin(),s.entries.end(),id,[](const Entry& e,auto key){return e.id<key;});
  if(it==s.entries.end() || it->id!=id) fail("object not found");
  return read_object(s,*it);
}
void ObjectStoreSnapshot::verify_all() const { if(!impl_) fail("moved-from snapshot");verify(impl_->state); }
std::shared_ptr<StoreByteIo> ObjectStoreSnapshot::backing_for_volume_head() const {
  if(!impl_)fail("moved-from volume image");return impl_->state.image;
}

ObjectStoreWriter ObjectStoreWriter::create(std::shared_ptr<StoreByteIo> image,StoreIdentity id,StoreCompressionOptions compression) {
  return create(std::move(image),id,compression,{});
}
ObjectStoreWriter ObjectStoreWriter::create(std::shared_ptr<StoreByteIo> image,StoreIdentity id,
    StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  admit_identity(id);detail::validate_store_compression(compression);
  auto p=std::make_unique<Impl>(compression,image);
  if(image->extent()!=0) fail("writer requires a new empty exclusive image");
  admit_contract(image->contract());
  p->prefer_compact_metadata=checkpoints.compact_metadata;
  if(checkpoints.compact_metadata)p->preferred_metadata_unit=compact_metadata_unit(image->contract());
  p->state={std::move(image),id,0,kDataStart+kStoreCommitUnit,{},false};
  p->state.index_offset=kDataStart;p->append_floor=p->state.end;
  auto& s=p->state;const auto index=manifest(s);
  s.image->admit_write(s.end);
  write_zeros(*s.image,0,kDataStart);
  s.image->write_exact(kDataStart,index);write_zeros(*s.image,kDataStart+index.size(),s.end);
  s.image->flush();
  s.image->write_exact(0,root(s,0,kDataStart,index));
  s.image->write_exact(kStoreCommitUnit,root(s,1,kDataStart,index));
  s.image->flush();
  return ObjectStoreWriter(std::move(p));
}
ObjectStoreWriter ObjectStoreWriter::resume(std::shared_ptr<StoreByteIo> image,StoreIdentity expected_id,
    std::uint64_t expected_generation,StoreCompressionOptions compression) {
  admit_identity(expected_id);detail::validate_store_compression(compression);
  auto p=std::make_unique<Impl>(compression,image);
  admit_contract(image->contract());
  auto state=load(image);verify(state);admit_state_contract(state);
  p->prefer_compact_metadata=state.compact_metadata;p->preferred_metadata_unit=state.metadata_unit;
  if(state.id!=expected_id || state.generation!=expected_generation) fail("stale recovery identity or generation");
  const auto observed_extent=image->extent();
  p->append_floor=aligned(observed_extent,state.metadata_unit);
  if(p->append_floor>kStoreMaxPhysicalBytes) fail("orphan tail exceeds physical budget");
  image->admit_write(p->append_floor);
  auto fence=[&] {
    admit_state_contract(state);
    image->flush();
    admit_state_contract(state);
    if(image->extent()!=observed_extent) fail("backend extent changed during recovery fence");
    auto checked=load(image);verify(checked);
    if(!same_state(checked,state))fail("backend recovery state changed during fence");
    state=std::move(checked);
  };
  if(state.replicated && state.degraded_checkpoint) {
    if(state.generation>(std::numeric_limits<std::uint64_t>::max)()-2)fail("replicated repair generation exhausted");
    State first=state;++first.generation;
    const auto plan=replicated_plan(std::move(first),p->append_floor);
    image->admit_write(plan.last.end);
    fence();
    publish_replicated(plan,*image);
    p->state=load(image);verify(p->state);
    if(!same_state(p->state,plan.last))fail("replicated repair did not restore a complete checkpoint");
    p->append_floor=aligned(image->extent(),p->state.metadata_unit);
    return ObjectStoreWriter(std::move(p));
  }
  if(!state.degraded) {
    // A complete latest root may have been visible before its failed final
    // flush. Adopt it only after fencing all prior backend writes durably.
    fence();
    p->state=std::move(state);
    return ObjectStoreWriter(std::move(p));
  }
  if(state.generation==0) {
    // Either initial slot can be the survivor. Repair the OTHER slot at the
    // same initial generation; positive-generation parity rules do not apply.
    const auto missing=1U-state.selected_slot;
    const auto index=manifest(state);
    const auto locator=root(state,missing,state.index_offset,index);
    fence();
    image->write_exact(missing*kStoreCommitUnit,locator);
    image->flush();
  } else {
    if(state.generation==(std::numeric_limits<std::uint64_t>::max)()) fail("recovery generation exhausted");
    State next=state;++next.generation;next.degraded=false;
    next.index_offset=p->append_floor;next.selected_slot=static_cast<unsigned>(next.generation%2);
    const auto index=manifest(next);
    next.end=aligned(next.index_offset+index.size());
    if(next.end>kStoreMaxPhysicalBytes) fail("no reserve to repair recovery root");
    const auto locator=root(next,next.selected_slot,next.index_offset,index);
    image->admit_write(next.end);
    fence(); // Revalidate the surviving baseline before touching its peer.
    image->write_exact(next.index_offset,index);
    write_zeros(*image,next.index_offset+index.size(),next.end);
    image->flush();
    image->write_exact(next.selected_slot*kStoreCommitUnit,locator);
    image->flush();
  }
  // Re-parse both repaired roots and all references before granting a writer.
  p->state=load(image);verify(p->state);
  if(p->state.degraded || p->state.id!=expected_id) fail("recovery repair did not establish a complete root pair");
  p->append_floor=aligned(image->extent(),p->state.metadata_unit);
  return ObjectStoreWriter(std::move(p));
}

ObjectStoreWriter ObjectStoreWriter::compact(const ObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions options) {
  return compact_replacing(source,std::move(target),id,0,{},options);
}
ObjectStoreWriter ObjectStoreWriter::compact(const ObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions options,
    StoreCheckpointOptions checkpoints) {
  return compact_replacing(source,std::move(target),id,0,{},options,checkpoints);
}
ObjectStoreWriter ObjectStoreWriter::compact_replacing(const ObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,std::uint64_t replacement_id,
    std::span<const std::byte> replacement,StoreCompressionOptions options,StoreCheckpointOptions checkpoints) {
  if(!source.impl_)fail("moved-from compaction source");
  admit_identity(id);detail::validate_store_compression(options);
  const auto& cached=source.impl_->state;
  if(id==cached.id || !target || target==cached.image)fail("compaction requires a distinct new identity and target");
  if(replacement.size()>kStoreMaxObjectBytes || (!replacement_id && !replacement.empty()))
    fail("invalid compaction replacement");
  // Read-only ownership for this operation uses the same per-backend lease.
  // OS/custom backend owners must additionally exclude different facades.
  Impl::Lease source_lease(cached.image);
  auto original=load(cached.image);verify(original);
  if(!same_state(original,cached))fail("compaction source is not the latest recovered snapshot");
  const auto source_extent=original.image->extent();
  if(original.generation==0) {
    if(replacement_id)fail("cannot replace an object in an empty initial source");
    return create(std::move(target),id,options,checkpoints);
  }
  if(original.generation>(std::numeric_limits<std::uint64_t>::max)()-2)
    fail("compaction generation exhausted");
  auto p=std::make_unique<Impl>(options,target);
  if(target->extent()!=0)fail("compaction requires a new empty target");
  admit_contract(target->contract());
  State baseline=original;baseline.image=target;baseline.id=id;
  if(checkpoints.compact_metadata) {
    baseline.compact_metadata=true;baseline.metadata_unit=compact_metadata_unit(target->contract());
  }
  admit_state_contract(baseline);
  p->prefer_compact_metadata=baseline.compact_metadata;p->preferred_metadata_unit=baseline.metadata_unit;
  baseline.generation=original.generation+1;baseline.degraded=false;
  baseline.decoder=std::make_shared<detail::StoreCodecDecoder>();
  std::optional<detail::EncodedStoreObject> substitute;
  if(replacement_id) {
    auto it=std::lower_bound(baseline.entries.begin(),baseline.entries.end(),replacement_id,
                            [](const Entry& e,auto key){return e.id<key;});
    if(it==baseline.entries.end() || it->id!=replacement_id)fail("compaction replacement object missing");
    const auto logical=state_info(original).logical_bytes-it->bytes+replacement.size();
    if(logical>kStoreMaxLogicalBytes)fail("compaction decoded-byte budget");
    target->admit_write(kDataStart); // Headroom before private encoding.
    substitute=p->encoder.encode(Bytes(replacement.begin(),replacement.end()));
    it->bytes=static_cast<std::uint32_t>(replacement.size());
    it->stored=static_cast<std::uint32_t>(substitute->payload.size());
    it->generation=baseline.generation;
  }
  std::uint64_t cursor=kDataStart;
  for(auto& e:baseline.entries){e.offset=cursor;cursor+=kDataHeader+e.stored;}
  auto plan=replicated_plan(baseline,cursor);
  baseline.end=plan.last.end;
  target->admit_write(baseline.end);
  // Failure before publication leaves only a partial NEW destination. Keep a
  // single record payload in scratch, rather than the whole live volume.
  write_zeros(*target,0,kDataStart);
  for(std::size_t i=0;i<baseline.entries.size();++i) {
    target->admit_write(baseline.end);
    auto& e=baseline.entries[i];StoredRecord record;
    if(e.id==replacement_id) {
      auto& h=record.header;copy_field(h,0,kDataMagic);put32(h,8,1);
      put32(h,12,substitute->codec);put64(h,16,e.id);put64(h,24,e.generation);
      put32(h,32,e.bytes);put32(h,36,e.stored);
      if(substitute->codec!=detail::kStoreRaw)copy_field(h,56,substitute->decoded_digest);
      record.payload=std::move(substitute->payload);
    } else record=read_record(original,original.entries[i]);
    copy_field(record.header,40,id);
    e.digest=detail::record_digest(std::span<const std::byte>(record.header).first(96),record.payload);
    copy_field(record.header,96,e.digest);
    target->write_exact(e.offset,record.header);
    if(!record.payload.empty())target->write_exact(e.offset+kDataHeader,record.payload);
  }
  // Independently decode target bytes before exposing any root. Recheck that
  // the exclusively owned source still matches the selected baseline.
  verify(baseline);
  auto checked=load(original.image);verify(checked);
  if(original.image->extent()!=source_extent || !same_state(original,checked))
    fail("compaction source changed during copy");
  // Record digests were rebound while streaming; finish the replica plan only
  // after those exact digests are known, before publishing any locator.
  plan=replicated_plan(std::move(baseline),cursor);
  target->admit_write(plan.last.end);
  publish_replicated(plan,*target);
  p->state=load(target);verify(p->state);admit_state_contract(p->state);
  if(!same_state(p->state,plan.last))fail("compaction publication readback differs");
  p->append_floor=p->state.end;
  return ObjectStoreWriter(std::move(p));
}

ObjectStoreWriter ObjectStoreWriter::create_file(const std::filesystem::path& path,StoreIdentity id,StoreIoContract c,StoreCompressionOptions compression) {
  admit_identity(id);admit_contract(c);detail::validate_store_compression(compression);
  return create(detail::new_store_file(path,c),id,compression);
}
ObjectStoreWriter ObjectStoreWriter::create_file(const std::filesystem::path& path,StoreIdentity id,
    StoreIoContract c,StoreCompressionOptions compression,StoreCheckpointOptions checkpoints) {
  admit_identity(id);admit_contract(c);detail::validate_store_compression(compression);
  return create(detail::new_store_file(path,c),id,compression,checkpoints);
}
StoreCommitInfo ObjectStoreWriter::commit(std::span<const StoreMutation> changes) {
  return commit_impl(changes,impl_ && (impl_->state.replicated || impl_->prefer_compact_metadata));
}
StoreCommitInfo ObjectStoreWriter::commit_replicated(std::span<const StoreMutation> changes) {
  return commit_impl(changes,true);
}
StoreCommitInfo ObjectStoreWriter::commit_impl(std::span<const StoreMutation> changes,bool replicated) {
  if(!impl_) fail("moved-from writer");
  if(impl_->poisoned) fail("writer poisoned; destroy it before independent recovery");
  if(changes.empty() || changes.size()>kStoreMaxObjects) fail("mutation count must be 1..256");
  std::uint64_t input_bytes=0;
  for(std::size_t i=0;i<changes.size();++i) {
    const auto& m=changes[i];
    if(!m.id || m.bytes.size()>kStoreMaxObjectBytes || (m.erase && !m.bytes.empty())) fail("invalid mutation");
    for(std::size_t j=0;j<i;++j) if(changes[j].id==m.id) fail("duplicate mutation ID");
    input_bytes+=m.bytes.size();
    if(input_bytes>kStoreMaxTransactionBytes) fail("transaction memory budget");
  }
  const auto& old=impl_->state;
  try {
    admit_state_contract(old);
    if(impl_->prefer_compact_metadata && old.image->contract().write_failure_unit_bytes>impl_->preferred_metadata_unit)
      fail("backend failure unit changed before compact metadata publication");
  }catch(...){impl_->poisoned=true;throw;}
  if(old.generation>(std::numeric_limits<std::uint64_t>::max)()-(replicated?2U:1U)) fail("generation exhausted");
  // Admit the FINAL inventory before compression/private payload copies. Puts
  // may precede erases in the supplied batch without transient count failures.
  auto count=old.entries.size();auto logical=state_info(old).logical_bytes;
  for(const auto& m:changes) {
    const auto it=std::lower_bound(old.entries.begin(),old.entries.end(),m.id,[](const Entry& e,auto key){return e.id<key;});
    const bool exists=it!=old.entries.end() && it->id==m.id;
    if(m.erase && !exists)fail("cannot erase missing object");
    if(exists){--count;logical-=it->bytes;}
    if(!m.erase){++count;logical+=m.bytes.size();}
  }
  if(count>kStoreMaxObjects)fail("object-count budget");
  if(logical>kStoreMaxLogicalBytes)fail("live decoded-byte budget");
  old.image->admit_write(impl_->append_floor); // Admit headroom before private copies or codec work.
  if(replicated) {
    // A newly lost root must not turn the next peer write into destruction of
    // the sole survivor. Recheck metadata, not all unchanged data payloads.
    try {
      const auto checked=load(old.image);
      if(!same_state(checked,old) || checked.degraded_checkpoint)
        fail("checkpoint changed/degraded; independent recovery required");
    }catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){impl_->poisoned=true;throw;}
  }
  State next=old;next.generation++;next.degraded=false;
  if(impl_->prefer_compact_metadata) {
    next.compact_metadata=true;next.metadata_unit=impl_->preferred_metadata_unit;
  }
  struct Prepared {std::uint64_t offset;std::array<std::byte,kDataHeader> header;Bytes data;};
  std::vector<Prepared> prepared;prepared.reserve(changes.size());
  auto cursor=impl_->append_floor;
  // Private copies and every admission check precede the first backing write.
  for(const auto& m:changes) {
    auto it=std::lower_bound(next.entries.begin(),next.entries.end(),m.id,[](const Entry& e,auto key){return e.id<key;});
    if(m.erase) {
      if(it==next.entries.end() || it->id!=m.id) fail("cannot erase missing object");
      next.entries.erase(it);continue;
    }
    auto encoded=impl_->encoder.encode(m.bytes);
    Prepared record{cursor,{},std::move(encoded.payload)};
    auto& h=record.header;
    copy_field(h,0,kDataMagic);put32(h,8,1);put32(h,12,encoded.codec);put64(h,16,m.id);put64(h,24,next.generation);
    put32(h,32,static_cast<std::uint32_t>(m.bytes.size()));put32(h,36,static_cast<std::uint32_t>(record.data.size()));
    copy_field(h,40,next.id);
    if(encoded.codec!=detail::kStoreRaw) copy_field(h,56,encoded.decoded_digest);
    const auto digest=detail::record_digest(std::span<const std::byte>(h).first(96),record.data);
    copy_field(h,96,digest);
    Entry e{m.id,next.generation,cursor,static_cast<std::uint32_t>(m.bytes.size()),static_cast<std::uint32_t>(record.data.size()),digest};
    if(it!=next.entries.end() && it->id==m.id) *it=e;else next.entries.insert(it,e);
    cursor+=kDataHeader+record.data.size();prepared.push_back(std::move(record));
  }
  if(next.entries.size()>kStoreMaxObjects) fail("object-count budget");
  if(replicated) {
    auto plan=replicated_plan(std::move(next),cursor);
    plan.last.image->admit_write(plan.last.end);
    try {
      for(const auto& p:prepared) {
        plan.last.image->write_exact(p.offset,p.header);
        if(!p.data.empty())plan.last.image->write_exact(p.offset+kDataHeader,p.data);
      }
      publish_replicated(plan,*plan.last.image);
    }catch(...){impl_->poisoned=true;throw;}
    impl_->state=std::move(plan.last);impl_->append_floor=impl_->state.end;
    return state_info(impl_->state);
  }
  const auto index=manifest(next);
  const auto index_offset=cursor;next.index_offset=index_offset;
  next.selected_slot=static_cast<unsigned>(next.generation%2);
  next.end=aligned(cursor+index.size());
  if(next.end>kStoreMaxPhysicalBytes) fail("physical history budget; compaction not implemented");
  const auto locator=root(next,static_cast<unsigned>(next.generation%2),index_offset,index);
  next.image->admit_write(next.end); // No I/O mutation; a pressure rejection leaves writer usable.
  try {
    for(const auto& p:prepared) {
      next.image->write_exact(p.offset,p.header);
      if(!p.data.empty()) next.image->write_exact(p.offset+kDataHeader,p.data);
    }
    next.image->write_exact(index_offset,index);
    write_zeros(*next.image,index_offset+index.size(),next.end);
    next.image->flush(); // Dependencies durable before root publication.
    next.image->write_exact((next.generation%2)*kStoreCommitUnit,locator);
    next.image->flush(); // Acknowledge ONLY after publication durability.
  } catch(...) {impl_->poisoned=true;throw;}
  impl_->state=std::move(next);impl_->append_floor=impl_->state.end;
  return state_info(impl_->state);
}
StoreCommitInfo ObjectStoreWriter::flush_checkpoint() {
  if(!impl_)fail("moved-from writer");
  if(impl_->poisoned)fail("writer poisoned; destroy it before independent recovery");
  const auto& old=impl_->state;
  bool barrier_entered=false;
  try {
    const auto extent=old.image->extent();
    if(extent<old.end || extent>kStoreMaxPhysicalBytes)
      fail("checkpoint extent changed/out of bounds");
    auto check=[&] {
      admit_state_contract(old);
      const auto current=load(old.image);
      if(!same_state(current,old) || current.degraded || current.degraded_checkpoint ||
          old.image->extent()!=extent)
        fail("checkpoint changed/degraded during flush; independent recovery required");
    };
    check();
    const auto result=state_info(old); // Allocation-free result formed before the barrier.
    barrier_entered=true;
    old.image->flush();
    check();
    return result;
  }catch(const std::bad_alloc&) {
    if(barrier_entered)impl_->poisoned=true;
    throw;
  }catch(...) {impl_->poisoned=true;throw;}
}
ObjectStoreSnapshot ObjectStoreWriter::snapshot_for_staging() const {
  if(!impl_ || impl_->poisoned)fail("staging requires a healthy writer");
  auto p=std::make_unique<ObjectStoreSnapshot::Impl>();p->state=impl_->state;
  return ObjectStoreSnapshot(std::move(p));
}
ObjectStoreSnapshot ObjectStoreWriter::snapshot() const {
  if(!impl_) fail("moved-from writer");
  auto p=std::make_unique<ObjectStoreSnapshot::Impl>();p->state=impl_->state;
  p->state.degraded=p->state.degraded || impl_->poisoned;
  p->state.degraded_checkpoint=p->state.degraded_checkpoint || (p->state.replicated && impl_->poisoned);
  verify(p->state);return ObjectStoreSnapshot(std::move(p));
}
bool ObjectStoreWriter::poisoned() const noexcept { return !impl_ || impl_->poisoned; }
} // namespace flitzfs
