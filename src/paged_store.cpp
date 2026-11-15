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
constexpr std::size_t page_bytes=4096,header_bytes=64,entry_bytes=64,ref_bytes=112;
constexpr std::size_t digest_at=page_bytes-32,leaf_capacity=(digest_at-header_bytes)/entry_bytes;
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
void encode_entry(std::span<std::byte> b,std::size_t at,const Entry& e) {
  put64(b,at,e.id);put64(b,at+8,e.generation);put64(b,at+16,e.offset);
  put32(b,at+24,e.logical);put32(b,at+28,e.stored);copy(b,at+32,e.digest);
}
Entry decode_entry(std::span<const std::byte> b,std::size_t at) {
  return {get64(b,at),get64(b,at+8),get64(b,at+16),get32(b,at+24),get32(b,at+28),field<32>(b,at+32)};
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
  std::shared_ptr<detail::StoreCodecDecoder> decoder=std::make_shared<detail::StoreCodecDecoder>();
};
PagedStoreInfo info(const State& s) {
  return {s.generation,s.checkpoint,s.end,s.root.count,s.root.logical,s.root.stored,s.root.height,
          s.degraded_roots,s.degraded_metadata};
}
void admit_ref(const State& s,const Ref& r,bool root=false) {
  if(r.height>maximum_height || !r.generation || r.generation>s.checkpoint ||
      r.primary<start || r.primary%page_bytes || r.peer!=add(r.primary,s.unit) ||
      r.peer>s.end || page_bytes>s.end-r.peer ||
      (r.primary/s.unit)==(r.peer/s.unit) ||
      (r.count ? (!r.first || r.last<r.first || r.count>r.last-r.first+1) :
                 (!root || r.height || r.first || r.last || r.logical || r.stored)) ||
      r.stored>r.logical || (r.count && r.logical/r.count>kStoreMaxObjectBytes))
    fail("page reference bounds/summary");
}
struct Node {std::vector<Entry> entries;std::vector<Ref> children;};
Ref summary(const Node& n,std::uint32_t height) {
  Ref r;r.height=height;
  if(height) {
    if(n.children.empty() || n.children.size()>branch_capacity)fail("branch arity");
    for(const auto& c:n.children) {
      if(!c.count || c.height+1!=height || (r.count && c.first<=r.last))fail("child ordering/height");
      if(!r.count)r.first=c.first;
      r.last=c.last;r.count=add(r.count,c.count);r.logical=add(r.logical,c.logical);r.stored=add(r.stored,c.stored);
    }
  } else {
    if(n.entries.size()>leaf_capacity)fail("leaf arity");
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
  const auto count=get32(bytes,40);const auto capacity=r.height?branch_capacity:leaf_capacity;
  if(!magic(bytes,page_magic) || get32(bytes,8)!=1 || get32(bytes,12)!=r.height ||
      get64(bytes,16)!=r.generation || field<16>(bytes,24)!=s.id || count>capacity ||
      !zero(bytes.subspan(44,20)) ||
      !zero(bytes.subspan(header_bytes+count*(r.height?ref_bytes:entry_bytes),
                         digest_at-header_bytes-count*(r.height?ref_bytes:entry_bytes))))
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
      auto e=decode_entry(bytes,header_bytes+i*entry_bytes);
      if(!e.generation || e.generation>r.generation || e.logical>kStoreMaxObjectBytes || e.stored>e.logical ||
          e.offset<start || e.offset>r.primary || 128ULL+e.stored>r.primary-e.offset)
        fail("object descriptor bounds");
      n.entries.push_back(e);
    }
  }
  auto total=summary(n,r.height);
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
Bytes read_object(const State& s,const Entry& e) {
  std::array<std::byte,128> h{};s.image->read_exact(e.offset,h);const auto codec=get32(h,12);
  if(!magic(h,data_magic) || get32(h,8)!=1 || get64(h,16)!=e.id || get64(h,24)!=e.generation ||
      get32(h,32)!=e.logical || get32(h,36)!=e.stored || field<16>(h,40)!=s.id ||
      !detail::store_codec_framing(codec,e.logical,e.stored) ||
      !zero(std::span<const std::byte>(h).subspan(codec==detail::kStoreRaw?56:88,codec==detail::kStoreRaw?40:8)) ||
      field<32>(h,96)!=e.digest)fail("object identity/codec/framing");
  Bytes payload(e.stored);if(!payload.empty())s.image->read_exact(e.offset+128,payload);
  if(detail::record_digest(std::span<const std::byte>(h).first(96),payload)!=e.digest)fail("object stored digest");
  if(codec==detail::kStoreRaw)return payload;
  auto decoded=s.decoder->decode(codec,payload,e.logical);
  if(detail::record_digest(decoded)!=field<32>(h,56))fail("object decoded digest");
  return decoded;
}
template<class Visitor> void walk(const State& s,const Ref& r,std::uint64_t after,Visitor& visitor,bool& done) {
  if(done || !r.count || r.last<=after)return;
  auto n=read_node(s,r);
  if(r.height) {for(const auto& child:n.children)walk(s,child,after,visitor,done);}
  else for(const auto& e:n.entries)if(e.id>after && !done)done=!visitor(e);
}
void verify(const State& s) {
  (void)read_node(s,s.root);
  auto visitor=[&](const Entry& e){(void)read_object(s,e);return true;};
  bool done=false;walk(s,s.root,0,visitor,done);
}
std::array<std::byte,page_bytes> root_bytes(const State& s,std::uint64_t generation) {
  std::array<std::byte,page_bytes> b{};set_magic(b,root_magic);put32(b,8,1);
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
    s.checkpoint=get64(b,24);s.end=get64(b,32);s.unit=get32(b,56);s.root=decode_ref(b,64);
    if(!magic(b,root_magic) || get32(b,8)!=1 || get32(b,12)!=slot ||
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
    if(other->id!=s.id || other->unit!=s.unit || s.generation<=other->generation ||
        s.generation-other->generation!=1)fail("conflicting root identities/generations");
    paired=other->checkpoint==s.checkpoint && other->root==s.root && other->end==s.end;
    if(s.generation!=s.checkpoint && !paired)fail("conflicting intact root pair");
  }
  s.degraded_roots=!paired;(void)read_node(s,s.root);return s;
}
bool same(const State& a,const State& b) {
  return a.id==b.id && a.generation==b.generation && a.checkpoint==b.checkpoint &&
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
struct PreparedPage {Ref ref;std::array<std::byte,page_bytes> bytes{};};
struct PreparedObject {Entry entry;std::array<std::byte,128> header{};Bytes payload;};
struct Change {std::uint64_t id;bool erase;Entry entry;};
struct Plan {
  State next;
  std::size_t changed_keys=kStoreMaxObjects;
  std::uint64_t page_start=0;
  std::vector<PreparedPage> pages;
  Ref make_page(const Node& node,std::uint32_t height) {
    // A bounded transaction changes <=256 paths, <=17 levels plus splits.
    // This work limit is independent of the volume's object count.
    if(pages.size()>=2*(maximum_height+1)*changed_keys+maximum_height+1)capacity("transaction page budget");
    PreparedPage p;auto& r=p.ref;r=summary(node,height);r.generation=next.checkpoint;
    const auto per_unit=next.unit/page_bytes;
    r.primary=add(page_start,add((pages.size()/per_unit)*2ULL*next.unit,(pages.size()%per_unit)*page_bytes));
    r.peer=add(r.primary,next.unit);
    if(add(r.peer,page_bytes)>next.limits.maximum_physical_bytes)capacity("physical capacity");
    auto& b=p.bytes;set_magic(b,page_magic);put32(b,8,1);put32(b,12,height);
    put64(b,16,r.generation);copy(b,24,next.id);
    put32(b,40,static_cast<std::uint32_t>(height?node.children.size():node.entries.size()));
    if(height)for(std::size_t i=0;i<node.children.size();++i)encode_ref(b,header_bytes+i*ref_bytes,node.children[i]);
    else for(std::size_t i=0;i<node.entries.size();++i)encode_entry(b,header_bytes+i*entry_bytes,node.entries[i]);
    r.digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));copy(b,digest_at,r.digest);
    const auto result=r;pages.push_back(std::move(p));return result;
  }
  std::vector<Ref> leaves(const std::vector<Entry>& entries) {
    std::vector<Ref> output;if(entries.empty())return output;
    const auto count=(entries.size()+leaf_capacity-1)/leaf_capacity;
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
  Node leaf;
  std::array<Node,maximum_height+1> levels;
  Ref write_node(const Node& node,std::uint32_t height) {
    Plan plan;plan.next=next;plan.page_start=align(cursor,next.unit);
    auto ref=plan.make_page(node,height);plan.finish({ref});next.image->admit_write(plan.next.end);
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
    if(leaf.entries.size()==leaf_capacity) {auto ref=write_node(leaf,0);leaf.entries.clear();emit(ref);}
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
  Impl(std::shared_ptr<StoreByteIo> image,StoreCompressionOptions options):lease(std::move(image)),encoder(options){}
};
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
PagedObjectStoreSnapshot::PagedObjectStoreSnapshot(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot& PagedObjectStoreSnapshot::operator=(PagedObjectStoreSnapshot&&) noexcept=default;
PagedObjectStoreSnapshot::~PagedObjectStoreSnapshot()=default;
PagedObjectStoreSnapshot PagedObjectStoreSnapshot::open(std::shared_ptr<StoreByteIo> image,PagedStoreLimits limits) {
  auto p=std::make_unique<Impl>();p->state=load(std::move(image),limits);return PagedObjectStoreSnapshot(std::move(p));
}
StoreIdentity PagedObjectStoreSnapshot::identity() const noexcept {return impl_?impl_->state.id:StoreIdentity{};}
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
  admit_limits(limits);if(zero(id))fail("nonzero store identity required");
  auto p=std::make_unique<Impl>(image,options);admit_contract(image->contract());
  if(image->extent())fail("new empty backend required");
  State s;s.image=image;s.limits=limits;s.id=id;s.generation=2;s.checkpoint=1;
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
  verify(s);if(s.degraded_metadata)fail("metadata peer rebuild required before writable resume");
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
  if(!source.impl_)fail("moved-from rebuild source");admit_limits(limits);
  const auto& original=source.impl_->state;
  if(zero(id) || id==original.id)fail("rebuild requires a new nonzero identity");
  Impl::Lease source_lease(original.image);auto observed=load(original.image,original.limits);
  if(!same(original,observed))fail("rebuild source is not the current checkpoint");
  const auto source_extent=original.image->extent();
  auto p=std::make_unique<Impl>(target,options);admit_contract(target->contract());
  if(target->extent())fail("rebuild requires an empty target");
  Rebuild build;build.next.image=target;build.next.limits=limits;build.next.id=id;
  build.next.generation=add(original.generation,2);build.next.checkpoint=original.generation+1;
  build.next.unit=(std::max)(static_cast<std::uint32_t>(page_bytes),target->contract().write_failure_unit_bytes);
  target->admit_write(start+2*build.next.unit);zeros(*target,0,start);
  auto visitor=[&](const Entry& source_entry) {
    auto bytes=read_object(original,source_entry);auto encoded=p->encoder.encode(bytes);
    Entry entry{source_entry.id,source_entry.generation,build.cursor,static_cast<std::uint32_t>(bytes.size()),
                static_cast<std::uint32_t>(encoded.payload.size()),{}};
    std::array<std::byte,128> header{};set_magic(header,data_magic);put32(header,8,1);put32(header,12,encoded.codec);
    put64(header,16,entry.id);put64(header,24,entry.generation);put32(header,32,entry.logical);
    put32(header,36,entry.stored);copy(header,40,id);
    if(encoded.codec!=detail::kStoreRaw)copy(header,56,encoded.decoded_digest);
    entry.digest=detail::record_digest(std::span<const std::byte>(header).first(96),encoded.payload);copy(header,96,entry.digest);
    const auto end=add(build.cursor,128ULL+entry.stored);
    if(end>limits.maximum_physical_bytes)capacity("rebuild capacity");
    target->admit_write(end);target->write_exact(build.cursor,header);
    if(!encoded.payload.empty())target->write_exact(build.cursor+128,encoded.payload);
    build.cursor=end;
    if(read_object(build.next,entry)!=bytes)fail("rebuild CPU comparison");
    build.entry(entry);return true;
  };
  bool done=false;(void)read_node(original,original.root);walk(original,original.root,0,visitor,done);build.finish();
  if(build.next.root.count!=original.root.count || build.next.root.logical!=original.root.logical)
    fail("rebuild inventory mismatch");
  target->flush();verify(build.next);
  if(!same(original,load(original.image,original.limits)) || original.image->extent()!=source_extent)
    fail("rebuild source changed before publication");
  publish_roots(build.next);p->state=std::move(build.next);p->append_floor=p->state.end;
  return PagedObjectStoreWriter(std::move(p));
}
PagedStoreInfo PagedObjectStoreWriter::commit(std::span<const StoreMutation> mutations) {
  return commit_impl(mutations,kStoreMaxObjects);
}
PagedStoreInfo PagedObjectStoreWriter::commit_native(std::span<const StoreMutation> mutations) {
  return commit_impl(mutations,2*kStoreMaxObjects);
}
PagedStoreInfo PagedObjectStoreWriter::commit_impl(std::span<const StoreMutation> mutations,std::size_t maximum_operations) {
  if(!impl_ || impl_->poisoned)fail("writer unavailable/poisoned");
  auto& p=*impl_;const auto& old=p.state;
  if(mutations.empty() || mutations.size()>maximum_operations)fail("transaction operation budget");
  if(old.generation>(std::numeric_limits<std::uint64_t>::max)()-2)fail("generation exhausted");
  std::vector<const StoreMutation*> sorted;sorted.reserve(mutations.size());std::uint64_t input=0;
  for(const auto& m:mutations) {
    if(!m.id || m.bytes.size()>kStoreMaxObjectBytes || m.lz4_candidate.size()>kStoreMaxObjectBytes ||
        (m.erase && (!m.bytes.empty() || !m.lz4_candidate.empty())))fail("mutation fields");
    input=add(input,m.bytes.size());if(input>kStoreMaxTransactionBytes)fail("transaction input budget");sorted.push_back(&m);
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
    record.payload=std::move(encoded.payload);cursor=add(cursor,128ULL+e.stored);
    if(cursor>old.limits.maximum_physical_bytes)fail("physical capacity");
    changes.push_back({m->id,false,e});objects.push_back(std::move(record));
  }
  plan.page_start=align(cursor,old.unit);
  std::vector<Ref> roots;
  try{roots=plan.rewrite(old,old.root,changes);}
  catch(const CapacityError&){throw;}
  catch(const std::bad_alloc&){throw;}
  catch(...){p.poisoned=true;throw;}
  if(old.degraded_metadata) {p.poisoned=true;fail("metadata replica lost during staging");}
  plan.finish(std::move(roots));old.image->admit_write(plan.next.end);
  try {
    for(const auto& record:objects) {
      old.image->write_exact(record.entry.offset,record.header);
      if(!record.payload.empty())old.image->write_exact(record.entry.offset+128,record.payload);
    }
    plan.write_pages(cursor);publish(plan.next);
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
