#include "flitzfs/paged_store.hpp"
#include "record_digest.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_MSC_VER) && !defined(_CPPUNWIND)
#error "FlitzFS paged transactions require C++ exception unwinding (/EHsc)."
#endif

namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
using detail::Digest;
constexpr std::uint64_t start=2*kStoreCommitUnit;
constexpr std::size_t page_bytes=4096,header_bytes=64,ref_bytes=112;
constexpr std::size_t digest_at=page_bytes-32;
constexpr std::size_t branch_capacity=(digest_at-header_bytes)/ref_bytes;
constexpr std::uint32_t maximum_height=16;
constexpr char root_magic[]="FLTXPG01",page_magic[]="FLTXPGN1",data_magic[]="FLTXDAT1";
[[noreturn]] void fail(const char* why) {throw std::runtime_error(std::string("FlitzFS paged store: ")+why);}
class CapacityError final:public std::runtime_error {
 public: explicit CapacityError(const char* why):std::runtime_error(why){}
};
[[noreturn]] void capacity(const char* why) {throw CapacityError(why);}
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
  if(b>(std::numeric_limits<std::uint64_t>::max)()-a)fail("integer overflow");
  return a+b;
}
std::uint64_t align(std::uint64_t n,std::uint64_t unit) {return add(n,unit-1)/unit*unit;}
std::uint64_t stream_position(std::uint64_t at,std::uint64_t bytes,std::uint32_t unit) {
  const auto within=at%unit,total=within+bytes;
  return add(add(at-within,(total/unit)*2*unit),total%unit);
}
std::uint64_t stream_end(std::uint64_t at,std::uint64_t bytes,std::uint32_t unit) {
  return bytes?add(stream_position(at,bytes-1,unit),1):at;
}
void read_record_bytes(const StoreByteIo& image,std::uint64_t at,std::uint64_t offset,
    std::span<std::byte> bytes,std::uint32_t unit) {
  if(!unit){image.read_exact(add(at,offset),bytes);return;}
  at=stream_position(at,offset,unit);std::size_t done=0;
  while(done<bytes.size()) {
    const auto take=(std::min)(bytes.size()-done,static_cast<std::size_t>(unit-at%unit));
    image.read_exact(at,bytes.subspan(done,take));done+=take;
    if(done<bytes.size())at=add(add(at,take),unit);
  }
}
void write_record_bytes(StoreByteIo& image,std::uint64_t at,std::uint64_t offset,
    std::span<const std::byte> bytes,std::uint32_t unit) {
  if(!unit){image.write_exact(add(at,offset),bytes);return;}
  at=stream_position(at,offset,unit);std::size_t done=0;
  while(done<bytes.size()) {
    const auto take=(std::min)(bytes.size()-done,static_cast<std::size_t>(unit-at%unit));
    image.write_exact(at,bytes.subspan(done,take));done+=take;
    if(done<bytes.size())at=add(add(at,take),unit);
  }
}
void put32(std::span<std::byte> b,std::size_t at,std::uint32_t n) {
  for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::byte>(n>>(8*i));
}
void put64(std::span<std::byte> b,std::size_t at,std::uint64_t n) {
  for(unsigned i=0;i<8;++i)b[at+i]=static_cast<std::byte>(n>>(8*i));
}
std::uint32_t get32(std::span<const std::byte> b,std::size_t at) {
  std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
std::uint64_t get64(std::span<const std::byte> b,std::size_t at) {
  std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
template<std::size_t N> std::array<std::byte,N> field(std::span<const std::byte> b,std::size_t at) {
  std::array<std::byte,N> n{};std::copy_n(b.begin()+static_cast<std::ptrdiff_t>(at),N,n.begin());return n;
}
template<std::size_t N> void copy(std::span<std::byte> b,std::size_t at,const std::array<std::byte,N>& value) {
  std::copy(value.begin(),value.end(),b.begin()+static_cast<std::ptrdiff_t>(at));
}
bool zero(std::span<const std::byte> b) {return std::all_of(b.begin(),b.end(),[](auto x){return x==std::byte{};});}
bool magic(std::span<const std::byte> b,const char* value) {
  for(std::size_t i=0;i<8;++i)if(b[i]!=static_cast<std::byte>(value[i]))return false;
  return true;
}
void set_magic(std::span<std::byte> b,const char* value) {for(std::size_t i=0;i<8;++i)b[i]=static_cast<std::byte>(value[i]);}
void admit_limits(PagedStoreLimits limits) {
  if(limits.maximum_physical_bytes<start+2*page_bytes ||
      limits.maximum_physical_bytes>static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
    fail("explicit supported physical capacity required");
}
void admit_contract(StoreIoContract contract) {if(!valid_store_io_contract(contract))fail("unqualified backend contract");}
struct Ref {
  std::uint64_t first=0,last=0,count=0,logical=0,stored=0,primary=0,peer=0,generation=0;
  std::uint32_t height=0;
  Digest digest{};
  bool operator==(const Ref&) const =default;
};
struct Entry {
  std::uint64_t id=0,generation=0,offset=0;
  std::uint32_t logical=0,stored=0;
  Digest digest{};
  std::uint64_t peer=0;
};
void encode_ref(std::span<std::byte> b,std::size_t at,const Ref& r) {
  put64(b,at,r.first);put64(b,at+8,r.last);put64(b,at+16,r.count);
  put64(b,at+24,r.logical);put64(b,at+32,r.stored);put64(b,at+40,r.primary);
  put64(b,at+48,r.peer);put64(b,at+56,r.generation);put32(b,at+64,r.height);copy(b,at+80,r.digest);
}
Ref decode_ref(std::span<const std::byte> b,std::size_t at) {
  if(!zero(b.subspan(at+68,12)))fail("reference reserved bytes");
  return {get64(b,at),get64(b,at+8),get64(b,at+16),get64(b,at+24),get64(b,at+32),
          get64(b,at+40),get64(b,at+48),get64(b,at+56),get32(b,at+64),field<32>(b,at+80)};
}
void encode_entry(std::span<std::byte> b,std::size_t at,const Entry& e,bool mirrored) {
  put64(b,at,e.id);put64(b,at+8,e.generation);put64(b,at+16,e.offset);
  put32(b,at+24,e.logical);put32(b,at+28,e.stored);copy(b,at+32,e.digest);
  if(mirrored)put64(b,at+64,e.peer);
}
Entry decode_entry(std::span<const std::byte> b,std::size_t at,bool mirrored) {
  return {get64(b,at),get64(b,at+8),get64(b,at+16),get32(b,at+24),get32(b,at+28),field<32>(b,at+32),
      mirrored?get64(b,at+64):0};
}
struct State {
  std::shared_ptr<StoreByteIo> image;
  PagedStoreLimits limits;
  StoreIdentity id{};
  std::uint64_t generation=0,checkpoint=0,end=0;
  std::uint32_t unit=0;
  Ref root;
  bool degraded_roots=false;
  mutable bool degraded_metadata=false;
  bool mirrored=false;
  mutable bool degraded_payloads=false;
  std::shared_ptr<detail::StoreCodecDecoder> decoder=std::make_shared<detail::StoreCodecDecoder>();
};
std::size_t entry_bytes(const State& s){return s.mirrored?72:64;}
std::size_t leaf_capacity(const State& s){return (digest_at-header_bytes)/entry_bytes(s);}
PagedStoreInfo info(const State& s) {
  return {s.generation,s.checkpoint,s.end,s.root.count,s.root.logical,s.root.stored*(s.mirrored?2:1),s.root.height,
          s.degraded_roots,s.degraded_metadata,s.mirrored,s.degraded_payloads};
}
void admit_ref(const State& s,const Ref& r,bool root=false) {
  if(r.height>maximum_height || !r.generation || r.generation>s.checkpoint ||
      r.primary<start || r.primary%page_bytes || r.peer!=add(r.primary,s.unit) ||
      r.peer>s.end || page_bytes>s.end-r.peer ||
      (r.primary/s.unit)==(r.peer/s.unit) ||
      (r.count ? (!r.first || r.last<r.first || r.count>r.last-r.first+1) :
                 (!root || r.height || r.first || r.last || r.logical || r.stored)) ||
      r.stored>r.logical || r.stored>s.limits.maximum_physical_bytes/(s.mirrored?2:1) ||
      (r.count && r.logical/r.count>kStoreMaxObjectBytes))
    fail("page reference bounds/summary");
}
struct Node {std::vector<Entry> entries;std::vector<Ref> children;};
Ref summary(const State& s,const Node& n,std::uint32_t height) {
  Ref r;r.height=height;
  if(height) {
    if(n.children.empty() || n.children.size()>branch_capacity)fail("branch arity");
    for(const auto& c:n.children) {
      if(!c.count || c.height+1!=height || (r.count && c.first<=r.last))fail("child ordering/height");
      if(!r.count)r.first=c.first;
      r.last=c.last;r.count=add(r.count,c.count);r.logical=add(r.logical,c.logical);r.stored=add(r.stored,c.stored);
    }
  } else {
    if(n.entries.size()>leaf_capacity(s))fail("leaf arity");
    for(const auto& e:n.entries) {
      if(!e.id || (r.count && e.id<=r.last))fail("leaf key ordering");
      if(!r.count)r.first=e.id;
      r.last=e.id;++r.count;r.logical=add(r.logical,e.logical);r.stored=add(r.stored,e.stored);
    }
  }
  return r;
}
Node read_node(const State& s,const Ref& r) {
  admit_ref(s,r,r==s.root);
  std::array<std::byte,page_bytes> a{},b{};
  auto read=[&](std::uint64_t at,auto& bytes) {
    try{s.image->read_exact(at,bytes);}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){return false;}
    return field<32>(bytes,digest_at)==r.digest &&
        detail::record_digest(std::span<const std::byte>(bytes).first(digest_at))==r.digest;
  };
  const bool own=read(r.primary,a),peer=read(r.peer,b);
  if(!own && !peer)fail("both metadata replicas invalid; no rollback");
  if(!own || !peer)s.degraded_metadata=true;
  const std::span<const std::byte> bytes=own?std::span<const std::byte>(a):std::span<const std::byte>(b);
  const auto count=get32(bytes,40);const auto capacity=r.height?branch_capacity:leaf_capacity(s);
  const auto stride=r.height?ref_bytes:entry_bytes(s);
  if(!magic(bytes,page_magic) || get32(bytes,8)!=(s.mirrored?2U:1U) || get32(bytes,12)!=r.height ||
      get64(bytes,16)!=r.generation || field<16>(bytes,24)!=s.id || count>capacity ||
      !zero(bytes.subspan(44,20)) ||
      !zero(bytes.subspan(header_bytes+count*stride,digest_at-header_bytes-count*stride)))
    fail("intact page framing/identity");
  Node n;
  if(r.height) {
    n.children.reserve(count);
    for(std::size_t i=0;i<count;++i) {
      auto c=decode_ref(bytes,header_bytes+i*ref_bytes);admit_ref(s,c);
      if(c.generation>r.generation || c.primary>=r.primary || c.peer>=r.peer)
        fail("child generation/allocation order");
      n.children.push_back(c);
    }
  } else {
    n.entries.reserve(count);
    for(std::size_t i=0;i<count;++i) {
      auto e=decode_entry(bytes,header_bytes+i*entry_bytes(s),s.mirrored);
      if(!e.generation || e.generation>r.generation || e.logical>kStoreMaxObjectBytes || e.stored>e.logical ||
          e.offset<start || e.offset>r.primary || 128ULL+e.stored>r.primary-e.offset)
        fail("object descriptor bounds");
      if(s.mirrored && (e.offset/s.unit%2 || e.peer!=add(e.offset,s.unit) || e.peer>r.primary ||
          stream_end(e.offset,128ULL+e.stored,s.unit)>r.primary || stream_end(e.peer,128ULL+e.stored,s.unit)>r.primary))
        fail("payload replica isolation/bounds");
      n.entries.push_back(e);
    }
  }
  auto total=summary(s,n,r.height);
  if(total.first!=r.first || total.last!=r.last || total.count!=r.count ||
      total.logical!=r.logical || total.stored!=r.stored)fail("page summary mismatch");
  return n;
}
std::optional<Entry> lookup(const State& s,std::uint64_t id) {
  if(!id || !s.root.count || id<s.root.first || id>s.root.last)return {};
  auto r=s.root;
  for(std::uint32_t depth=0;depth<=maximum_height;++depth) {
    auto n=read_node(s,r);
    if(!r.height) {
      const auto i=std::lower_bound(n.entries.begin(),n.entries.end(),id,[](const auto& e,auto key){return e.id<key;});
      if(i!=n.entries.end() && i->id==id)return *i;
      return {};
    }
    auto i=std::lower_bound(n.children.begin(),n.children.end(),id,[](const auto& c,auto key){return c.last<key;});
    if(i==n.children.end() || id<i->first)return {};
    r=*i;
  }
  fail("lookup depth");
}
struct ObjectRecord {std::array<std::byte,128> header{};Bytes payload;};
ObjectRecord read_record_at(const State& s,const Entry& e,std::uint64_t offset) {
  ObjectRecord record;auto& h=record.header;
  read_record_bytes(*s.image,offset,0,h,s.mirrored?s.unit:0);const auto codec=get32(h,12);
  if(!magic(h,data_magic) || get32(h,8)!=1 || get64(h,16)!=e.id || get64(h,24)!=e.generation ||
      get32(h,32)!=e.logical || get32(h,36)!=e.stored || field<16>(h,40)!=s.id ||
      !detail::store_codec_framing(codec,e.logical,e.stored) ||
      !zero(std::span<const std::byte>(h).subspan(codec==detail::kStoreRaw?56:88,codec==detail::kStoreRaw?40:8)) ||
      field<32>(h,96)!=e.digest)fail("object identity/codec/framing");
  auto& payload=record.payload;payload.resize(e.stored);
  if(!payload.empty())read_record_bytes(*s.image,offset,128,payload,s.mirrored?s.unit:0);
  if(detail::record_digest(std::span<const std::byte>(h).first(96),payload)!=e.digest)fail("object stored digest");
  return record;
}
ObjectRecord read_record(const State& s,const Entry& e) {
  try{return read_record_at(s,e,e.offset);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&) {
    if(!s.mirrored)throw;
    s.degraded_payloads=true;return read_record_at(s,e,e.peer);
  }
}
Bytes decode_record(const State& s,const Entry& e,const ObjectRecord& record) {
  const auto& h=record.header;const auto& payload=record.payload;const auto codec=get32(h,12);
  if(codec==detail::kStoreRaw)return payload;
  auto decoded=s.decoder->decode(codec,payload,e.logical);
  if(detail::record_digest(decoded)!=field<32>(h,56))fail("object decoded digest");
  return decoded;
}
Bytes read_object(const State& s,const Entry& e) {
  auto record=read_record(s,e);
  if(get32(record.header,12)==detail::kStoreRaw)return std::move(record.payload);
  return decode_record(s,e,record);
}
void verify_object(const State& s,const Entry& e) {
  if(!s.mirrored){(void)read_object(s,e);return;}
  bool valid=false;
  for(const auto at:{e.offset,e.peer}) {
    try{(void)decode_record(s,e,read_record_at(s,e,at));valid=true;}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){s.degraded_payloads=true;}
  }
  if(!valid)fail("both payload copies invalid; no rollback");
}
template<class Visitor> void walk(const State& s,const Ref& r,std::uint64_t after,Visitor& visitor,bool& done) {
  if(done || !r.count || r.last<=after)return;
  auto n=read_node(s,r);
  if(r.height) {for(const auto& child:n.children)walk(s,child,after,visitor,done);}
  else for(const auto& e:n.entries)if(e.id>after && !done)done=!visitor(e);
}
void verify(const State& s) {
  (void)read_node(s,s.root);
  auto visitor=[&](const Entry& e){verify_object(s,e);return true;};
  bool done=false;walk(s,s.root,0,visitor,done);
}
std::array<std::byte,page_bytes> root_bytes(const State& s,std::uint64_t generation) {
  std::array<std::byte,page_bytes> b{};set_magic(b,root_magic);put32(b,8,s.mirrored?2:1);
  put32(b,12,static_cast<std::uint32_t>(generation%2));put64(b,16,generation);
  put64(b,24,s.checkpoint);put64(b,32,s.end);copy(b,40,s.id);put32(b,56,s.unit);
  put32(b,60,page_bytes);encode_ref(b,64,s.root);
  copy(b,digest_at,detail::record_digest(std::span<const std::byte>(b).first(digest_at)));return b;
}
State load(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits) {
  admit_limits(limits);if(!image)fail("missing backend");
  const auto extent=image->extent();
  if(extent<start+2*page_bytes || extent>limits.maximum_physical_bytes)fail("physical capacity/truncation");
  auto read=[&](unsigned slot)->std::optional<State> {
    std::array<std::byte,page_bytes> b{};
    try{image->read_exact(slot*kStoreCommitUnit,b);}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){return {};}
    if(field<32>(b,digest_at)!=detail::record_digest(std::span<const std::byte>(b).first(digest_at)))return {};
    State s;s.image=image;s.limits=limits;s.id=field<16>(b,40);s.generation=get64(b,16);
    s.mirrored=get32(b,8)==2;
    s.checkpoint=get64(b,24);s.end=get64(b,32);s.unit=get32(b,56);s.root=decode_ref(b,64);
    if(!magic(b,root_magic) || (get32(b,8)!=1 && get32(b,8)!=2) || get32(b,12)!=slot ||
        s.generation%2!=slot || !s.checkpoint || s.checkpoint==(std::numeric_limits<std::uint64_t>::max)() ||
        (s.generation!=s.checkpoint && s.generation!=s.checkpoint+1) || zero(s.id) ||
        s.unit<page_bytes || s.unit>kStoreCommitUnit || (s.unit&(s.unit-1)) ||
        get32(b,60)!=page_bytes || s.end>extent || s.end>limits.maximum_physical_bytes ||
        s.end<start+2*s.unit || s.end%s.unit || !zero(std::span<const std::byte>(b).subspan(176,digest_at-176)))
      fail("unsupported/intact root semantics");
    admit_ref(s,s.root,true);return s;
  };
  auto a=read(0),b=read(1);if(!a && !b)fail("no recovery root");
  const bool choose_a=a && (!b || a->generation>b->generation);
  auto s=choose_a?*a:*b;const auto& other=choose_a?b:a;
  bool paired=false;
  if(other) {
    if(other->id!=s.id || other->unit!=s.unit || other->mirrored!=s.mirrored || s.generation<=other->generation ||
        s.generation-other->generation!=1)fail("conflicting root identities/generations");
    paired=other->checkpoint==s.checkpoint && other->root==s.root && other->end==s.end;
    if(s.generation!=s.checkpoint && !paired)fail("conflicting intact root pair");
  }
  s.degraded_roots=!paired;(void)read_node(s,s.root);return s;
}
bool same(const State& a,const State& b) {
  return a.id==b.id && a.mirrored==b.mirrored && a.generation==b.generation && a.checkpoint==b.checkpoint &&
      a.end==b.end && a.unit==b.unit && a.root==b.root;
}
void zeros(StoreByteIo& io,std::uint64_t at,std::uint64_t end) {
  std::array<std::byte,page_bytes> b{};
  while(at<end) {const auto size=static_cast<std::size_t>((std::min)(end-at,std::uint64_t(page_bytes)));
    io.write_exact(at,std::span<const std::byte>(b).first(size));at+=size;}
}
void publish_roots(const State& s) {
  s.image->write_exact(s.checkpoint%2*kStoreCommitUnit,root_bytes(s,s.checkpoint));s.image->flush();
  s.image->write_exact((s.checkpoint+1)%2*kStoreCommitUnit,root_bytes(s,s.checkpoint+1));s.image->flush();
}
void publish(const State& s) {s.image->flush();publish_roots(s);}
// Pack logical record streams across paired isolation units. Every primary
// chunk occupies an even unit and its peer the following odd unit. Padding is sealed before metadata;
// no acknowledged unit is reused. The planner itself never performs I/O.
struct PayloadAllocation {
  std::uint64_t bank=0,used=0,unit=0;
  std::vector<std::pair<std::uint64_t,std::uint64_t>> padding;
  void seal() {
    if(unit) {
      if(used<unit){padding.emplace_back(bank+used,bank+unit);padding.emplace_back(bank+unit+used,bank+2*unit);}
      unit=used=0;
    }
  }
  void place(const State& s,Entry& e,std::uint64_t bytes,std::uint64_t& cursor) {
    e.peer=0;
    if(!s.mirrored){e.offset=cursor;cursor=add(cursor,bytes);return;}
    const auto primary=unit?bank+used:align(cursor,2ULL*s.unit);
    if(!unit && primary>cursor)padding.emplace_back(cursor,primary);
    e.offset=primary;e.peer=add(primary,s.unit);
    const auto next=stream_position(primary,bytes,s.unit);used=next%s.unit;
    if(used){bank=next-used;unit=s.unit;cursor=add(bank,2ULL*unit);}
    else {unit=0;cursor=next;}
  }
  void write_padding(StoreByteIo& image) {
    for(const auto& [begin,end]:padding)zeros(image,begin,end);padding.clear();
  }
};
void write_object_record(StoreByteIo& image,const Entry& e,std::span<const std::byte> header,
    std::span<const std::byte> payload,std::uint32_t unit) {
  for(const auto at:{e.offset,e.peer}) {
    if(at==0)continue;
    write_record_bytes(image,at,0,header,e.peer?unit:0);
    if(!payload.empty())write_record_bytes(image,at,128,payload,e.peer?unit:0);
  }
}
struct PreparedPage {Ref ref;std::array<std::byte,page_bytes> bytes{};};
struct PreparedObject {Entry entry;std::array<std::byte,128> header{};Bytes payload;};
struct Change {std::uint64_t id;bool erase;Entry entry;};
struct Plan {
  State next;
  std::size_t changed_keys=kStoreMaxObjects;
  std::uint64_t page_start=0;
  std::vector<PreparedPage> pages;
  Ref make_page(const Node& node,std::uint32_t height) {
    // Bound staging from this admitted public/native transaction's changed
    // paths, at most 17 levels per path plus splits.
    // This work limit is independent of the volume's object count.
    if(pages.size()>=2*(maximum_height+1)*changed_keys+maximum_height+1)capacity("transaction page budget");
    PreparedPage p;auto& r=p.ref;r=summary(next,node,height);r.generation=next.checkpoint;
    const auto per_unit=next.unit/page_bytes;
    r.primary=add(page_start,add((pages.size()/per_unit)*2ULL*next.unit,(pages.size()%per_unit)*page_bytes));
    r.peer=add(r.primary,next.unit);
    if(add(r.peer,page_bytes)>next.limits.maximum_physical_bytes)capacity("physical capacity");
    auto& b=p.bytes;set_magic(b,page_magic);put32(b,8,next.mirrored?2:1);put32(b,12,height);
    put64(b,16,r.generation);copy(b,24,next.id);
    put32(b,40,static_cast<std::uint32_t>(height?node.children.size():node.entries.size()));
    if(height)for(std::size_t i=0;i<node.children.size();++i)encode_ref(b,header_bytes+i*ref_bytes,node.children[i]);
    else for(std::size_t i=0;i<node.entries.size();++i)encode_entry(b,header_bytes+i*entry_bytes(next),node.entries[i],next.mirrored);
    r.digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));copy(b,digest_at,r.digest);
    const auto result=r;pages.push_back(std::move(p));return result;
  }
  std::vector<Ref> leaves(const std::vector<Entry>& entries) {
    std::vector<Ref> output;if(entries.empty())return output;
    const auto capacity=leaf_capacity(next);const auto count=(entries.size()+capacity-1)/capacity;
    const auto size=(entries.size()+count-1)/count;
    for(std::size_t i=0;i<entries.size();i+=size) {
      Node n;n.entries.assign(entries.begin()+static_cast<std::ptrdiff_t>(i),
        entries.begin()+static_cast<std::ptrdiff_t>((std::min)(entries.size(),i+size)));
      output.push_back(make_page(n,0));
    }
    return output;
  }
  std::vector<Ref> branches(const std::vector<Ref>& children,std::uint32_t height) {
    std::vector<Ref> output;if(children.empty())return output;
    if(height>maximum_height)fail("tree height budget");
    const auto count=(children.size()+branch_capacity-1)/branch_capacity;
    const auto size=(children.size()+count-1)/count;
    for(std::size_t i=0;i<children.size();i+=size) {
      Node n;n.children.assign(children.begin()+static_cast<std::ptrdiff_t>(i),
        children.begin()+static_cast<std::ptrdiff_t>((std::min)(children.size(),i+size)));
      output.push_back(make_page(n,height));
    }
    return output;
  }
  std::vector<Ref> rewrite(const State& old,const Ref& ref,std::span<const Change> changes) {
    if(changes.empty())return {ref};
    auto node=read_node(old,ref);
    if(!ref.height) {
      auto entries=std::move(node.entries);
      for(const auto& c:changes) {
        auto at=std::lower_bound(entries.begin(),entries.end(),c.id,[](const auto& e,auto id){return e.id<id;});
        if(c.erase) {if(at==entries.end() || at->id!=c.id)fail("erase missing object");entries.erase(at);}
        else if(at!=entries.end() && at->id==c.id)*at=c.entry;
        else entries.insert(at,c.entry);
      }
      return leaves(entries);
    }
    std::vector<Ref> children;std::size_t begin=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      auto end=begin;
      while(end<changes.size() && (i+1==node.children.size() || changes[end].id<=node.children[i].last))++end;
      auto result=rewrite(old,node.children[i],changes.subspan(begin,end-begin));
      children.insert(children.end(),result.begin(),result.end());begin=end;
    }
    return branches(children,ref.height);
  }
  void finish(std::vector<Ref> roots) {
    if(roots.empty())next.root=make_page(Node{},0);
    else {
      while(roots.size()>1)roots=branches(roots,roots.front().height+1);
      next.root=roots.front();
    }
    const auto per_unit=next.unit/page_bytes;
    next.end=add(page_start,((pages.size()+per_unit-1)/per_unit)*2ULL*next.unit);
    if(next.end>next.limits.maximum_physical_bytes)capacity("physical capacity");
  }
  void write_pages(std::uint64_t data_end) {
    zeros(*next.image,data_end,next.end);
    for(const auto& p:pages) {next.image->write_exact(p.ref.primary,p.bytes);next.image->write_exact(p.ref.peer,p.bytes);}
  }
};
// Bulk construction keeps one unfinished leaf plus <=35 child references per
// level. Completed pages stream directly to the unpublished target. It does not
// retain a page plan or object inventory proportional to the source volume.
struct Rebuild {
  State next;
  std::uint64_t cursor=start;
  PayloadAllocation allocation;
  bool size_only=false;
  Node leaf;
  std::array<Node,maximum_height+1> levels;
  Ref write_node(const Node& node,std::uint32_t height) {
    if(size_only) {
      auto ref=summary(next,node,height);ref.generation=next.checkpoint;
      ref.primary=align(cursor,next.unit);ref.peer=add(ref.primary,next.unit);cursor=add(ref.peer,next.unit);
      allocation.seal();allocation.padding.clear();return ref;
    }
    Plan plan;plan.next=next;plan.page_start=align(cursor,next.unit);
    auto ref=plan.make_page(node,height);plan.finish({ref});next.image->admit_write(plan.next.end);
    allocation.seal();allocation.write_padding(*next.image);
    plan.write_pages(cursor);cursor=plan.next.end;return ref;
  }
  void emit(Ref ref) {
    if(ref.height>maximum_height)fail("rebuild tree height");
    auto& node=levels[ref.height];node.children.push_back(ref);
    if(node.children.size()==branch_capacity) {
      if(ref.height==maximum_height)fail("rebuild tree height");
      auto parent=write_node(node,ref.height+1);node.children.clear();emit(parent);
    }
  }
  void entry(const Entry& e) {
    leaf.entries.push_back(e);
    if(leaf.entries.size()==leaf_capacity(next)) {auto ref=write_node(leaf,0);leaf.entries.clear();emit(ref);}
  }
  void finish() {
    if(!leaf.entries.empty()) {auto ref=write_node(leaf,0);leaf.entries.clear();emit(ref);}
    for(std::uint32_t h=0;h<=maximum_height;++h) {
      auto& node=levels[h];if(node.children.empty())continue;
      bool higher=false;for(auto k=h+1;k<=maximum_height;++k)higher=higher || !levels[k].children.empty();
      if(node.children.size()==1 && !higher) {next.root=node.children.front();next.end=cursor;return;}
      if(h==maximum_height)fail("rebuild tree height");
      auto ref=write_node(node,h+1);node.children.clear();emit(ref);
    }
    next.root=write_node(Node{},0);next.end=cursor;
  }
};
void copy_rebuild_object(Rebuild& build,const State& original,const Entry& source_entry,
    detail::StoreCodecEncoder& encoder,std::uint64_t replacement_id,
    std::span<const std::byte> replacement,bool reencode) {
  auto source_record=read_record(original,source_entry);auto bytes=decode_record(original,source_entry,source_record);
  if(source_entry.id==replacement_id)bytes.assign(replacement.begin(),replacement.end());
  auto encoded=[&] {
    if(reencode || source_entry.id==replacement_id)return encoder.encode(bytes);
    return detail::EncodedStoreObject{get32(source_record.header,12),std::move(source_record.payload),
        field<32>(source_record.header,56)};
  }();
  Entry entry{source_entry.id,source_entry.generation,build.cursor,static_cast<std::uint32_t>(bytes.size()),
              static_cast<std::uint32_t>(encoded.payload.size()),{}};
  std::array<std::byte,128> header{};set_magic(header,data_magic);put32(header,8,1);put32(header,12,encoded.codec);
  put64(header,16,entry.id);put64(header,24,entry.generation);put32(header,32,entry.logical);
  put32(header,36,entry.stored);copy(header,40,build.next.id);
  if(encoded.codec!=detail::kStoreRaw)copy(header,56,encoded.decoded_digest);
  entry.digest=detail::record_digest(std::span<const std::byte>(header).first(96),encoded.payload);copy(header,96,entry.digest);
  build.allocation.place(build.next,entry,128ULL+entry.stored,build.cursor);
  if(build.cursor>build.next.limits.maximum_physical_bytes)capacity("rebuild capacity");
  build.next.image->admit_write(build.cursor);build.allocation.write_padding(*build.next.image);
  write_object_record(*build.next.image,entry,header,encoded.payload,build.next.unit);
  if(read_object(build.next,entry)!=bytes)fail("rebuild CPU comparison");
  if(build.next.mirrored)verify_object(build.next,entry);
  if(build.next.degraded_payloads)fail("rebuild produced degraded payload replicas");
  build.entry(entry);
}
std::optional<Entry> next_entry(const State& state,std::uint64_t after) {
  std::optional<Entry> entry;bool done=false;
  auto visitor=[&](const Entry& value){entry=value;return false;};
  walk(state,state.root,after,visitor,done);return entry;
}
} // namespace

struct PagedObjectStoreSnapshot::Impl {State state;};
struct PagedObjectStoreWriter::Impl {
  // Nested lease shares the parent's friendship, as in the flat writer.
  struct Lease {
    std::shared_ptr<StoreByteIo> image;
    explicit Lease(std::shared_ptr<StoreByteIo> io):image(std::move(io)) {
      if(!image || !image->acquire_writer())fail("backend already has a writer or is missing");
    }
    ~Lease(){image->release_writer();}
  } lease;
  State state;
  detail::StoreCodecEncoder encoder;
  std::uint64_t append_floor=0;
  bool poisoned=false;
  std::shared_ptr<const void> owner=std::make_shared<const unsigned char>(0);
  mutable std::weak_ptr<const void> pending_rebuild;
  Impl(std::shared_ptr<StoreByteIo> image,StoreCompressionOptions options):lease(std::move(image)),encoder(options){}
};
struct PagedObjectStoreWriter::Rewrite::Impl {
  State original;
  std::weak_ptr<const void> owner;
  std::shared_ptr<const void> reservation=std::make_shared<const unsigned char>(0);
  std::unique_ptr<PagedObjectStoreWriter::Impl> target;
  Rebuild build;
  Bytes replacement;
  std::uint64_t source_extent=0,target_extent=0,after=0,completed=0,logical=0,stored=0;
  PagedRewritePhase phase=PagedRewritePhase::verifying_source;
  bool reencode=true,replacement_seen=false;

  Impl(const PagedObjectStoreWriter::Impl& writer,std::shared_ptr<StoreByteIo> output,
      StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> root,bool encode)
      :original(writer.state),owner(writer.owner),reencode(encode) {
    if(zero(id) || id==original.id)fail("staged rebuild needs a new identity");
    if(root.size()>kStoreMaxObjectBytes)fail("staged replacement budget");
    replacement.assign(root.begin(),root.end());
    if(!same(original,load(original.image,original.limits)))fail("staged rebuild source changed");
    source_extent=original.image->extent();
    target=std::make_unique<PagedObjectStoreWriter::Impl>(std::move(output),compression);
    auto image=target->lease.image;admit_contract(image->contract());
    if(image->extent())fail("staged rebuild requires an empty target");
    build.next.image=image;build.next.limits=original.limits;build.next.id=id;build.next.mirrored=original.mirrored;
    build.next.generation=add(original.generation,2);build.next.checkpoint=original.generation+1;
    build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),image->contract().write_failure_unit_bytes);
  }
  void source_check() const {
    admit_contract(original.image->contract());
    if(original.image->contract().write_failure_unit_bytes>original.unit ||
        original.image->extent()!=source_extent || !same(original,load(original.image,original.limits)))
      fail("staged rebuild source checkpoint/contract changed");
  }
  void reset(PagedRewritePhase next) noexcept {phase=next;after=completed=logical=stored=0;}
  void count(const Entry& entry) {
    after=entry.id;completed=add(completed,1);logical=add(logical,entry.logical);stored=add(stored,entry.stored);
  }
  void totals(const State& state) const {
    if(completed!=state.root.count || logical!=state.root.logical || stored!=state.root.stored)
      fail("staged verification inventory differs");
  }
  void compare(const Entry& entry) {
    const auto source=lookup(original,entry.id);
    if(!source || source->generation!=entry.generation || source->logical!=entry.logical)
      fail("staged target object differs");
    auto expected=read_object(original,*source);
    if(entry.id==1 && !replacement.empty())expected=replacement;
    if(read_object(build.next,entry)!=expected)fail("staged target CPU comparison");
    if(build.next.mirrored)verify_object(build.next,entry);
  }
  void step() {
    auto image=build.next.image;
    if(image->extent()!=target_extent)fail("staged target extent changed");
    admit_contract(image->contract());
    if(image->contract().write_failure_unit_bytes>build.next.unit)fail("staged target contract changed");
    if(phase==PagedRewritePhase::verifying_source) {
      if(const auto entry=next_entry(original,after)) {
        auto bytes=read_object(original,*entry);if(original.mirrored)verify_object(original,*entry);
        if(entry->id==1 && !replacement.empty()) {
          if(bytes.size()<56 || bytes.size()!=replacement.size())fail("staged native root length");
          copy(bytes,40,build.next.id);
          if(bytes!=replacement)fail("staged replacement differs beyond identity");
          replacement_seen=true;
        }
        count(*entry);
      } else {
        totals(original);
        if(!replacement.empty() && !replacement_seen)fail("staged native root missing");
        source_check();image->admit_write(start+2*build.next.unit);zeros(*image,0,start);
        reset(PagedRewritePhase::copying);
      }
    } else if(phase==PagedRewritePhase::copying) {
      if(const auto entry=next_entry(original,after)) {
        copy_rebuild_object(build,original,*entry,target->encoder,replacement.empty()?0:1,replacement,reencode);count(*entry);
      } else {
        totals(original);build.finish();
        if(build.next.root.count!=original.root.count || build.next.root.logical!=original.root.logical)
          fail("staged copy inventory differs");
        reset(PagedRewritePhase::verifying_target);
      }
    } else if(phase==PagedRewritePhase::verifying_target || phase==PagedRewritePhase::verifying_publication) {
      if(const auto entry=next_entry(build.next,after)) {compare(*entry);count(*entry);}
      else {
        totals(build.next);
        if(phase==PagedRewritePhase::verifying_target)reset(PagedRewritePhase::publishing);
        else {
          source_check();target->state=build.next;target->append_floor=build.next.end;
          phase=PagedRewritePhase::ready;
        }
      }
      if(build.next.degraded_metadata || build.next.degraded_payloads)fail("staged target replicas are degraded");
    } else if(phase==PagedRewritePhase::publishing) {
      source_check();image->admit_write(build.next.end);publish(build.next);
      auto published=load(image,build.next.limits);
      if(!same(build.next,published) || published.degraded_roots || published.degraded_metadata || published.degraded_payloads)
        fail("staged published checkpoint differs/degraded");
      build.next=std::move(published);reset(PagedRewritePhase::verifying_publication);
    }
    target_extent=image->extent();
  }
};
PagedObjectStoreWriter::Rewrite::Rewrite(std::unique_ptr<Impl> value) noexcept:impl_(std::move(value)){}
PagedObjectStoreWriter::Rewrite::Rewrite(Rewrite&&) noexcept=default;
PagedObjectStoreWriter::Rewrite& PagedObjectStoreWriter::Rewrite::operator=(Rewrite&&) noexcept=default;
PagedObjectStoreWriter::Rewrite::~Rewrite()=default;
bool PagedObjectStoreWriter::Rewrite::matches(const PagedObjectStoreWriter& writer) const noexcept {
  return impl_ && !writer.poisoned() && impl_->owner.lock()==writer.impl_->owner && same(impl_->original,writer.impl_->state);
}
StoreIdentity PagedObjectStoreWriter::Rewrite::identity() const noexcept {return impl_?impl_->build.next.id:StoreIdentity{};}
bool PagedObjectStoreWriter::Rewrite::replaces_native_root() const noexcept {return impl_ && !impl_->replacement.empty();}
PagedRewriteProgress PagedObjectStoreWriter::Rewrite::progress() const noexcept {
  return impl_?PagedRewriteProgress{impl_->phase,impl_->original.root.count,impl_->completed}:PagedRewriteProgress{};
}
bool PagedObjectStoreWriter::Rewrite::advance(const PagedObjectStoreWriter& writer) {
  if(!matches(writer)) {impl_.reset();fail("staged rebuild source owner/generation is stale");}
  if(impl_->phase==PagedRewritePhase::ready)return true;
  try{impl_->step();return impl_->phase==PagedRewritePhase::ready;}
  catch(...){impl_.reset();throw;}
}
PagedObjectStoreWriter PagedObjectStoreWriter::Rewrite::finish(const PagedObjectStoreWriter& writer) {
  if(!matches(writer)) {impl_.reset();fail("staged rebuild source owner/generation is stale");}
  if(impl_->phase!=PagedRewritePhase::ready)fail("staged rebuild is incomplete");
  auto candidate=std::move(impl_);return PagedObjectStoreWriter(std::move(candidate->target));
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,bool reencode) const {
  return prepare_rebuild_replacing(std::move(target),id,compression,{},reencode);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_replacing(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> replacement,bool reencode) const {
  if(poisoned())fail("staged rebuild requires a healthy source writer");
  if(!impl_->pending_rebuild.expired())fail("source already has a staged rebuild");
  auto candidate=std::make_unique<Rewrite::Impl>(*impl_,std::move(target),id,compression,replacement,reencode);
  impl_->pending_rebuild=candidate->reservation;return Rewrite(std::move(candidate));
}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot& PagedObjectStoreSnapshot::operator=(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot::~PagedObjectStoreSnapshot()=default;
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits) {
  auto p=std::make_unique<Impl>();p->state=load(std::move(image),limits);return PagedObjectStoreSnapshot(std::move(p));
}
StoreIdentity PagedObjectStoreSnapshot::identity() const noexcept {return impl_?impl_->state.id:StoreIdentity{};}
std::shared_ptr<StoreByteIo> PagedObjectStoreSnapshot::backing_for_volume_head() const {
  if(!impl_)fail("moved-from paged snapshot");return impl_->state.image;
}
PagedStoreLimits PagedObjectStoreSnapshot::limits() const noexcept {return impl_?impl_->state.limits:PagedStoreLimits{};}
std::uint32_t PagedObjectStoreSnapshot::isolation_unit() const noexcept {return impl_?impl_->state.unit:0;}
std::uint64_t PagedObjectStoreSnapshot::rebuilt_size(std::span<const StoreObjectInfo> records) const {
  if(!impl_)fail("moved-from size estimate");
  Rebuild build;build.size_only=true;build.next=impl_->state;std::uint64_t previous=0;
  for(const auto& record:records) {
    if(!record.id || record.id<=previous || record.logical_bytes>kStoreMaxObjectBytes || record.stored_payload_bytes>record.logical_bytes)
      fail("hypothetical record ordering/length");
    Entry entry{record.id,1,0,record.logical_bytes,record.stored_payload_bytes,{}};
    build.allocation.place(build.next,entry,128ULL+entry.stored,build.cursor);
    build.entry(entry);previous=entry.id;
  }
  build.finish();return build.next.end;
}
PagedStoreInfo PagedObjectStoreSnapshot::info() const noexcept {return impl_?flitzfs::info(impl_->state):PagedStoreInfo{};}
std::optional<StoreObjectInfo> PagedObjectStoreSnapshot::stat(std::uint64_t id) const {
  if(!impl_)fail("moved-from snapshot");auto e=lookup(impl_->state,id);
  if(!e)return {};return StoreObjectInfo{e->id,e->generation,e->logical,e->stored};
}
Bytes PagedObjectStoreSnapshot::read(std::uint64_t id) const {
  if(!impl_)fail("moved-from snapshot");auto e=lookup(impl_->state,id);if(!e)fail("missing object");
  return read_object(impl_->state,*e);
}
std::vector<StoreObjectInfo> PagedObjectStoreSnapshot::list_page(std::uint64_t after,std::size_t limit) const {
  if(!impl_)fail("moved-from snapshot");if(!limit || limit>4096)fail("pagination budget");
  std::vector<StoreObjectInfo> result;result.reserve(limit);bool done=false;
  auto visitor=[&](const Entry& e){result.push_back({e.id,e.generation,e.logical,e.stored});return result.size()<limit;};
  walk(impl_->state,impl_->state.root,after,visitor,done);return result;
}
void PagedObjectStoreSnapshot::verify_all() const {if(!impl_)fail("moved-from snapshot");verify(impl_->state);}
PagedObjectStoreWriter::PagedObjectStoreWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
PagedObjectStoreWriter::PagedObjectStoreWriter(PagedObjectStoreWriter&&) noexcept=default;
PagedObjectStoreWriter& PagedObjectStoreWriter::operator=(PagedObjectStoreWriter&&) noexcept=default;
PagedObjectStoreWriter::~PagedObjectStoreWriter()=default;
PagedObjectStoreWriter PagedObjectStoreWriter::create(std::shared_ptr<StoreByteIo> image,StoreIdentity id,
    PagedStoreLimits limits,StoreCompressionOptions options) {
  return create_impl(std::move(image),id,limits,options,false);
}
PagedObjectStoreWriter PagedObjectStoreWriter::create_mirrored(std::shared_ptr<StoreByteIo> image,StoreIdentity id,
    PagedStoreLimits limits,StoreCompressionOptions options) {
  return create_impl(std::move(image),id,limits,options,true);
}
PagedObjectStoreWriter PagedObjectStoreWriter::create_impl(std::shared_ptr<StoreByteIo> image,StoreIdentity id,
    PagedStoreLimits limits,StoreCompressionOptions options,bool mirrored) {
  admit_limits(limits);if(zero(id))fail("nonzero store identity required");
  auto p=std::make_unique<Impl>(image,options);admit_contract(image->contract());
  if(image->extent())fail("new empty backend required");
  State s;s.image=image;s.limits=limits;s.id=id;s.generation=2;s.checkpoint=1;s.mirrored=mirrored;
  s.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),image->contract().write_failure_unit_bytes);
  Plan plan;plan.next=s;plan.page_start=start;plan.finish({});
  image->admit_write(plan.next.end);zeros(*image,0,start);plan.write_pages(start);publish(plan.next);
  p->state=std::move(plan.next);p->append_floor=p->state.end;return PagedObjectStoreWriter(std::move(p));
}
PagedObjectStoreWriter PagedObjectStoreWriter::resume(std::shared_ptr<StoreByteIo> image,StoreIdentity expected,
    std::uint64_t generation,PagedStoreLimits limits,StoreCompressionOptions options) {
  auto p=std::make_unique<Impl>(image,options);auto s=load(image,limits);admit_contract(image->contract());
  if(s.id!=expected || s.generation!=generation || image->contract().write_failure_unit_bytes>s.unit)
    fail("recovery lease/contract mismatch");
  verify(s);if(s.degraded_metadata || s.degraded_payloads)fail("replica rebuild required before writable resume");
  const auto extent=image->extent();image->flush();auto observed=load(image,limits);
  if(!same(s,observed) || image->extent()!=extent)fail("recovery baseline changed");
  p->append_floor=align(extent,s.unit);
  if(s.degraded_roots) {
    s.checkpoint=add(s.generation,1);s.generation=add(s.generation,2);
    if(s.checkpoint==(std::numeric_limits<std::uint64_t>::max)())fail("generation exhausted");
    s.end=p->append_floor;image->admit_write(s.end);zeros(*image,extent,s.end);publish(s);
    s=load(image,limits);verify(s);if(s.degraded_roots || s.degraded_metadata)fail("root repair verification");
  }
  p->state=std::move(s);return PagedObjectStoreWriter(std::move(p));
}
PagedObjectStoreWriter PagedObjectStoreWriter::rebuild(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,PagedStoreLimits limits,StoreCompressionOptions options) {
  return rebuild_replacing(source,std::move(target),id,limits,options,0,{},true);
}
PagedObjectStoreWriter PagedObjectStoreWriter::rebuild_replacing(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,PagedStoreLimits limits,StoreCompressionOptions options,
    std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode) {
  if(!source.impl_)fail("moved-from rebuild source");admit_limits(limits);
  const auto& original=source.impl_->state;
  if(zero(id) || id==original.id)fail("rebuild requires a new nonzero identity");
  Impl::Lease source_lease(original.image);auto observed=load(original.image,original.limits);
  if(!same(original,observed))fail("rebuild source is not the current checkpoint");
  const auto source_extent=original.image->extent();
  if(replacement_id) {
    const auto entry=lookup(original,replacement_id);
    if(!entry || replacement.size()!=entry->logical)fail("rebuild replacement identity/length");
    // Native rebinding changes only the fixed identity field; reject a stale
    // caller's metadata before creating any target records.
    auto expected=read_object(original,*entry);
    if(expected.size()<56)fail("rebuild replacement framing");
    copy(expected,40,id);if(!std::equal(expected.begin(),expected.end(),replacement.begin()))
      fail("rebuild replacement differs beyond image identity");
    verify(original);
  }
  auto p=std::make_unique<Impl>(target,options);admit_contract(target->contract());
  if(target->extent())fail("rebuild requires an empty target");
  Rebuild build;build.next.image=target;build.next.limits=limits;build.next.id=id;build.next.mirrored=original.mirrored;
  build.next.generation=add(original.generation,2);build.next.checkpoint=original.generation+1;
  build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),target->contract().write_failure_unit_bytes);
  target->admit_write(start+2*build.next.unit);zeros(*target,0,start);
  auto visitor=[&](const Entry& source_entry) {
    copy_rebuild_object(build,original,source_entry,p->encoder,replacement_id,replacement,reencode);return true;
  };
  bool done=false;(void)read_node(original,original.root);walk(original,original.root,0,visitor,done);build.finish();
  if(build.next.root.count!=original.root.count || build.next.root.logical!=original.root.logical)
    fail("rebuild inventory mismatch");
  target->flush();verify(build.next);
  if(build.next.degraded_metadata || build.next.degraded_payloads)fail("rebuild produced degraded target replicas");
  if(replacement_id)verify(original);
  if(!same(original,load(original.image,original.limits)) || original.image->extent()!=source_extent)
    fail("rebuild source changed before publication");
  publish_roots(build.next);
  auto published=load(target,limits);verify(published);
  if(!same(build.next,published) || published.degraded_roots || published.degraded_metadata || published.degraded_payloads)
    fail("rebuild published checkpoint failed independent verification");
  p->state=std::move(published);p->append_floor=p->state.end;
  return PagedObjectStoreWriter(std::move(p));
}
PagedStoreInfo PagedObjectStoreWriter::commit(std::span<const StoreMutation> mutations) {
  return commit_impl(mutations,kStoreMaxObjects);
}
PagedStoreInfo PagedObjectStoreWriter::commit_native(std::span<const StoreMutation> mutations,bool directory_indexes,bool shared_bases) {
  return commit_impl(mutations,(shared_bases?4:directory_indexes?3:2)*kStoreMaxObjects,
      kStoreMaxTransactionBytes+(shared_bases?131072U:directory_indexes?65536U:0U));
}
PagedStoreInfo PagedObjectStoreWriter::commit_impl(std::span<const StoreMutation> mutations,std::size_t maximum_operations,
    std::uint64_t maximum_input) {
  if(!impl_ || impl_->poisoned)fail("writer unavailable/poisoned");
  auto& p=*impl_;const auto& old=p.state;
  if(old.degraded_payloads){p.poisoned=true;fail("payload rebuild required before further writes");}
  if(mutations.empty() || mutations.size()>maximum_operations)fail("transaction operation budget");
  if(old.generation>(std::numeric_limits<std::uint64_t>::max)()-2)fail("generation exhausted");
  std::vector<const StoreMutation*> sorted;sorted.reserve(mutations.size());std::uint64_t input=0;
  for(const auto& m:mutations) {
    if(!m.id || m.bytes.size()>kStoreMaxObjectBytes || m.lz4_candidate.size()>kStoreMaxObjectBytes ||
        (m.erase && (!m.bytes.empty() || !m.lz4_candidate.empty())))fail("mutation fields");
    input=add(input,m.bytes.size());if(input>maximum_input)fail("transaction input budget");sorted.push_back(&m);
  }
  std::sort(sorted.begin(),sorted.end(),[](auto a,auto b){return a->id<b->id;});
  for(std::size_t i=1;i<sorted.size();++i)if(sorted[i-1]->id==sorted[i]->id)fail("duplicate mutation id");
  // Metadata reads/I/O failures fence; caller argument/admission failures do not.
  try {
    auto actual=load(old.image,old.limits);
    if(!same(actual,old) || actual.degraded_roots || actual.degraded_metadata)fail("writer checkpoint health changed");
    admit_contract(old.image->contract());
    if(old.image->contract().write_failure_unit_bytes>old.unit)fail("backend failure unit changed");
  } catch(...) {p.poisoned=true;throw;}
  // Missing deletes are argument errors, verified before encoding or writes.
  for(auto m:sorted)if(m->erase) {
    std::optional<Entry> e;
    try{e=lookup(old,m->id);}catch(...){p.poisoned=true;throw;}
    if(!e)fail("erase missing object");
  }
  Plan plan;plan.next=old;plan.next.checkpoint=old.generation+1;plan.next.generation=old.generation+2;
  plan.changed_keys=mutations.size();
  plan.next.degraded_roots=false;plan.next.degraded_metadata=false;
  auto cursor=p.append_floor;
  PayloadAllocation allocation;
  std::vector<PreparedObject> objects;objects.reserve(sorted.size());std::vector<Change> changes;changes.reserve(sorted.size());
  for(auto m:sorted) {
    if(m->erase) {changes.push_back({m->id,true,{}});continue;}
    auto encoded=p.encoder.encode_with_lz4(m->bytes,m->lz4_candidate);PreparedObject record;
    auto& e=record.entry;e={m->id,plan.next.checkpoint,cursor,static_cast<std::uint32_t>(m->bytes.size()),
                           static_cast<std::uint32_t>(encoded.payload.size()),{}};
    auto& h=record.header;set_magic(h,data_magic);put32(h,8,1);put32(h,12,encoded.codec);
    put64(h,16,e.id);put64(h,24,e.generation);put32(h,32,e.logical);put32(h,36,e.stored);copy(h,40,old.id);
    if(encoded.codec!=detail::kStoreRaw)copy(h,56,encoded.decoded_digest);
    e.digest=detail::record_digest(std::span<const std::byte>(h).first(96),encoded.payload);copy(h,96,e.digest);
    record.payload=std::move(encoded.payload);allocation.place(old,e,128ULL+e.stored,cursor);
    if(cursor>old.limits.maximum_physical_bytes)fail("physical capacity");
    changes.push_back({m->id,false,e});objects.push_back(std::move(record));
  }
  allocation.seal();plan.page_start=align(cursor,old.unit);
  std::vector<Ref> roots;
  try{roots=plan.rewrite(old,old.root,changes);}
  catch(const CapacityError&){throw;}
  catch(const std::bad_alloc&){throw;}
  catch(...){p.poisoned=true;throw;}
  if(old.degraded_metadata) {p.poisoned=true;fail("metadata replica lost during staging");}
  plan.finish(std::move(roots));old.image->admit_write(plan.next.end);
  try {
    for(const auto& record:objects) {
      write_object_record(*old.image,record.entry,record.header,record.payload,old.unit);
    }
    allocation.write_padding(*old.image);
    plan.write_pages(cursor);
    if(plan.next.mirrored) {
      for(const auto& record:objects)verify_object(plan.next,record.entry);
      if(plan.next.degraded_payloads)fail("new payload replica failed independent readback");
    }
    publish(plan.next);
  } catch(...) {p.poisoned=true;throw;}
  p.state=std::move(plan.next);p.append_floor=p.state.end;return flitzfs::info(p.state);
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot() const {
  if(!impl_)fail("moved-from writer");auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();
  p->state=impl_->state;if(impl_->poisoned)p->state.degraded_roots=true;
  (void)read_node(p->state,p->state.root);return PagedObjectStoreSnapshot(std::move(p));
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot_for_staging() const {
  if(!impl_ || impl_->poisoned)fail("staging requires a healthy writer");
  auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();p->state=impl_->state;
  return PagedObjectStoreSnapshot(std::move(p));
}
std::uint64_t PagedObjectStoreWriter::generation_for_staging() const noexcept {
  return poisoned()?0:impl_->state.generation;
}
std::uint64_t PagedObjectStoreWriter::physical_end_for_staging() const noexcept {
  return poisoned()?0:impl_->state.end;
}
PagedStoreInfo PagedObjectStoreWriter::flush_checkpoint() {
  if(poisoned())fail("flush requires a healthy writer");
  auto& p=*impl_;const auto& old=p.state;bool barrier_entered=false;
  try {
    const auto extent=old.image->extent();
    if(extent<old.end || extent>old.limits.maximum_physical_bytes)fail("flush extent bounds");
    auto check=[&] {
      admit_contract(old.image->contract());
      if(old.image->contract().write_failure_unit_bytes>old.unit)fail("flush failure unit changed");
      const auto actual=load(old.image,old.limits);
      if(!same(actual,old) || actual.degraded_roots || actual.degraded_metadata || old.image->extent()!=extent)
        fail("flush checkpoint changed/degraded; independent recovery required");
    };
    check();const auto result=flitzfs::info(old);
    barrier_entered=true;old.image->flush();check();return result;
  }catch(const std::bad_alloc&) {if(barrier_entered)p.poisoned=true;throw;}
  catch(...) {p.poisoned=true;throw;}
}
bool PagedObjectStoreWriter::poisoned() const noexcept {return !impl_ || impl_->poisoned;}
} // namespace flitzfs
