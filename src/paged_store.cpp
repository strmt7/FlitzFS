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
constexpr std::size_t page_bytes=4096,header_bytes=64,ref_bytes=112,record_header_bytes=128;
constexpr std::size_t digest_at=page_bytes-32;
constexpr std::size_t branch_capacity=(digest_at-header_bytes)/ref_bytes;
constexpr std::uint32_t maximum_height=16;
constexpr char root_magic[]="FLTXPG01",page_magic[]="FLTXPGN1",data_magic[]="FLTXDAT1";
[[noreturn]] void fail(const char* why) {throw std::runtime_error(std::string("FlitzFS paged store: ")+why);}
[[noreturn]] void capacity(const char* why) {throw PagedStoreCapacityError(why);}
void check_physical_capacity(std::uint64_t required,std::uint64_t maximum) {
  if(required>maximum)throw PagedStoreCapacityError("physical capacity",true,required-maximum);
}
std::uint64_t add(std::uint64_t a,std::uint64_t b) {
  if(b>(std::numeric_limits<std::uint64_t>::max)()-a)fail("integer overflow");
  return a+b;
}
std::uint64_t multiply(std::uint64_t n,std::uint64_t factor) {
  if(factor && n>(std::numeric_limits<std::uint64_t>::max)()/factor)fail("integer overflow");
  return n*factor;
}
std::uint64_t round_up_divide(std::uint64_t n,std::uint64_t unit) {return n/unit+(n%unit!=0);}
std::uint64_t align(std::uint64_t n,std::uint64_t unit) {return multiply(round_up_divide(n,unit),unit);}
std::uint64_t paired_bank_bytes(std::uint64_t bytes,std::uint32_t unit) {return multiply(align(bytes,unit),2);}
std::uint64_t stream_position(std::uint64_t at,std::uint64_t bytes,std::uint32_t unit) {
  const auto within=at%unit,total=add(within,bytes);
  return add(add(at-within,multiply(total/unit,2ULL*unit)),total%unit);
}
std::uint64_t stream_end(std::uint64_t at,std::uint64_t bytes,std::uint32_t unit) {
  if(!unit)return add(at,bytes);
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
  bool operator==(const Entry&) const =default;
};
// Only observed failures use this queue. When it fills, a bounded keyset sweep
// recovers notifications without allocating an inventory proportional to the volume.
struct PayloadRepairQueue {
  std::array<Entry,kStoreMaxObjects> records{};
  std::size_t count=0;
  std::uint64_t scan_after=0,blocked_generation=0;
  std::optional<Entry> blocked_record;
  bool scan=false;

  void observe(const Entry& entry) noexcept {
    const auto pending=std::span(records).first(count);
    if(std::ranges::find(pending,entry)!=pending.end())return;
    if(count<records.size())records[count++]=entry;
    else {scan=true;scan_after=0;}
  }
  bool pending(std::uint64_t generation) const noexcept {
    return blocked_generation!=generation && (count || scan);
  }
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
  std::shared_ptr<PayloadRepairQueue> payload_repairs;
};
std::size_t entry_bytes(const State& s){return s.mirrored?72:64;}
std::size_t leaf_capacity(const State& s){return (digest_at-header_bytes)/entry_bytes(s);}
std::uint64_t packed_rebuild_size(const State& state,std::uint64_t count,std::uint64_t stored) {
  const auto record_bytes=add(multiply(count,record_header_bytes),stored);
  const auto payload=state.mirrored?paired_bank_bytes(record_bytes,state.unit):align(record_bytes,state.unit);
  auto level=(std::max)(std::uint64_t{1},round_up_divide(count,leaf_capacity(state)));
  auto pages=level;
  for(std::uint32_t height=0;level>1;++height) {
    if(height==maximum_height)fail("rebuild tree height");
    level=round_up_divide(level,branch_capacity);
    pages=add(pages,level);
  }
  const auto index=paired_bank_bytes(multiply(pages,page_bytes),state.unit);
  return add(add(start,payload),index);
}
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
          e.offset<start || e.offset>r.primary || std::uint64_t{record_header_bytes}+e.stored>r.primary-e.offset)
        fail("object descriptor bounds");
      if(s.mirrored && (e.offset/s.unit%2 || e.peer!=add(e.offset,s.unit) || e.peer>r.primary ||
          stream_end(e.offset,std::uint64_t{record_header_bytes}+e.stored,s.unit)>r.primary || stream_end(e.peer,std::uint64_t{record_header_bytes}+e.stored,s.unit)>r.primary))
        fail("payload replica isolation/bounds");
      n.entries.push_back(e);
    }
  }
  auto total=summary(s,n,r.height);
  if(total.first!=r.first || total.last!=r.last || total.count!=r.count ||
      total.logical!=r.logical || total.stored!=r.stored)fail("page summary mismatch");
  return n;
}
// Lives only in a single native query, with the immutable State that admitted
// these exact references. Record payloads are still checked on every read.
class LookupPages {
 public:
  const Node& get(const State& state,const Ref& reference) {
    for(const auto& page:pages_)
      if(page && page->reference==reference)return page->node;
    auto& slot=pages_[next_];
    slot=Page{reference,read_node(state,reference)};
    next_=(next_+1)%pages_.size();
    return slot->node;
  }
 private:
  struct Page {Ref reference;Node node;};
  std::array<std::optional<Page>,4> pages_{};
  std::size_t next_=0;
};
struct QueryMetadata {
  struct Record {
    std::uint64_t id=0;
    Bytes bytes;
  };
  std::array<Record,4> records{};
  std::size_t next=0;
};
std::optional<Entry> lookup(const State& s,std::uint64_t id,LookupPages* scoped=nullptr) {
  if(!id || !s.root.count || id<s.root.first || id>s.root.last)return {};
  auto r=s.root;
  for(std::uint32_t depth=0;depth<=maximum_height;++depth) {
    Node temporary;
    const auto& n=scoped?scoped->get(s,r):(temporary=read_node(s,r));
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
using RecordHeader=std::array<std::byte,record_header_bytes>;
struct ObjectRecord {RecordHeader header{};Bytes payload;bool recovered_primary=false;};
void verify_record_header(const State& s,const Entry& e,const RecordHeader& h) {
  const auto codec=get32(h,12);
  if(!magic(h,data_magic) || get32(h,8)!=1 || get64(h,16)!=e.id || get64(h,24)!=e.generation ||
      get32(h,32)!=e.logical || get32(h,36)!=e.stored || field<16>(h,40)!=s.id ||
      !detail::store_codec_framing(codec,e.logical,e.stored) ||
      !zero(std::span<const std::byte>(h).subspan(codec==detail::kStoreRaw?56:88,codec==detail::kStoreRaw?40:8)) ||
      field<32>(h,96)!=e.digest)fail("object identity/codec/framing");
}
ObjectRecord read_record_at(const State& s,const Entry& e,std::uint64_t offset) {
  ObjectRecord record;auto& h=record.header;
  read_record_bytes(*s.image,offset,0,h,s.mirrored?s.unit:0);
  verify_record_header(s,e,h);
  auto& payload=record.payload;payload.resize(e.stored);
  if(!payload.empty())read_record_bytes(*s.image,offset,record_header_bytes,payload,s.mirrored?s.unit:0);
  if(detail::record_digest(std::span<const std::byte>(h).first(96),payload)!=e.digest)fail("object stored digest");
  return record;
}
ObjectRecord read_record(const State& s,const Entry& e) {
  try{return read_record_at(s,e,e.offset);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&) {
    if(!s.mirrored)throw;
    s.degraded_payloads=true;
    auto survivor=read_record_at(s,e,e.peer);
    survivor.recovered_primary=true;
    return survivor;
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
  auto bytes=get32(record.header,12)==detail::kStoreRaw?std::move(record.payload):decode_record(s,e,record);
  if(record.recovered_primary && s.payload_repairs)s.payload_repairs->observe(e);
  return bytes;
}
unsigned verify_payload_copies(const State& s,const Entry& e) {
  unsigned verified=0;
  for(const auto at:{e.offset,e.peer}) {
    try{(void)decode_record(s,e,read_record_at(s,e,at));++verified;}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){s.degraded_payloads=true;}
  }
  return verified;
}
// True reports a verified survivor and a failed replica.
bool verify_object(const State& s,const Entry& e) {
  if(!s.mirrored){(void)read_object(s,e);return false;}
  const auto verified=verify_payload_copies(s,e);
  if(!verified)fail("both payload copies invalid; no rollback");
  return verified==1;
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
    if(!payload.empty())write_record_bytes(image,at,record_header_bytes,payload,e.peer?unit:0);
  }
}
struct PreparedPage {Ref ref;std::array<std::byte,page_bytes> bytes{};};
PreparedPage prepare_page(const State& state,const Node& node,std::uint32_t height,
    std::uint64_t page_start,std::uint64_t ordinal) {
  PreparedPage page;
  auto& ref=page.ref;
  ref=summary(state,node,height);
  ref.generation=state.checkpoint;
  ref.primary=stream_position(page_start,multiply(ordinal,page_bytes),state.unit);
  ref.peer=add(ref.primary,state.unit);
  if(add(ref.peer,page_bytes)>state.limits.maximum_physical_bytes)capacity("physical capacity");
  auto& bytes=page.bytes;
  set_magic(bytes,page_magic);
  put32(bytes,8,state.mirrored?2:1);
  put32(bytes,12,height);
  put64(bytes,16,ref.generation);
  copy(bytes,24,state.id);
  put32(bytes,40,static_cast<std::uint32_t>(height?node.children.size():node.entries.size()));
  if(height) {
    for(std::size_t i=0;i<node.children.size();++i)encode_ref(bytes,header_bytes+i*ref_bytes,node.children[i]);
  } else {
    for(std::size_t i=0;i<node.entries.size();++i)
      encode_entry(bytes,header_bytes+i*entry_bytes(state),node.entries[i],state.mirrored);
  }
  ref.digest=detail::record_digest(std::span<const std::byte>(bytes).first(digest_at));
  copy(bytes,digest_at,ref.digest);
  return page;
}
void write_page(StoreByteIo& image,const PreparedPage& page) {
  image.write_exact(page.ref.primary,page.bytes);
  image.write_exact(page.ref.peer,page.bytes);
}
void pad_index(StoreByteIo& image,std::uint32_t unit,std::uint64_t end,std::uint64_t pages) {
  const auto used=(pages%(unit/page_bytes))*page_bytes;
  if(!used)return;
  const auto peer_start=end-unit;
  const auto primary_start=peer_start-unit;
  zeros(image,primary_start+used,peer_start);
  zeros(image,peer_start+used,end);
}
struct PreparedObject {Entry entry;RecordHeader header{};Bytes payload;};
PreparedObject prepare_object(const State& state,detail::StoreCodecEncoder& encoder,
    const StoreMutation& mutation) {
  auto encoded=encoder.encode_with_lz4(mutation.bytes,mutation.lz4_candidate,mutation.lz4_literal_prefix_bytes);
  PreparedObject record;
  auto& e=record.entry;
  e={mutation.id,state.checkpoint,0,static_cast<std::uint32_t>(mutation.bytes.size()),
     static_cast<std::uint32_t>(encoded.payload.size()),{}};
  auto& h=record.header;
  set_magic(h,data_magic);put32(h,8,1);put32(h,12,encoded.codec);
  put64(h,16,e.id);put64(h,24,e.generation);put32(h,32,e.logical);put32(h,36,e.stored);copy(h,40,state.id);
  if(encoded.codec!=detail::kStoreRaw)copy(h,56,encoded.decoded_digest);
  e.digest=detail::record_digest(std::span<const std::byte>(h).first(96),encoded.payload);copy(h,96,e.digest);
  record.payload=std::move(encoded.payload);
  return record;
}
void place_object(const State& state,PreparedObject& record,PayloadAllocation& allocation,std::uint64_t& cursor) {
  allocation.place(state,record.entry,std::uint64_t{record_header_bytes}+record.entry.stored,cursor);
  check_physical_capacity(cursor,state.limits.maximum_physical_bytes);
}
struct Change {std::uint64_t id;bool erase;Entry entry;};
PagedCompactedSizes compacted_sizes_after(const State& state,std::span<const Change> changes,
    std::uint64_t rebound_object) {
  LookupPages scoped;
  std::uint64_t removed=0,removed_bytes=0,inserted=0,inserted_bytes=0;
  std::optional<Entry> rebound_before,rebound_after;
  for(const auto& change:changes) {
    const auto previous=lookup(state,change.id,&scoped);
    if(previous) {
      ++removed;
      removed_bytes=add(removed_bytes,previous->stored);
    } else if(change.erase)fail("compacted erase references absent object");
    if(!change.erase) {
      ++inserted;
      inserted_bytes=add(inserted_bytes,change.entry.stored);
    }
    if(change.id==rebound_object) {
      rebound_before=previous;
      if(!change.erase)rebound_after=change.entry;
    }
  }
  if(rebound_object && !rebound_before && !rebound_after) {
    rebound_before=lookup(state,rebound_object,&scoped);
    rebound_after=rebound_before;
  }
  if(removed>state.root.count || removed_bytes>state.root.stored ||
      (rebound_before && rebound_before->stored>state.root.stored))
    fail("compacted change totals exceed verified inventory");
  // Compaction preserves the sealed payloads except for an identity-bound
  // descriptor. Its prior size can be zero in the lower bound, and its new
  // encoded size cannot exceed its logical length.
  const auto before_stored=state.root.stored-(rebound_before?rebound_before->stored:0);
  const auto after_count=add(state.root.count-removed,inserted);
  auto after_stored=add(state.root.stored-removed_bytes,inserted_bytes);
  if(rebound_after)after_stored=add(after_stored,rebound_after->logical-rebound_after->stored);
  return {packed_rebuild_size(state,state.root.count,before_stored),
          packed_rebuild_size(state,after_count,after_stored)};
}
struct Plan {
  State next;
  std::size_t changed_keys=kStoreMaxObjects;
  std::uint64_t page_start=0,obsolete_bytes=0;
  std::vector<PreparedPage> pages;
  Ref make_page(const Node& node,std::uint32_t height) {
    // Bound staging from this admitted public/native transaction's changed
    // paths, at most 17 levels per path plus splits.
    // This work limit is independent of the volume's object count.
    if(pages.size()>=2*(maximum_height+1)*changed_keys+maximum_height+1)capacity("transaction page budget");
    auto page=prepare_page(next,node,height,page_start,pages.size());
    const auto result=page.ref;
    pages.push_back(std::move(page));
    return result;
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
    obsolete_bytes=add(obsolete_bytes,2*page_bytes);
    if(!ref.height) {
      auto entries=std::move(node.entries);
      for(const auto& c:changes) {
        auto at=std::lower_bound(entries.begin(),entries.end(),c.id,[](const auto& e,auto id){return e.id<id;});
        if(at!=entries.end() && at->id==c.id) {
          obsolete_bytes=add(obsolete_bytes,(std::uint64_t{record_header_bytes}+at->stored)*(old.mirrored?2:1));
          if(c.erase)entries.erase(at);
          else *at=c.entry;
        }else {
          if(c.erase)fail("erase missing object");
          entries.insert(at,c.entry);
        }
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
    next.end=add(page_start,paired_bank_bytes(multiply(pages.size(),page_bytes),next.unit));
    check_physical_capacity(next.end,next.limits.maximum_physical_bytes);
  }
  void write_pages(std::uint64_t data_end) {
    // Initialize only padding. Each complete metadata page initializes its
    // own range, and all page banks except the last are fully occupied.
    zeros(*next.image,data_end,page_start);
    pad_index(*next.image,next.unit,next.end,pages.size());
    for(const auto& page:pages)write_page(*next.image,page);
  }
};
// Merge immutable versions of the same tree by key. An identical reference
// skips its subtree. Each frontier retains only the remaining siblings on its
// current path and one decoded leaf.
struct TreeDifference {
  struct Side {
    std::vector<Ref> pending;
    std::vector<Entry> entries;

    bool present() const noexcept {return !entries.empty() || !pending.empty();}
    std::uint64_t first() const noexcept {return entries.empty()?pending.back().first:entries.back().id;}
    void open(const State& state) {
      auto node=read_node(state,pending.back());
      pending.pop_back();
      pending.insert(pending.end(),node.children.rbegin(),node.children.rend());
      entries=std::move(node.entries);
      std::ranges::reverse(entries);
    }
    Entry take() noexcept {auto entry=entries.back();entries.pop_back();return entry;}
  };
  struct Edit {
    std::uint64_t id;
    std::optional<Entry> source;
  };
  Side prior,current;

  TreeDifference(const Ref& before,const Ref& after):prior{{before},{}},current{{after},{}} {}
  bool done() const noexcept {return !prior.present() && !current.present();}
  std::optional<Edit> next(const State& before,const State& after,std::size_t& steps) {
    while(steps && !done()) {
      --steps;
      if(prior.entries.empty() && current.entries.empty() && prior.present() && current.present() &&
          prior.pending.back()==current.pending.back()) {
        prior.pending.pop_back();current.pending.pop_back();continue;
      }
      const bool old_first=prior.present() && (!current.present() || prior.first()<=current.first());
      const bool new_first=current.present() && (!prior.present() || current.first()<=prior.first());
      if(old_first && prior.entries.empty() && (!new_first || !current.entries.empty() ||
          prior.pending.back().height>=current.pending.back().height)) {
        prior.open(before);continue;
      }
      if(new_first && current.entries.empty()) {current.open(after);continue;}
      if(!new_first)return Edit{prior.take().id,{}};
      const auto entry=current.take();
      if(!old_first || !(prior.take()==entry))return Edit{entry.id,entry};
    }
    return {};
  }
};

// Bulk construction keeps one unfinished leaf plus <=35 child references per
// level. Completed pages stream directly to the unpublished target. It does not
// retain a page plan or object inventory proportional to the source volume.
struct Rebuild {
  State next;
  std::uint64_t payload_cursor=start,index_start=0,index_cursor=start,pages=0;
  PayloadAllocation allocation;
  // Bind the index pass to the headers generated and verified during copy.
  // Otherwise the unpublished target could supply its own descriptor digest.
  Digest written_headers{},indexed_headers{};
  Node leaf;
  std::array<Node,maximum_height+1> levels;

  void append_payload(Entry& entry,const RecordHeader& header,std::span<const std::byte> encoded,
      const Bytes& decoded) {
    allocation.place(next,entry,std::uint64_t{record_header_bytes}+entry.stored,payload_cursor);
    if(payload_cursor>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    next.image->admit_write(payload_cursor);
    allocation.write_padding(*next.image);
    write_object_record(*next.image,entry,header,encoded,next.unit);
    if(read_object(next,entry)!=decoded)fail("rebuild CPU comparison");
    if(next.mirrored)verify_object(next,entry);
    if(next.degraded_payloads)fail("rebuild produced degraded payload replicas");
    written_headers=detail::record_digest(written_headers,header);
  }
  void finish_payloads() {
    index_start=align(payload_cursor,next.unit);
    if(index_start>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    next.image->admit_write(index_start);
    allocation.seal();
    allocation.write_padding(*next.image);
    zeros(*next.image,payload_cursor,index_start);
  }
  Entry index_record() {
    const auto unit=next.mirrored?next.unit:0;
    Entry entry;
    entry.offset=index_cursor;
    if(unit)entry.peer=add(entry.offset,unit);
    const auto last_copy=entry.peer?entry.peer:entry.offset;
    if(!index_start || stream_end(last_copy,record_header_bytes,unit)>index_start)
      fail("rebuild descriptor header exceeds payload stream");
    RecordHeader header{};
    read_record_bytes(*next.image,entry.offset,0,header,unit);
    entry.id=get64(header,16);
    entry.generation=get64(header,24);
    entry.logical=get32(header,32);
    entry.stored=get32(header,36);
    entry.digest=field<32>(header,96);
    const auto size=std::uint64_t{record_header_bytes}+entry.stored;
    if(!entry.id || !entry.generation || entry.generation>next.checkpoint ||
        entry.logical>kStoreMaxObjectBytes || entry.stored>entry.logical ||
        stream_end(last_copy,size,unit)>index_start)
      fail("rebuild descriptor exceeds payload stream");
    verify_record_header(next,entry,header);
    indexed_headers=detail::record_digest(indexed_headers,header);
    index_cursor=unit?stream_position(entry.offset,size,unit):add(entry.offset,size);
    leaf.entries.push_back(entry);
    if(leaf.entries.size()==leaf_capacity(next)) {
      auto ref=write_node(leaf,0);
      leaf.entries.clear();
      emit(ref);
    }
    return entry;
  }
  Ref write_node(const Node& node,std::uint32_t height) {
    const auto page=prepare_page(next,node,height,index_start,pages);
    const auto next_pages=add(pages,1);
    const auto end=add(index_start,paired_bank_bytes(multiply(next_pages,page_bytes),next.unit));
    if(end>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    next.image->admit_write(end);
    write_page(*next.image,page);
    pages=next_pages;
    next.end=end;
    return page.ref;
  }
  void emit(Ref ref) {
    if(ref.height>maximum_height)fail("rebuild tree height");
    auto& node=levels[ref.height];node.children.push_back(ref);
    if(node.children.size()==branch_capacity) {
      if(ref.height==maximum_height)fail("rebuild tree height");
      auto parent=write_node(node,ref.height+1);node.children.clear();emit(parent);
    }
  }
  Ref finish_tree() {
    if(!leaf.entries.empty()) {auto ref=write_node(leaf,0);leaf.entries.clear();emit(ref);}
    for(std::uint32_t h=0;h<=maximum_height;++h) {
      auto& node=levels[h];if(node.children.empty())continue;
      bool higher=false;for(auto k=h+1;k<=maximum_height;++k)higher=higher || !levels[k].children.empty();
      if(node.children.size()==1 && !higher)return node.children.front();
      if(h==maximum_height)fail("rebuild tree height");
      auto ref=write_node(node,h+1);node.children.clear();emit(ref);
    }
    return write_node(Node{},0);
  }
  void finish() {
    const auto payload_alignment=next.mirrored?2ULL*next.unit:next.unit;
    if(align(index_cursor,payload_alignment)!=index_start || indexed_headers!=written_headers)
      fail("rebuild payload stream differs from verified copy");
    next.root=finish_tree();
    if(next.end!=packed_rebuild_size(next,next.root.count,next.root.stored))
      fail("rebuild packed size differs from index totals");
    pad_index(*next.image,next.unit,next.end,pages);
  }
};
PreparedObject prepare_rebuilt_object(const State& target,const Entry& source_entry,ObjectRecord source_record,
    const Bytes& decoded,detail::StoreCodecEncoder& encoder,bool reencode,bool improve=false) {
  auto encoded=[&] {
    if(reencode)return encoder.encode(decoded);
    if(improve)if(auto smaller=encoder.try_cold_encoding(decoded,source_record.payload.size()))
      return std::move(*smaller);
    return detail::EncodedStoreObject{get32(source_record.header,12),std::move(source_record.payload),
        field<32>(source_record.header,56)};
  }();
  PreparedObject record;
  auto& entry=record.entry;
  entry={source_entry.id,source_entry.generation,0,static_cast<std::uint32_t>(decoded.size()),
         static_cast<std::uint32_t>(encoded.payload.size()),{}};
  auto& header=record.header;
  set_magic(header,data_magic);put32(header,8,1);put32(header,12,encoded.codec);
  put64(header,16,entry.id);put64(header,24,entry.generation);put32(header,32,entry.logical);
  put32(header,36,entry.stored);copy(header,40,target.id);
  if(encoded.codec!=detail::kStoreRaw)copy(header,56,encoded.decoded_digest);
  entry.digest=detail::record_digest(std::span<const std::byte>(header).first(96),encoded.payload);copy(header,96,entry.digest);
  record.payload=std::move(encoded.payload);
  return record;
}
bool copy_rebuild_object(Rebuild& build,const State& original,const Entry& source_entry,
    detail::StoreCodecEncoder& encoder,std::uint64_t replacement_id,
    std::span<const std::byte> replacement,bool reencode,
    const std::function<bool(std::uint64_t,std::span<const std::byte>)>* keep=nullptr,bool improve=false) {
  auto source_record=read_record(original,source_entry);
  auto bytes=decode_record(original,source_entry,source_record);
  if(keep && !(*keep)(source_entry.id,bytes))return false;
  if(source_entry.id==replacement_id)bytes.assign(replacement.begin(),replacement.end());
  auto record=prepare_rebuilt_object(build.next,source_entry,std::move(source_record),bytes,
      encoder,reencode || source_entry.id==replacement_id,improve);
  build.append_payload(record.entry,record.header,record.payload,bytes);
  return true;
}

} // namespace

struct PagedObjectStoreSnapshot::Impl {
  State state;
  std::optional<LookupPages> lookup_pages;
  std::optional<QueryMetadata> query_metadata;
};
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
  std::uint64_t append_floor=0,obsolete_bytes=0;
  bool poisoned=false;
  std::shared_ptr<const void> owner=std::make_shared<const unsigned char>(0);
  mutable std::weak_ptr<const void> pending_rebuild;
  mutable std::shared_ptr<PayloadRepairQueue> payload_repairs;
  std::uint64_t scrub_after=0,scrub_through=0;
  bool scrub_prefix_verified=true;
  Impl(std::shared_ptr<StoreByteIo> image,StoreCompressionOptions options):lease(std::move(image)),encoder(options){}
};
struct PagedObjectStoreWriter::Rewrite::Impl {
  State original;
  std::weak_ptr<const void> owner;
  std::shared_ptr<const void> reservation=std::make_shared<const unsigned char>(0);
  std::unique_ptr<PagedObjectStoreWriter::Impl> target;
  Rebuild build;
  Bytes replacement;
  std::size_t replacement_identity_offset;
  std::optional<RebuildRetention> retention;
  std::optional<RebuildColdEncoding> cold;
  struct RetainedInventory {
    std::uint64_t objects=0,logical=0,stored=0;
    Digest digest{};
    void include(const Entry& e) {
      objects=add(objects,1);logical=add(logical,e.logical);stored=add(stored,e.stored);
      digest=detail::record_digest(digest,e.digest);
    }
    bool operator==(const RetainedInventory&) const = default;
  } retained,copied;
  std::uint64_t source_extent=0,target_extent=0,after=0,completed=0,logical=0,stored=0,write_bound=0;
  PagedRewritePhase phase=PagedRewritePhase::verifying_source;
  bool reencode=true,replacement_seen=false;
  PagedRewriteMode mode;
  std::optional<Entry> capacity_damage;
  std::uint64_t capacity_repair_generation=0,original_root_stored=0;
  bool repair_confirmed=false;
  struct Reconciliation {
    State source;
    Bytes root;
    TreeDifference edits;
    std::optional<TreeDifference::Edit> deferred;
  };
  std::optional<Reconciliation> reconciliation;

  Impl(const PagedObjectStoreWriter::Impl& writer,std::shared_ptr<StoreByteIo> output,
      StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> root,bool encode,
      std::size_t identity_offset,std::optional<RebuildRetention> filter,PagedRewriteMode rewrite_mode,
      std::optional<RebuildColdEncoding> cold_encoding)
      :original(writer.state),owner(writer.owner),replacement_identity_offset(identity_offset),
       retention(std::move(filter)),cold(cold_encoding),reencode(encode),mode(rewrite_mode) {
    if(mode==PagedRewriteMode::reconcile_foreground && (retention || root.empty() || encode))
      fail("foreground reconciliation requires a lossless native identity-only rebuild");
    if(cold && (!cold->eligible || mode!=PagedRewriteMode::reconcile_foreground ||
        cold->after_generation>original.generation))
      fail("cold encoding requires a bound indexed reconciliation");
    if(mode==PagedRewriteMode::reconcile_foreground && writer.payload_repairs &&
        writer.payload_repairs->blocked_generation==original.generation && writer.payload_repairs->blocked_record) {
      capacity_damage=writer.payload_repairs->blocked_record;
      capacity_repair_generation=original.generation;
    }
    if(zero(id) || (id==original.id && !retention))fail("staged rebuild needs a new identity");
    if(retention && (id!=original.id || !root.empty() || encode || original.mirrored ||
        !retention->keep || !retention->charge || !retention->maximum_object_bytes ||
        retention->maximum_object_bytes>page_bytes))
      fail("filtered rebuild requires bounded advisory records and a charge");
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
  void source_check(const State& current) const {
    auto& image=*current.image;
    admit_contract(image.contract());
    const auto expected_extent=current.generation==original.generation?source_extent:current.end;
    if(image.contract().write_failure_unit_bytes>current.unit || image.extent()!=expected_extent ||
        !same(current,load(current.image,current.limits)))
      fail("staged rebuild source checkpoint/contract changed");
  }
  bool compatible(const State& current) const noexcept {
    if(same(original,current))return true;
    return mode==PagedRewriteMode::reconcile_foreground && original.image==current.image &&
        current.id==original.id && current.unit==original.unit && current.mirrored==original.mirrored &&
        current.end>=original.end && current.generation>original.generation;
  }
  void reset(PagedRewritePhase next) noexcept {phase=next;after=completed=logical=stored=0;}
  void count(const Entry& entry) {
    after=entry.id;completed=add(completed,1);logical=add(logical,entry.logical);stored=add(stored,entry.stored);
  }
  void totals(const State& state) const {
    if(completed!=state.root.count || logical!=state.root.logical || stored!=state.root.stored)
      fail("staged verification inventory differs");
  }
  template<class Visit> bool visit_quantum(const State& input,Visit&& visit) {
    std::size_t remaining=retention?leaf_capacity(input)/4:1;
    const auto before=completed;
    auto visitor=[&](const Entry& entry) {
      if(retention && entry.logical>retention->maximum_object_bytes)fail("oversized advisory record");
      visit(entry);
      count(entry);
      return --remaining!=0;
    };
    bool done=false;
    walk(input,input.root,after,visitor,done);
    return completed!=before;
  }
  void begin_copy(const State& current) {
    source_check(current);
    if(capacity_damage) {
      const auto rebound=target->encoder.encode(replacement).payload.size();
      const auto stored=add(original.root.stored-original_root_stored,rebound);
      const auto projected=packed_rebuild_size(build.next,original.root.count,stored);
      check_physical_capacity(projected,build.next.limits.maximum_physical_bytes);
      if(!repair_confirmed && projected>=source_extent)
        capacity("staged repair found neither the reported fault nor a smaller image");
    }
    build.next.image->admit_write(start+2*build.next.unit);
    if(retention) {
      if(!retention->charge(write_bound))return;
      build.next.limits.maximum_physical_bytes=write_bound-2*page_bytes;
    }
    zeros(*build.next.image,0,start);
    reset(PagedRewritePhase::copying);
  }
  void compare(const State& source,const Entry& expected_entry,const Entry& entry,
      std::span<const std::byte> native_root) const {
    if(expected_entry.generation!=entry.generation || expected_entry.logical!=entry.logical)
      fail("staged target object differs");
    auto expected=read_object(source,expected_entry);
    if(entry.id==1 && !native_root.empty())expected.assign(native_root.begin(),native_root.end());
    if(read_object(build.next,entry)!=expected)fail("staged target CPU comparison");
    if(build.next.mirrored)verify_object(build.next,entry);
  }
  void mark_ready(const State& current) {
    source_check(current);
    build.next.limits=original.limits;
    target->state=build.next;target->append_floor=build.next.end;
    phase=PagedRewritePhase::ready;
  }
  void begin_reconciliation(const State& current) {
    source_check(current);
    const auto descriptor=lookup(current,1);
    if(!descriptor || descriptor->logical!=replacement.size())fail("reconciled native root length");
    auto root=read_object(current,*descriptor);
    if(replacement_identity_offset>root.size() || current.id.size()>root.size()-replacement_identity_offset ||
        field<16>(root,replacement_identity_offset)!=current.id)
      fail("reconciled native root identity");
    copy(root,replacement_identity_offset,build.next.id);
    reconciliation.emplace(Reconciliation{current,std::move(root),TreeDifference(original.root,current.root),{}});
    build.next.generation=add(current.generation,2);
    build.next.checkpoint=current.generation+1;
    phase=PagedRewritePhase::reconciling;
  }
  void apply_reconciliation() {
    auto& work=*reconciliation;
    Plan plan{build.next};
    auto cursor=build.next.end;
    PayloadAllocation allocation;
    std::vector<PreparedObject> objects;
    std::vector<Change> changes;
    std::uint64_t remaining=kStoreMaxObjectBytes;
    for(auto steps=leaf_capacity(work.source);steps;) {
      auto edit=std::exchange(work.deferred,std::nullopt);
      if(edit)--steps;
      else edit=work.edits.next(original,work.source,steps);
      if(!edit)break;
      if(!edit->source) {changes.push_back({edit->id,true,{}});continue;}
      const auto& entry=*edit->source;
      if(entry.logical>remaining) {work.deferred=std::move(edit);break;}
      remaining-=entry.logical;
      auto from=read_record(work.source,entry);
      const bool rebind=entry.id==1;
      const auto decoded=rebind?Bytes{}:decode_record(work.source,entry,from);
      const auto& bytes=rebind?work.root:decoded;
      auto record=prepare_rebuilt_object(plan.next,entry,std::move(from),bytes,target->encoder,rebind);
      allocation.place(plan.next,record.entry,add(record_header_bytes,record.entry.stored),cursor);
      changes.push_back({entry.id,false,record.entry});
      objects.push_back(std::move(record));
    }
    if(!changes.empty()) {
      allocation.seal();plan.page_start=align(cursor,plan.next.unit);
      plan.changed_keys=changes.size();
      plan.finish(plan.rewrite(build.next,build.next.root,changes));
      if(plan.next.end>=work.source.end)capacity("reconciled rebuild cannot reduce occupied bytes");
      plan.next.image->admit_write(plan.next.end);
      for(const auto& record:objects)
        write_object_record(*plan.next.image,record.entry,record.header,record.payload,plan.next.unit);
      allocation.write_padding(*plan.next.image);
      plan.write_pages(cursor);
      target->obsolete_bytes=add(target->obsolete_bytes,plan.obsolete_bytes);
      build.next=std::move(plan.next);
    }
    if(work.edits.done() && !work.deferred) {
      if(build.next.root.count!=work.source.root.count || build.next.root.logical!=work.source.root.logical)
        fail("reconciled rebuild inventory differs");
      phase=PagedRewritePhase::publishing;
    }
  }
  void verify_reconciliation(const State& current) {
    auto& work=*reconciliation;
    // One affected payload per step. A deleted key must be absent in the
    // independently reparsed target. Unchanged subtrees were already checked.
    auto steps=leaf_capacity(work.source);
    if(const auto edit=work.edits.next(original,work.source,steps)) {
      const auto entry=lookup(build.next,edit->id);
      if(entry.has_value()!=edit->source.has_value())fail("reconciled rebuild membership differs");
      if(entry)compare(work.source,*edit->source,*entry,work.root);
    }
    if(work.edits.done()) {
      original=std::move(work.source);
      source_extent=original.end;
      replacement=std::move(work.root);
      reconciliation.reset();
      mark_ready(current);
    }
  }
  void step(const State& current) {
    auto image=build.next.image;
    if(image->extent()!=target_extent)fail("staged target extent changed");
    admit_contract(image->contract());
    if(image->contract().write_failure_unit_bytes>build.next.unit)fail("staged target contract changed");
    if(phase==PagedRewritePhase::verifying_source) {
      const bool advanced=visit_quantum(original,[&](const Entry& entry) {
        auto bytes=read_object(original,entry);
        if(original.mirrored && verify_object(original,entry) && capacity_damage==entry)repair_confirmed=true;
        if(entry.id==1 && !replacement.empty()) {
          original_root_stored=entry.stored;
          if(bytes.size()!=replacement.size() || replacement_identity_offset>bytes.size() ||
              build.next.id.size()>bytes.size()-replacement_identity_offset)
            fail("staged native root length");
          copy(bytes,replacement_identity_offset,build.next.id);
          if(bytes!=replacement)fail("staged replacement differs beyond identity");
          replacement_seen=true;
        }
        if(retention && retention->keep(entry.id,bytes))retained.include(entry);
      });
      if(!advanced) {
        totals(original);
        if(!replacement.empty() && !replacement_seen)fail("staged native root missing");
        if(retention) {
          const auto projected=packed_rebuild_size(build.next,retained.objects,retained.stored);
          if(!retained.objects || projected>=source_extent || projected>build.next.limits.maximum_physical_bytes)
            capacity("filtered copy cannot reduce occupied bytes");
          write_bound=add(projected,2*page_bytes);
          phase=PagedRewritePhase::awaiting_charge;
        }else begin_copy(current);
      }
    } else if(phase==PagedRewritePhase::awaiting_charge) {
      begin_copy(current);
    } else if(phase==PagedRewritePhase::copying) {
      const bool advanced=visit_quantum(original,[&](const Entry& entry) {
        const bool improve=cold && entry.generation>cold->after_generation && cold->eligible(entry.id);
        const auto kept=copy_rebuild_object(build,original,entry,target->encoder,replacement.empty()?0:1,
            replacement,reencode,retention?&retention->keep:nullptr,improve);
        if(retention && kept)copied.include(entry);
      });
      if(!advanced) {
        totals(original);
        if(retention && retained!=copied)fail("filtered copy selection changed");
        build.finish_payloads();
        reset(PagedRewritePhase::building_index);
      }
    } else if(phase==PagedRewritePhase::building_index) {
      const auto expected_count=retention?retained.objects:original.root.count;
      const auto expected_logical=retention?retained.logical:original.root.logical;
      const auto remaining=expected_count-completed;
      if(remaining) {
        const auto batch=(std::min)(remaining,std::uint64_t{leaf_capacity(build.next)});
        for(std::uint64_t i=0;i<batch;++i)count(build.index_record());
      } else {
        build.finish();
        totals(build.next);
        if(completed!=expected_count || logical!=expected_logical)fail("staged copy inventory differs");
        if(retention && (stored!=retained.stored || add(build.next.end,2*page_bytes)!=write_bound))
          fail("filtered copy exceeded its charged plan");
        reset(PagedRewritePhase::verifying_target);
      }
    } else if(phase==PagedRewritePhase::verifying_target || phase==PagedRewritePhase::verifying_publication) {
      if(!visit_quantum(build.next,[&](const Entry& entry){
        const auto source=lookup(original,entry.id);
        if(!source)fail("staged target object differs");
        compare(original,*source,entry,replacement);
      })) {
        totals(build.next);
        if(phase==PagedRewritePhase::verifying_target)reset(PagedRewritePhase::publishing);
        else mark_ready(current);
      }
    } else if(phase==PagedRewritePhase::publishing) {
      source_check(current);image->admit_write(build.next.end);publish(build.next);
      auto published=load(image,build.next.limits);
      if(!same(build.next,published) || published.degraded_roots || published.degraded_metadata || published.degraded_payloads)
        fail("staged published checkpoint differs/degraded");
      build.next=std::move(published);
      if(reconciliation) {
        reconciliation->edits=TreeDifference(original.root,reconciliation->source.root);
        reset(PagedRewritePhase::verifying_reconciliation);
      } else reset(PagedRewritePhase::verifying_publication);
    } else if(phase==PagedRewritePhase::ready) {
      begin_reconciliation(current);
    } else if(phase==PagedRewritePhase::reconciling) {
      apply_reconciliation();
    } else if(phase==PagedRewritePhase::verifying_reconciliation) {
      verify_reconciliation(current);
    }
    if(build.next.degraded_metadata || build.next.degraded_payloads)fail("staged target replicas are degraded");
    target_extent=image->extent();
  }
};
PagedObjectStoreWriter::Rewrite::Rewrite(std::unique_ptr<Impl> value) noexcept:impl_(std::move(value)){}
PagedObjectStoreWriter::Rewrite::Rewrite(Rewrite&&) noexcept=default;
PagedObjectStoreWriter::Rewrite& PagedObjectStoreWriter::Rewrite::operator=(Rewrite&&) noexcept=default;
PagedObjectStoreWriter::Rewrite::~Rewrite()=default;
bool PagedObjectStoreWriter::Rewrite::matches(const PagedObjectStoreWriter& writer) const noexcept {
  return impl_ && !writer.poisoned() && impl_->owner.lock()==writer.impl_->owner && impl_->compatible(writer.impl_->state);
}
bool PagedObjectStoreWriter::Rewrite::ready_for(const PagedObjectStoreWriter& writer) const noexcept {
  return matches(writer) && impl_->phase==PagedRewritePhase::ready && same(impl_->original,writer.impl_->state);
}
std::uint64_t PagedObjectStoreWriter::Rewrite::verified_capacity_repair_generation() const noexcept {
  return impl_ && impl_->phase==PagedRewritePhase::ready && impl_->repair_confirmed
      ?impl_->capacity_repair_generation:0;
}
StoreIdentity PagedObjectStoreWriter::Rewrite::identity() const noexcept {return impl_?impl_->build.next.id:StoreIdentity{};}
bool PagedObjectStoreWriter::Rewrite::replaces_native_root() const noexcept {return impl_ && !impl_->replacement.empty();}
PagedRewriteProgress PagedObjectStoreWriter::Rewrite::progress() const noexcept {
  return impl_?PagedRewriteProgress{impl_->phase,impl_->original.root.count,impl_->completed,impl_->write_bound}:PagedRewriteProgress{};
}
bool PagedObjectStoreWriter::Rewrite::advance(const PagedObjectStoreWriter& writer) {
  if(!matches(writer)) {impl_.reset();fail("staged rebuild source owner/generation is stale");}
  if(ready_for(writer))return true;
  try{impl_->step(writer.impl_->state);return ready_for(writer);}
  catch(...){impl_.reset();throw;}
}
PagedObjectStoreWriter PagedObjectStoreWriter::Rewrite::finish(const PagedObjectStoreWriter& writer) {
  if(!matches(writer)) {impl_.reset();fail("staged rebuild source owner/generation is stale");}
  if(!ready_for(writer))fail("staged rebuild has not verified the current source");
  auto candidate=std::move(impl_);return PagedObjectStoreWriter(std::move(candidate->target));
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,bool reencode) const {
  return prepare_rebuild_replacing(std::move(target),id,compression,{},reencode);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_replacing(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> replacement,bool reencode,
    std::size_t identity_offset,PagedRewriteMode mode,std::optional<RebuildColdEncoding> cold) const {
  return prepare_rebuild_impl(std::move(target),id,compression,replacement,reencode,identity_offset,{},mode,cold);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_retaining(std::shared_ptr<StoreByteIo> target,
    StoreCompressionOptions compression,RebuildRetention keep) const {
  if(poisoned())fail("filtered rebuild requires a healthy source writer");
  return prepare_rebuild_impl(std::move(target),impl_->state.id,compression,{},false,0,std::move(keep),
      PagedRewriteMode::exact_checkpoint);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_impl(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> replacement,bool reencode,
    std::size_t identity_offset,std::optional<RebuildRetention> retention,PagedRewriteMode mode,
    std::optional<RebuildColdEncoding> cold) const {
  if(poisoned())fail("staged rebuild requires a healthy source writer");
  if(!impl_->pending_rebuild.expired())fail("source already has a staged rebuild");
  auto candidate=std::make_unique<Rewrite::Impl>(*impl_,std::move(target),id,compression,
      replacement,reencode,identity_offset,std::move(retention),mode,cold);
  impl_->pending_rebuild=candidate->reservation;return Rewrite(std::move(candidate));
}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot& PagedObjectStoreSnapshot::operator=(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot::~PagedObjectStoreSnapshot()=default;
PagedObjectStoreSnapshot::QueryScope::QueryScope(Impl& impl) noexcept {
  if(impl.query_metadata)return;
  owner_=&impl;
  owns_lookup_pages_=!impl.lookup_pages;
  if(owns_lookup_pages_)impl.lookup_pages.emplace();
  impl.query_metadata.emplace();
}
PagedObjectStoreSnapshot::QueryScope::~QueryScope() {
  if(!owner_)return;
  owner_->query_metadata.reset();
  if(owns_lookup_pages_)owner_->lookup_pages.reset();
}
PagedObjectStoreSnapshot::QueryScope PagedObjectStoreSnapshot::scoped_query() const noexcept {
  return QueryScope(*impl_);
}
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits) {
  auto p=std::make_unique<Impl>();p->state=load(std::move(image),limits);return PagedObjectStoreSnapshot(std::move(p));
}
StoreIdentity PagedObjectStoreSnapshot::identity() const noexcept {return impl_?impl_->state.id:StoreIdentity{};}
std::shared_ptr<StoreByteIo> PagedObjectStoreSnapshot::backing_for_volume_head() const {
  if(!impl_)fail("moved-from paged snapshot");return impl_->state.image;
}
bool PagedObjectStoreSnapshot::same_checkpoint(const PagedObjectStoreSnapshot& other) const noexcept {
  return impl_ && other.impl_ && same(impl_->state,other.impl_->state);
}
PagedStoreLimits PagedObjectStoreSnapshot::limits() const noexcept {return impl_?impl_->state.limits:PagedStoreLimits{};}
std::uint32_t PagedObjectStoreSnapshot::isolation_unit() const noexcept {return impl_?impl_->state.unit:0;}
std::uint64_t PagedObjectStoreSnapshot::rebuilt_size(std::span<const StoreObjectInfo> records) const {
  if(!impl_)fail("moved-from size estimate");
  std::uint64_t previous=0,stored=0;
  for(const auto& record:records) {
    if(!record.id || record.id<=previous || record.logical_bytes>kStoreMaxObjectBytes || record.stored_payload_bytes>record.logical_bytes)
      fail("hypothetical record ordering/length");
    stored=add(stored,record.stored_payload_bytes);
    previous=record.id;
  }
  return packed_rebuild_size(impl_->state,records.size(),stored);
}
PagedCompactedSizes PagedObjectStoreSnapshot::rebuilt_size_after(std::span<const StoreMutation> changes,
    std::uint64_t rebound_object) const {
  if(!impl_)fail("moved-from compacted size estimate");
  if(changes.size()>kStoreMaxObjects)fail("compacted change inventory exceeds transaction budget");
  std::vector<Change> ordered;
  ordered.reserve(changes.size());
  for(const auto& change:changes) {
    if(!change.id || change.bytes.size()>kStoreMaxObjectBytes || (change.erase && !change.bytes.empty()))
      fail("invalid compacted record change");
    const auto size=static_cast<std::uint32_t>(change.bytes.size());
    ordered.push_back({change.id,change.erase,{change.id,0,0,size,size}});
  }
  std::ranges::sort(ordered,{},&Change::id);
  if(std::ranges::adjacent_find(ordered,{},&Change::id)!=ordered.end())
    fail("duplicate compacted record change");
  return compacted_sizes_after(impl_->state,ordered,rebound_object);
}
PagedStoreInfo PagedObjectStoreSnapshot::info() const noexcept {return impl_?flitzfs::info(impl_->state):PagedStoreInfo{};}
std::optional<StoreObjectInfo> PagedObjectStoreSnapshot::stat(std::uint64_t id) const {
  if(!impl_)fail("moved-from snapshot");
  auto e=lookup(impl_->state,id,impl_->lookup_pages?&*impl_->lookup_pages:nullptr);
  if(!e)return {};return StoreObjectInfo{e->id,e->generation,e->logical,e->stored};
}
Bytes PagedObjectStoreSnapshot::read(std::uint64_t id) const {
  return read(id,kStoreMaxObjectBytes);
}
Bytes PagedObjectStoreSnapshot::read(std::uint64_t id,std::size_t maximum_decoded_bytes) const {
  auto bytes=read_if_present(id,maximum_decoded_bytes);
  if(!bytes)fail("missing object");
  return std::move(*bytes);
}
std::span<const std::byte> PagedObjectStoreSnapshot::read_query_metadata(std::uint64_t id,
    std::size_t maximum_decoded_bytes,Bytes& uncached) const {
  if(!impl_ || !impl_->query_metadata || maximum_decoded_bytes>page_bytes) {
    uncached=read(id,maximum_decoded_bytes);
    return uncached;
  }
  auto& query=*impl_->query_metadata;
  for(const auto& entry:query.records) {
    if(!entry.id || entry.id!=id)continue;
    if(entry.bytes.size()>maximum_decoded_bytes)fail("object exceeds decoded read budget");
    return entry.bytes;
  }
  auto& entry=query.records[query.next];
  entry={id,read(id,maximum_decoded_bytes)};
  query.next=(query.next+1)%query.records.size();
  return entry.bytes;
}
std::optional<Bytes> PagedObjectStoreSnapshot::read_if_present(std::uint64_t id,std::size_t maximum_decoded_bytes) const {
  return read_matching(id,maximum_decoded_bytes,std::nullopt);
}
std::optional<Bytes> PagedObjectStoreSnapshot::read_if_generation(std::uint64_t id,std::uint64_t generation,
    std::size_t maximum_decoded_bytes) const {
  return read_matching(id,maximum_decoded_bytes,generation);
}
std::optional<Bytes> PagedObjectStoreSnapshot::read_matching(std::uint64_t id,std::size_t maximum_decoded_bytes,
    std::optional<std::uint64_t> expected_generation) const {
  if(!impl_)fail("moved-from snapshot");
  auto e=lookup(impl_->state,id,impl_->lookup_pages?&*impl_->lookup_pages:nullptr);
  if(!e || (expected_generation && e->generation!=*expected_generation))return {};
  if(e->logical>maximum_decoded_bytes)fail("object exceeds decoded read budget");
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
    PagedStoreLimits limits,StoreCompressionOptions options,bool mirrored,std::span<const StoreMutation> initial,
    std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve_writes) {
  admit_limits(limits);if(zero(id))fail("nonzero store identity required");
  if(initial.size()>kStoreMaxObjects)capacity("initial transaction operation budget");
  auto p=std::make_unique<Impl>(image,options);admit_contract(image->contract());
  if(image->extent())fail("new empty backend required");
  State s;s.image=image;s.limits=limits;s.id=id;s.generation=2;s.checkpoint=1;s.mirrored=mirrored;
  s.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),image->contract().write_failure_unit_bytes);
  auto cursor=start;
  std::uint64_t previous=0,input=0;
  PayloadAllocation allocation;
  std::vector<PreparedObject> objects;objects.reserve(initial.size());
  std::vector<Entry> entries;entries.reserve(initial.size());
  for(const auto& mutation:initial) {
    if(mutation.id<=previous || mutation.erase || mutation.bytes.size()>kStoreMaxObjectBytes ||
        !mutation.lz4_candidate.empty() || mutation.lz4_literal_prefix_bytes)fail("initial transaction fields");
    previous=mutation.id;
    input=add(input,mutation.bytes.size());
    if(input>kStoreMaxTransactionBytes)capacity("initial transaction input budget");
    auto record=prepare_object(s,p->encoder,mutation);
    place_object(s,record,allocation,cursor);
    entries.push_back(record.entry);
    objects.push_back(std::move(record));
  }
  allocation.seal();
  Plan plan;plan.next=s;plan.page_start=align(cursor,s.unit);plan.finish(plan.leaves(entries));
  const auto write_bound=add(plan.next.end,2*page_bytes);
  if(write_bound>maximum_write_bytes)capacity("initial transaction write budget");
  image->admit_write(plan.next.end);
  if(reserve_writes && !reserve_writes(write_bound))capacity("initial transaction write reservation declined");
  zeros(*image,0,start);
  for(const auto& record:objects)
    write_object_record(*image,record.entry,record.header,record.payload,s.unit);
  allocation.write_padding(*image);
  plan.write_pages(cursor);
  publish(plan.next);
  if(!initial.empty()) {
    const auto observed=load(image,limits);
    if(!same(plan.next,observed) || observed.degraded_roots || observed.degraded_metadata)
      fail("initial transaction checkpoint failed readback");
  }
  p->state=std::move(plan.next);p->append_floor=p->state.end;return PagedObjectStoreWriter(std::move(p));
}
PagedObjectStoreWriter PagedObjectStoreWriter::resume(std::shared_ptr<StoreByteIo> image,StoreIdentity expected,
    std::uint64_t generation,PagedStoreLimits limits,StoreCompressionOptions options) {
  return resume_impl(std::move(image),expected,generation,limits,options,false);
}
PagedObjectStoreWriter PagedObjectStoreWriter::resume_impl(std::shared_ptr<StoreByteIo> image,StoreIdentity expected,
    std::uint64_t generation,PagedStoreLimits limits,StoreCompressionOptions options,bool repair_payloads,
    const PagedObjectStoreSnapshot* verified_indexed_source) {
  auto p=std::make_unique<Impl>(image,options);auto s=load(image,limits);admit_contract(image->contract());
  if(s.id!=expected || s.generation!=generation || image->contract().write_failure_unit_bytes>s.unit)
    fail("recovery lease/contract mismatch");
  if(repair_payloads && !s.mirrored)fail("startup repair requires mirrored payloads");
  if(verified_indexed_source) {
    // This private path only retains a healthy indexed source through the
    // head fence. Its owner independently verifies the complete view again.
    if(repair_payloads || !verified_indexed_source->impl_ || s.degraded_roots || s.degraded_metadata)
      fail("unchecked or degraded indexed reopen");
    const auto& verified=verified_indexed_source->impl_->state;
    if(verified.image!=image || !verified.mirrored || verified.degraded_roots ||
        verified.degraded_metadata || verified.degraded_payloads || !same(s,verified))
      fail("verified indexed reopen root changed");
  }else verify(s);
  if(s.degraded_metadata || (s.degraded_payloads && !repair_payloads))
    fail("replica rebuild required before writable resume");
  const auto extent=image->extent();image->flush();auto observed=load(image,limits);
  if(!same(s,observed) || image->extent()!=extent ||
      (verified_indexed_source && (observed.degraded_roots || observed.degraded_metadata)))
    fail("recovery baseline changed");
  p->append_floor=align(extent,s.unit);
  if(s.degraded_roots) {
    s.checkpoint=add(s.generation,1);s.generation=add(s.generation,2);
    if(s.checkpoint==(std::numeric_limits<std::uint64_t>::max)())fail("generation exhausted");
    s.end=p->append_floor;image->admit_write(s.end);zeros(*image,extent,s.end);publish(s);
    s=load(image,limits);verify(s);if(s.degraded_roots || s.degraded_metadata)fail("root repair verification");
  }
  p->state=std::move(s);
  PagedObjectStoreWriter writer(std::move(p));
  if(!writer.impl_->state.degraded_payloads)return writer;
  if(!repair_payloads)fail("replica rebuild required before writable resume");

  // The full source pass above refused unreadable objects before any writes.
  // Walk its immutable index once. Each repair re-resolves the exact descriptor
  // from the current checkpoint before writing fresh banks.
  const auto source=writer.impl_->state;
  auto& queue=writer.impl_->payload_repairs;
  queue=std::make_shared<PayloadRepairQueue>();
  auto visit=[&](const Entry& entry) {
    if(source.degraded_metadata)fail("startup repair source index degraded");
    if(verify_object(source,entry)) {
      queue->observe(entry);
      switch(writer.repair_payload()) {
        case PagedPayloadRepairProgress::checked:
        case PagedPayloadRepairProgress::repaired:break;
        case PagedPayloadRepairProgress::capacity:capacity("indexed startup repair needs append space");
        case PagedPayloadRepairProgress::idle:fail("indexed startup repair lost its observation");
      }
    }
    return true;
  };
  bool done=false;walk(source,source.root,0,visit,done);
  if(source.degraded_metadata)fail("startup repair source index degraded");
  auto checked=load(image,limits);verify(checked);
  if(!same(writer.impl_->state,checked) || checked.degraded_roots || checked.degraded_metadata || checked.degraded_payloads)
    fail("indexed startup repair did not restore healthy copies");
  writer.impl_->state=std::move(checked);
  return writer;
}
PagedObjectStoreWriter PagedObjectStoreWriter::rebuild(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,PagedStoreLimits limits,StoreCompressionOptions options) {
  return rebuild_replacing(source,std::move(target),id,limits,options,0,{},true);
}
PagedObjectStoreWriter PagedObjectStoreWriter::rebuild_replacing(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,PagedStoreLimits limits,StoreCompressionOptions options,
    std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode,std::size_t identity_offset) {
  if(!source.impl_)fail("moved-from rebuild source");admit_limits(limits);
  const auto& original=source.impl_->state;
  if(zero(id) || id==original.id)fail("rebuild requires a new nonzero identity");
  Impl::Lease source_lease(original.image);auto observed=load(original.image,original.limits);
  if(!same(original,observed))fail("rebuild source is not the current checkpoint");
  const auto source_extent=original.image->extent();
  std::uint64_t replaced_stored=0;
  if(replacement_id) {
    const auto entry=lookup(original,replacement_id);
    if(!entry || replacement.size()!=entry->logical)fail("rebuild replacement identity/length");
    // Native rebinding changes only the fixed identity field; reject a stale
    // caller's metadata before creating any target records.
    auto expected=read_object(original,*entry);
    if(identity_offset>expected.size() || id.size()>expected.size()-identity_offset)
      fail("rebuild replacement framing");
    copy(expected,identity_offset,id);
    if(!std::equal(expected.begin(),expected.end(),replacement.begin()))
      fail("rebuild replacement differs beyond image identity");
    replaced_stored=entry->stored;
    verify(original);
  }
  auto p=std::make_unique<Impl>(target,options);admit_contract(target->contract());
  if(target->extent())fail("rebuild requires an empty target");
  Rebuild build;build.next.image=target;build.next.limits=limits;build.next.id=id;build.next.mirrored=original.mirrored;
  build.next.generation=add(original.generation,2);build.next.checkpoint=original.generation+1;
  build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),target->contract().write_failure_unit_bytes);
  if(!reencode) {
    const auto rebound=replacement_id?p->encoder.encode(Bytes(replacement.begin(),replacement.end())).payload.size():0;
    const auto projected_stored=add(original.root.stored-replaced_stored,rebound);
    const auto projected=packed_rebuild_size(build.next,original.root.count,projected_stored);
    check_physical_capacity(projected,limits.maximum_physical_bytes);
    target->admit_write(projected);
  }else target->admit_write(start+2*build.next.unit);
  zeros(*target,0,start);
  auto visitor=[&](const Entry& source_entry) {
    copy_rebuild_object(build,original,source_entry,p->encoder,replacement_id,replacement,reencode);return true;
  };
  bool done=false;
  (void)read_node(original,original.root);
  walk(original,original.root,0,visitor,done);
  build.finish_payloads();
  for(std::uint64_t indexed=0;indexed<original.root.count;++indexed)build.index_record();
  build.finish();
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
  return commit_with_write_budget(mutations,(std::numeric_limits<std::uint64_t>::max)()).storage;
}
PagedStoreWriteResult PagedObjectStoreWriter::commit_with_write_budget(
    std::span<const StoreMutation> mutations,std::uint64_t maximum_write_bytes) {
  return commit_impl(mutations,kStoreMaxObjects,kStoreMaxTransactionBytes,maximum_write_bytes);
}
PagedStoreInfo PagedObjectStoreWriter::commit_native(std::span<const StoreMutation> mutations,bool directory_indexes,bool shared_bases) {
  return commit_impl(mutations,(shared_bases?4:directory_indexes?3:2)*kStoreMaxObjects,
      kStoreMaxTransactionBytes+(shared_bases?131072U:directory_indexes?65536U:0U),
      (std::numeric_limits<std::uint64_t>::max)()).storage;
}
std::optional<PagedStoreInfo> PagedObjectStoreWriter::commit_if_compacted_smaller(
    std::span<const StoreMutation> mutations,std::uint64_t rebound_object,PagedCompactedSizes& sizes,
    std::span<const std::span<const StoreMutation>> alternatives,std::size_t& selected) {
  selected=0;
  CompactedAdmission admission{rebound_object,sizes,alternatives};
  const auto result=commit_impl(mutations,kStoreMaxObjects,kStoreMaxTransactionBytes,
      (std::numeric_limits<std::uint64_t>::max)(),{},&admission);
  if(!result.write_bytes_upper_bound)return {};
  selected=admission.selected;
  return result.storage;
}
PagedStoreWriteResult PagedObjectStoreWriter::commit_impl(
    std::span<const StoreMutation> mutations,std::size_t maximum_operations,
    std::uint64_t maximum_input,std::uint64_t maximum_write_bytes,
    const std::function<bool(std::uint64_t)>& reserve_writes,CompactedAdmission* compacted) {
  if(!impl_ || impl_->poisoned)fail("writer unavailable/poisoned");
  auto& p=*impl_;const auto& old=p.state;
  if(old.degraded_payloads){p.poisoned=true;fail("payload rebuild required before further writes");}
  if(mutations.empty())fail("empty transaction");
  if(old.generation>(std::numeric_limits<std::uint64_t>::max)()-2)capacity("generation exhausted");
  const auto ordered=[&](std::span<const StoreMutation> batch) {
    if(batch.size()>maximum_operations)capacity("transaction operation budget");
    std::vector<const StoreMutation*> result;
    result.reserve(batch.size());
    std::uint64_t input=0;
    for(const auto& mutation:batch) {
      if(!mutation.id || mutation.bytes.size()>kStoreMaxObjectBytes || mutation.lz4_candidate.size()>kStoreMaxObjectBytes ||
          (mutation.erase && (!mutation.bytes.empty() || !mutation.lz4_candidate.empty() ||
              mutation.lz4_literal_prefix_bytes)))fail("mutation fields");
      input=add(input,mutation.bytes.size());
      if(input>maximum_input)capacity("transaction input budget");
      result.push_back(&mutation);
    }
    std::ranges::sort(result,{},&StoreMutation::id);
    if(std::ranges::adjacent_find(result,{},&StoreMutation::id)!=result.end())fail("duplicate mutation id");
    return result;
  };
  const auto sorted=ordered(mutations);
  std::vector<std::vector<const StoreMutation*>> alternatives;
  if(compacted)for(const auto batch:compacted->alternatives) {
    auto alternate=ordered(batch);
    if(!alternate.empty() && !std::ranges::equal(sorted,alternate,[](const auto* first,const auto* second) {
      return first->id==second->id && first->erase==second->erase;
    }))fail("alternate compacted transaction changes object membership");
    alternatives.push_back(std::move(alternate));
  }
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
  struct PreparedChanges {
    std::vector<PreparedObject> objects;
    std::vector<Change> changes;
    std::uint64_t stored=0;
  };
  const auto prepare=[&](const auto& batch) {
    PreparedChanges result;
    result.objects.reserve(batch.size());
    result.changes.reserve(batch.size());
    for(const auto* mutation:batch) {
      if(mutation->erase) {
        result.changes.push_back({mutation->id,true,{}});
        continue;
      }
      auto record=prepare_object(plan.next,p.encoder,*mutation);
      if(!compacted)place_object(plan.next,record,allocation,cursor);
      else if(mutation->id!=compacted->rebound_object)result.stored=add(result.stored,record.entry.stored);
      result.changes.push_back({mutation->id,false,record.entry});
      result.objects.push_back(std::move(record));
    }
    return result;
  };
  auto [objects,changes,stored]=prepare(sorted);
  if(compacted) {
    try {
      compacted->sizes=compacted_sizes_after(old,changes,compacted->rebound_object);
      for(std::size_t i=0;i<alternatives.size();++i) {
        if(alternatives[i].empty())continue;
        std::optional<PreparedChanges> variant;
        try{variant.emplace(prepare(alternatives[i]));}
        catch(const std::bad_alloc&){continue;}
        const auto sizes=compacted_sizes_after(old,variant->changes,compacted->rebound_object);
        if(std::pair{sizes.after_upper,variant->stored}<std::pair{compacted->sizes.after_upper,stored}) {
          compacted->sizes=sizes;
          compacted->selected=i+1;
          stored=variant->stored;
          objects=std::move(variant->objects);
          changes=std::move(variant->changes);
        }
      }
      if(old.degraded_metadata)fail("metadata replica lost during compacted size admission");
    }catch(const std::bad_alloc&){throw;}
    catch(...){p.poisoned=true;throw;}
    if(compacted->sizes.after_upper>=compacted->sizes.before)return {flitzfs::info(old),0};

    // A declined group never needs append space. Use the same sealed data to
    // lay out and write the selected transaction.
    auto record=objects.begin();
    for(auto& change:changes)if(!change.erase) {
      place_object(plan.next,*record,allocation,cursor);
      change.entry=record->entry;
      ++record;
    }
  }
  allocation.seal();plan.page_start=align(cursor,old.unit);
  std::vector<Ref> roots;
  try{roots=plan.rewrite(old,old.root,changes);}
  catch(const PagedStoreCapacityError&){throw;}
  catch(const std::bad_alloc&){throw;}
  catch(...){p.poisoned=true;throw;}
  if(old.degraded_metadata) {p.poisoned=true;fail("metadata replica lost during staging");}
  plan.finish(std::move(roots));
  const auto obsolete=add(p.obsolete_bytes,plan.obsolete_bytes);
  if(obsolete>plan.next.end-start)fail("obsolete byte accounting exceeds committed history");
  const auto write_bound=add(plan.next.end-old.end,2*page_bytes);
  if(write_bound>maximum_write_bytes)capacity("transaction write budget");
  try{old.image->admit_write(plan.next.end);}
  catch(const std::runtime_error& error){capacity(error.what());}
  // The reserved catalog is independent of its credit journal. A failed
  // settlement leaves this catalog's bytes and checkpoint untouched.
  if(reserve_writes && !reserve_writes(write_bound))capacity("transaction write reservation declined");
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
  p.state=std::move(plan.next);p.append_floor=p.state.end;p.obsolete_bytes=obsolete;
  return {flitzfs::info(p.state),write_bound};
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot() const {
  if(!impl_)fail("moved-from writer");auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();
  p->state=impl_->state;if(impl_->poisoned)p->state.degraded_roots=true;
  (void)read_node(p->state,p->state.root);return PagedObjectStoreSnapshot(std::move(p));
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot_for_staging(bool query_cache) const {
  if(!impl_ || impl_->poisoned)fail("staging requires a healthy writer");
  auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();p->state=impl_->state;
  if(query_cache)p->lookup_pages.emplace();
  return PagedObjectStoreSnapshot(std::move(p));
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot_for_indexed_read() const {
  auto snapshot=snapshot_for_staging(true);
  if(impl_->state.mirrored) {
    // Repair bookkeeping must not prevent a verified read under pressure.
    if(!impl_->payload_repairs) {
      try{impl_->payload_repairs=std::make_shared<PayloadRepairQueue>();}
      catch(const std::bad_alloc&){}
    }
    snapshot.impl_->state.payload_repairs=impl_->payload_repairs;
  }
  return snapshot;
}
bool PagedObjectStoreWriter::payload_repair_pending() const noexcept {
  return !poisoned() && impl_->payload_repairs && impl_->payload_repairs->pending(impl_->state.generation);
}
std::uint64_t PagedObjectStoreWriter::capacity_repair_generation() const noexcept {
  if(poisoned() || !impl_->payload_repairs)return 0;
  const auto& queue=*impl_->payload_repairs;
  return queue.blocked_record && queue.blocked_generation==impl_->state.generation?queue.blocked_generation:0;
}
bool PagedObjectStoreWriter::restore_payload_scrub(const PagedPayloadScrubPosition& position) noexcept {
  if(poisoned() || !impl_->state.mirrored || impl_->scrub_through || !position.generation ||
      position.image!=impl_->state.id || position.generation>impl_->state.generation ||
      !position.after || position.after>=position.through || position.through>impl_->state.root.last)return false;
  impl_->scrub_after=position.after;
  impl_->scrub_through=position.through;
  return true;
}
PagedPayloadScrubResult PagedObjectStoreWriter::scrub_payload() {
  using S=PagedPayloadScrubState;
  if(poisoned() || !impl_->state.mirrored)return {};
  auto& writer=*impl_;
  const auto finish=[&](S state) {
    writer.scrub_after=writer.scrub_through=0;
    writer.scrub_prefix_verified=true;
    return PagedPayloadScrubResult{state,0,0,{}};
  };
  const auto& selected=writer.state;
  try {
    auto source=load(selected.image,selected.limits);
    if(!same(selected,source) || source.degraded_roots || source.degraded_metadata)
      return finish(S::unverified_index);
    if(!writer.scrub_through)writer.scrub_through=source.root.last;
    std::optional<Entry> record;
    auto first=[&](const Entry& entry){record=entry;return false;};
    bool done=false;
    walk(source,source.root,writer.scrub_after,first,done);
    if(source.degraded_metadata)return finish(S::unverified_index);
    if(!record || record->id>writer.scrub_through)return finish(S::complete);
    const auto verified=verify_payload_copies(source,*record);
    if(verified==1) {
      if(!writer.payload_repairs)writer.payload_repairs=std::make_shared<PayloadRepairQueue>();
      writer.payload_repairs->observe(*record);
    }
    writer.scrub_after=record->id;
    if(verified!=2)writer.scrub_prefix_verified=false;
    PagedPayloadScrubResult result{verified==2?S::checked:verified==1?S::repair_queued:S::unverified_payload,
        record->id,verified==2?2ULL*(record_header_bytes+record->stored):0,{}};
    if(writer.scrub_prefix_verified && writer.scrub_after<writer.scrub_through)
      result.resumable=PagedPayloadScrubPosition{source.id,source.generation,writer.scrub_after,writer.scrub_through};
    return result;
  }catch(const std::runtime_error&) {
    // An unreadable index prevents any safe interpretation of later entries.
    return finish(S::unverified_index);
  }
}
PagedPayloadRepairProgress PagedObjectStoreWriter::repair_payload() {
  using Progress=PagedPayloadRepairProgress;
  if(!payload_repair_pending())return Progress::idle;

  auto& writer=*impl_;
  auto& queue=*writer.payload_repairs;
  const auto& selected=writer.state;
  const bool queued=queue.count!=0;
  std::optional<Entry> confirmed_damage;
  bool writing=false;
  try {
    auto source=load(selected.image,selected.limits);
    const auto contract=source.image->contract();
    admit_contract(contract);
    if(!same(selected,source) || source.degraded_roots || source.degraded_metadata ||
        contract.write_failure_unit_bytes>source.unit || source.image->extent()>writer.append_floor)
      fail("payload repair source checkpoint or backing changed");

    std::optional<Entry> record;
    if(queued) {
      const auto& observed=queue.records[queue.count-1];
      record=lookup(source,observed.id);
      if(record!=observed)record.reset();
    } else {
      bool done=false;
      auto first=[&](const Entry& entry){record=entry;return false;};
      walk(source,source.root,queue.scan_after,first,done);
    }
    const auto settle=[&] {
      if(queued)--queue.count;
      else if(record)queue.scan_after=record->id;
      else {queue.scan=false;queue.scan_after=0;}
    };
    if(source.degraded_metadata)fail("payload repair metadata replica lost");
    if(!record){settle();return Progress::checked;}

    const auto verified_copy=[&](std::uint64_t offset)->std::optional<ObjectRecord> {
      try {
        auto copy=read_record_at(source,*record,offset);
        (void)decode_record(source,*record,copy);
        return copy;
      }catch(const std::bad_alloc&){throw;}
      catch(const std::exception&){return {};}
    };
    auto primary=verified_copy(record->offset);
    auto peer=verified_copy(record->peer);
    if(primary && peer){settle();return Progress::checked;}
    if(!primary && !peer)fail("payload repair requires an independently verified copy");
    confirmed_damage=record;
    const auto& survivor=primary?*primary:*peer;

    // Use the ordinary append and COW index planner. The source digest and
    // object generation remain valid because only the physical address moves.
    Plan plan{source};
    plan.next.checkpoint=add(source.generation,1);
    plan.next.generation=add(source.generation,2);
    plan.changed_keys=1;
    auto replacement=*record;
    auto cursor=writer.append_floor;
    PayloadAllocation allocation;
    allocation.place(plan.next,replacement,add(record_header_bytes,replacement.stored),cursor);
    allocation.seal();
    plan.page_start=align(cursor,source.unit);
    const Change change{replacement.id,false,replacement};
    plan.finish(plan.rewrite(source,source.root,std::span(&change,1)));
    if(source.degraded_metadata || plan.next.root.count!=source.root.count ||
        plan.next.root.logical!=source.root.logical || plan.next.root.stored!=source.root.stored)
      fail("payload repair inventory or metadata changed");
    const auto obsolete=add(writer.obsolete_bytes,plan.obsolete_bytes);
    if(obsolete>plan.next.end-start)fail("payload repair obsolete byte accounting");
    try{source.image->admit_write(plan.next.end);}
    catch(const std::runtime_error& error){capacity(error.what());}

    writing=true;
    write_object_record(*source.image,replacement,survivor.header,survivor.payload,source.unit);
    allocation.write_padding(*source.image);
    plan.write_pages(cursor);
    verify_object(plan.next,replacement);
    if(plan.next.degraded_payloads)fail("payload repair wrote a degraded replica");
    publish(plan.next);
    auto published=load(source.image,source.limits);
    if(!same(plan.next,published) || published.degraded_roots ||
        lookup(published,replacement.id)!=replacement || published.degraded_metadata)
      fail("payload repair checkpoint did not retain the verified replacement");

    writer.state=std::move(published);writer.append_floor=writer.state.end;writer.obsolete_bytes=obsolete;
    settle();
    return Progress::repaired;
  }catch(const PagedStoreCapacityError&) {
    if(writing){writer.poisoned=true;throw;}
    queue.blocked_generation=selected.generation;
    queue.blocked_record=confirmed_damage;
    return Progress::capacity;
  }catch(const std::bad_alloc&) {
    if(writing)writer.poisoned=true;
    throw;
  }catch(...) {
    writer.poisoned=true;
    throw;
  }
}
std::uint64_t PagedObjectStoreWriter::generation_for_staging() const noexcept {
  return poisoned()?0:impl_->state.generation;
}
std::uint64_t PagedObjectStoreWriter::physical_end_for_staging() const noexcept {
  return poisoned()?0:impl_->state.end;
}
std::uint64_t PagedObjectStoreWriter::obsolete_bytes() const noexcept {
  return poisoned()?0:impl_->obsolete_bytes;
}
std::uint64_t PagedObjectStoreWriter::append_floor_for_staging() const noexcept {
  return poisoned()?0:impl_->append_floor;
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
