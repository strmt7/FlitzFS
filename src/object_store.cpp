#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include "store_file.hpp"
#include <algorithm>
#include <limits>
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
std::uint64_t aligned(std::uint64_t n) { return (n+kStoreCommitUnit-1)/kStoreCommitUnit*kStoreCommitUnit; }

struct Entry {
  std::uint64_t id=0,generation=0,offset=0;
  std::uint32_t bytes=0;
  Digest digest{};
};
struct State {
  std::shared_ptr<StoreByteIo> image;
  StoreIdentity id{};
  std::uint64_t generation=0,end=0;
  std::vector<Entry> entries;
  bool degraded=false;
};
StoreCommitInfo state_info(const State& s) {
  std::uint64_t total=0;for(const auto& e:s.entries) total+=e.bytes;
  return {s.generation,s.end,total,s.entries.size(),s.degraded};
}
Bytes manifest(const State& s) {
  Bytes b(kManifestHeader+s.entries.size()*kEntryBytes);
  copy_field(b,0,kIndexMagic);put32(b,8,1);put32(b,12,static_cast<std::uint32_t>(s.entries.size()));
  put64(b,16,s.generation);put64(b,24,state_info(s).logical_bytes);copy_field(b,32,s.id);
  for(std::size_t i=0;i<s.entries.size();++i) {
    const auto at=kManifestHeader+i*kEntryBytes;const auto& e=s.entries[i];
    put64(b,at,e.id);put64(b,at+8,e.generation);put64(b,at+16,e.offset);
    put32(b,at+24,e.bytes);put32(b,at+28,static_cast<std::uint32_t>(kDataHeader)+e.bytes);
    copy_field(b,at+32,e.digest);
  }
  return b;
}
Bytes root(const State& s,unsigned slot,std::uint64_t index_offset,const Bytes& index) {
  Bytes b(kRootBytes);
  copy_field(b,0,kRootMagic);put32(b,8,1);put32(b,12,slot);put64(b,16,s.generation);
  put64(b,24,s.end);put64(b,32,index_offset);put32(b,40,static_cast<std::uint32_t>(index.size()));
  put32(b,44,static_cast<std::uint32_t>(s.entries.size()));copy_field(b,48,s.id);
  copy_field(b,64,detail::record_digest(index));
  copy_field(b,kRootBytes-32,detail::record_digest(std::span<const std::byte>(b).first(kRootBytes-32)));
  return b;
}
struct Root {
  StoreIdentity id{};
  std::uint64_t generation=0,end=0,index=0;
  std::uint32_t index_bytes=0,count=0;
  Digest digest{};
};
std::optional<Root> read_root(StoreByteIo& image,unsigned slot,std::uint64_t extent) {
  Bytes b(kRootBytes);image.read_exact(slot*kStoreCommitUnit,b);
  if(field<32>(b,kRootBytes-32)!=detail::record_digest(std::span<const std::byte>(b).first(kRootBytes-32))) return {};
  // A valid digest with unsupported/invalid semantics is not a torn candidate.
  // Refuse it explicitly rather than silently rolling back to an older root.
  if(!magic(b,kRootMagic) || get32(b,8)!=1) fail("unsupported intact root format");
  if(get32(b,12)!=slot || !zero(std::span<const std::byte>(b).subspan(96,kRootBytes-32-96)))
    fail("intact root identity/reserved fields invalid");
  Root r{field<16>(b,48),get64(b,16),get64(b,24),get64(b,32),get32(b,40),get32(b,44),field<32>(b,64)};
  if(!nonzero(r.id) || (r.generation && r.generation%2!=slot) ||
      r.end<kDataStart+kStoreCommitUnit || r.end>extent || r.end>kStoreMaxPhysicalBytes ||
      r.end%kStoreCommitUnit || r.count>kStoreMaxObjects ||
      r.index_bytes!=kManifestHeader+r.count*kEntryBytes || r.index<kDataStart ||
      r.index>r.end || r.index_bytes>r.end-r.index) fail("intact root fields outside resource/format limits");
  return r;
}
State load(std::shared_ptr<StoreByteIo> image) {
  if(!image) fail("missing backing image");
  const auto extent=image->extent();
  if(extent<kDataStart+kStoreCommitUnit || extent>kStoreMaxPhysicalBytes) fail("physical image budget or truncation");
  const auto a=read_root(*image,0,extent),b=read_root(*image,1,extent);
  if(!a && !b) fail("no valid recovery root");
  if(a && b) {
    if(a->id!=b->id) fail("conflicting store identities");
    const auto high=(std::max)(a->generation,b->generation),low=(std::min)(a->generation,b->generation);
    if(high-low>1 || (high==low && high!=0)) fail("inconsistent root generations");
    if(high==0 && (a->index!=b->index || a->end!=b->end || a->digest!=b->digest)) fail("conflicting initial roots");
  }
  const auto& r=(a && (!b || a->generation>=b->generation)) ? *a : *b;
  State s{std::move(image),r.id,r.generation,r.end,{},!(a && b)};
  Bytes index(r.index_bytes);s.image->read_exact(r.index,index);
  if(detail::record_digest(index)!=r.digest) fail("latest root has corrupt manifest; refusing silent rollback");
  if(!magic(index,kIndexMagic) || get32(index,8)!=1 || get32(index,12)!=r.count ||
      get64(index,16)!=r.generation || field<16>(index,32)!=r.id ||
      !zero(std::span<const std::byte>(index).subspan(48,16))) fail("manifest identity/framing");
  s.entries.reserve(r.count);
  std::uint64_t total=0,previous=0;
  for(std::size_t i=0;i<r.count;++i) {
    const auto at=kManifestHeader+i*kEntryBytes;
    Entry e{get64(index,at),get64(index,at+8),get64(index,at+16),get32(index,at+24),field<32>(index,at+32)};
    if(!e.id || e.id<=previous || !e.generation || e.generation>r.generation ||
        e.bytes>kStoreMaxObjectBytes || get32(index,at+28)!=kDataHeader+e.bytes ||
        e.offset<kDataStart || e.offset>r.index || kDataHeader+e.bytes>r.index-e.offset)
      fail("manifest entry extent or ordering");
    for(const auto& old:s.entries)
      if(e.offset<old.offset+kDataHeader+old.bytes && old.offset<e.offset+kDataHeader+e.bytes)
        fail("overlapping object records");
    total+=e.bytes;previous=e.id;s.entries.push_back(e);
  }
  if(total!=get64(index,24) || total>kStoreMaxObjects*std::uint64_t(kStoreMaxObjectBytes)) fail("manifest logical budget");
  return s;
}
Bytes read_object(const State& s,const Entry& e) {
  std::array<std::byte,kDataHeader> h{};s.image->read_exact(e.offset,h);
  if(!magic(h,kDataMagic) || get32(h,8)!=1 || get32(h,12)!=0 || get64(h,16)!=e.id ||
      get64(h,24)!=e.generation || get32(h,32)!=e.bytes || get32(h,36)!=e.bytes ||
      field<16>(h,40)!=s.id || !zero(std::span<const std::byte>(h).subspan(56,40)) ||
      field<32>(h,96)!=e.digest) fail("object identity, codec or framing");
  Bytes bytes(e.bytes);if(!bytes.empty()) s.image->read_exact(e.offset+kDataHeader,bytes);
  if(detail::record_digest(std::span<const std::byte>(h).first(96),bytes)!=e.digest) fail("object content digest mismatch");
  return bytes;
}
void verify(const State& s) { for(const auto& e:s.entries) (void)read_object(s,e); }
void write_zeros(StoreByteIo& image,std::uint64_t offset,std::uint64_t end) {
  const std::array<std::byte,4096> zeroes{};
  while(offset<end) {
    const auto count=static_cast<std::size_t>((std::min)(end-offset,std::uint64_t(zeroes.size())));
    image.write_exact(offset,std::span<const std::byte>(zeroes).first(count));offset+=count;
  }
}
}

struct ObjectStoreSnapshot::Impl { State state; };
struct ObjectStoreWriter::Impl { State state;bool poisoned=false; };
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
std::vector<StoreObjectInfo> ObjectStoreSnapshot::list() const {
  if(!impl_) fail("moved-from snapshot");
  std::vector<StoreObjectInfo> result;result.reserve(impl_->state.entries.size());
  for(const auto& e:impl_->state.entries) result.push_back({e.id,e.generation,e.bytes});
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

ObjectStoreWriter ObjectStoreWriter::create(std::shared_ptr<StoreByteIo> image,StoreIdentity id) {
  admit_identity(id);
  if(!image || image->extent()!=0) fail("writer requires a new empty exclusive image");
  admit_contract(image->contract());
  auto p=std::make_unique<Impl>();p->state={std::move(image),id,0,kDataStart+kStoreCommitUnit,{},false};
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
ObjectStoreWriter ObjectStoreWriter::create_file(const std::filesystem::path& path,StoreIdentity id,StoreIoContract c) {
  admit_identity(id);admit_contract(c);
  return create(detail::new_store_file(path,c),id);
}
StoreCommitInfo ObjectStoreWriter::commit(std::span<const StoreMutation> changes) {
  if(!impl_) fail("moved-from writer");
  if(impl_->poisoned) fail("writer poisoned; reopen independently read-only");
  if(changes.empty() || changes.size()>kStoreMaxObjects) fail("mutation count must be 1..16");
  std::uint64_t input_bytes=0;
  for(std::size_t i=0;i<changes.size();++i) {
    const auto& m=changes[i];
    if(!m.id || m.bytes.size()>kStoreMaxObjectBytes || (m.erase && !m.bytes.empty())) fail("invalid mutation");
    for(std::size_t j=0;j<i;++j) if(changes[j].id==m.id) fail("duplicate mutation ID");
    input_bytes+=m.bytes.size();
    if(input_bytes>kStoreMaxTransactionBytes) fail("transaction memory budget");
  }
  const auto& old=impl_->state;
  if(old.generation==(std::numeric_limits<std::uint64_t>::max)()) fail("generation exhausted");
  State next=old;next.generation++;next.degraded=false;
  struct Prepared {std::uint64_t offset;std::array<std::byte,kDataHeader> header;Bytes data;};
  std::vector<Prepared> prepared;prepared.reserve(changes.size());
  auto cursor=old.end;
  // Private copies and every admission check precede the first backing write.
  for(const auto& m:changes) {
    auto it=std::lower_bound(next.entries.begin(),next.entries.end(),m.id,[](const Entry& e,auto key){return e.id<key;});
    if(m.erase) {
      if(it==next.entries.end() || it->id!=m.id) fail("cannot erase missing object");
      next.entries.erase(it);continue;
    }
    Prepared record{cursor,{},m.bytes};
    auto& h=record.header;
    copy_field(h,0,kDataMagic);put32(h,8,1);put64(h,16,m.id);put64(h,24,next.generation);
    put32(h,32,static_cast<std::uint32_t>(m.bytes.size()));put32(h,36,static_cast<std::uint32_t>(m.bytes.size()));
    copy_field(h,40,next.id);
    const auto digest=detail::record_digest(std::span<const std::byte>(h).first(96),record.data);
    copy_field(h,96,digest);
    Entry e{m.id,next.generation,cursor,static_cast<std::uint32_t>(m.bytes.size()),digest};
    if(it!=next.entries.end() && it->id==m.id) *it=e;else next.entries.insert(it,e);
    cursor+=kDataHeader+record.data.size();prepared.push_back(std::move(record));
  }
  if(next.entries.size()>kStoreMaxObjects) fail("object-count budget");
  const auto index=manifest(next);
  const auto index_offset=cursor;
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
  impl_->state=std::move(next);
  return state_info(impl_->state);
}
ObjectStoreSnapshot ObjectStoreWriter::snapshot() const {
  if(!impl_) fail("moved-from writer");
  auto p=std::make_unique<ObjectStoreSnapshot::Impl>();p->state=impl_->state;
  p->state.degraded=p->state.degraded || impl_->poisoned;
  verify(p->state);return ObjectStoreSnapshot(std::move(p));
}
bool ObjectStoreWriter::poisoned() const noexcept { return !impl_ || impl_->poisoned; }
} // namespace flitzfs
