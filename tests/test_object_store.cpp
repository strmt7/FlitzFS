#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include "store_codec.hpp"
#include "blake3_vectors.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool ok,const char* text) {++checks;if(!ok) throw std::runtime_error(text);}
template<class F> void rejects(F&& f,const char* text) {
  ++checks;try {f();}catch(const std::exception&){return;}throw std::runtime_error(text);
}
Bytes bytes(std::size_t n,unsigned seed) {
  Bytes out(n);for(std::size_t i=0;i<n;++i) out[i]=static_cast<std::byte>((i*37+seed*19)%251);return out;
}
StoreIdentity identity() {StoreIdentity id{};id[0]=std::byte{17};return id;}
class Ram final:public StoreByteIo {
 public:
  Bytes live,stable;
  StoreIoContract declared{65536,true};
  std::size_t calls=0,writes=0,fail_call=(std::numeric_limits<std::size_t>::max)();
  unsigned failure=0;
  bool pressure=false;
  std::function<void(std::uint64_t,std::span<const std::byte>)> before_write;
  std::uint64_t extent() const override {return live.size();}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(at>live.size() || out.size()>live.size()-at) throw std::runtime_error("RAM read bounds");
    std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),out.size(),out.begin());
  }
  void copy(Bytes& target,std::uint64_t at,std::span<const std::byte> input) {
    if(at>kStoreMaxPhysicalBytes || input.size()>kStoreMaxPhysicalBytes-at) throw std::runtime_error("RAM output cap");
    if(target.size()<at+input.size()) target.resize(static_cast<std::size_t>(at+input.size()));
    std::copy(input.begin(),input.end(),target.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> input) override {
    const bool broken=calls++==fail_call;++writes;
    if(before_write) before_write(at,input);
    if(broken) {
      if(failure==1) {
        const auto prefix=input.first(input.size()/2);copy(live,at,prefix);copy(stable,at,prefix);
      }
      if(failure==2) {
        const auto unit=(at/kStoreCommitUnit)*kStoreCommitUnit;
        if(stable.size()<unit+kStoreCommitUnit) stable.resize(static_cast<std::size_t>(unit+kStoreCommitUnit));
        std::fill_n(stable.begin()+static_cast<std::ptrdiff_t>(unit),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
        live=stable;
      }
      throw std::runtime_error("injected write failure");
    }
    copy(live,at,input);
  }
  void flush() override {
    const bool broken=calls++==fail_call;
    if(broken && failure==0) throw std::runtime_error("injected failed flush");
    stable=live;
    if(broken) throw std::runtime_error("injected failure after flush publication");
  }
  void admit_write(std::uint64_t projected) override {
    if(pressure || projected>kStoreMaxPhysicalBytes) throw std::runtime_error("RAM admission");
  }
  StoreIoContract contract() const noexcept override {return declared;}
};
std::shared_ptr<Ram> image(Bytes data) {auto p=std::make_shared<Ram>();p->live=std::move(data);return p;}
std::array<StoreMutation,2> initial() {return {StoreMutation{1,false,bytes(97,1)},StoreMutation{2,false,bytes(33,2)}};}
std::array<StoreMutation,3> updated() {return {StoreMutation{1,false,bytes(128,3)},StoreMutation{2,true,{}},StoreMutation{3,false,bytes(257,4)}};}
void check_state(const ObjectStoreSnapshot& s) {
  const auto generation=s.info().generation;
  require(generation==1 || generation==2,"recovery lost acknowledged generation or fabricated future");
  require(s.list().size()==2,"partial namespace transaction");
  if(generation==1) {
    require(s.read(1)==bytes(97,1) && s.read(2)==bytes(33,2),"old acknowledged bytes changed");
    rejects([&]{(void)s.read(3);},"new object leaked into old transaction");
  } else {
    require(s.read(1)==bytes(128,3) && s.read(3)==bytes(257,4),"new transaction bytes mismatched");
    rejects([&]{(void)s.read(2);},"deleted object leaked into new transaction");
  }
}
void known_vectors() {
  for(const auto& test:digest_vectors::cases) {
    Bytes input(test.bytes);for(std::size_t i=0;i<input.size();++i) input[i]=static_cast<std::byte>(i%251);
    detail::Digest expected{};
    auto digit=[](char c)->unsigned {return c>='0' && c<='9' ? unsigned(c-'0') : unsigned(c-'a'+10);};
    for(std::size_t i=0;i<expected.size();++i)
      expected[i]=static_cast<std::byte>((digit(test.hex[2*i])<<4)|digit(test.hex[2*i+1]));
    require(detail::record_digest(input)==expected,"official BLAKE3-256 vector mismatch");
    for(const auto split:{std::size_t{0},std::size_t{1},std::size_t{63},std::size_t{64},std::size_t{1023},input.size()/2,input.size()}) {
      if(split>input.size()) continue;
      const auto all=std::span<const std::byte>(input);
      require(detail::record_digest(all.first(split),all.subspan(split))==expected,"split record digest mismatch");
    }
  }
}
void basic_and_admission() {
  auto ram=std::make_shared<Ram>();
  rejects([&]{(void)ObjectStoreWriter::create(ram,{});},"zero store identity accepted");
  for(auto bad:{StoreIoContract{},StoreIoContract{512,false},StoreIoContract{3,true},StoreIoContract{131072,true}}) {
    ram->declared=bad;
    rejects([&]{(void)ObjectStoreWriter::create(ram,identity());},"unknown/unsupported durability contract accepted");
    require(ram->writes==0,"invalid contract wrote backing bytes");
  }
  ram->declared={4096,true};
  auto writer=ObjectStoreWriter::create(ram,identity());
  require(writer.snapshot().list().empty(),"new store not empty");
  rejects([&]{(void)ObjectStoreWriter::create(ram,identity());},"existing image reopened writable");
  const auto first=initial();require(writer.commit(first).generation==1,"first commit generation");
  auto old=writer.snapshot();
  require(writer.commit(updated()).generation==2,"second commit generation");
  check_state(writer.snapshot());check_state(ObjectStoreSnapshot::open(image(ram->stable)));
  require(old.info().generation==1 && old.read(2)==bytes(33,2),"immutable snapshot changed after erase");
  auto moved=std::move(old);require(moved.read(1)==bytes(97,1),"snapshot move lost backing");
  rejects([&]{(void)old.read(1);},"moved-from snapshot operational read");
  const auto before=ram->live;const auto calls=ram->calls;
  ram->pressure=true;
  rejects([&]{(void)writer.commit(initial());},"resource admission ignored");
  require(!writer.poisoned() && ram->calls==calls,"resource pressure changed backing or poisoned writer");
  ram->pressure=false;
  std::vector<StoreMutation> bad{{0,false,{}}};
  rejects([&]{(void)writer.commit(bad);},"zero object ID");
  bad={{4,false,{}},{4,false,{}}};rejects([&]{(void)writer.commit(bad);},"duplicate mutation ID");
  bad={{99,true,{}}};rejects([&]{(void)writer.commit(bad);},"erase missing object");
  bad={{1,true,bytes(1,1)}};rejects([&]{(void)writer.commit(bad);},"erase with payload");
  bad={{1,false,Bytes(kStoreMaxObjectBytes+1)}};rejects([&]{(void)writer.commit(bad);},"oversized object");
  bad.clear();for(unsigned i=0;i<5;++i) bad.push_back({10+i,false,Bytes(kStoreMaxObjectBytes)});
  rejects([&]{(void)writer.commit(bad);},"oversized transaction");
  rejects([&]{(void)writer.commit({});},"empty mutation batch");
  require(ram->live==before && ram->calls==calls && !writer.poisoned(),"argument rejection changed durable state");
  std::array<StoreMutation,1> empty{{{(std::numeric_limits<std::uint64_t>::max)(),false,{}}}};
  writer.commit(empty);require(writer.snapshot().read(empty[0].id).empty(),"empty/high-ID object");
  auto another=std::make_shared<Ram>();auto capacity=ObjectStoreWriter::create(another,identity());
  std::vector<StoreMutation> many;for(unsigned i=1;i<=16;++i) many.push_back({i,false,bytes(4,i)});
  capacity.commit(many);const auto baseline=another->live;
  const StoreMutation extra{17,false,{}};
  rejects([&]{(void)capacity.commit({&extra,1});},"namespace cap");
  require(another->live==baseline && !capacity.poisoned(),"namespace admission wrote bytes");
  const std::array<StoreMutation,2> replacement{{{17,false,{}},{1,true,{}}}};
  capacity.commit(replacement);require(capacity.snapshot().list().size()==16,"atomic replace under namespace cap");
  auto owned=std::make_shared<Ram>();auto w=ObjectStoreWriter::create(owned,identity());
  std::vector<StoreMutation> changes{{7,false,bytes(71,8)}};
  const auto expected=changes[0].bytes;bool once=false;
  owned->before_write=[&](auto,auto) {if(!once){once=true;changes[0].id=999;changes[0].bytes=bytes(2,1);}};
  w.commit(changes);owned->before_write={};
  require(w.snapshot().read(7)==expected,"caller mutation changed privately owned transaction");
}
void interrupted_io() {
  auto probe=std::make_shared<Ram>();auto w=ObjectStoreWriter::create(probe,identity());w.commit(initial());
  probe->calls=0;w.commit(updated());const auto operations=probe->calls;
  for(unsigned mode=0;mode<3;++mode) for(std::size_t step=0;step<operations;++step) {
    auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity());writer.commit(initial());
    ram->calls=0;ram->fail_call=step;ram->failure=mode;
    rejects([&]{(void)writer.commit(updated());},"injected operation did not fail");
    require(writer.poisoned(),"I/O error did not poison writer");
    ram->fail_call=(std::numeric_limits<std::size_t>::max)();
    const auto ack=writer.snapshot();
    require(ack.info().generation==1 && ack.read(1)==bytes(97,1) && ack.read(2)==bytes(33,2),"last acknowledged CPU snapshot lost");
    const auto calls=ram->calls;
    rejects([&]{(void)writer.commit(updated());},"poisoned writer retried");
    require(ram->calls==calls,"poisoned writer touched backing");
    auto recovered=ObjectStoreSnapshot::open(image(ram->stable));check_state(recovered);
  }
}
void torn_roots() {
  auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity());writer.commit(initial());
  Bytes before,publication;std::uint64_t offset=0;
  ram->before_write=[&](auto at,auto input) {
    if(at<2*kStoreCommitUnit && input.size()==4096) {
      before=ram->stable;publication.assign(input.begin(),input.end());offset=at;
    }
  };
  writer.commit(updated());ram->before_write={};
  require(publication.size()==4096 && offset==0,"root-publication fixture");
  for(std::size_t cut=0;cut<=publication.size();++cut) for(bool suffix:{false,true}) {
    auto candidate=before;
    const auto begin=suffix ? cut : 0,end=suffix ? publication.size() : cut;
    std::copy(publication.begin()+static_cast<std::ptrdiff_t>(begin),publication.begin()+static_cast<std::ptrdiff_t>(end),
              candidate.begin()+static_cast<std::ptrdiff_t>(offset+begin));
    auto recovered=ObjectStoreSnapshot::open(image(std::move(candidate)));check_state(recovered);
  }
  auto destroyed=before;
  std::fill_n(destroyed.begin()+static_cast<std::ptrdiff_t>(offset),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
  auto recovery=ObjectStoreSnapshot::open(image(std::move(destroyed)));
  require(recovery.info().degraded_root && recovery.info().generation==1,"write-unit root damage lost acknowledged state");
}
std::uint64_t u64(const Bytes& b,std::size_t at) {
  std::uint64_t n=0;for(unsigned i=0;i<8;++i) n|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
void root_digest(Bytes& b,std::size_t at) {
  const auto digest=detail::record_digest(std::span<const std::byte>(b).subspan(at,4096-32));
  std::copy(digest.begin(),digest.end(),b.begin()+static_cast<std::ptrdiff_t>(at+4096-32));
}
void put32(Bytes& b,std::size_t at,std::uint32_t n) {
  for(unsigned i=0;i<4;++i) b[at+i]=static_cast<std::byte>(n>>(8*i));
}
void put64(Bytes& b,std::size_t at,std::uint64_t n) {
  for(unsigned i=0;i<8;++i) b[at+i]=static_cast<std::byte>(n>>(8*i));
}
void manifest_digest(Bytes& b,std::size_t slot) {
  const auto offset=static_cast<std::size_t>(u64(b,slot+32));
  const auto count=std::to_integer<unsigned>(b[slot+44]);
  const auto digest=detail::record_digest(std::span<const std::byte>(b).subspan(offset,64+64*count));
  std::copy(digest.begin(),digest.end(),b.begin()+static_cast<std::ptrdiff_t>(slot+64));
  root_digest(b,slot);
}
void malformed_images() {
  auto ram=std::make_shared<Ram>();auto w=ObjectStoreWriter::create(ram,identity());w.commit(initial());
  const auto baseline=ram->stable;
  auto bad=baseline;bad[65536+8]=std::byte{2};root_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"unsupported intact root silently rolled back");
  bad=baseline;bad[65536+96]=std::byte{1};root_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"intact reserved root field accepted");
  bad=baseline;bad[65536+44]=std::byte{17};root_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"oversized namespace parsed");
  const auto index=static_cast<std::size_t>(u64(baseline,65536+32));
  bad=baseline;bad[index+24]^=std::byte{1};
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"bad latest manifest silently rolled back");
  const auto data=static_cast<std::size_t>(u64(baseline,index+64+16));
  bad=baseline;bad[data+128]^=std::byte{1};
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"corrupt payload exposed or rolled back");
  // Recompute enclosing hashes: exercise structural admission rather than merely
  // detecting a checksum mismatch. These are malicious/intact unsupported images.
  bad=baseline;put64(bad,index+64,0);manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"zero manifest ID accepted");
  bad=baseline;put64(bad,index+128,1);manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"duplicate manifest IDs accepted");
  bad=baseline;put64(bad,index+64+8,2);manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"future object generation accepted");
  bad=baseline;put64(bad,index+128+16,data);manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"overlapping manifest extents accepted");
  bad=baseline;put32(bad,index+64+24,kStoreMaxObjectBytes+1);manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"oversized intact manifest entry accepted");
  bad=baseline;put32(bad,data+12,255);
  const auto record=detail::record_digest(std::span<const std::byte>(bad).subspan(data,96),
      std::span<const std::byte>(bad).subspan(data+128,97));
  std::copy(record.begin(),record.end(),bad.begin()+static_cast<std::ptrdiff_t>(data+96));
  std::copy(record.begin(),record.end(),bad.begin()+static_cast<std::ptrdiff_t>(index+64+32));
  manifest_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"unknown intact data codec accepted");
  for(const auto length:{std::size_t{0},std::size_t{4095},baseline.size()-65536}) {
    bad=baseline;bad.resize(length);
    rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"truncated image accepted");
  }
}
Bytes noise(std::size_t n) {
  Bytes out(n);std::uint64_t x=0x13198a2e03707344ULL;
  for(auto& b:out){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>23);}
  return out;
}
Bytes structured(std::size_t n,unsigned variant) {
  auto out=noise(n);
  for(std::size_t i=16384;i<n;++i)out[i]=out[i%16384];
  if(n)out.back()^=static_cast<std::byte>(variant);return out;
}
std::array<StoreMutation,3> compressible(unsigned generation) {
  return {StoreMutation{1,false,Bytes(65537,static_cast<std::byte>(generation==1?0:51))},
          StoreMutation{2,false,bytes(131073,generation)},
          StoreMutation{3,false,structured(131073,generation)}};
}
void compressed_state(const ObjectStoreSnapshot& s) {
  const auto generation=s.info().generation;
  require(generation==1 || generation==2,"compressed recovery lost acknowledgement or fabricated generation");
  const auto expected=compressible(static_cast<unsigned>(generation));
  require(s.list().size()==expected.size(),"compressed partial transaction namespace");
  for(const auto& m:expected)require(s.read(m.id)==m.bytes,"compressed transaction byte mismatch");
  require(s.info().stored_payload_bytes<=s.info().logical_bytes,"active payload grew after compression");
}
void compressed_transactions() {
  const std::array<StoreCompressionOptions,2> policies{{
    {StoreCompressionMode::raw,6,false},{StoreCompressionMode::automatic,6,false}}};
  for(const auto policy:policies) {
    auto probe=std::make_shared<Ram>();auto w=ObjectStoreWriter::create(probe,identity(),policy);
    w.commit(compressible(1));auto first=w.snapshot();probe->calls=0;
    w.commit(compressible(2));const auto steps=probe->calls;
    compressed_state(first);require(first.info().generation==1,"compressed old snapshot changed");
    compressed_state(ObjectStoreSnapshot::open(image(probe->stable)));
    if(policy.mode==StoreCompressionMode::automatic)
      require(w.snapshot().info().stored_payload_bytes<w.snapshot().info().logical_bytes,"automatic compression did not reduce payload");
    for(unsigned mode=0;mode<3;++mode)for(std::size_t step=0;step<steps;++step) {
      auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity(),policy);
      writer.commit(compressible(1));ram->calls=0;ram->fail_call=step;ram->failure=mode;
      rejects([&]{writer.commit(compressible(2));},"compressed injected I/O did not fail");
      require(writer.poisoned(),"compressed failure left writer usable");
      const auto ack=writer.snapshot();require(ack.info().generation==1 && ack.info().degraded_root,"poisoned compressed snapshot not fenced");
      compressed_state(ack);compressed_state(ObjectStoreSnapshot::open(image(ram->stable)));
    }
  }
  auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity());
  const StoreMutation motif{1,false,Bytes(65537,std::byte{91})};
  writer.commit({&motif,1});const auto baseline=ram->stable;
  const auto index=static_cast<std::size_t>(u64(baseline,65536+32));
  const auto record=static_cast<std::size_t>(u64(baseline,index+64+16));
  require(std::to_integer<unsigned>(baseline[record+12])==detail::kStorePattern,"decoded-integrity fixture not a pattern");
  auto repair_record=[&](Bytes& bad) {
    const auto count=static_cast<std::size_t>(u64(bad,index+64+28)&0xffffffffU)-128;
    const auto digest=detail::record_digest(std::span<const std::byte>(bad).subspan(record,96),
                                           std::span<const std::byte>(bad).subspan(record+128,count));
    std::copy(digest.begin(),digest.end(),bad.begin()+static_cast<std::ptrdiff_t>(record+96));
    std::copy(digest.begin(),digest.end(),bad.begin()+static_cast<std::ptrdiff_t>(index+64+32));
    manifest_digest(bad,65536);
  };
  auto bad=baseline;bad[record+56]^=std::byte{1};repair_record(bad);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"invalid decoded digest escaped recomputed stored chain");
  bad=baseline;bad[record+128]^=std::byte{1};repair_record(bad);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"wrong decoded bytes escaped intact stored chain");
  bad=baseline;put32(bad,record+12,detail::kStoreZero);repair_record(bad);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"zero codec with payload admitted");
  bad=baseline;bad[record+88]=std::byte{1};repair_record(bad);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"compressed reserved fields admitted");
  auto empty=std::make_shared<Ram>();
  for(auto options:{StoreCompressionOptions{StoreCompressionMode::automatic,0,false},
                    StoreCompressionOptions{StoreCompressionMode::raw,6,true}}) {
    rejects([&]{(void)ObjectStoreWriter::create(empty,identity(),options);},"invalid codec options created store");
    require(empty->writes==0,"invalid codec policy wrote initialization");
  }
}
// Sparse, immediately persistent RAM backend reaches the physical-history cap
// without allocating 64 MiB or touching real storage.
class Sparse final:public StoreByteIo {
 public:
  std::map<std::uint64_t,Bytes> pieces;std::uint64_t end=0;unsigned writes=0;
  std::uint64_t extent() const override {return end;}
  StoreIoContract contract() const noexcept override {return {65536,true};}
  void flush() override {}
  void admit_write(std::uint64_t projected) override {if(projected>kStoreMaxPhysicalBytes) throw std::runtime_error("sparse admission");}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    ++writes;if(at>kStoreMaxPhysicalBytes || in.size()>kStoreMaxPhysicalBytes-at) throw std::runtime_error("sparse cap");
    end=(std::max)(end,at+in.size());
    if(std::any_of(in.begin(),in.end(),[](auto b){return b!=std::byte{0};})) pieces[at]=Bytes(in.begin(),in.end());
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(at>end || out.size()>end-at) throw std::runtime_error("sparse read");
    std::fill(out.begin(),out.end(),std::byte{0});
    for(const auto& [offset,data]:pieces) {
      const auto begin=(std::max)(at,offset),limit=(std::min)(at+out.size(),offset+data.size());
      if(begin<limit) std::copy(data.begin()+static_cast<std::ptrdiff_t>(begin-offset),
          data.begin()+static_cast<std::ptrdiff_t>(limit-offset),out.begin()+static_cast<std::ptrdiff_t>(begin-at));
    }
  }
};
void history_cap() {
  auto ram=std::make_shared<Sparse>();auto writer=ObjectStoreWriter::create(ram,identity());
  const StoreMutation empty{1,false,{}};
  while(writer.snapshot().info().physical_end<kStoreMaxPhysicalBytes) writer.commit({&empty,1});
  const auto count=ram->writes;
  rejects([&]{(void)writer.commit({&empty,1});},"history cap exceeded");
  require(!writer.poisoned() && count==ram->writes,"history cap did not reject before I/O");
  require(ObjectStoreSnapshot::open(ram).read(1).empty(),"history-cap recovery failed");
}
}
int main() {
  try {
    known_vectors();basic_and_admission();interrupted_io();torn_roots();malformed_images();compressed_transactions();history_cap();
    std::cout<<"PASS: "<<checks<<" object-store checks, official digest vectors, I/O failures, torn roots and bounded sparse history; RAM only.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
