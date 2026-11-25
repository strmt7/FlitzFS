#include "flitzfs/paged_store.hpp"
#include "record_digest.hpp"
#include "folded_checkpoint.hpp"
#include "protected_stripe_image.hpp"
#include "native_group_read.hpp"
#include "native_lz4_decode_wire.hpp"
#include "store_codec.hpp"
#include "store_read_scope.hpp"
#include <algorithm>
#include <bit>
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
constexpr std::size_t page_bytes=detail::kPagedStoreCheckpointWriteBytes/2,
    header_bytes=64,ref_bytes=112,record_header_bytes=128;
constexpr std::uint32_t compact_metadata_bytes=1024;
constexpr std::size_t inline_record_read_bytes=1024;
constexpr std::size_t compact_ref_bytes=88;
static_assert(kPagedScrubRecordWorkBytes==2ULL*(record_header_bytes+2ULL*kStoreMaxObjectBytes));
constexpr std::size_t digest_at=page_bytes-32;
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
detail::PagedGroupReadRecord group_read_record(const Entry& entry) noexcept {
  return {entry.id,entry.generation,entry.offset,entry.peer,entry.digest};
}
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
  bool suspect(const Entry& entry) const noexcept {
    if(scan || blocked_record==entry)return true;
    const auto pending=std::span(records).first(count);
    return std::ranges::find(pending,entry)!=pending.end();
  }
};
void encode_ref(std::span<std::byte> b,std::size_t at,const Ref& r,bool compact=false) {
  put64(b,at,r.first);put64(b,at+8,r.last);put64(b,at+16,r.count);
  put64(b,at+24,r.logical);put64(b,at+32,r.stored);put64(b,at+40,r.primary);
  if(compact) {
    put64(b,at+48,r.generation);
    copy(b,at+56,r.digest);
    return;
  }
  put64(b,at+48,r.peer);put64(b,at+56,r.generation);put32(b,at+64,r.height);copy(b,at+80,r.digest);
}
Ref decode_ref(std::span<const std::byte> b,std::size_t at,std::uint32_t compact_unit=0,std::uint32_t child_height=0) {
  if(compact_unit) {
    const auto primary=get64(b,at+40);
    return {get64(b,at),get64(b,at+8),get64(b,at+16),get64(b,at+24),get64(b,at+32),primary,
        add(primary,compact_unit),get64(b,at+48),child_height,field<32>(b,at+56)};
  }
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
  // Commit records and isolation alignment use the wider root format.
  std::uint32_t unit=0,metadata_bytes=page_bytes;
  Ref root;
  bool degraded_roots=false;
  mutable bool degraded_metadata=false;
  bool mirrored=false;
  mutable bool degraded_payloads=false;
  std::shared_ptr<detail::StoreCodecDecoder> decoder=std::make_shared<detail::StoreCodecDecoder>();
  std::shared_ptr<PayloadRepairQueue> payload_repairs;
};
bool healthy_lz4_index(const State& s) noexcept {
  return !s.degraded_roots && !s.degraded_metadata && !s.degraded_payloads && !s.image->backing_degraded();
}
struct CheckpointKey {
  StoreIdentity identity;
  Ref root;
  std::uint64_t generation,checkpoint,end;
  std::uint32_t unit,metadata_bytes;
  bool mirrored;

  explicit CheckpointKey(const State& s) noexcept:
      identity(s.id),root(s.root),generation(s.generation),checkpoint(s.checkpoint),end(s.end),
      unit(s.unit),metadata_bytes(s.metadata_bytes),mirrored(s.mirrored) {}
  bool matches(const State& s) const noexcept {
    return identity==s.id && mirrored==s.mirrored && generation==s.generation && checkpoint==s.checkpoint &&
        end==s.end && unit==s.unit && metadata_bytes==s.metadata_bytes && root==s.root;
  }
};
bool can_reuse_query(const State& s) noexcept {
  return healthy_lz4_index(s) && (!s.payload_repairs || !s.payload_repairs->pending(s.generation));
}
std::size_t metadata_digest_at(const State& s) {return s.metadata_bytes-Digest{}.size();}
std::size_t branch_entry_bytes(const State& s) {
  return s.metadata_bytes==compact_metadata_bytes?compact_ref_bytes:ref_bytes;
}
std::size_t branch_capacity(const State& s) {return (metadata_digest_at(s)-header_bytes)/branch_entry_bytes(s);}
std::size_t entry_bytes(const State& s){return s.mirrored?72:64;}
std::size_t leaf_capacity(const State& s){return (metadata_digest_at(s)-header_bytes)/entry_bytes(s);}
std::uint64_t packed_rebuild_size(const State& state,std::uint64_t count,std::uint64_t stored) {
  const auto record_bytes=add(multiply(count,record_header_bytes),stored);
  const auto payload=state.mirrored?paired_bank_bytes(record_bytes,state.unit):align(record_bytes,state.unit);
  auto level=(std::max)(std::uint64_t{1},round_up_divide(count,leaf_capacity(state)));
  auto pages=level;
  for(std::uint32_t height=0;level>1;++height) {
    if(height==maximum_height)fail("rebuild tree height");
    level=round_up_divide(level,branch_capacity(state));
    pages=add(pages,level);
  }
  const auto index=paired_bank_bytes(multiply(pages,state.metadata_bytes),state.unit);
  return add(add(start,payload),index);
}
PagedStoreInfo info(const State& s) {
  return {s.generation,s.checkpoint,s.end,s.root.count,s.root.logical,s.root.stored*(s.mirrored?2:1),s.root.height,
          s.degraded_roots,s.degraded_metadata,s.mirrored,s.degraded_payloads || s.image->backing_degraded(),s.metadata_bytes};
}
void admit_ref(const State& s,const Ref& r,bool root=false) {
  if(r.height>maximum_height || !r.generation || r.generation>s.checkpoint ||
      r.primary<start || r.primary%s.metadata_bytes || r.peer!=add(r.primary,s.unit) ||
      r.peer>s.end || s.metadata_bytes>s.end-r.peer ||
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
    if(n.children.empty() || n.children.size()>branch_capacity(s))fail("branch arity");
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
  const auto signature=metadata_digest_at(s);
  auto read=[&](std::uint64_t at,std::span<std::byte> bytes) {
    try{s.image->read_exact(at,bytes);}
    catch(const std::bad_alloc&){throw;}
    catch(const detail::StoreCaptureInterrupted&){throw;}
    catch(const std::exception&){return false;}
    return field<32>(bytes,signature)==r.digest &&
        detail::record_digest(bytes.first(signature))==r.digest;
  };
  const auto first=std::span(a).first(s.metadata_bytes),second=std::span(b).first(s.metadata_bytes);
  const bool own=read(r.primary,first),peer=read(r.peer,second);
  if(!own && !peer)fail("both metadata replicas invalid; no rollback");
  if(!own || !peer)s.degraded_metadata=true;
  const std::span<const std::byte> bytes=own?first:second;
  const auto count=get32(bytes,40);const auto capacity=r.height?branch_capacity(s):leaf_capacity(s);
  const auto stride=r.height?branch_entry_bytes(s):entry_bytes(s);
  if(!magic(bytes,page_magic) || get32(bytes,8)!=(s.mirrored?2U:1U) || get32(bytes,12)!=r.height ||
      get64(bytes,16)!=r.generation || field<16>(bytes,24)!=s.id || count>capacity ||
      !zero(bytes.subspan(44,20)) ||
      !zero(bytes.subspan(header_bytes+count*stride,signature-header_bytes-count*stride)))
    fail("intact page framing/identity");
  Node n;
  if(r.height) {
    n.children.reserve(count);
    const auto compact_unit=s.metadata_bytes==compact_metadata_bytes?s.unit:0;
    for(std::size_t i=0;i<count;++i) {
      auto c=decode_ref(bytes,header_bytes+i*stride,compact_unit,r.height-1);admit_ref(s,c);
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
    // The admitted tree depth bounds the retained branch path. Leaf churn
    // cannot evict its ancestors, even when lookups alternate between keys.
    if(reference.height) {
      if(branches_.size()<reference.height)branches_.resize(reference.height);
      auto& branch=branches_[reference.height-1];
      if(!branch || branch->reference!=reference)
        branch=Page{reference,read_node(state,reference)};
      return branch->node;
    }
    for(const auto& page:leaves_)
      if(page && page->reference==reference)return page->node;
    auto& slot=leaves_[next_leaf_];
    slot=Page{reference,read_node(state,reference)};
    next_leaf_=(next_leaf_+1)%leaves_.size();
    return slot->node;
  }
 private:
  struct Page {Ref reference;Node node;};
  std::vector<std::optional<Page>> branches_;
  std::array<std::optional<Page>,4> leaves_{};
  std::size_t next_leaf_=0;
};
struct QueryMetadata {
  struct Record {
    std::uint64_t id=0;
    Bytes bytes;
  };
  std::array<Record,4> records{};
  std::size_t next=0;
};
const Node& lookup_node(const State& state,const Ref& reference,Node& temporary,
    LookupPages* scoped,const Node* retained_root) {
  if(retained_root && reference.height==state.root.height)return *retained_root;
  return scoped?scoped->get(state,reference):(temporary=read_node(state,reference));
}
std::optional<Entry> lookup(const State& s,std::uint64_t id,LookupPages* scoped=nullptr,
    const Node* retained_root=nullptr) {
  if(!id || !s.root.count || id<s.root.first || id>s.root.last)return {};
  auto r=s.root;
  for(std::uint32_t depth=0;depth<=maximum_height;++depth) {
    Node temporary;
    const auto& n=lookup_node(s,r,temporary,scoped,retained_root);
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
std::uint64_t rank_before(const State& s,std::uint64_t id) {
  std::uint64_t count=0;
  auto ref=s.root;
  while(ref.count && id>ref.first) {
    if(id>ref.last)return add(count,ref.count);
    const auto node=read_node(s,ref);
    if(!ref.height) {
      const auto first=std::ranges::lower_bound(node.entries,id,{},&Entry::id);
      return add(count,static_cast<std::uint64_t>(first-node.entries.begin()));
    }
    auto child=node.children.begin();
    for(;child!=node.children.end() && child->last<id;++child)count=add(count,child->count);
    if(child==node.children.end())fail("compression sampling rank");
    ref=*child;
  }
  return count;
}
Entry entry_at_rank(const State& s,std::uint64_t rank) {
  if(rank>=s.root.count)fail("compression sampling bounds");
  auto ref=s.root;
  for(;;) {
    const auto node=read_node(s,ref);
    if(!ref.height)return node.entries.at(static_cast<std::size_t>(rank));
    auto child=node.children.begin();
    while(child!=node.children.end() && rank>=child->count) {
      rank-=child->count;
      ++child;
    }
    if(child==node.children.end())fail("compression sampling rank");
    ref=*child;
  }
}
using RecordHeader=std::array<std::byte,record_header_bytes>;
struct ObjectRecord {RecordHeader header{};Bytes payload;bool recovered_primary=false;};
void verify_record_header(const State& s,const Entry& e,std::span<const std::byte,record_header_bytes> h) {
  const auto codec=get32(h,12);
  if(!magic(h,data_magic) || get32(h,8)!=1 || get64(h,16)!=e.id || get64(h,24)!=e.generation ||
      get32(h,32)!=e.logical || get32(h,36)!=e.stored || field<16>(h,40)!=s.id ||
      !detail::store_codec_framing(codec,e.logical,e.stored) ||
      !zero(std::span<const std::byte>(h).subspan(codec==detail::kStoreRaw?56:88,codec==detail::kStoreRaw?40:8)) ||
      field<32>(h,96)!=e.digest)fail("object identity/codec/framing");
}
void verify_stored_digest(const Entry& e,std::span<const std::byte,record_header_bytes> header,
    std::span<const std::byte> payload) {
  if(detail::record_digest(header.first<96>(),payload)!=e.digest)fail("object stored digest");
}
std::span<const std::byte> checked_frame_payload(const State& s,const Entry& e,
    std::span<const std::byte> frame) {
  const auto header=frame.first<record_header_bytes>();
  const auto payload=frame.subspan<record_header_bytes>();
  verify_record_header(s,e,header);
  verify_stored_digest(e,header,payload);
  return payload;
}
std::span<const std::byte> read_checked_frame(const State& s,const Entry& e,std::uint64_t offset,
    std::span<std::byte> scratch) {
  const auto frame=scratch.first(record_header_bytes+e.stored);
  read_record_bytes(*s.image,offset,0,frame,s.mirrored?s.unit:0);
  return checked_frame_payload(s,e,frame);
}
void read_record_payload(const State& s,const Entry& e,std::uint64_t offset,ObjectRecord& record) {
  auto& payload=record.payload;
  payload.resize(e.stored);
  if(!payload.empty())read_record_bytes(*s.image,offset,record_header_bytes,payload,s.mirrored?s.unit:0);
  verify_stored_digest(e,record.header,payload);
}
ObjectRecord read_record_at(const State& s,const Entry& e,std::uint64_t offset) {
  ObjectRecord record;
  if(e.stored<=inline_record_read_bytes-record_header_bytes) {
    // Tiny objects share a bounded read for framing and payload. The stream
    // mapper still stops each backing read at the paired-bank boundary.
    std::array<std::byte,inline_record_read_bytes> scratch;
    const auto payload=read_checked_frame(s,e,offset,scratch);
    std::copy_n(scratch.begin(),record.header.size(),record.header.begin());
    record.payload.assign(payload.begin(),payload.end());
    return record;
  }
  read_record_bytes(*s.image,offset,0,record.header,s.mirrored?s.unit:0);
  verify_record_header(s,e,record.header);
  read_record_payload(s,e,offset,record);
  return record;
}
ObjectRecord read_record(const State& s,const Entry& e) {
  try{return read_record_at(s,e,e.offset);}
  catch(const std::bad_alloc&){throw;}
  catch(const detail::StoreCaptureInterrupted&){throw;}
  catch(const std::exception&) {
    if(!s.mirrored)throw;
    s.degraded_payloads=true;
    auto survivor=read_record_at(s,e,e.peer);
    survivor.recovered_primary=true;
    return survivor;
  }
}
Bytes decode_record(const State& s,const Entry& e,std::span<const std::byte> h,std::span<const std::byte> payload) {
  const auto codec=get32(h,12);
  if(codec==detail::kStoreRaw)return Bytes(payload.begin(),payload.end());
  auto decoded=s.decoder->decode(codec,payload,e.logical);
  if(detail::record_digest(decoded)!=field<32>(h,56))fail("object decoded digest");
  return decoded;
}
Bytes consume_decoded_record(const State& s,const Entry& e,ObjectRecord& record) {
  if(get32(record.header,12)==detail::kStoreRaw)return std::move(record.payload);
  return decode_record(s,e,record.header,record.payload);
}
Bytes read_object(const State& s,const Entry& e,detail::NativeLz4ReadRoute* route=nullptr) {
  auto record=read_record(s,e);
  if(route && !record.recovered_primary && !s.degraded_payloads &&
      get32(record.header,12)==detail::kStoreLz4 && store_lz4_available()) {
    const detail::NativeLz4ReadKey key{s.id,e.id,e.generation,e.logical,field<32>(record.header,56)};
    if(auto candidate=route->try_ready(key,record.payload)) {
      if(candidate->size()==e.logical && detail::record_digest(*candidate)==key.decoded_digest)
        return std::move(*candidate);
      route->reject(key);
    }
  }
  auto bytes=consume_decoded_record(s,e,record);
  if(record.recovered_primary && s.payload_repairs)s.payload_repairs->observe(e);
  return bytes;
}
enum class PayloadVerification { decoded, stored };
unsigned verify_payload_copies(const State& s,const Entry& e,Bytes* payload=nullptr,
    Bytes* stored_frame=nullptr) {
  unsigned verified=0;
  std::optional<ObjectRecord> decoded_primary;
  for(const auto at:{e.offset,e.peer}) {
    try {
      if(stored_frame) {
        const auto length=record_header_bytes+e.stored;
        if(stored_frame->size()<length)stored_frame->resize(length);
        (void)read_checked_frame(s,e,at,*stored_frame);
        ++verified;
        continue;
      }
      auto record=read_record_at(s,e,at);
      // The peer has passed its own stored digest and header check. Matching
      // encoded bytes share the primary's successful decoded digest check.
      if(decoded_primary && record.header==decoded_primary->header && record.payload==decoded_primary->payload) {
        ++verified;
        continue;
      }
      auto decoded=consume_decoded_record(s,e,record);
      if(payload && !verified) {
        *payload=std::move(decoded);
        if(at==e.peer && s.payload_repairs)s.payload_repairs->observe(e);
      }
      if(at==e.offset && get32(record.header,12)!=detail::kStoreRaw)
        decoded_primary.emplace(std::move(record));
      ++verified;
    }
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){s.degraded_payloads=true;}
  }
  return verified;
}
// True reports a verified survivor and a failed replica.
bool verify_object(const State& s,const Entry& e,Bytes* payload=nullptr,Bytes* stored_frame=nullptr) {
  if(!s.mirrored) {
    auto decoded=read_object(s,e);
    if(payload)*payload=std::move(decoded);
    return false;
  }
  const auto verified=verify_payload_copies(s,e,payload,stored_frame);
  if(!verified)fail("both payload copies invalid; no rollback");
  return verified==1;
}
template<class Visitor> void walk_leaves(const State& s,const Ref& r,std::uint64_t after,Visitor& visitor,bool& done) {
  if(done || !r.count || r.last<=after)return;
  const auto n=read_node(s,r);
  if(r.height) {for(const auto& child:n.children)walk_leaves(s,child,after,visitor,done);}
  else done=!visitor(std::span<const Entry>(n.entries));
}
template<class Visitor> void walk(const State& s,const Ref& r,std::uint64_t after,Visitor& visitor,bool& done) {
  auto leaf=[&](std::span<const Entry> entries) {
    for(const auto& entry:entries)if(entry.id>after && !visitor(entry))return false;
    return true;
  };
  walk_leaves(s,r,after,leaf,done);
}
struct MirroredRecordRun {
  std::size_t count=1,bytes=0;
};
MirroredRecordRun mirrored_record_run(const State& s,std::span<const Entry> entries) noexcept {
  const auto& first=entries.front();
  const auto bank_bytes=static_cast<std::size_t>(s.unit-first.offset%s.unit);
  MirroredRecordRun run{1,record_header_bytes+first.stored};
  while(run.count<entries.size() && run.bytes<=bank_bytes) {
    const auto& next=entries[run.count];
    const auto length=record_header_bytes+next.stored;
    if(next.offset!=first.offset+run.bytes || next.peer!=first.peer+run.bytes || length>bank_bytes-run.bytes)
      break;
    run.bytes+=length;
    ++run.count;
  }
  return run;
}
bool verify_stored_run(const State& s,std::span<const Entry> entries,std::size_t length,Bytes& scratch) {
  if(scratch.size()<2*length)scratch.resize(2*length);
  const auto primary=std::span(scratch).first(length);
  const auto peer=std::span(scratch).subspan(length,length);
  try {
    s.image->read_exact(entries.front().offset,primary);
    s.image->read_exact(entries.front().peer,peer);
    // Both banks must return the exact frame bytes. Full equality lets the
    // peer share the framing and digest checks on those identical bytes.
    if(!std::ranges::equal(primary,peer))fail("stored replica bytes disagree");
    std::span<const std::byte> remaining=primary;
    for(const auto& entry:entries) {
      const auto frame_bytes=record_header_bytes+entry.stored;
      (void)checked_frame_payload(s,entry,remaining.first(frame_bytes));
      remaining=remaining.subspan(frame_bytes);
    }
    return true;
  }
  catch(const std::bad_alloc&){throw;}
  catch(const detail::StoreCaptureInterrupted&){throw;}
  catch(const std::exception&) {
    // Inspect each replica individually for diagnosis, while keeping even a
    // transient failure from granting writable admission after the fences.
    s.degraded_payloads=true;
    return false;
  }
}
using VerifiedObjectVisitor=std::function<void(std::uint64_t,Bytes)>;
void verify_decoded_run(const State& s,std::span<const Entry> entries,std::span<const std::byte> frames,
    const VerifiedObjectVisitor* consume) {
  for(const auto& entry:entries) {
    const auto frame=frames.first(record_header_bytes+entry.stored);
    const auto header=frame.first<record_header_bytes>();
    Bytes decoded;
    if(consume || get32(header,12)!=detail::kStoreRaw) {
      try {decoded=decode_record(s,entry,header,frame.subspan<record_header_bytes>());}
      catch(const std::bad_alloc&){throw;}
      catch(const std::exception&) {
        // The checked replicas contain identical encoded bytes. A decode
        // failure applies to both copies.
        s.degraded_payloads=true;
        fail("both payload copies invalid; no rollback");
      }
    }
    if(consume)(*consume)(entry.id,std::move(decoded));
    frames=frames.subspan(frame.size());
  }
}
void verify(const State& s,const VerifiedObjectVisitor* consume=nullptr,
    PayloadVerification verification=PayloadVerification::decoded) {
  s.image->verify_backing();
  (void)read_node(s,s.root);
  bool done=false;
  Bytes scratch;
  const bool stored=verification==PayloadVerification::stored;
  const auto visit_one=[&](const Entry& entry) {
    Bytes decoded;
    verify_object(s,entry,consume?&decoded:nullptr,stored?&scratch:nullptr);
    if(consume)(*consume)(entry.id,std::move(decoded));
  };
  auto leaf=[&](std::span<const Entry> entries) {
    if(!s.mirrored) {
      for(const auto& entry:entries)visit_one(entry);
      return true;
    }
    while(!entries.empty()) {
      const auto run=mirrored_record_run(s,entries);
      const auto records=entries.first(run.count);
      if(run.count>1 && verify_stored_run(s,records,run.bytes,scratch)) {
        if(!stored)verify_decoded_run(s,records,std::span<const std::byte>(scratch).first(run.bytes),consume);
      }else {
        for(const auto& entry:records)visit_one(entry);
      }
      entries=entries.subspan(run.count);
    }
    return true;
  };
  walk_leaves(s,s.root,0,leaf,done);
}
std::array<std::byte,page_bytes> root_bytes(const State& s,std::uint64_t generation) {
  std::array<std::byte,page_bytes> b{};set_magic(b,root_magic);put32(b,8,s.mirrored?2:1);
  put32(b,12,static_cast<std::uint32_t>(generation%2));put64(b,16,generation);
  put64(b,24,s.checkpoint);put64(b,32,s.end);copy(b,40,s.id);put32(b,56,s.unit);
  put32(b,60,s.metadata_bytes);encode_ref(b,64,s.root);
  copy(b,digest_at,detail::record_digest(std::span<const std::byte>(b).first(digest_at)));return b;
}
detail::FoldedCheckpoint folded_checkpoint(const State& state) {
  const auto even=root_bytes(state,state.generation%2?state.checkpoint:state.generation);
  const auto odd=root_bytes(state,state.generation%2?state.generation:state.checkpoint);
  return {state.end,state.generation,state.unit,detail::record_digest(even,odd)};
}
State load(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits,LookupPages* query_pages=nullptr,
    Node* retain_tree_root=nullptr) {
  admit_limits(limits);if(!image)fail("missing backend");
  const auto extent=image->extent();
  if(extent<start+2*page_bytes || extent>limits.maximum_physical_bytes)fail("physical capacity/truncation");
  auto read=[&](unsigned slot)->std::optional<State> {
    std::array<std::byte,page_bytes> b{};
    try{image->read_exact(slot*kStoreCommitUnit,b);}
    catch(const std::bad_alloc&){throw;}
    catch(const detail::StoreCaptureInterrupted&){throw;}
    catch(const std::exception&){return {};}
    if(field<32>(b,digest_at)!=detail::record_digest(std::span<const std::byte>(b).first(digest_at)))return {};
    State s;s.image=image;s.limits=limits;s.id=field<16>(b,40);s.generation=get64(b,16);
    s.mirrored=get32(b,8)==2;
    s.checkpoint=get64(b,24);s.end=get64(b,32);s.unit=get32(b,56);
    s.metadata_bytes=get32(b,60);s.root=decode_ref(b,64);
    if(!magic(b,root_magic) || (get32(b,8)!=1 && get32(b,8)!=2) || get32(b,12)!=slot ||
        s.generation%2!=slot || !s.checkpoint || s.checkpoint==(std::numeric_limits<std::uint64_t>::max)() ||
        (s.generation!=s.checkpoint && s.generation!=s.checkpoint+1) || zero(s.id) ||
        s.unit<page_bytes || s.unit>kStoreCommitUnit || (s.unit&(s.unit-1)) ||
        (s.metadata_bytes!=page_bytes && (s.mirrored || s.metadata_bytes!=compact_metadata_bytes)) ||
        s.end>extent || s.end>limits.maximum_physical_bytes ||
        s.end<start+2*s.unit || s.end%s.unit || !zero(std::span<const std::byte>(b).subspan(176,digest_at-176)))
      fail("unsupported/intact root semantics");
    admit_ref(s,s.root,true);return s;
  };
  auto a=read(0),b=read(1);if(!a && !b)fail("no recovery root");
  const bool choose_a=a && (!b || a->generation>b->generation);
  auto s=choose_a?*a:*b;const auto& other=choose_a?b:a;
  bool paired=false;
  if(other) {
    if(other->id!=s.id || other->unit!=s.unit || other->metadata_bytes!=s.metadata_bytes ||
        other->mirrored!=s.mirrored || s.generation<=other->generation ||
        s.generation-other->generation!=1)fail("conflicting root identities/generations");
    paired=other->checkpoint==s.checkpoint && other->root==s.root && other->end==s.end;
    if(s.generation!=s.checkpoint && !paired)fail("conflicting intact root pair");
  }
  s.degraded_roots=!paired;
  if(query_pages)(void)query_pages->get(s,s.root);
  else {
    auto root=read_node(s,s.root);
    if(retain_tree_root)*retain_tree_root=std::move(root);
  }
  return s;
}
bool same(const State& a,const State& b) {
  return CheckpointKey(a).matches(b);
}
void zeros(StoreByteIo& io,std::uint64_t at,std::uint64_t end) {
  std::array<std::byte,page_bytes> b{};
  while(at<end) {const auto size=static_cast<std::size_t>((std::min)(end-at,std::uint64_t(page_bytes)));
    io.write_exact(at,std::span<const std::byte>(b).first(size));at+=size;}
}
void publish_root(const State& s,StoreByteIo& output,std::uint64_t generation) {
  output.write_exact(generation%2*kStoreCommitUnit,root_bytes(s,generation));
  output.flush();
}
void publish_roots(const State& s,StoreByteIo& output) {
  publish_root(s,output,s.checkpoint);
  publish_root(s,output,s.checkpoint+1);
}
void publish_roots(const State& s) {publish_roots(s,*s.image);}
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
  void write_padding(StoreByteIo& image,std::uint32_t folded_unit=0) {
    for(const auto& [begin,end]:padding) {
      if(!folded_unit)zeros(image,begin,end);
      else {
        if(begin<start || end-begin>folded_unit-begin%folded_unit)
          fail("invalid folded payload padding");
        if((begin/folded_unit)%2==0) {
          const auto first=detail::folded_mirror_offset(begin,folded_unit);
          zeros(image,first,first+(end-begin));
        }
      }
    }
    padding.clear();
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
  ref.primary=stream_position(page_start,multiply(ordinal,state.metadata_bytes),state.unit);
  ref.peer=add(ref.primary,state.unit);
  auto& bytes=page.bytes;
  set_magic(bytes,page_magic);
  put32(bytes,8,state.mirrored?2:1);
  put32(bytes,12,height);
  put64(bytes,16,ref.generation);
  copy(bytes,24,state.id);
  put32(bytes,40,static_cast<std::uint32_t>(height?node.children.size():node.entries.size()));
  if(height) {
    const auto stride=branch_entry_bytes(state);
    const bool compact=state.metadata_bytes==compact_metadata_bytes;
    for(std::size_t i=0;i<node.children.size();++i)encode_ref(bytes,header_bytes+i*stride,node.children[i],compact);
  } else {
    for(std::size_t i=0;i<node.entries.size();++i)
      encode_entry(bytes,header_bytes+i*entry_bytes(state),node.entries[i],state.mirrored);
  }
  const auto signature=metadata_digest_at(state);
  ref.digest=detail::record_digest(std::span<const std::byte>(bytes).first(signature));
  copy(bytes,signature,ref.digest);
  return page;
}
void write_page(const State& state,const PreparedPage& page) {
  const auto bytes=std::span<const std::byte>(page.bytes).first(state.metadata_bytes);
  state.image->write_exact(page.ref.primary,bytes);
  state.image->write_exact(page.ref.peer,bytes);
}
void pad_index(const State& state,std::uint64_t pages,StoreByteIo* folded=nullptr) {
  const auto unit=state.unit;
  const auto used=(pages%(unit/state.metadata_bytes))*state.metadata_bytes;
  if(!used)return;
  const auto peer_start=state.end-unit;
  const auto primary_start=peer_start-unit;
  if(folded)zeros(*folded,detail::folded_mirror_offset(primary_start+used,unit),detail::folded_mirror_extent(state.end));
  else {
    zeros(*state.image,primary_start+used,peer_start);
    zeros(*state.image,peer_start+used,state.end);
  }
}
struct PreparedObject {
  Entry entry;
  RecordHeader header{};
  Bytes payload;
  bool used_lz4_candidate=false;
};
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
  record.used_lz4_candidate=encoded.used_lz4_candidate;
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
    const auto capacity=branch_capacity(next);
    const auto count=(children.size()+capacity-1)/capacity;
    const auto size=(children.size()+count-1)/count;
    for(std::size_t i=0;i<children.size();i+=size) {
      Node n;n.children.assign(children.begin()+static_cast<std::ptrdiff_t>(i),
        children.begin()+static_cast<std::ptrdiff_t>((std::min)(children.size(),i+size)));
      output.push_back(make_page(n,height));
    }
    return output;
  }
  void amend_leaf(const State& old,std::vector<Entry>& entries,std::span<const Change> changes) {
    for(const auto& change:changes) {
      auto at=std::ranges::lower_bound(entries,change.id,{},&Entry::id);
      if(at!=entries.end() && at->id==change.id) {
        obsolete_bytes=add(obsolete_bytes,(std::uint64_t{record_header_bytes}+at->stored)*(old.mirrored?2:1));
        if(change.erase)entries.erase(at);
        else *at=change.entry;
      }else {
        if(change.erase)fail("erase missing object");
        entries.insert(at,change.entry);
      }
    }
  }
  std::vector<Ref> write_nodes(const Node& node,std::uint32_t height) {
    return height?branches(node.children,height):leaves(node.entries);
  }
  std::vector<Ref> rewrite(const State& old,const Ref& ref,std::span<const Change> changes) {
    if(changes.empty())return {ref};
    return write_nodes(rewrite_contents(old,ref,changes),ref.height);
  }
  Node rewrite_contents(const State& old,const Ref& ref,std::span<const Change> changes) {
    auto node=read_node(old,ref);
    obsolete_bytes=add(obsolete_bytes,2*old.metadata_bytes);
    if(!ref.height) {
      amend_leaf(old,node.entries,changes);
      return node;
    }
    Node revised,adjacent;
    const auto child_height=ref.height-1;
    const bool compact=next.metadata_bytes==compact_metadata_bytes;
    const auto finish_adjacent=[&] {
      if(adjacent.entries.empty() && adjacent.children.empty())return;
      const auto packed=write_nodes(adjacent,child_height);
      revised.children.insert(revised.children.end(),packed.begin(),packed.end());
      adjacent.entries.clear();
      adjacent.children.clear();
    };
    std::size_t begin=0;
    for(std::size_t i=0;i<node.children.size();++i) {
      auto end=begin;
      const auto& child=node.children[i];
      while(end<changes.size() && (i+1==node.children.size() || changes[end].id<=child.last))++end;
      const auto slice=changes.subspan(begin,end-begin);
      if(compact && !slice.empty()) {
        // Each changed neighbor already needs replacement. Pack their
        // contents before allocating pages, at the same tree level.
        auto replacement=rewrite_contents(old,child,slice);
        adjacent.entries.insert(adjacent.entries.end(),replacement.entries.begin(),replacement.entries.end());
        adjacent.children.insert(adjacent.children.end(),replacement.children.begin(),replacement.children.end());
      }else {
        finish_adjacent();
        const auto result=rewrite(old,child,slice);
        revised.children.insert(revised.children.end(),result.begin(),result.end());
      }
      begin=end;
    }
    finish_adjacent();
    return revised;
  }
  void finish(std::vector<Ref> roots) {
    if(roots.empty())next.root=make_page(Node{},0);
    else {
      while(roots.size()>1)roots=branches(roots,roots.front().height+1);
      next.root=roots.front();
    }
    next.end=add(page_start,paired_bank_bytes(multiply(pages.size(),next.metadata_bytes),next.unit));
    // The complete paired layout supplies the shortfall for reclamation.
    check_physical_capacity(next.end,next.limits.maximum_physical_bytes);
  }
  void write_pages(std::uint64_t data_end) {
    // Initialize only padding. Each complete metadata page initializes its
    // own range, and all page banks except the last are fully occupied.
    zeros(*next.image,data_end,page_start);
    pad_index(next,pages.size());
    for(const auto& page:pages)write_page(next,page);
  }
};
// Keep the remaining siblings on one current path and one checked leaf.
struct TreeFrontier {
  std::vector<Ref> pending;
  std::vector<Entry> entries;
  std::size_t next_entry=0;

  bool has_entries() const noexcept {return next_entry<entries.size();}
  bool present() const noexcept {return has_entries() || !pending.empty();}
  std::uint64_t first() const noexcept {return has_entries()?entries[next_entry].id:pending.back().first;}
  void open(const State& state) {
    auto node=read_node(state,pending.back());
    pending.pop_back();
    pending.insert(pending.end(),node.children.rbegin(),node.children.rend());
    entries=std::move(node.entries);
    next_entry=0;
  }
  std::span<const Entry> remaining_entries() const noexcept {
    return std::span(entries).subspan(next_entry);
  }
  void advance(std::size_t count) noexcept {next_entry+=count;}
  Entry take() noexcept {return entries[next_entry++];}
};
// Merge immutable versions of the same tree by key. An identical reference
// skips its subtree.
struct TreeDifference {
  struct Edit {
    std::uint64_t id;
    std::optional<Entry> source;
  };
  TreeFrontier prior,current;

  TreeDifference(const Ref& before,const Ref& after):prior{{before},{}},current{{after},{}} {}
  bool done() const noexcept {return !prior.present() && !current.present();}
  std::optional<Edit> next(const State& before,const State& after,std::size_t& steps) {
    while(steps && !done()) {
      --steps;
      if(!prior.has_entries() && !current.has_entries() && prior.present() && current.present() &&
          prior.pending.back()==current.pending.back()) {
        prior.pending.pop_back();current.pending.pop_back();continue;
      }
      const bool old_first=prior.present() && (!current.present() || prior.first()<=current.first());
      const bool new_first=current.present() && (!prior.present() || current.first()<=prior.first());
      if(old_first && !prior.has_entries() && (!new_first || current.has_entries() ||
          prior.pending.back().height>=current.pending.back().height)) {
        prior.open(before);continue;
      }
      if(new_first && !current.has_entries()) {current.open(after);continue;}
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
class FoldedRebuildView final : public StoreByteIo {
 public:
  FoldedRebuildView(std::shared_ptr<StoreByteIo> workspace,std::uint32_t unit,std::uint64_t limit)
      :workspace_(std::move(workspace)),unit_(unit),limit_(limit) {}
  std::uint64_t extent() const override {
    const auto physical=workspace_->extent();
    if(physical<start || physical>detail::folded_mirror_extent(limit_))fail("folded workspace extent");
    return start+2*(physical-start);
  }
  void read_exact(std::uint64_t at,std::span<std::byte> output) const override {
    if(at>limit_ || output.size()>limit_-at)fail("folded workspace read bounds");
    if(at<start && !output.empty()) {
      const auto count=(std::min<std::uint64_t>)(output.size(),start-at);
      workspace_->read_exact(at,output.first(count));
      output=output.subspan(count);
      at+=count;
    }
    while(!output.empty()) {
      const auto count=(std::min<std::uint64_t>)(output.size(),unit_-at%unit_);
      workspace_->read_exact(detail::folded_mirror_offset(at,unit_),output.first(count));
      output=output.subspan(count);
      at+=count;
    }
  }
  StoreIoContract contract() const noexcept override {return {};}
  void write_exact(std::uint64_t,std::span<const std::byte>) override {fail("read-only folded workspace view");}
  void admit_write(std::uint64_t) override {fail("read-only folded workspace view");}
  void flush() override {fail("read-only folded workspace view");}
 private:
  bool allows_writer() const noexcept override {return false;}
  std::shared_ptr<StoreByteIo> workspace_;
  const std::uint32_t unit_;
  const std::uint64_t limit_;
};

struct Rebuild {
  State next;
  // The folded output is private workspace. The ordinary indexed reader
  // sees only its checked, read-only virtual mapping.
  StoreByteIo* folded=nullptr;
  std::uint64_t payload_cursor=start,index_start=0,index_cursor=start,pages=0;
  PayloadAllocation allocation;
  // Bind the index pass to the headers generated and verified during copy.
  // Otherwise the unpublished target could supply its own descriptor digest.
  Digest written_headers{},indexed_headers{};
  Node leaf;
  std::array<Node,maximum_height+1> levels;

  void admit_target(std::uint64_t end) {
    if(folded)folded->admit_write(detail::folded_mirror_extent(end));
    else next.image->admit_write(end);
  }
  void write_padding() {
    if(folded)allocation.write_padding(*folded,next.unit);
    else allocation.write_padding(*next.image);
  }
  Bytes readback(const Entry& entry) const {
    if(!next.mirrored || folded)return read_object(next,entry);
    Bytes bytes;
    verify_object(next,entry,&bytes);
    return bytes;
  }
  void append_payload(Entry& entry,const RecordHeader& header,std::span<const std::byte> encoded,
      const Bytes& decoded) {
    allocation.place(next,entry,std::uint64_t{record_header_bytes}+entry.stored,payload_cursor);
    if(payload_cursor>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    admit_target(payload_cursor);
    write_padding();
    if(folded) {
      const auto offset=detail::folded_mirror_offset(entry.offset,next.unit);
      folded->write_exact(offset,header);
      if(!encoded.empty())folded->write_exact(offset+header.size(),encoded);
    }else write_object_record(*next.image,entry,header,encoded,next.unit);
    if(readback(entry)!=decoded)fail("rebuild CPU comparison");
    if(next.degraded_payloads)fail("rebuild produced degraded payload replicas");
    written_headers=detail::record_digest(written_headers,header);
  }
  std::uint64_t remaining_payload_bank_bytes() const noexcept {
    return allocation.unit?allocation.unit-allocation.used:next.unit;
  }
  void append_mirrored_records(std::span<Entry> entries,std::span<const std::byte> frames,
      std::span<const Bytes> decoded,Bytes& readback_frames) {
    if(!next.mirrored || folded || entries.size()<2 || decoded.size()!=entries.size())
      fail("invalid mirrored rebuild batch");
    for(auto& entry:entries)
      allocation.place(next,entry,std::uint64_t{record_header_bytes}+entry.stored,payload_cursor);
    if(payload_cursor>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    const auto run=mirrored_record_run(next,entries);
    if(run.count!=entries.size() || run.bytes!=frames.size())
      fail("mirrored rebuild batch exceeds its bank");
    admit_target(payload_cursor);
    write_padding();
    next.image->write_exact(entries.front().offset,frames);
    next.image->write_exact(entries.front().peer,frames);
    if(!verify_stored_run(next,entries,run.bytes,readback_frames))
      fail("rebuild produced degraded payload replicas");
    auto verified=std::span<const std::byte>(readback_frames).first(run.bytes);
    for(std::size_t i=0;i<entries.size();++i) {
      const auto& entry=entries[i];
      if(decode_record(next,entry,verified.first<record_header_bytes>(),
          verified.subspan(record_header_bytes,entry.stored))!=decoded[i])
        fail("rebuild CPU comparison");
      written_headers=detail::record_digest(written_headers,frames.first<record_header_bytes>());
      const auto frame_size=record_header_bytes+entry.stored;
      frames=frames.subspan(frame_size);
      verified=verified.subspan(frame_size);
    }
  }
  void finish_payloads() {
    index_start=align(payload_cursor,next.unit);
    if(index_start>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    admit_target(index_start);
    allocation.seal();
    write_padding();
    if(folded) {
      if(payload_cursor!=index_start)fail("folded payload alignment changed");
    }else zeros(*next.image,payload_cursor,index_start);
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
    const auto end=add(index_start,paired_bank_bytes(multiply(next_pages,next.metadata_bytes),next.unit));
    if(end>next.limits.maximum_physical_bytes)capacity("rebuild capacity");
    admit_target(end);
    if(folded)folded->write_exact(detail::folded_mirror_offset(page.ref.primary,next.unit),
        std::span<const std::byte>(page.bytes).first(next.metadata_bytes));
    else write_page(next,page);
    pages=next_pages;
    next.end=end;
    return page.ref;
  }
  void emit(Ref ref) {
    if(ref.height>maximum_height)fail("rebuild tree height");
    auto& node=levels[ref.height];node.children.push_back(ref);
    if(node.children.size()==branch_capacity(next)) {
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
    pad_index(next,pages,folded);
  }
};
PreparedObject prepare_rebuilt_object(const State& target,const Entry& source_entry,ObjectRecord source_record,
    const Bytes& decoded,detail::StoreCodecEncoder& encoder,bool reencode,bool* cold_evaluated=nullptr) {
  auto encoded=[&] {
    if(reencode)return encoder.encode(decoded);
    if(cold_evaluated) {
      auto trial=encoder.try_cold_encoding(decoded,source_record.payload.size());
      *cold_evaluated=trial.evaluated;
      if(trial.smaller)return std::move(*trial.smaller);
    }
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
    const std::function<bool(std::uint64_t,std::span<const std::byte>)>* keep=nullptr,
    bool* cold_evaluated=nullptr) {
  auto source_record=read_record(original,source_entry);
  auto bytes=decode_record(original,source_entry,source_record.header,source_record.payload);
  if(keep && !(*keep)(source_entry.id,bytes))return false;
  if(source_entry.id==replacement_id)bytes.assign(replacement.begin(),replacement.end());
  auto record=prepare_rebuilt_object(build.next,source_entry,std::move(source_record),bytes,
      encoder,reencode || source_entry.id==replacement_id,cold_evaluated);
  build.append_payload(record.entry,record.header,record.payload,bytes);
  return true;
}

} // namespace

class detail::PagedReadMetadata final {
 public:
  explicit PagedReadMetadata(const State& state) noexcept:backing_(state.image),key_(state) {}
  bool matches(const State& state) const noexcept {
    const auto backing=backing_.lock();
    return backing && backing==state.image && !backing.owner_before(state.image) &&
        !state.image.owner_before(backing) && key_.matches(state) && can_reuse_query(state);
  }
  void discard() noexcept {
    pages.reset();
    records.reset();
  }
  std::optional<LookupPages> pages;
  std::optional<QueryMetadata> records;

 private:
  std::weak_ptr<StoreByteIo> backing_;
  CheckpointKey key_;
};
struct PagedObjectStoreSnapshot::Impl {
  State state;
  bool sealed_payloads=false;
  // Checked descriptors for this snapshot's immutable root. Payload reads,
  // publication and recovery continue to verify their own backing bytes.
  std::optional<Node> retained_root;
  std::optional<LookupPages> lookup_pages;
  std::optional<QueryMetadata> query_metadata;
};
struct PagedObjectStoreWriter::Impl {
  // Nested lease shares the parent's friendship, as in the flat writer.
  struct Lease {
    std::shared_ptr<StoreByteIo> image;
    explicit Lease(std::shared_ptr<StoreByteIo> io,bool frozen=false):image(std::move(io)) {
      if(!image || !(frozen?image->acquire_exclusive_use():image->acquire_writer()))
        fail("backend already has a writer or is missing");
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
  struct ScrubCheckpoint {
    CheckpointKey key;
    std::uint64_t objects=0;
  };
  std::optional<ScrubCheckpoint> scrub_checkpoint;
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
  std::optional<TreeFrontier> frontier;
  std::optional<LookupPages> comparison_source_pages;
  Bytes verification_frames;
  std::optional<RebuildColdEncoding> cold;
  std::function<void(std::uint64_t)> folded_admission;
  struct CompressionScreen {
    std::uint64_t start=0,count=0,samples=0,completed=0,compared=0,saved=0;
  } screen;
  struct RetainedInventory {
    std::uint64_t objects=0,logical=0,stored=0;
    Digest digest{};
    void include(const Entry& e) {
      objects=add(objects,1);logical=add(logical,e.logical);stored=add(stored,e.stored);
      digest=detail::record_digest(digest,e.digest);
    }
    bool operator==(const RetainedInventory&) const = default;
  } retained,copied;
  std::uint64_t source_extent=0,target_extent=0,completed=0,logical=0,stored=0,write_bound=0;
  PagedRewritePhase phase=PagedRewritePhase::verifying_source;
  bool reencode=true,replacement_seen=false;
  PagedRewriteMode mode;
  std::uint64_t cold_through=0;
  std::optional<Entry> capacity_damage;
  std::uint64_t capacity_repair_generation=0,original_root_stored=0;
  bool repair_confirmed=false;
  unsigned folded_barriers=0;
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
      std::optional<RebuildColdEncoding> cold_encoding,std::function<void(std::uint64_t)> fold)
      :original(writer.state),owner(writer.owner),replacement_identity_offset(identity_offset),
       retention(std::move(filter)),cold(std::move(cold_encoding)),folded_admission(std::move(fold)),
       reencode(encode),mode(rewrite_mode) {
    if(folded_admission && (mode!=PagedRewriteMode::exact_checkpoint || retention || cold ||
        root.empty() || encode || !original.mirrored))
      fail("folded staging requires an exact mirrored native checkpoint");
    if(mode!=PagedRewriteMode::exact_checkpoint && (retention || root.empty() || encode))
      fail("foreground reconciliation requires a lossless native identity-only rebuild");
    const bool screened=mode==PagedRewriteMode::cold_compression;
    if((screened && !cold) || (cold && (mode==PagedRewriteMode::exact_checkpoint || !cold->first_object ||
        cold->first_object>=cold->past_last_object || cold->after_generation>original.generation ||
        bool(cold->reserve_writes)!=screened)))
      fail("cold encoding requires a bound indexed reconciliation");
    if(screened)phase=PagedRewritePhase::screening_compression;
    if(cold)cold_through=original.generation;
    if(mode==PagedRewriteMode::reconcile_foreground && writer.payload_repairs &&
        writer.payload_repairs->blocked_generation==original.generation && writer.payload_repairs->blocked_record) {
      capacity_damage=writer.payload_repairs->blocked_record;
      capacity_repair_generation=original.generation;
    }
    if(zero(id) || (id==original.id && !retention))fail("staged rebuild needs a new identity");
    if(retention && (!root.empty() || encode || original.mirrored ||
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
    build.next.metadata_bytes=original.metadata_bytes;
    build.next.generation=add(original.generation,2);build.next.checkpoint=original.generation+1;
    build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),image->contract().write_failure_unit_bytes);
    if(folded_admission) {
      build.folded=image.get();
      build.next.image=std::make_shared<FoldedRebuildView>(image,build.next.unit,build.next.limits.maximum_physical_bytes);
    }
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
        current.id==original.id && current.unit==original.unit && current.metadata_bytes==original.metadata_bytes &&
        current.mirrored==original.mirrored &&
        current.end>=original.end && current.generation>original.generation;
  }
  void reset(PagedRewritePhase next) noexcept {
    phase=next;completed=logical=stored=0;
    frontier.reset();
    comparison_source_pages.reset();
  }
  void count(const Entry& entry) {
    completed=add(completed,1);logical=add(logical,entry.logical);stored=add(stored,entry.stored);
  }
  void totals(const State& state) const {
    if(completed!=state.root.count || logical!=state.root.logical || stored!=state.root.stored)
      fail("staged verification inventory differs");
  }
  bool cold_copy_candidate(const Entry& entry) const noexcept {
    return cold && entry.generation>cold->after_generation && cold->eligible(entry.id);
  }
  bool copy_mirrored_prefix(std::span<const Entry> leaf) {
    if(reencode || retention || build.folded || !build.next.mirrored)return false;
    const auto limit=(std::min)(leaf.size(),(std::max)(std::size_t{1},leaf_capacity(original)/4));
    auto bank_left=build.remaining_payload_bank_bytes();
    std::uint64_t work_left=2*kStoreCommitUnit;
    std::size_t selected=0,frame_bytes=0;
    // Preserve the stream order. A record that needs encoding or spans
    // banks keeps the single-record path and its own admission.
    for(const auto& entry:leaf.first(limit)) {
      const auto size=record_header_bytes+entry.stored;
      const auto work=std::uint64_t{entry.logical}+size;
      if((entry.id==1 && !replacement.empty()) || cold_copy_candidate(entry) ||
          size>bank_left || work>work_left)break;
      bank_left-=size;
      work_left-=work;
      frame_bytes+=size;
      ++selected;
    }
    if(selected<2)return false;

    const auto sources=leaf.first(selected);
    std::vector<Entry> destinations;
    std::vector<Bytes> decoded;
    Bytes frames;
    destinations.reserve(selected);
    decoded.reserve(selected);
    frames.reserve(frame_bytes);
    for(const auto& entry:sources) {
      auto source=read_record(original,entry);
      decoded.push_back(decode_record(original,entry,source.header,source.payload));
      auto prepared=prepare_rebuilt_object(build.next,entry,std::move(source),decoded.back(),target->encoder,false);
      destinations.push_back(prepared.entry);
      frames.insert(frames.end(),prepared.header.begin(),prepared.header.end());
      frames.insert(frames.end(),prepared.payload.begin(),prepared.payload.end());
    }
    build.append_mirrored_records(destinations,frames,decoded,verification_frames);
    for(const auto& entry:sources)count(entry);
    frontier->advance(selected);
    return true;
  }
  bool copy_quantum() {
    if(!original.root.count)return false;
    if(!frontier)frontier.emplace(TreeFrontier{{original.root},{}});
    while(frontier->present() && !frontier->has_entries())frontier->open(original);
    if(!frontier->has_entries())return false;
    if(copy_mirrored_prefix(frontier->remaining_entries()))return true;

    auto remaining=retention?(std::max)(std::size_t{1},leaf_capacity(original)/4):1;
    while(remaining && frontier->present()) {
      if(!frontier->has_entries()) {
        frontier->open(original);
        continue;
      }
      const auto entry=frontier->take();
      if(retention && entry.logical>retention->maximum_object_bytes)fail("oversized advisory record");
      const bool improve=cold_copy_candidate(entry);
      bool evaluated=false;
      const auto kept=copy_rebuild_object(build,original,entry,target->encoder,replacement.empty()?0:1,
          replacement,reencode,retention?&retention->keep:nullptr,improve?&evaluated:nullptr);
      // A failed trial leaves its generation eligible. Foreground replay
      // never advances the coverage of this copy.
      if(improve && !evaluated)cold_through=(std::min)(cold_through,entry.generation-1);
      if(retention && kept)copied.include(entry);
      count(entry);
      --remaining;
    }
    return true;
  }
  template<class Visit> bool verify_quantum(const State& input,bool verify_replicas,Visit&& visit) {
    if(!input.root.count)return false;
    if(!frontier)frontier.emplace(TreeFrontier{{input.root},{}});
    while(frontier->present() && !frontier->has_entries())frontier->open(input);
    const auto leaf=frontier->remaining_entries();
    const auto limit=(std::min)(leaf.size(),(std::max)(std::size_t{1},leaf_capacity(input)/4));
    // Keep the same per-step work budget and stay inside the checked leaf.
    std::uint64_t bytes_left=2*kStoreCommitUnit;
    std::size_t selected=0;
    for(const auto& entry:leaf.first(limit)) {
      const auto bytes=std::uint64_t{entry.logical}+entry.stored+record_header_bytes;
      if(selected && bytes>bytes_left)break;
      if(retention && entry.logical>retention->maximum_object_bytes)fail("oversized advisory record");
      bytes_left-=(std::min)(bytes_left,bytes);
      ++selected;
    }
    const auto accept=[&](const Entry& entry,Bytes bytes,bool survivor) {
      visit(entry,std::move(bytes),survivor);
      count(entry);
    };
    for(auto remaining=leaf.first(selected);!remaining.empty();) {
      const auto run=input.mirrored && verify_replicas?mirrored_record_run(input,remaining):MirroredRecordRun{};
      const auto entries=remaining.first(run.count);
      if(run.count>1 && verify_stored_run(input,entries,run.bytes,verification_frames)) {
        auto frames=std::span<const std::byte>(verification_frames).first(run.bytes);
        for(const auto& entry:entries) {
          accept(entry,decode_record(input,entry,frames.first<record_header_bytes>(),
              frames.subspan(record_header_bytes,entry.stored)),false);
          frames=frames.subspan(record_header_bytes+entry.stored);
        }
      } else {
        // Gaps, bank boundaries and failed runs keep per-copy verification.
        for(const auto& entry:entries) {
          Bytes bytes;
          bool survivor=false;
          if(verify_replicas)survivor=verify_object(input,entry,&bytes);
          else bytes=read_object(input,entry);
          accept(entry,std::move(bytes),survivor);
        }
      }
      remaining=remaining.subspan(run.count);
    }
    frontier->advance(selected);
    return selected!=0;
  }
  void measure_compression() {
    auto& work=screen;
    if(!work.samples) {
      work.start=rank_before(original,cold->first_object);
      const auto end=rank_before(original,cold->past_last_object);
      if(end<=work.start) {phase=PagedRewritePhase::declined;return;}
      work.count=end-work.start;
      work.samples=(std::min)(work.count,2*static_cast<std::uint64_t>(std::bit_width(work.count)));
    }
    // Divide the live data ranks into disjoint intervals and sample one
    // record from each. All multiplication stays within the live count.
    const auto width=work.count/work.samples,remainder=work.count%work.samples;
    const auto extra=work.completed<remainder?1U:0U;
    const auto begin=width*work.completed+(std::min)(work.completed,remainder);
    const auto entry=entry_at_rank(original,work.start+begin+(width+extra)/2);
    if(!cold->eligible(entry.id))fail("compression sample outside data range");
    work.compared+=record_header_bytes+entry.stored;
    if(entry.generation>cold->after_generation && entry.stored>16) {
      const auto decoded=read_object(original,entry);
      const auto result=target->encoder.try_cold_encoding(decoded,entry.stored);
      if(result.smaller)work.saved+=entry.stored-result.smaller->payload.size();
    }
    if(++work.completed==work.samples) {
      // Limit private full-image writes on weak samples. The proof remains
      // advisory. Final publication requires a verified smaller image.
      phase=work.saved>=round_up_divide(work.compared,32)?
          PagedRewritePhase::verifying_source:PagedRewritePhase::declined;
    }
  }
  void begin_copy(const State& current) {
    source_check(current);
    if(capacity_damage || folded_admission) {
      const auto rebound=target->encoder.encode(replacement).payload.size();
      const auto stored=add(original.root.stored-original_root_stored,rebound);
      const auto projected=packed_rebuild_size(build.next,original.root.count,stored);
      check_physical_capacity(projected,build.next.limits.maximum_physical_bytes);
      if(capacity_damage && !repair_confirmed && projected>=source_extent)
        capacity("staged repair found neither the reported fault nor a smaller image");
      if(folded_admission)folded_admission(projected);
    }
    const auto initial_end=start+2*build.next.unit;
    if(mode==PagedRewriteMode::cold_compression && original.end<initial_end) {
      phase=PagedRewritePhase::declined;
      return;
    }
    // Memory providers commit on admission. Admit only the first bounded
    // growth here. Subsequent records admit their own growth.
    build.admit_target(initial_end);
    if(mode==PagedRewriteMode::cold_compression) {
      if(!cold->reserve_writes(write_bound)){phase=PagedRewritePhase::declined;return;}
      build.next.limits.maximum_physical_bytes=original.end;
    }
    if(retention) {
      if(!retention->charge(write_bound))return;
      build.next.limits.maximum_physical_bytes=write_bound-2*page_bytes;
    }
    zeros(*target->lease.image,0,start);
    reset(PagedRewritePhase::copying);
  }
  void compare(const State& source,const Entry& expected_entry,const Entry& entry,std::span<const std::byte> actual,
      std::span<const std::byte> native_root) const {
    if(expected_entry.generation!=entry.generation || expected_entry.logical!=entry.logical)
      fail("staged target object differs");
    auto expected=read_object(source,expected_entry);
    if(entry.id==1 && !native_root.empty())expected.assign(native_root.begin(),native_root.end());
    if(!std::ranges::equal(actual,expected))fail("staged target CPU comparison");
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
      const auto decoded=rebind?Bytes{}:decode_record(work.source,entry,from.header,from.payload);
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
      if(entry)compare(work.source,*edit->source,*entry,build.readback(*entry),work.root);
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
    const auto& image=target->lease.image;
    if(image->extent()!=target_extent)fail("staged target extent changed");
    admit_contract(image->contract());
    if(image->contract().write_failure_unit_bytes>build.next.unit)fail("staged target contract changed");
    if(phase==PagedRewritePhase::screening_compression) {
      measure_compression();
    } else if(phase==PagedRewritePhase::verifying_source) {
      const bool advanced=verify_quantum(original,true,[&](const Entry& entry,Bytes bytes,bool survivor) {
        if(survivor && capacity_damage==entry)repair_confirmed=true;
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
          auto projected=packed_rebuild_size(build.next,retained.objects,retained.stored);
          if(build.next.metadata_bytes==compact_metadata_bytes) {
            auto dense=build.next;
            dense.metadata_bytes=page_bytes;
            const auto size=packed_rebuild_size(dense,retained.objects,retained.stored);
            if(size<projected) {
              build.next.metadata_bytes=dense.metadata_bytes;
              projected=size;
            }
          }
          if(!retained.objects || projected>=source_extent || projected>build.next.limits.maximum_physical_bytes)
            capacity("filtered copy cannot reduce occupied bytes");
          write_bound=add(projected,2*page_bytes);
          phase=PagedRewritePhase::awaiting_charge;
        }else if(mode==PagedRewriteMode::cold_compression) {
          write_bound=add(original.end,detail::kPagedStoreCheckpointWriteBytes);
          phase=PagedRewritePhase::awaiting_charge;
        }else begin_copy(current);
      }
    } else if(phase==PagedRewritePhase::awaiting_charge) {
      begin_copy(current);
    } else if(phase==PagedRewritePhase::copying) {
      if(!copy_quantum()) {
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
        if(build.folded && image->extent()!=detail::folded_mirror_extent(build.next.end))
          fail("staged folded length differs from its checkpoint");
        totals(build.next);
        if(completed!=expected_count || logical!=expected_logical)fail("staged copy inventory differs");
        if(retention && (stored!=retained.stored || add(build.next.end,2*page_bytes)!=write_bound))
          fail("filtered copy exceeded its charged plan");
        reset(PagedRewritePhase::verifying_target);
      }
    } else if(phase==PagedRewritePhase::verifying_target || phase==PagedRewritePhase::verifying_publication) {
      if(!comparison_source_pages)comparison_source_pages.emplace();
      if(!verify_quantum(build.next,!build.folded,[&](const Entry& entry,const Bytes& bytes,bool){
        const auto source=lookup(original,entry.id,&*comparison_source_pages);
        if(!source)fail("staged target object differs");
        compare(original,*source,entry,bytes,replacement);
      })) {
        totals(build.next);
        if(phase==PagedRewritePhase::verifying_target)reset(PagedRewritePhase::publishing);
        else mark_ready(current);
      }
    } else if(phase==PagedRewritePhase::publishing) {
      source_check(current);
      build.admit_target(build.next.end);
      if(build.folded) {
        // Keep the data barrier and both isolated root barriers in separate
        // opportunities. The workspace stays private through verification.
        if(!folded_barriers)image->flush();
        else publish_root(build.next,*image,build.next.checkpoint+folded_barriers-1);
        if(++folded_barriers<3)return;
      }else publish(build.next);
      auto published=load(build.next.image,build.next.limits);
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
std::uint64_t PagedObjectStoreWriter::Rewrite::cold_encoding_through() const noexcept {
  return impl_ && impl_->phase==PagedRewritePhase::ready?impl_->cold_through:0;
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
  if(impl_->folded_admission)fail("folded workspace cannot become a writable store");
  auto candidate=std::move(impl_);return PagedObjectStoreWriter(std::move(candidate->target));
}
void PagedObjectStoreWriter::Rewrite::verify_folded_source(const PagedObjectStoreWriter& writer) const {
  if(!ready_for(writer) || !impl_->folded_admission)fail("folded workspace source checkpoint changed");
  impl_->source_check(writer.impl_->state);
}
detail::FoldedCheckpoint PagedObjectStoreWriter::Rewrite::release_folded_workspace(const PagedObjectStoreWriter& writer) {
  verify_folded_source(writer);
  if(!impl_->target)fail("folded workspace has been released");
  const auto checked=folded_checkpoint(impl_->build.next);
  impl_->target.reset();
  impl_->build.folded=nullptr;
  impl_->build.next.image.reset();
  return checked;
}

PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,bool reencode) const {
  return prepare_rebuild_replacing(std::move(target),id,compression,{},reencode);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_replacing(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> replacement,bool reencode,
    std::size_t identity_offset,PagedRewriteMode mode,std::optional<RebuildColdEncoding> cold) const {
  return prepare_rebuild_impl(std::move(target),id,compression,replacement,reencode,identity_offset,{},mode,std::move(cold));
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_retaining(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,RebuildRetention keep) const {
  if(poisoned())fail("filtered rebuild requires a healthy source writer");
  return prepare_rebuild_impl(std::move(target),id,compression,{},false,0,std::move(keep),
      PagedRewriteMode::exact_checkpoint);
}
PagedObjectStoreWriter::Rewrite PagedObjectStoreWriter::prepare_rebuild_impl(std::shared_ptr<StoreByteIo> target,
    StoreIdentity id,StoreCompressionOptions compression,std::span<const std::byte> replacement,bool reencode,
    std::size_t identity_offset,std::optional<RebuildRetention> retention,PagedRewriteMode mode,
    std::optional<RebuildColdEncoding> cold,std::function<void(std::uint64_t)> folded_admission) const {
  if(poisoned())fail("staged rebuild requires a healthy source writer");
  if(!impl_->pending_rebuild.expired())fail("source already has a staged rebuild");
  auto candidate=std::make_unique<Rewrite::Impl>(*impl_,std::move(target),id,compression,
      replacement,reencode,identity_offset,std::move(retention),mode,std::move(cold),std::move(folded_admission));
  impl_->pending_rebuild=candidate->reservation;return Rewrite(std::move(candidate));
}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot& PagedObjectStoreSnapshot::operator=(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot::~PagedObjectStoreSnapshot()=default;
PagedObjectStoreSnapshot::QueryScope::QueryScope(Impl& impl,detail::PagedReadMetadata* offered) noexcept {
  if(impl.query_metadata)return;
  owner_=&impl;
  owns_lookup_pages_=!impl.lookup_pages;
  if(offered) {
    if(offered->pages && offered->records && offered->matches(impl.state)) {
      reusable_=offered;
      impl.lookup_pages.swap(offered->pages);
      impl.query_metadata.swap(offered->records);
    }else offered->discard();
  }
  if(!impl.lookup_pages)impl.lookup_pages.emplace();
  if(!impl.query_metadata)impl.query_metadata.emplace();
}
PagedObjectStoreSnapshot::QueryScope::~QueryScope() {
  if(!owner_)return;
  if(reusable_) {
    if(can_reuse_query(owner_->state)) {
      owner_->lookup_pages.swap(reusable_->pages);
      owner_->query_metadata.swap(reusable_->records);
    }else reusable_->discard();
  }
  owner_->query_metadata.reset();
  if(owns_lookup_pages_)owner_->lookup_pages.reset();
}
PagedObjectStoreSnapshot::QueryScope PagedObjectStoreSnapshot::scoped_query(detail::PagedReadMetadata* reusable) const noexcept {
  return QueryScope(*impl_,reusable);
}
std::shared_ptr<detail::PagedReadMetadata> PagedObjectStoreSnapshot::take_query_metadata() const noexcept {
  if(!impl_->lookup_pages || !impl_->query_metadata || !can_reuse_query(impl_->state))return {};
  try {
    auto result=std::make_shared<detail::PagedReadMetadata>(impl_->state);
    result->pages.swap(impl_->lookup_pages);
    result->records.swap(impl_->query_metadata);
    return result;
  }catch(const std::bad_alloc&){return {};}
}
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits) {
  return open_impl(std::move(image),limits,false);
}
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open_sealed(std::shared_ptr<detail::ProtectedStripeImage> backing) {
  auto view=open(backing,{backing->extent()});
  const auto info=view.info();
  if(view.identity()!=backing->identity() || info.mirrored_payloads ||
      info.degraded_roots || info.physical_end!=backing->extent())
    fail("sealed image does not bind an exact single-stream checkpoint");
  view.impl_->sealed_payloads=true;
  return view;
}
bool PagedObjectStoreSnapshot::payloads_protected() const noexcept {
  return impl_ && (impl_->state.mirrored || impl_->sealed_payloads);
}
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open_impl(std::shared_ptr<StoreByteIo> image,
    PagedStoreLimits limits,bool retain_tree_root) {
  auto p=std::make_unique<Impl>();
  auto* root=retain_tree_root?&p->retained_root.emplace():nullptr;
  p->state=load(std::move(image),limits,nullptr,root);
  return PagedObjectStoreSnapshot(std::move(p));
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
    std::optional<std::uint64_t> expected_generation,detail::NativeLz4ReadRoute* route) const {
  if(!impl_)fail("moved-from snapshot");
  auto e=lookup(impl_->state,id,impl_->lookup_pages?&*impl_->lookup_pages:nullptr,
      impl_->retained_root?&*impl_->retained_root:nullptr);
  if(!e || (expected_generation && e->generation!=*expected_generation))return {};
  if(e->logical>maximum_decoded_bytes)fail("object exceeds decoded read budget");
  const auto& state=impl_->state;
  // Only captured frames carry storage provenance. Other advisory routes
  // still need the checked on-backing record before they may supply bytes.
  if(auto* ready=dynamic_cast<detail::NativeLz4ReadyReads*>(route);
      ready && healthy_lz4_index(state) && (!state.payload_repairs || !state.payload_repairs->suspect(*e))) {
    if(auto bytes=ready->try_indexed(state.id,e->id,e->generation,e->logical,e->digest))
      return bytes;
  }
  return read_object(state,*e,route);
}
bool PagedObjectStoreSnapshot::capture_native_lz4(std::uint64_t object,std::size_t maximum_decoded,
    std::size_t maximum_encoded,detail::NativeLz4CapturedFrame& out,
    std::optional<std::size_t> remaining_frame_bytes) const {
  if(!impl_ || !maximum_encoded || !store_lz4_available())return false;
  const auto& state=impl_->state;
  if(!healthy_lz4_index(state))return false;
  const auto entry=lookup(state,object,impl_->lookup_pages?&*impl_->lookup_pages:nullptr,
      impl_->retained_root?&*impl_->retained_root:nullptr);
  if(!entry || entry->logical>maximum_decoded || entry->stored>maximum_encoded || entry->stored>=entry->logical ||
      !healthy_lz4_index(state) || (state.payload_repairs && state.payload_repairs->suspect(*entry)))return false;
  if(remaining_frame_bytes && std::uint64_t{entry->logical}+entry->stored>*remaining_frame_bytes)return false;
  // Speculation neither opens an unsuitable payload nor schedules repair.
  ObjectRecord record;
  read_record_bytes(*state.image,entry->offset,0,record.header,state.mirrored?state.unit:0);
  verify_record_header(state,*entry,record.header);
  if(get32(record.header,12)!=detail::kStoreLz4)return false;
  read_record_payload(state,*entry,entry->offset,record);
  if(!healthy_lz4_index(state))return false;
  out.key_={state.id,entry->id,entry->generation,entry->logical,field<32>(record.header,56)};
  out.stored_digest_=entry->digest;
  out.encoded_=std::move(record.payload);
  return true;
}
bool PagedObjectStoreSnapshot::capture_group_read_record(std::uint64_t object,std::size_t maximum_decoded,
    std::optional<std::uint64_t> generation,detail::PagedGroupReadFrame& out) const {
  const auto& state=impl_->state;
  if(!state.mirrored || !can_reuse_query(state))return false;
  const auto entry=lookup(state,object,impl_->lookup_pages?&*impl_->lookup_pages:nullptr,
      impl_->retained_root?&*impl_->retained_root:nullptr);
  if(!entry || entry->logical>maximum_decoded || (generation && entry->generation!=*generation) ||
      !can_reuse_query(state) || (state.payload_repairs && state.payload_repairs->suspect(*entry)))return false;
  auto record=read_record_at(state,*entry,entry->offset);
  auto decoded=consume_decoded_record(state,*entry,record);
  if(!can_reuse_query(state))return false;
  out={group_read_record(*entry),std::move(decoded)};
  return true;
}
bool PagedObjectStoreSnapshot::matches_group_read(const detail::PagedGroupReadRecord& member,
    const detail::PagedGroupReadRecord& base,const detail::PagedReadMetadata& capture) const noexcept {
  const auto& state=impl_->state;
  if(!state.mirrored || !capture.matches(state))return false;
  try {
    for(const auto* binding:{&member,&base}) {
      const auto entry=lookup(state,binding->object,impl_->lookup_pages?&*impl_->lookup_pages:nullptr,
          impl_->retained_root?&*impl_->retained_root:nullptr);
      if(!entry || group_read_record(*entry)!=*binding ||
          (state.payload_repairs && state.payload_repairs->suspect(*entry)))return false;
    }
    return capture.matches(state);
  }catch(const std::exception&){return false;}
}
std::optional<std::pair<std::uint64_t,Bytes>> PagedObjectStoreSnapshot::read_first_after(
    std::uint64_t after,std::uint64_t last,std::size_t maximum_decoded_bytes) const {
  if(!impl_)fail("moved-from snapshot");
  if(after>=last)return {};
  const auto& state=impl_->state;
  const auto* root=impl_->retained_root?&*impl_->retained_root:nullptr;
  auto reference=state.root;
  while(reference.count && reference.last>after && reference.first<=last) {
    Node temporary;
    const auto& node=lookup_node(state,reference,temporary,nullptr,root);
    if(!reference.height) {
      const auto entry=std::ranges::upper_bound(node.entries,after,{},&Entry::id);
      if(entry==node.entries.end() || entry->id>last)return {};
      if(entry->logical>maximum_decoded_bytes)fail("object exceeds decoded read budget");
      return std::pair{entry->id,read_object(state,*entry)};
    }
    // This child contains the first possible successor. Its checked last
    // key is above after, so every later child starts beyond that successor.
    const auto child=std::ranges::upper_bound(node.children,after,{},&Ref::last);
    if(child==node.children.end())return {};
    reference=*child;
  }
  return {};
}
std::vector<StoreObjectInfo> PagedObjectStoreSnapshot::list_page(std::uint64_t after,std::size_t limit) const {
  if(!impl_)fail("moved-from snapshot");if(!limit || limit>4096)fail("pagination budget");
  std::vector<StoreObjectInfo> result;result.reserve(limit);bool done=false;
  auto visitor=[&](const Entry& e){result.push_back({e.id,e.generation,e.logical,e.stored});return result.size()<limit;};
  walk(impl_->state,impl_->state.root,after,visitor,done);return result;
}
void PagedObjectStoreSnapshot::verify_all() const {if(!impl_)fail("moved-from snapshot");verify(impl_->state);}
void PagedObjectStoreSnapshot::visit_verified_objects(const VerifiedObjectVisitor& consume) const {
  if(!impl_)fail("moved-from snapshot");
  verify(impl_->state,&consume);
}
void PagedObjectStoreSnapshot::recheck_verified_mirrors(const PagedObjectStoreSnapshot& audited) const {
  if(!impl_ || !audited.impl_)fail("moved-from snapshot");
  const auto& before=audited.impl_->state;
  const auto& current=impl_->state;
  if(before.image!=current.image || !before.mirrored || before.degraded_roots ||
      before.degraded_metadata || before.degraded_payloads || !same(before,current))
    fail("stored mirror recheck needs the exact healthy audited checkpoint");
  // The checked root binds every leaf descriptor to its exact stored frame.
  // A matching frame retains the earlier decoded digest and native audit.
  verify(current,nullptr,PayloadVerification::stored);
}
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
    std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve_writes,bool compact_metadata) {
  admit_limits(limits);if(zero(id))fail("nonzero store identity required");
  if(initial.size()>kStoreMaxObjects)capacity("initial transaction operation budget");
  auto p=std::make_unique<Impl>(image,options);admit_contract(image->contract());
  if(image->extent())fail("new empty backend required");
  State s;s.image=image;s.limits=limits;s.id=id;s.generation=2;s.checkpoint=1;s.mirrored=mirrored;
  if(compact_metadata) {
    if(mirrored)fail("compact metadata requires an advisory store");
    s.metadata_bytes=compact_metadata_bytes;
  }
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
PagedObjectStoreWriter PagedObjectStoreWriter::resume_advisory(const PagedObjectStoreSnapshot& observed,
    StoreCompressionOptions options) {
  if(!observed.impl_)fail("moved-from advisory snapshot");
  const auto& prior=observed.impl_->state;
  if(prior.mirrored || prior.metadata_bytes!=compact_metadata_bytes || prior.degraded_roots ||
      prior.degraded_metadata)fail("advisory reopen requires healthy compact roots");
  auto p=std::make_unique<Impl>(prior.image,options);
  const auto contract=prior.image->contract();
  admit_contract(contract);
  if(contract.write_failure_unit_bytes>prior.unit)fail("advisory reopen contract changed");
  const auto extent=prior.image->extent();
  prior.image->flush();
  auto current=load(prior.image,prior.limits);
  if(!same(prior,current) || current.degraded_roots || current.degraded_metadata ||
      prior.image->extent()!=extent)fail("advisory reopen checkpoint changed");
  p->append_floor=align(extent,current.unit);
  p->state=std::move(current);
  return PagedObjectStoreWriter(std::move(p));
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
  if(s.image->backing_degraded())fail("backing protection requires a verified rebuild before writable resume");
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
namespace {
State rebuild_checkpoint(const State& original,const std::shared_ptr<StoreByteIo>& target,StoreIdentity id,
    PagedStoreLimits limits,detail::StoreCodecEncoder& encoder,std::uint64_t replacement_id,
    std::span<const std::byte> replacement,bool reencode,std::size_t identity_offset,bool mirrored,bool folded) {
  auto observed=load(original.image,original.limits);
  if(!same(original,observed))fail("rebuild source is not the current checkpoint");
  const auto source_extent=original.image->extent();
  std::uint64_t replaced_stored=0;
  if(replacement_id) {
    const auto entry=lookup(original,replacement_id);
    if(!entry || replacement.size()!=entry->logical)fail("rebuild replacement identity/length");
    // Native rebinding changes only the fixed identity field. Validate the
    // current metadata before any target writes.
    auto expected=read_object(original,*entry);
    if(identity_offset>expected.size() || id.size()>expected.size()-identity_offset)
      fail("rebuild replacement framing");
    copy(expected,identity_offset,id);
    if(!std::equal(expected.begin(),expected.end(),replacement.begin()))
      fail("rebuild replacement differs beyond image identity");
    replaced_stored=entry->stored;
    verify(original);
  }
  admit_contract(target->contract());
  if(target->extent())fail("rebuild requires an empty target");
  Rebuild build;
  build.next.limits=limits;
  build.next.id=id;
  build.next.mirrored=mirrored;
  build.next.metadata_bytes=original.metadata_bytes;
  build.next.generation=add(original.generation,2);
  build.next.checkpoint=original.generation+1;
  build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),target->contract().write_failure_unit_bytes);
  if(folded) {
    build.folded=target.get();
    build.next.image=std::make_shared<FoldedRebuildView>(target,build.next.unit,limits.maximum_physical_bytes);
  }else build.next.image=target;
  if(!reencode) {
    const auto rebound=replacement_id?encoder.encode(Bytes(replacement.begin(),replacement.end())).payload.size():0;
    const auto projected_stored=add(original.root.stored-replaced_stored,rebound);
    const auto projected=packed_rebuild_size(build.next,original.root.count,projected_stored);
    check_physical_capacity(projected,limits.maximum_physical_bytes);
    build.admit_target(projected);
  }else build.admit_target(start+2*build.next.unit);
  zeros(*target,0,start);
  auto visitor=[&](const Entry& source_entry) {
    copy_rebuild_object(build,original,source_entry,encoder,replacement_id,replacement,reencode);
    return true;
  };
  bool done=false;
  (void)read_node(original,original.root);
  walk(original,original.root,0,visitor,done);
  build.finish_payloads();
  for(std::uint64_t indexed=0;indexed<original.root.count;++indexed)build.index_record();
  build.finish();
  if(build.next.root.count!=original.root.count || build.next.root.logical!=original.root.logical)
    fail("rebuild inventory mismatch");
  if(folded && target->extent()!=detail::folded_mirror_extent(build.next.end))
    fail("folded workspace length differs from the indexed checkpoint");
  target->flush();
  verify(build.next);
  if(build.next.degraded_metadata || build.next.degraded_payloads)fail("rebuild produced degraded target");
  if(replacement_id)verify(original);
  if(!same(original,load(original.image,original.limits)) || original.image->extent()!=source_extent)
    fail("rebuild source changed before publication");
  publish_roots(build.next,*target);
  auto published=load(build.next.image,limits);
  verify(published);
  if(!same(build.next,published) || published.degraded_roots || published.degraded_metadata || published.degraded_payloads)
    fail("rebuild published checkpoint failed independent verification");
  return published;
}
} // namespace

PagedObjectStoreWriter PagedObjectStoreWriter::rebuild_replacing(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,PagedStoreLimits limits,StoreCompressionOptions options,
    std::uint64_t replacement_id,std::span<const std::byte> replacement,bool reencode,std::size_t identity_offset,
    RebuildPayloads protection) {
  if(!source.impl_)fail("moved-from rebuild source");
  admit_limits(limits);
  const auto& original=source.impl_->state;
  const bool workspace=protection==RebuildPayloads::sealed_workspace;
  const bool recovering=protection==RebuildPayloads::mirrored_recovery;
  if(workspace && (!original.mirrored || !replacement_id || reencode))
    fail("sealed workspace requires a mirrored identity-only source");
  if(recovering!=source.impl_->sealed_payloads || (recovering && !replacement_id))
    fail("rebuild requires an explicit sealed recovery source");
  if(zero(id) || id==original.id)fail("rebuild requires a new nonzero identity");
  // Allow exact reconstruction to a fresh target even if the source can
  // only provide an exclusive read lease.
  Impl::Lease source_lease(original.image,true);
  auto p=std::make_unique<Impl>(target,options);
  p->state=rebuild_checkpoint(original,target,id,limits,p->encoder,replacement_id,replacement,
      reencode,identity_offset,recovering || (original.mirrored && !workspace),false);
  p->append_floor=p->state.end;
  return PagedObjectStoreWriter(std::move(p));
}

detail::FoldedCheckpoint PagedObjectStoreWriter::rebuild_folded(const PagedObjectStoreSnapshot& source,
    std::shared_ptr<StoreByteIo> target,StoreIdentity id,StoreCompressionOptions compression,
    std::span<const std::byte> rebound_root,std::size_t identity_offset) {
  if(!source.impl_ || source.impl_->sealed_payloads || !source.impl_->state.mirrored ||
      zero(id) || id==source.impl_->state.id || rebound_root.empty())
    fail("folded workspace requires fresh identity and a checked mirrored source");
  const auto& original=source.impl_->state;
  admit_limits(original.limits);
  Impl::Lease source_lease(original.image,true),target_lease(target);
  detail::StoreCodecEncoder encoder(compression);
  const auto state=rebuild_checkpoint(original,target,id,original.limits,encoder,1,rebound_root,
      false,identity_offset,true,true);
  return folded_checkpoint(state);
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
  std::uint32_t stored_lz4_candidates=0;
  try {
    for(const auto& record:objects) {
      write_object_record(*old.image,record.entry,record.header,record.payload,old.unit);
      stored_lz4_candidates+=record.used_lz4_candidate;
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
  auto result=flitzfs::info(p.state);
  result.stored_lz4_candidates=stored_lz4_candidates;
  return {result,write_bound};
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot() const {
  return snapshot_impl(false);
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot_impl(bool retain_tree_root) const {
  if(!impl_)fail("moved-from writer");auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();
  p->state=impl_->state;if(impl_->poisoned)p->state.degraded_roots=true;
  auto root=read_node(p->state,p->state.root);
  if(retain_tree_root)p->retained_root.emplace(std::move(root));
  return PagedObjectStoreSnapshot(std::move(p));
}
PagedObjectStoreSnapshot PagedObjectStoreWriter::snapshot_for_staging(bool query_cache) const {
  if(!impl_ || impl_->poisoned)fail("staging requires a healthy writer");
  auto p=std::make_unique<PagedObjectStoreSnapshot::Impl>();p->state=impl_->state;
  if(query_cache)p->lookup_pages.emplace();
  return PagedObjectStoreSnapshot(std::move(p));
}
bool PagedObjectStoreWriter::concurrent_reads_supported() const noexcept {
  return impl_ && impl_->state.image->concurrent_reads_supported();
}
std::optional<PagedObjectStoreSnapshot> PagedObjectStoreWriter::snapshot_for_capture() const {
  if(!concurrent_reads_supported() || impl_->poisoned || !healthy_lz4_index(impl_->state) ||
      payload_repair_pending())return {};
  auto result=std::make_unique<PagedObjectStoreSnapshot::Impl>();
  auto decoder=std::move(result->state.decoder);
  result->state=impl_->state;
  result->state.decoder=std::move(decoder);
  result->state.payload_repairs.reset();
  result->lookup_pages.emplace();
  return PagedObjectStoreSnapshot(std::move(result));
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
  impl_->scrub_checkpoint.reset();
  return true;
}
PagedPayloadScrubResult PagedObjectStoreWriter::scrub_payload(const PagedPayloadScrubBudget& budget) {
  using S=PagedPayloadScrubState;
  if(poisoned() || !impl_->state.mirrored || !budget.maximum_objects || !budget.maximum_object_work_bytes ||
      (budget.should_yield && budget.should_yield(0)))return {};
  auto& writer=*impl_;
  PagedPayloadScrubResult result;
  const auto finish=[&](S state) {
    writer.scrub_after=writer.scrub_through=0;
    writer.scrub_prefix_verified=true;
    writer.scrub_checkpoint.reset();
    result.state=state;
    result.resumable.reset();
    result.object_id=0;
    return result;
  };
  const auto& selected=writer.state;
  try {
    const auto source=load(selected.image,selected.limits);
    if(!same(selected,source) || source.degraded_roots || source.degraded_metadata)
      return finish(S::unverified_index);
    if(!writer.scrub_through) {
      writer.scrub_through=source.root.last;
      writer.scrub_checkpoint.emplace(Impl::ScrubCheckpoint{CheckpointKey(source)});
    }
    auto& checkpoint=writer.scrub_checkpoint;
    if(checkpoint && (!checkpoint->key.matches(source) || source.image->backing_degraded()))checkpoint.reset();

    auto remaining=budget.maximum_object_work_bytes;
    bool reached_limit=false;
    auto examine=[&](const Entry& record) {
      if(source.degraded_metadata)return false;
      if(record.id>writer.scrub_through) {
        reached_limit=true;
        return false;
      }
      const auto cost=2ULL*(record_header_bytes+record.stored+record.logical);
      if(cost>remaining || (budget.should_yield && budget.should_yield(result.examined_objects)))return false;
      const auto verified=verify_payload_copies(source,record);
      if(verified==1) {
        if(!writer.payload_repairs)writer.payload_repairs=std::make_shared<PayloadRepairQueue>();
        writer.payload_repairs->observe(record);
      }
      writer.scrub_after=record.id;
      result.object_id=record.id;
      if(!result.examined_objects)result.first_examined_object_id=record.id;
      ++result.examined_objects;
      remaining-=cost;
      if(verified!=2) {
        writer.scrub_prefix_verified=false;
        checkpoint.reset();
        result.state=verified==1?S::repair_queued:S::unverified_payload;
        return false;
      }
      result.verified_record_bytes+=2ULL*(record_header_bytes+record.stored);
      result.state=S::checked;
      if(checkpoint)++checkpoint->objects;
      return result.examined_objects<budget.maximum_objects;
    };
    bool stopped=false;
    walk(source,source.root,writer.scrub_after,examine,stopped);
    if(source.degraded_metadata)return finish(S::unverified_index);
    if(source.image->backing_degraded())checkpoint.reset();
    if(!stopped || reached_limit) {
      if(checkpoint && checkpoint->objects==source.root.count) {
        const auto observed=load(source.image,source.limits);
        if(!same(source,observed) || observed.degraded_roots || observed.degraded_metadata)
          return finish(S::unverified_index);
        if(!source.image->backing_degraded())
          result.verified_checkpoint=PagedPayloadCheckpointObservation{source.id,source.generation,
              folded_checkpoint(observed).roots};
      }
      return finish(S::complete);
    }
    if(writer.scrub_prefix_verified && writer.scrub_after && writer.scrub_after<writer.scrub_through)
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
        (void)decode_record(source,*record,copy.header,copy.payload);
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

std::uint64_t PagedObjectStoreWriter::compaction_savings_lower_bound(std::uint64_t extra_stored_bytes) const noexcept {
  if(poisoned())return 0;
  const auto& state=impl_->state;
  try {
    const auto packed=packed_rebuild_size(state,state.root.count,add(state.root.stored,extra_stored_bytes));
    return state.end>packed?state.end-packed:0;
  }catch(const std::exception&) {
    return 0; // Arithmetic outside the admitted packed layout cannot justify a rewrite.
  }
}
std::uint64_t PagedObjectStoreWriter::append_floor_for_staging() const noexcept {
  return poisoned()?0:impl_->append_floor;
}
PagedStoreInfo PagedObjectStoreWriter::flush_checkpoint() {
  return flush_checkpoint_verified({});
}
PagedStoreInfo PagedObjectStoreWriter::flush_checkpoint_verified(
    const std::function<void(const PagedObjectStoreSnapshot&)>& verify_namespace) {
  if(poisoned())fail("flush requires a healthy writer");
  auto& p=*impl_;const auto& old=p.state;bool barrier_entered=false;
  try {
    const auto extent=old.image->extent();
    if(extent<old.end || extent>old.limits.maximum_physical_bytes)fail("flush extent bounds");
    const auto check_checkpoint=[&](LookupPages* pages) {
      const auto contract=old.image->contract();
      admit_contract(contract);
      if(contract.write_failure_unit_bytes>old.unit)fail("flush failure unit changed");
      const auto actual=load(old.image,old.limits,pages);
      if(!same(actual,old) || actual.degraded_roots || actual.degraded_metadata)
        fail("flush checkpoint changed/degraded; independent recovery required");
    };
    const auto check=[&] {
      if(verify_namespace) {
        const auto snapshot=snapshot_for_indexed_read();
        // Seed this snapshot with the root just verified from both replicas.
        // Exact checkpoint equality admits reuse only for this phase.
        check_checkpoint(&*snapshot.impl_->lookup_pages);
        verify_namespace(snapshot);
      } else check_checkpoint(nullptr);
      if(old.image->extent()!=extent)fail("flush backing extent changed; independent recovery required");
    };
    check();const auto result=flitzfs::info(old);
    barrier_entered=true;old.image->flush();check();return result;
  }catch(const std::bad_alloc&) {if(barrier_entered)p.poisoned=true;throw;}
  catch(...) {p.poisoned=true;throw;}
}
bool PagedObjectStoreWriter::poisoned() const noexcept {return !impl_ || impl_->poisoned;}
} // namespace flitzfs
