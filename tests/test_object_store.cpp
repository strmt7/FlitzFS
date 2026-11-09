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
#include <optional>
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
  std::function<void()> before_flush;
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
    if(before_flush)before_flush();
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
  std::vector<StoreMutation> many;
  for(std::size_t i=1;i<=kStoreMaxObjects;++i)many.push_back({i,false,bytes(4,static_cast<unsigned>(i))});
  capacity.commit(many);const auto baseline=another->live;const auto capacity_calls=another->calls;
  const StoreMutation extra{kStoreMaxObjects+1,false,{}};
  rejects([&]{(void)capacity.commit({&extra,1});},"namespace cap");
  require(another->live==baseline && another->calls==capacity_calls && !capacity.poisoned(),"namespace admission wrote bytes");
  const std::array<StoreMutation,2> replacement{{{kStoreMaxObjects+1,false,{}},{1,true,{}}}};
  capacity.commit(replacement);
  require(capacity.snapshot().list().size()==kStoreMaxObjects,"atomic replace under namespace cap");
  require(ObjectStoreSnapshot::open(image(another->stable)).read(kStoreMaxObjects+1).empty(),"full-count reopen failed");
  std::vector<StoreMutation> too_many(kStoreMaxObjects+1);
  for(std::size_t i=0;i<too_many.size();++i)too_many[i].id=1000+i;
  const auto full=another->live;
  rejects([&]{(void)capacity.commit(too_many);},"oversized mutation count");
  require(full==another->live && !capacity.poisoned(),"mutation-count rejection wrote bytes");
  std::vector<StoreMutation> erase_all;
  for(const auto& object:capacity.snapshot().list())erase_all.push_back({object.id,true,{}});
  capacity.commit(erase_all);require(capacity.snapshot().list().empty(),"full-count atomic erase failed");
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
  std::uint32_t count=0;for(unsigned i=0;i<4;++i)count|=std::uint32_t(std::to_integer<unsigned char>(b[slot+44+i]))<<(8*i);
  const auto digest=detail::record_digest(std::span<const std::byte>(b).subspan(offset,64+64*count));
  std::copy(digest.begin(),digest.end(),b.begin()+static_cast<std::ptrdiff_t>(slot+64));
  root_digest(b,slot);
}

void decoded_live_budget() {
  // Compressible data must still consume the decoded budget. This invariant
  // keeps aggregate allocation independent from the live object-count ceiling.
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity(),{mode,6,false});
    for(std::uint64_t first=1;first<=16;first+=4) {
      std::vector<StoreMutation> batch;
      for(std::uint64_t i=0;i<4;++i)batch.push_back({first+i,false,Bytes(kStoreMaxObjectBytes,std::byte{91})});
      writer.commit(batch);
    }
    require(writer.snapshot().info().logical_bytes==kStoreMaxLogicalBytes,"decoded live budget not reachable");
    const auto baseline=ram->live;const auto calls=ram->calls;
    StoreMutation extra{100,false,{std::byte{1}}};
    rejects([&]{writer.commit({&extra,1});},"decoded budget admitted overfull put");
    require(ram->live==baseline && ram->calls==calls && !writer.poisoned(),"decoded budget wrote or poisoned");
    // Final-inventory admission permits a put followed by an erase at capacity.
    const std::array<StoreMutation,2> replace{{{100,false,Bytes(kStoreMaxObjectBytes,std::byte{7})},{1,true,{}}}};
    writer.commit(replace);
    require(writer.snapshot().info().logical_bytes==kStoreMaxLogicalBytes,"decoded-cap replacement failed");
    auto snapshot=ObjectStoreSnapshot::open(image(ram->stable));
    require(snapshot.read(100)==Bytes(kStoreMaxObjectBytes,std::byte{7}),"decoded-cap readback");
    // Create a digest-consistent oversized declared inventory. The parser
    // must reject before allocating/reading the fabricated object payload.
    auto bad=ram->stable;const auto slot=static_cast<std::size_t>((snapshot.info().generation%2)*kStoreCommitUnit);
    const auto index=static_cast<std::size_t>(u64(bad,slot+32));const auto old_count=snapshot.list().size();
    const auto old_size=64+64*old_count;
    bad.insert(bad.begin()+static_cast<std::ptrdiff_t>(index+old_size),64,std::byte{0});
    // Inserting changes neither earlier record offsets nor the manifest start;
    // its padding and physical size are rebuilt explicitly for this candidate.
    bad.resize(static_cast<std::size_t>(snapshot.info().physical_end));
    put32(bad,slot+40,static_cast<std::uint32_t>(old_size+64));
    put32(bad,slot+44,static_cast<std::uint32_t>(old_count+1));
    put32(bad,index+12,static_cast<std::uint32_t>(old_count+1));
    put64(bad,index+24,kStoreMaxLogicalBytes+1);
    const auto entry=index+old_size;
    put64(bad,entry,101);put64(bad,entry+8,snapshot.info().generation);
    // Use a distinct one-byte range in pre-manifest padding. Data identity is
    // intentionally unavailable: aggregate admission must precede payload I/O.
    put64(bad,entry+16,2*kStoreCommitUnit);put32(bad,entry+24,1);put32(bad,entry+28,129);
    manifest_digest(bad,slot);
    rejects([&]{(void)ObjectStoreSnapshot::open(image(std::move(bad)));},"oversized decoded manifest accepted");
  }
}
void physical_record_order() {
  auto ram=std::make_shared<Ram>();auto writer=ObjectStoreWriter::create(ram,identity());
  const std::array<StoreMutation,3> scrambled{{{300,false,bytes(7,1)},{1,false,bytes(11,2)},{200,false,bytes(3,3)}}};
  writer.commit(scrambled);auto snap=ObjectStoreSnapshot::open(image(ram->stable));
  require(snap.list()[0].id==1 && snap.read(300)==bytes(7,1),"physical ordering confused logical IDs");
  const StoreMutation update{1,false,bytes(19,4)};writer.commit({&update,1});
  require(ObjectStoreSnapshot::open(image(ram->stable)).read(1)==bytes(19,4),"new physical order rejected");
}

void malformed_images() {
  auto ram=std::make_shared<Ram>();auto w=ObjectStoreWriter::create(ram,identity());w.commit(initial());
  const auto baseline=ram->stable;
  auto bad=baseline;bad[65536+8]=std::byte{2};root_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"unsupported intact root silently rolled back");
  bad=baseline;bad[65536+96]=std::byte{1};root_digest(bad,65536);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"intact reserved root field accepted");
  bad=baseline;put32(bad,65536+44,static_cast<std::uint32_t>(kStoreMaxObjects+1));root_digest(bad,65536);
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
void old_bytes(const ObjectStoreSnapshot& s) {
  require(s.list().size()==2 && s.read(1)==bytes(97,1) && s.read(2)==bytes(33,2),"recovery changed acknowledged payloads");
}
void resume_leases_and_orphans() {
  auto ram=std::make_shared<Ram>();
  std::optional<ObjectStoreSnapshot> old;
  {
    auto writer=ObjectStoreWriter::create(ram,identity());writer.commit(initial());old.emplace(writer.snapshot());
    const auto calls=ram->calls;
    rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),1);},"two writers acquired one backend");
    require(ram->calls==calls,"failed writer lease touched I/O");
    auto moved=std::move(writer);
    rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),1);},"move released active writer lease");
    old_bytes(moved.snapshot());
  }
  const auto baseline=ram->live;const auto calls=ram->calls;
  auto wrong=identity();wrong[1]=std::byte{19};
  rejects([&]{(void)ObjectStoreWriter::resume(ram,wrong,1);},"wrong store identity resumed");
  rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),0);},"stale generation resumed");
  ram->pressure=true;
  rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),1);},"resource-starved resume admitted");
  ram->pressure=false;
  require(ram->calls==calls && ram->live==baseline,"rejected resume modified backing");
  // Preserve an unaligned uncertain tail; never append into its failure unit.
  ram->copy(ram->live,ram->live.size(),Bytes(1234,std::byte{0xa5}));ram->stable=ram->live;
  const auto orphaned=ram->live;
  const auto floor=(orphaned.size()+65535)/65536*65536;
  {
    auto writer=ObjectStoreWriter::resume(ram,identity(),1);
    require(writer.snapshot().info().generation==1,"clean resume advanced generation");
    ram->before_write=[&](auto offset,auto){if(offset>=2*kStoreCommitUnit)require(offset>=floor,"resumed writer reused orphan failure unit");};
    require(writer.commit(updated()).generation==2,"resumed commit failed");
    ram->before_write={};check_state(writer.snapshot());
    old_bytes(*old);
  }
  require(std::equal(orphaned.begin()+2*kStoreCommitUnit,orphaned.end(),ram->live.begin()+2*kStoreCommitUnit),
          "resumed commit overwrote historical/orphan bytes");
  check_state(ObjectStoreSnapshot::open(image(ram->stable)));
  {
    auto unexpected=image(ram->stable);unexpected->stable=unexpected->live;
    const auto writes=unexpected->writes;
    unexpected->before_flush=[&]{unexpected->copy(unexpected->live,unexpected->live.size(),Bytes(1,std::byte{1}));};
    rejects([&]{(void)ObjectStoreWriter::resume(unexpected,identity(),2);},"extent changed during recovery fence");
    require(unexpected->writes==writes,"unstable recovery fence performed repair writes");
    unexpected->before_flush={};
    auto retry=ObjectStoreWriter::resume(unexpected,identity(),2);
    check_state(retry.snapshot());
  }
  // A poisoned writer remains the owner until its lifetime ends.
  {
    auto writer=ObjectStoreWriter::resume(ram,identity(),2);
    ram->calls=0;ram->fail_call=0;
    rejects([&]{writer.commit(initial());},"writer fault fixture");
    const auto n=ram->calls;
    rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),2);},"poisoned writer lease bypassed");
    require(ram->calls==n,"lease bypass attempted backing I/O");
    ram->fail_call=(std::numeric_limits<std::size_t>::max)();
  }
  auto recovered=ObjectStoreSnapshot::open(image(ram->stable));
  auto writer=ObjectStoreWriter::resume(ram,identity(),recovered.info().generation);
  require(!writer.poisoned(),"destroyed writer left permanent ownership");
}
void resume_complete_unacknowledged_candidate() {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto writer=ObjectStoreWriter::create(probe,identity());writer.commit(initial());
    probe->calls=0;writer.commit(updated());steps=probe->calls;}
  auto ram=std::make_shared<Ram>();
  {
    auto writer=ObjectStoreWriter::create(ram,identity());writer.commit(initial());
    ram->calls=0;ram->fail_call=steps-1;ram->failure=0;
    rejects([&]{writer.commit(updated());},"final flush fault not observed");
    require(writer.poisoned(),"final flush failure did not retain terminal writer");
  }
  ram->fail_call=(std::numeric_limits<std::size_t>::max)();
  require(ObjectStoreSnapshot::open(image(ram->stable)).info().generation==1,"failed final flush fixture already durable");
  require(ObjectStoreSnapshot::open(ram).info().generation==2,"complete candidate not visible before recovery fence");
  auto writer=ObjectStoreWriter::resume(ram,identity(),2);
  require(ObjectStoreSnapshot::open(image(ram->stable)).info().generation==2,"resume returned before fencing complete candidate");
  check_state(writer.snapshot());
}
Bytes damaged_root_fixture(unsigned generation,unsigned lost_slot) {
  auto ram=std::make_shared<Ram>();
  {auto writer=ObjectStoreWriter::create(ram,identity());if(generation)writer.commit(initial());}
  auto bytes=ram->stable;
  std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(lost_slot*kStoreCommitUnit),
              static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
  return bytes;
}
void repaired_contents(const ObjectStoreSnapshot& snapshot,unsigned original) {
  require(!original?(snapshot.info().generation==0):(snapshot.info().generation==1 || snapshot.info().generation==2),
          "root repair fabricated or rolled back generation");
  if(original)old_bytes(snapshot);else require(snapshot.list().empty(),"initial root repair fabricated objects");
}
void resume_root_repair() {
  for(const auto fixture:std::array<std::pair<unsigned,unsigned>,3>{{{0,0},{0,1},{1,0}}}) {
    const auto original=fixture.first,lost=fixture.second;
    const auto data=damaged_root_fixture(original,lost);
    auto probe=image(data);std::size_t operations=0;
    {
      auto writer=ObjectStoreWriter::resume(probe,identity(),original);
      require(!writer.snapshot().info().degraded_root,"resume returned degraded writable roots");
      repaired_contents(writer.snapshot(),original);operations=probe->calls;
    }
    for(unsigned mode=0;mode<3;++mode)for(std::size_t step=0;step<operations;++step) {
      auto ram=image(data);ram->stable=data;ram->failure=mode;ram->fail_call=step;
      rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),original);},"injected root-repair operation did not fail");
      ram->fail_call=(std::numeric_limits<std::size_t>::max)();
      const auto snapshot=ObjectStoreSnapshot::open(image(ram->stable));
      repaired_contents(snapshot,original);
      ram->live=ram->stable; // Model process/power restart before retrying the recovered state.
      auto retry=ObjectStoreWriter::resume(ram,identity(),snapshot.info().generation);
      require(!retry.poisoned() && !retry.snapshot().info().degraded_root,"root repair failure leaked writer lease or broke retry");
    }
  }
  auto ram=std::make_shared<Ram>();
  {auto w=ObjectStoreWriter::create(ram,identity());w.commit(initial());}
  auto valid=ram->live;
  const auto index=static_cast<std::size_t>(u64(valid,65536+32));
  const auto record=static_cast<std::size_t>(u64(valid,index+64+16));
  ram->live[record+128]^=std::byte{1};const auto before=ram->live;const auto calls=ram->calls;
  rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),1);},"corrupt latest dependency resumed");
  require(before==ram->live && ram->calls==calls,"corrupt resume flushed or changed source");
  ram->live=valid;ram->stable=valid;
  auto good=ObjectStoreWriter::resume(ram,identity(),1);old_bytes(good.snapshot());
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

StoreIdentity copied_identity(){auto id=identity();id[1]=std::byte{92};return id;}
void equal_objects(const ObjectStoreSnapshot& source,const ObjectStoreSnapshot& target) {
  const auto a=source.list(),b=target.list();
  require(a.size()==b.size() && source.info().logical_bytes==target.info().logical_bytes,"compaction inventory changed");
  require(source.info().stored_payload_bytes==target.info().stored_payload_bytes,"compaction recompressed preserved payloads");
  for(std::size_t i=0;i<a.size();++i) {
    require(a[i].id==b[i].id && a[i].generation==b[i].generation &&
        a[i].logical_bytes==b[i].logical_bytes && a[i].stored_payload_bytes==b[i].stored_payload_bytes,"compaction object metadata changed");
    require(source.read(a[i].id)==target.read(a[i].id),"compaction logical byte mismatch");
  }
}
bool valid_locator(const Bytes& bytes,std::size_t slot) {
  const auto at=slot*static_cast<std::size_t>(kStoreCommitUnit);
  if(at>bytes.size() || bytes.size()-at<4096)return false;
  const auto digest=detail::record_digest(std::span<const std::byte>(bytes).subspan(at,4064));
  return std::equal(digest.begin(),digest.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+4064));
}
void compacted_copies() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();std::optional<ObjectStoreSnapshot> source,old;
    const StoreCompressionOptions options{mode,6,false};
    {
      auto writer=ObjectStoreWriter::create(ram,identity(),options);writer.commit(initial());old.emplace(writer.snapshot());
      const StoreMutation motif{9,false,Bytes(131073,std::byte{42})};writer.commit({&motif,1});
      for(unsigned i=0;i<8;++i){const StoreMutation m{1,false,bytes(128,i+20)};writer.commit({&m,1});}
      source.emplace(writer.snapshot());auto rejected=std::make_shared<Ram>();
      rejects([&]{(void)ObjectStoreWriter::compact(*source,rejected,copied_identity(),options);},"active writer copied");
      require(rejected->writes==0,"active source rejection wrote target");
    }
    const auto original=ram->live;const auto source_calls=ram->calls;
    auto target=std::make_shared<Ram>();
    rejects([&]{(void)ObjectStoreWriter::compact(*source,target,identity(),options);},"same compaction identity accepted");
    rejects([&]{(void)ObjectStoreWriter::compact(*source,target,{},options);},"zero compaction identity accepted");
    rejects([&]{(void)ObjectStoreWriter::compact(*old,target,copied_identity(),options);},"stale snapshot compacted");
    target->declared={};
    rejects([&]{(void)ObjectStoreWriter::compact(*source,target,copied_identity(),options);},"unknown target contract copied");
    target->declared={65536,true};target->pressure=true;
    rejects([&]{(void)ObjectStoreWriter::compact(*source,target,copied_identity(),options);},"compaction resource admission ignored");
    target->pressure=false;require(target->writes==0 && target->live.empty(),"compaction argument rejection wrote target");
    bool checked=false;
    target->before_write=[&](auto,auto){
      if(checked)return;checked=true;
      rejects([&]{(void)ObjectStoreWriter::resume(ram,identity(),source->info().generation,options);},"source compaction lease bypassed");
      rejects([&]{(void)ObjectStoreWriter::create(target,copied_identity(),options);},"target compaction lease bypassed");
    };
    {
      auto copied=ObjectStoreWriter::compact(*source,target,copied_identity(),options);target->before_write={};
      auto snapshot=copied.snapshot();equal_objects(*source,snapshot);
      require(checked && snapshot.identity()==copied_identity() && !snapshot.info().degraded_root &&
          snapshot.info().generation==source->info().generation+2,"compaction publication pair invalid");
      require(snapshot.info().physical_end<source->info().physical_end,"compaction did not remove retained history");
      // Codec IDs and payload bytes survive; only identity-bound headers/hash
      // chains and physical references change.
      const auto old_index=static_cast<std::size_t>(u64(original,(source->info().generation%2)*65536+32));
      const auto new_index=static_cast<std::size_t>(u64(target->live,(snapshot.info().generation%2)*65536+32));
      const auto objects=source->list();
      for(std::size_t i=0;i<objects.size();++i) {
        const auto from=static_cast<std::size_t>(u64(original,old_index+64+64*i+16));
        const auto to=static_cast<std::size_t>(u64(target->live,new_index+64+64*i+16));
        require(std::equal(original.begin()+static_cast<std::ptrdiff_t>(from+12),original.begin()+static_cast<std::ptrdiff_t>(from+16),
                           target->live.begin()+static_cast<std::ptrdiff_t>(to+12)),"compaction changed codec ID");
        require(std::equal(original.begin()+static_cast<std::ptrdiff_t>(from+128),
                           original.begin()+static_cast<std::ptrdiff_t>(from+128+objects[i].stored_payload_bytes),
                           target->live.begin()+static_cast<std::ptrdiff_t>(to+128)),"compaction changed stored payload");
      }
      const StoreMutation more{99,false,bytes(31,91)};copied.commit({&more,1});
      require(copied.snapshot().read(99)==more.bytes,"compacted writer continuation failed");
      rejects([&]{(void)ObjectStoreWriter::compact(*source,target,copied_identity(),options);},"existing target overwritten");
    }
    const auto existing=target->live;const auto target_calls=target->calls;
    rejects([&]{(void)ObjectStoreWriter::compact(*source,target,copied_identity(),options);},"nonempty unleased target overwritten");
    require(target->live==existing && target->calls==target_calls,"nonempty target admission changed bytes");
    require(ram->live==original && ram->calls==source_calls,"compaction wrote/flushed source");
    equal_objects(*source,ObjectStoreSnapshot::open(image(original)));
    require(old->read(1)==bytes(97,1),"compaction invalidated an old snapshot");
    auto retry=std::make_shared<Ram>();{auto copy=ObjectStoreWriter::compact(*source,retry,copied_identity(),options);copy.snapshot().verify_all();}
    equal_objects(*source,ObjectStoreSnapshot::open(image(retry->stable)));
  }
  // Full manifest count exercises 32-bit count parsing and two 16 KiB catalogs.
  auto ram=std::make_shared<Ram>();
  {
    auto writer=ObjectStoreWriter::create(ram,identity());std::vector<StoreMutation> batch;
    for(std::size_t i=1;i<=kStoreMaxObjects;++i)batch.push_back({i,false,bytes(3,static_cast<unsigned>(i))});
    writer.commit(batch);
  }
  auto source=ObjectStoreSnapshot::open(ram);auto target=std::make_shared<Ram>();
  {auto copy=ObjectStoreWriter::compact(source,target,copied_identity());equal_objects(source,copy.snapshot());}
  require(ObjectStoreSnapshot::open(image(target->stable)).list().size()==kStoreMaxObjects,"full-count compacted reopen");
  auto empty=std::make_shared<Ram>();{auto writer=ObjectStoreWriter::create(empty,identity());}
  auto initial=ObjectStoreSnapshot::open(empty);auto empty_copy=std::make_shared<Ram>();
  {auto writer=ObjectStoreWriter::compact(initial,empty_copy,copied_identity());require(writer.snapshot().info().generation==0,"initial empty copy not initial");}
  {
    auto writer=ObjectStoreWriter::resume(empty,identity(),0);
    const StoreMutation add{1,false,{std::byte{1}}},erase{1,true,{}};
    writer.commit({&add,1});writer.commit({&erase,1});
  }
  auto erased=ObjectStoreSnapshot::open(empty);auto erased_copy=std::make_shared<Ram>();
  {auto writer=ObjectStoreWriter::compact(erased,erased_copy,copied_identity());require(writer.snapshot().list().empty() &&
      writer.snapshot().info().generation==erased.info().generation+2,"positive empty copy invalid");}
  auto exhausted=empty->stable;
  for(std::size_t slot=0;slot<2;++slot) {
    const auto root=slot*65536,index=static_cast<std::size_t>(u64(exhausted,root+32));
    const auto generation=(std::numeric_limits<std::uint64_t>::max)()-(1-slot);
    put64(exhausted,root+16,generation);put64(exhausted,index+16,generation);manifest_digest(exhausted,root);
  }
  auto terminal=ObjectStoreSnapshot::open(image(std::move(exhausted)));auto denied=std::make_shared<Ram>();
  rejects([&]{(void)ObjectStoreWriter::compact(terminal,denied,copied_identity());},"exhausted compaction generation accepted");
  require(denied->writes==0,"generation rejection wrote target");
}
void compaction_faults() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    const StoreCompressionOptions options{mode,6,false};
    auto source_backend=std::make_shared<Ram>();
    {
      auto writer=ObjectStoreWriter::create(source_backend,identity(),options);writer.commit(initial());
      const StoreMutation motif{9,false,Bytes(8193,std::byte{42})};writer.commit({&motif,1});
    }
    const auto untouched=source_backend->stable;const auto source_calls=source_backend->calls;
    auto source=ObjectStoreSnapshot::open(source_backend);
    auto probe=std::make_shared<Ram>();std::optional<std::size_t> first_root;
    probe->before_write=[&](auto at,auto input){
      if(!first_root && (at==0 || at==65536) && input.size()==4096 && input[0]==std::byte{'F'})
        first_root=probe->calls-1;
    };
    {auto copy=ObjectStoreWriter::compact(source,probe,copied_identity(),options);}
    const auto operations=probe->calls;require(first_root.has_value(),"compaction publication trace missing");
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<operations;++step) {
      auto target=std::make_shared<Ram>();target->failure=failure;target->fail_call=step;
      rejects([&]{(void)ObjectStoreWriter::compact(source,target,copied_identity(),options);},"compaction fault not propagated");
      require(source_backend->live==untouched && source_backend->calls==source_calls,"failed compaction touched source");
      source.verify_all();
      const bool recoverable=valid_locator(target->stable,0)||valid_locator(target->stable,1);
      if(step>*first_root+1)require(recoverable,"peer publication damaged a flushed baseline");
      if(recoverable) {
        auto backend=image(target->stable);auto snapshot=ObjectStoreSnapshot::open(backend);
        equal_objects(source,snapshot);
        require(snapshot.info().generation>=source.info().generation+1 && snapshot.info().generation<=source.info().generation+2,"partial compaction generation");
        auto writer=ObjectStoreWriter::resume(backend,copied_identity(),snapshot.info().generation,options);
        const StoreMutation more{99,false,bytes(23,19)};writer.commit({&more,1});
        require(ObjectStoreSnapshot::open(image(backend->stable)).read(99)==more.bytes,"failed-copy recovery continuation");
      } else rejects([&]{(void)ObjectStoreSnapshot::open(image(target->stable));},"rootless partial copy exposed");
    }
    // An ownership-contract violation is detected before root publication.
    auto changed=image(untouched);auto snapshot=ObjectStoreSnapshot::open(changed);auto target=std::make_shared<Ram>();bool once=false;
    const auto index=static_cast<std::size_t>(u64(untouched,(snapshot.info().generation%2)*65536+32));
    const auto record=static_cast<std::size_t>(u64(untouched,index+64+16));
    target->before_write=[&](auto,auto){if(!once){once=true;changed->live[record+128]^=std::byte{1};}};
    rejects([&]{(void)ObjectStoreWriter::compact(snapshot,target,copied_identity(),options);},"changing source copied");
    require(!valid_locator(target->live,0)&&!valid_locator(target->live,1),"unstable source published a target root");
    auto wrong=std::make_shared<Ram>();bool injected=false;
    wrong->before_write=[&](auto at,auto input){
      if(!injected && at>2*kStoreCommitUnit+128 && input.size()==128 && input[0]==std::byte{'F'}) {
        injected=true;wrong->live[2*kStoreCommitUnit+128]^=std::byte{1};
      }
    };
    rejects([&]{(void)ObjectStoreWriter::compact(source,wrong,copied_identity(),options);},"silently damaged copy published");
    require(injected && !valid_locator(wrong->live,0) && !valid_locator(wrong->live,1),"target CPU verification missed changed bytes");
    require(source_backend->live==untouched && source_backend->calls==source_calls,"copy verification failure touched source");
  }
}


void replicated_contents(const ObjectStoreSnapshot& snapshot,bool more=false) {
  require(snapshot.list().size()==(more?3U:2U),"replicated namespace partial");
  require(snapshot.read(1)==bytes(128,3) && snapshot.read(3)==bytes(257,4),"latest replicated acknowledgement lost");
  rejects([&]{(void)snapshot.read(2);},"replicated deletion rolled back");
  if(more)require(snapshot.read(99)==bytes(23,91),"replicated continuation lost");
}
void replicated_media_recovery() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    const StoreCompressionOptions options{mode,6,false};
    auto ram=std::make_shared<Ram>();std::optional<ObjectStoreSnapshot> old;
    {
      auto writer=ObjectStoreWriter::create(ram,identity(),options);
      require(writer.commit_replicated(initial()).generation==2,"first replicated publication generation");
      old.emplace(writer.snapshot());
      const auto committed=writer.commit(updated()); // Sticky protection on ordinary commit.
      require(committed.generation==4 && committed.checkpoint_generation==3 &&
          committed.replicated_checkpoint && !committed.degraded_checkpoint,"replication downgrade or invalid checkpoint identity");
    }
    const auto acknowledged=ram->stable;auto latest=ObjectStoreSnapshot::open(ram);
    replicated_contents(latest);
    const auto root=static_cast<std::size_t>((latest.info().generation%2)*kStoreCommitUnit);
    const std::array<std::uint64_t,4> units{{0,kStoreCommitUnit,u64(acknowledged,root+32),u64(acknowledged,root+96)}};
    require(units[2]!=units[3] && units[2]%kStoreCommitUnit==0 && units[3]%kStoreCommitUnit==0,"metadata replicas share a failure unit");
    for(const auto unit:units) {
      auto damaged=image(acknowledged);
      std::fill_n(damaged->live.begin()+static_cast<std::ptrdiff_t>(unit),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      auto survivor=ObjectStoreSnapshot::open(damaged);replicated_contents(survivor);
      require(survivor.info().replicated_checkpoint && survivor.info().degraded_checkpoint &&
          survivor.info().checkpoint_generation==latest.info().checkpoint_generation,"replica loss not explicitly reported");
      {
        auto writer=ObjectStoreWriter::resume(damaged,identity(),survivor.info().generation,options);
        auto repaired=writer.snapshot();replicated_contents(repaired);
        require(!repaired.info().degraded_root && !repaired.info().degraded_checkpoint &&
            repaired.info().replicated_checkpoint,"replicated resume did not restore redundancy");
        if(unit>=2*kStoreCommitUnit)
          require(std::all_of(damaged->live.begin()+static_cast<std::ptrdiff_t>(unit),
              damaged->live.begin()+static_cast<std::ptrdiff_t>(unit+kStoreCommitUnit),
              [](auto byte){return byte==std::byte{0};}),"repair overwrote the damaged catalog instead of appending");
        const StoreMutation more{99,false,bytes(23,91)};writer.commit({&more,1});
      }
      auto reopened=ObjectStoreSnapshot::open(image(damaged->stable));replicated_contents(reopened,true);
      // Every current acknowledgement survives either root OR either catalog,
      // including after repair followed by a different transaction.
      const auto at=static_cast<std::size_t>((reopened.info().generation%2)*kStoreCommitUnit);
      const std::array<std::uint64_t,4> next{{0,kStoreCommitUnit,u64(damaged->stable,at+32),u64(damaged->stable,at+96)}};
      for(const auto lost:next) {
        auto copy=damaged->stable;
        std::fill_n(copy.begin()+static_cast<std::ptrdiff_t>(lost),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
        replicated_contents(ObjectStoreSnapshot::open(image(std::move(copy))),true);
      }
    }
    require(old->read(1)==bytes(97,1) && old->read(2)==bytes(33,2),"replicated publication invalidated old snapshot");
    for(bool roots:{false,true}) {
      auto lost=acknowledged;
      const auto a=roots?0:units[2],b=roots?kStoreCommitUnit:units[3];
      std::fill_n(lost.begin()+static_cast<std::ptrdiff_t>(a),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      std::fill_n(lost.begin()+static_cast<std::ptrdiff_t>(b),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      rejects([&]{(void)ObjectStoreSnapshot::open(image(lost));},"lost both current replicas but accepted older history");
    }
    auto data_lost=acknowledged;const auto index=static_cast<std::size_t>(u64(data_lost,root+32));
    const auto record=static_cast<std::size_t>(u64(data_lost,index+64+16));data_lost[record+128]^=std::byte{1};
    rejects([&]{(void)ObjectStoreSnapshot::open(image(data_lost));},"metadata mirroring fabricated missing payload");
    for(unsigned field=0;field<4;++field) {
      auto bad=acknowledged;
      if(field==0)put64(bad,root+96,u64(bad,root+32));
      if(field==1)put64(bad,root+104,0);
      if(field==2)bad[root+112]=std::byte{1};
      if(field==3)put32(bad,root+8,3);
      root_digest(bad,root);
      rejects([&]{(void)ObjectStoreSnapshot::open(image(bad));},"intact unsupported replica semantics accepted");
    }
  }
}
void replicated_faults_and_fencing() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    const StoreCompressionOptions options{mode,6,false};
    auto seed=std::make_shared<Ram>();
    {auto writer=ObjectStoreWriter::create(seed,identity(),options);writer.commit_replicated(initial());}
    const auto baseline=seed->stable;
    auto probe=image(baseline);
    {
      auto writer=ObjectStoreWriter::resume(probe,identity(),2,options);probe->calls=0;writer.commit(updated());
    }
    const auto steps=probe->calls;
    for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
      auto ram=image(baseline);ram->stable=baseline;
      {
        auto writer=ObjectStoreWriter::resume(ram,identity(),2,options);
        ram->calls=0;ram->fail_call=step;ram->failure=fault;
        rejects([&]{writer.commit(updated());},"replicated write fault not propagated");
        require(writer.poisoned(),"replicated I/O failure not terminal");
      }
      auto recovered=image(ram->stable);auto snapshot=ObjectStoreSnapshot::open(recovered);
      if(snapshot.info().checkpoint_generation==1)
        require(snapshot.read(1)==bytes(97,1) && snapshot.read(2)==bytes(33,2),"old replicated acknowledgement lost");
      else {
        require(snapshot.info().checkpoint_generation==3,"fabricated replicated checkpoint");
        replicated_contents(snapshot);
      }
      auto writer=ObjectStoreWriter::resume(recovered,identity(),snapshot.info().generation,options);
      require(!writer.snapshot().info().degraded_checkpoint,"interrupted publication did not recover redundancy");
    }
    // Fence an unnoticed root loss before touching the remaining locator.
    auto ram=image(baseline);
    {
      auto writer=ObjectStoreWriter::resume(ram,identity(),2,options);
      std::fill_n(ram->live.begin(),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      const auto calls=ram->calls;
      rejects([&]{writer.commit(updated());},"replicated writer overwrote a sole surviving root");
      require(writer.poisoned() && ram->calls==calls,"checkpoint health fence wrote before recovery");
      require(writer.snapshot().read(2)==bytes(33,2),"fenced writer lost cached acknowledged objects");
    }
    auto survivor=ObjectStoreSnapshot::open(ram);
    {auto writer=ObjectStoreWriter::resume(ram,identity(),survivor.info().generation,options);
      require(writer.snapshot().read(2)==bytes(33,2),"root-loss recovery lost acknowledgement");}
    // Inject every repair write/flush failure after selected catalog loss.
    auto damaged=baseline;const auto catalog=u64(damaged,32);
    std::fill_n(damaged.begin()+static_cast<std::ptrdiff_t>(catalog),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
    auto repair_probe=image(damaged);
    {auto writer=ObjectStoreWriter::resume(repair_probe,identity(),2,options);}
    const auto repair_steps=repair_probe->calls;
    for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<repair_steps;++step) {
      auto broken=image(damaged);broken->stable=damaged;broken->failure=fault;broken->fail_call=step;
      rejects([&]{(void)ObjectStoreWriter::resume(broken,identity(),2,options);},"replicated repair fault not propagated");
      auto disk=image(broken->stable);auto remaining=ObjectStoreSnapshot::open(disk);
      require(remaining.read(1)==bytes(97,1) && remaining.read(2)==bytes(33,2),"failed repair lost acknowledged contents");
      auto writer=ObjectStoreWriter::resume(disk,identity(),remaining.info().generation,options);
      require(writer.snapshot().info().replicated_checkpoint && !writer.snapshot().info().degraded_checkpoint,"repair retry did not restore replicas");
    }
  }
}

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
    known_vectors();basic_and_admission();interrupted_io();torn_roots();malformed_images();decoded_live_budget();physical_record_order();compressed_transactions();resume_leases_and_orphans();resume_complete_unacknowledged_candidate();resume_root_repair();compacted_copies();compaction_faults();replicated_media_recovery();replicated_faults_and_fencing();history_cap();
    std::cout<<"PASS: "<<checks<<" object-store checks, official digest vectors, I/O failures, torn roots and bounded sparse history; RAM only.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
