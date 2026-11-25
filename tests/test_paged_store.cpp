#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/paged_store.hpp"
#include "record_digest.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
std::uint64_t checks=0;
constexpr PagedStoreLimits limits{768ULL*1024*1024};
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F&& f,const char* why) {
  ++checks;try{f();}catch(const std::runtime_error&){return;}throw std::runtime_error(why);
}
StoreIdentity identity(){StoreIdentity id{};id[0]=std::byte{187};return id;}
void put64(std::span<std::byte> b,std::size_t at,std::uint64_t n) {
  for(unsigned i=0;i<8;++i)b[at+i]=static_cast<std::byte>(n>>(8*i));
}
std::uint64_t get64(std::span<const std::byte> b,std::size_t at) {
  std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return n;
}
void digest(Bytes& b,std::size_t at) {
  const auto d=detail::record_digest(std::span<const std::byte>(b).subspan(at,4064));
  std::copy(d.begin(),d.end(),b.begin()+static_cast<std::ptrdiff_t>(at+4064));
}
Bytes payload(std::uint64_t id) {Bytes b(16);put64(b,0,id);put64(b,8,id^0x81d4a76159bdafe3ULL);return b;}
StoreMutation put(std::uint64_t id) {return {id,false,payload(id)};}
StoreMutation erase(std::uint64_t id) {return {id,true,{}};}
// Memory only. Stable state advances by dirty ranges rather than copying the
// whole growing image on every barrier. The injected failures are deterministic
// models, not real hardware power-loss experiments.
class Ram final:public StoreByteIo {
 public:
  Bytes live,stable;
  std::vector<std::pair<std::uint64_t,std::size_t>> dirty;
  std::uint32_t unit=4096;
  std::size_t calls=0,writes=0,flushes=0,fail_at=SIZE_MAX;
  unsigned mode=0;
  mutable std::uint64_t read_calls=0,read_bytes=0;
  mutable std::uint64_t damaged_read_at=UINT64_MAX;
  std::uint64_t write_bytes=0;
  bool refuse=false;
  std::array<bool,2> unreadable_roots{};
  StoreIoContract contract() const noexcept override{return {unit,true};}
  std::uint64_t extent() const override{return live.size();}
  void admit_write(std::uint64_t n) override {if(refuse || n>limits.maximum_physical_bytes)throw std::runtime_error("model admission");}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(at<2*kStoreCommitUnit && at%kStoreCommitUnit==0 && out.size()==4096 && unreadable_roots[at/kStoreCommitUnit])
      throw std::runtime_error("injected root read error");
    if(at>live.size() || out.size()>live.size()-at)throw std::runtime_error("model read bounds");
    ++read_calls;read_bytes+=out.size();std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),out.size(),out.begin());
    if(at==damaged_read_at && !out.empty()) {
      damaged_read_at=UINT64_MAX;
      out.front()^=std::byte{1};
    }
  }
  static void write(Bytes& out,std::uint64_t at,std::span<const std::byte> in) {
    if(at>limits.maximum_physical_bytes || in.size()>limits.maximum_physical_bytes-at)throw std::runtime_error("model capacity");
    if(at+in.size()>out.size())out.resize(static_cast<std::size_t>(at+in.size()));
    std::copy(in.begin(),in.end(),out.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    const bool failed=calls++==fail_at;++writes;write_bytes+=in.size();
    if(failed) {
      if(mode==1) {write(live,at,in.first(in.size()/2));write(stable,at,in.first(in.size()/2));}
      if(mode==2) {
        const auto begin=at/unit*unit;if(stable.size()<begin+unit)stable.resize(static_cast<std::size_t>(begin+unit));
        std::fill_n(stable.begin()+static_cast<std::ptrdiff_t>(begin),unit,std::byte{});live=stable;
      }
      throw std::runtime_error("injected write failure");
    }
    write(live,at,in);dirty.emplace_back(at,in.size());
  }
  void flush() override {
    ++flushes;const bool failed=calls++==fail_at;
    if(failed && mode==0)throw std::runtime_error("injected pre-barrier failure");
    if(stable.size()<live.size())stable.resize(live.size());
    for(const auto& [at,n]:dirty)std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),n,
        stable.begin()+static_cast<std::ptrdiff_t>(at));
    dirty.clear();if(failed)throw std::runtime_error("injected post-barrier failure");
  }
};
std::shared_ptr<Ram> image(const Bytes& bytes,std::uint32_t unit=4096) {
  auto ram=std::make_shared<Ram>();ram->live=bytes;ram->stable=bytes;ram->unit=unit;return ram;
}
PagedObjectStoreWriter create(const std::shared_ptr<Ram>& ram,StoreCompressionMode mode=StoreCompressionMode::raw) {
  return PagedObjectStoreWriter::create(ram,identity(),limits,{mode,6,false});
}
void apply(PagedObjectStoreWriter& writer,StoreMutation m) {writer.commit({&m,1});}
void populate(PagedObjectStoreWriter& writer,std::uint64_t count) {
  for(std::uint64_t first=1;first<=count;first+=256) {
    std::vector<StoreMutation> batch;
    for(auto id=first;id<=count && id<first+256;++id)batch.push_back(put(id));
    writer.commit(batch);
  }
}
IndexedCoherencePosting posting(std::uint64_t n) {
  OptimizationGroupFeature feature;
  feature.inode=n+2;
  feature.extent_index=n;
  feature.object=(1ULL<<61)+n+1;
  feature.revision=n+9;
  feature.content[0]=static_cast<std::byte>(n);
  feature.bands[0]=n+1000;
  return {0,feature};
}
void paged_coherence_catalog() {
  auto volume=identity(),source=volume;
  source[0]=std::byte{27};
  auto ram=std::make_shared<Ram>();
  constexpr std::uint64_t rows=2*kOptimizationGroupRepresentatives+23;
  std::uint64_t generation=0;
  {
    auto catalog=IndexedCoherenceWriter::create(ram,volume,source,limits);
    const auto first=posting(1);
    const auto before=ram->calls;
    bool refused=false;
    try{catalog.publish(std::span(&first,1),0);}
    catch(const PagedStoreCapacityError&){refused=true;}
    require(refused && ram->calls==before && !catalog.poisoned(),"advice write budget touched storage");
    for(std::uint64_t start=1;start<=rows;start+=96) {
      std::vector<IndexedCoherencePosting> batch;
      for(auto i=start;i<=rows && i<start+96;++i)batch.push_back(posting(i));
      const auto prior=ram->write_bytes;
      const auto published=catalog.publish(batch,2*1024*1024);
      require(published.write_bytes_upper_bound && published.write_bytes_upper_bound<=2*1024*1024 &&
          ram->write_bytes-prior<=published.write_bytes_upper_bound,"catalog undercharged physical writes");
    }
    auto earlier=catalog.snapshot();
    require(earlier.info().objects==rows+1,"coherence catalog inventory differs");
    require(earlier.info().height>0,"coherence catalog did not span multiple pages");
    const auto writes=ram->calls;
    require(catalog.publish(std::span(&first,1),0).write_bytes_upper_bound==0 && ram->calls==writes,
        "replaying an identical posting wrote new history");
    auto replacement=first;replacement.representative.revision+=rows;
    const auto changed=catalog.publish(std::span(&replacement,1),2*1024*1024);
    generation=changed.storage.generation;
    require(earlier.find(0,1001)==first.representative,"new posting changed an older reader");
    require(catalog.publish(std::span(&first,1),0).write_bytes_upper_bound==0,
        "older observation replaced a newer binding");
  }
  const auto prior=ram->calls;
  auto loaded=IndexedCoherenceSnapshot::open(ram,volume,source,limits);
  for(const auto n:{rows/2,rows}) {
    const auto read_start=ram->read_bytes;
    require(loaded.find(0,n+1000)==posting(n).representative &&
        ram->read_bytes-read_start<64*1024,"point query walked an unbounded inventory");
  }
  require(!loaded.find(1,1001) && !loaded.find(0,47),"unknown coherence key returned data");
  rejects([&]{(void)IndexedCoherenceSnapshot::open(ram,source,volume,limits);},"foreign volume matched");
  auto other=source;other[0]=std::byte{3};
  rejects([&]{(void)IndexedCoherenceWriter::resume(ram,volume,other,generation,limits);},
      "foreign source acquired a writer");
  rejects([&]{(void)IndexedCoherenceWriter::resume(ram,volume,source,generation-1,limits);},
      "stale catalog generation acquired a writer");
  require(ram->calls==prior,"coherence read or refused reopen mutated storage");
  {
    auto resumed=IndexedCoherenceWriter::resume(ram,volume,source,generation,limits);
    const auto row=resumed.snapshot().find(0,1001);
    require(row && row->revision==posting(1).representative.revision+rows,"coherence resume lost latest binding");
  }
  // The generic store validates this record's digest. The typed reader must
  // still reject an unknown reserved field without exposing a candidate.
  auto raw=PagedObjectStoreWriter::resume(ram,volume,generation,limits,{StoreCompressionMode::raw,1,false});
  const auto view=raw.snapshot();
  const auto key=view.list_page(1,1).at(0).id;
  auto record=view.read(key);
  const auto fingerprint=get64(record,16+64);
  record.at(9)=std::byte{1};
  raw.commit(std::array<StoreMutation,1>{{{key,false,std::move(record)}}});
  const auto malformed=IndexedCoherenceSnapshot::open(ram,volume,source,limits);
  rejects([&]{(void)malformed.find(0,fingerprint);},"malformed coherence row supplied advice");
}
void coherence_projection_transition(bool modular) {
  auto volume=identity(),source=volume;
  source[0]=std::byte{27};
  auto ram=std::make_shared<Ram>();
  auto catalog=IndexedCoherenceWriter::create(ram,volume,source,limits);
  const auto aligned=posting(7);
  auto masked=posting(8);
  masked.band=kCoherenceMaskedBand;
  masked.projected_fingerprint=0xfedcba9876543210ULL;
  const std::array previous{aligned,masked};
  catalog.publish(std::span(previous).first(modular?2:1),UINT64_MAX);
  const auto legacy=catalog.snapshot();
  const auto original=ram->stable;
  const auto projected_band=(modular?kCoherenceModularBand:kCoherenceMaskedBand)+2;
  const auto fingerprint=masked.projected_fingerprint;
  auto projected=posting(9);
  projected.band=projected_band;
  projected.projected_fingerprint=fingerprint;
  projected.representative.exact_base=true;
  auto expected=projected.representative;
  expected.bands={};
  require(legacy.has_projected_keys()==modular && !legacy.has_modular_keys() &&
      !legacy.find(projected_band,fingerprint),"older catalog exposed the new key family");
  const auto before=ram->calls;
  rejects([&]{catalog.publish(std::span(&projected,1),0);},"projection exceeded a zero write budget");
  require(ram->calls==before && !catalog.poisoned() && !catalog.snapshot().has_modular_keys() &&
      catalog.snapshot().has_projected_keys()==modular,
      "budget refusal promoted the catalog");
  const auto published=catalog.publish(std::span(&projected,1),UINT64_MAX);
  const auto calls=ram->calls-before;
  auto upgraded=IndexedCoherenceSnapshot::open(ram,volume,source,limits);
  require(upgraded.has_projected_keys() && upgraded.has_modular_keys()==modular &&
      upgraded.find(0,aligned.representative.bands[0])==aligned.representative &&
      upgraded.find(projected_band,fingerprint)==expected && !upgraded.find(projected_band+1,fingerprint) &&
      !upgraded.find(projected_band,fingerprint^1) && !legacy.find(projected_band,fingerprint),
      "catalog promotion lost a binding or exposed an unrelated key");
  if(modular) {
    auto inherited=masked.representative;
    inherited.bands={};
    require(upgraded.find(masked.band,fingerprint)==inherited && legacy.find(masked.band,fingerprint)==inherited,
        "promotion changed the preceding key family");
  }
  const auto writes=ram->calls;
  auto older=projected;
  --older.representative.revision;
  require(!catalog.publish(std::span(&projected,1),0).write_bytes_upper_bound &&
      !catalog.publish(std::span(&older,1),0).write_bytes_upper_bound && ram->calls==writes,
      "projected key replay wrote extra history");

  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<calls;++cut) {
    auto fault=image(original);
    auto writer=IndexedCoherenceWriter::resume(fault,volume,source,legacy.info().generation,limits);
    fault->mode=mode;
    fault->fail_at=fault->calls+cut;
    rejects([&]{writer.publish(std::span(&projected,1),UINT64_MAX);},"projection cut was not exercised");
    const auto recovered=IndexedCoherenceSnapshot::open(image(fault->stable),volume,source,limits);
    const auto checkpoint=recovered.info().checkpoint_generation;
    const bool promoted=checkpoint==published.storage.checkpoint_generation;
    require((promoted || checkpoint==legacy.info().checkpoint_generation) &&
        recovered.has_projected_keys()==(modular || promoted) && recovered.has_modular_keys()==(modular && promoted) &&
        recovered.find(0,aligned.representative.bands[0])==aligned.representative &&
        recovered.find(projected_band,fingerprint)==(promoted?std::optional{expected}:std::nullopt),
        "interrupted promotion exposed an unmatched schema or changed an existing key");
  }

  const auto altered=image(ram->stable);
  auto generic=PagedObjectStoreWriter::resume(altered,volume,upgraded.info().generation,limits,
      {StoreCompressionMode::raw,1,false});
  const auto raw=generic.snapshot();
  const auto rows=raw.list_page(1,3);
  const auto compact=std::ranges::find_if(rows,[&](const auto& row) {
    return row.logical_bytes==96 && std::to_integer<std::uint32_t>(raw.read(row.id).at(8))==projected_band;
  });
  require(compact!=rows.end(),"projected records exceeded the compact wire size");
  auto record=raw.read(compact->id);
  record.at(88)^=std::byte{1}; // The outer record is valid, but the key no longer matches its body.
  generic.commit(std::array<StoreMutation,1>{{{compact->id,false,std::move(record)}}});
  const auto malformed=IndexedCoherenceSnapshot::open(altered,volume,source,limits);
  rejects([&]{(void)malformed.find(projected_band,fingerprint);},"malformed projection supplied a candidate");
  require(malformed.find(0,aligned.representative.bands[0])==aligned.representative,
      "malformed projection changed an unrelated posting");
  std::cout<<(modular?"modular_catalog":"projected_catalog")
      <<" record_bytes=96 promotion_write_bytes="<<published.write_bytes_upper_bound
      <<" modeled_interruptions="<<3*calls<<'\n';
}
void basic_and_oracle() {
  auto ram=std::make_shared<Ram>();auto writer=create(ram);auto empty=writer.snapshot();
  require(empty.info().objects==0 && empty.list_page(0,1).empty(),"empty index");
  populate(writer,4096);auto prior=writer.snapshot();std::map<std::uint64_t,Bytes> oracle;
  for(std::uint64_t i=1;i<=4096;++i)oracle.emplace(i,payload(i));
  require(prior.info().height>=2 && prior.info().objects==4096,"multi-level tree did not grow");
  const auto reference=prior.stat(17);
  require(reference && prior.read_if_generation(17,reference->generation,16)==payload(17),
      "generation-bound read lost the protected object");
  require(!prior.read_if_generation(17,reference->generation+1,0),
      "generation mismatch reached the stored payload");
  rejects([&]{(void)prior.read_if_generation(17,reference->generation,0);},
      "generation matching bypassed the decoded length budget");
  // Deterministic mixed sparse keys, replacement, insertion before/after ranges,
  // and deletes, compared against an independent ordered map.
  std::uint64_t random=0x2468beef;
  for(unsigned round=0;round<48;++round) {
    std::map<std::uint64_t,StoreMutation> changes;
    for(unsigned j=0;j<53;++j) {
      random^=random<<13;random^=random>>7;random^=random<<17;
      const auto id=random%6144+1;
      changes[id]=(oracle.contains(id) && random%3==0)?erase(id):put(id);
    }
    std::vector<StoreMutation> batch;
    for(auto& [id,m]:changes) {if(m.erase)oracle.erase(id);else oracle[id]=m.bytes;batch.push_back(std::move(m));}
    writer.commit(batch);auto view=writer.snapshot();require(view.info().objects==oracle.size(),"oracle inventory count");
    for(const auto& m:batch)require(view.stat(m.id).has_value()==oracle.contains(m.id),"oracle membership");
  }
  auto current=PagedObjectStoreSnapshot::open(ram,limits);std::uint64_t after=0;std::size_t seen=0;
  auto expected=oracle.begin();
  for(;;) {
    const auto page=current.list_page(after,73);if(page.empty())break;
    for(const auto& e:page) {require(expected!=oracle.end() && e.id==expected->first,"pagination order");
      require(current.read(e.id)==expected->second,"oracle payload");after=e.id;++seen;++expected;}
  }
  require(seen==oracle.size() && expected==oracle.end(),"pagination omitted objects");
  require(prior.info().objects==4096 && prior.read(17)==payload(17),"retained snapshot changed");
  require(empty.list_page(0,4).empty(),"old empty snapshot acquired keys");
  current.verify_all();prior.verify_all();
  const auto calls=ram->calls;
  rejects([&]{current.list_page(0,0);},"zero page budget admitted");
  rejects([&]{current.list_page(0,4097);},"large page budget admitted");
  rejects([&]{apply(writer,erase(UINT64_MAX));},"missing deletion admitted");
  const std::array<StoreMutation,2> duplicate{put(7),put(7)};
  rejects([&]{writer.commit(duplicate);},"duplicate keys admitted");
  ram->refuse=true;rejects([&]{apply(writer,put(UINT64_MAX));},"refused admission wrote");ram->refuse=false;
  require(ram->calls==calls && !writer.poisoned(),"argument/admission refusal poisoned or wrote");
  apply(writer,put(UINT64_MAX));require(writer.snapshot().read(UINT64_MAX)==payload(UINT64_MAX),"maximum key");
  rejects([&]{(void)create(ram);},"second live writer acquired lease");
  rejects([&]{(void)ObjectStoreWriter::create(ram,identity());},"flat writer bypassed shared lease");
}
void remove_all_keys() {
  auto ram=std::make_shared<Ram>();auto writer=create(ram);populate(writer,3000);
  for(std::uint64_t first=1;first<=3000;first+=256) {
    std::vector<StoreMutation> batch;
    for(auto id=first;id<=3000 && id<first+256;++id)batch.push_back(erase(id));
    writer.commit(batch);
  }
  require(writer.snapshot().info().objects==0 && writer.snapshot().info().height==0,"empty tree retained branch");
  apply(writer,put(43));require(writer.snapshot().read(43)==payload(43),"empty tree did not regrow");
}
void append_write_accounting(PagedStoreBankLayout layout=PagedStoreBankLayout::separate) {
  const bool shared=layout==PagedStoreBankLayout::shared;
  for(const auto unit:{4096U,65536U}) {
    auto ram=std::make_shared<Ram>();ram->unit=unit;
    auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,6,false},layout);
    auto check_append=[&](std::span<const StoreMutation> changes) {
      constexpr std::uint64_t root_write_bytes=2*4096;
      const auto before_extent=ram->extent(),before_writes=ram->write_bytes;
      const auto writes=ram->writes,flushes=ram->flushes;
      const auto reads=ram->read_calls,read_bytes=ram->read_bytes;
      const auto committed=writer.commit(changes);
      const auto growth=committed.physical_end-before_extent;
      const auto written=ram->write_bytes-before_writes;
      std::cout<<"append_write_accounting unit="<<unit<<" shared="<<shared<<" records="<<changes.size()
          <<" growth="<<growth<<" written="<<written<<" writes="<<ram->writes-writes
          <<" reads="<<ram->read_calls-reads<<" read_bytes="<<ram->read_bytes-read_bytes
          <<" flushes="<<ram->flushes-flushes<<" image_digest=";
      const auto digest=detail::record_digest(ram->stable);
      for(std::size_t at=0;at<digest.size();at+=8)std::cout<<std::hex<<get64(digest,at)<<'/';
      std::cout<<std::dec<<'\n';
      require(committed.physical_end==ram->extent(),"paged image extent changed unexpectedly");
      require(written==growth+root_write_bytes,"paged commit rewrote bytes in fresh append space");
      if(changes.size()>1) {
        require(ram->writes-writes<changes.size(),"small frames were written individually");
        require(ram->read_calls-reads<changes.size(),"small frames were verified individually");
      }
      auto recovered=PagedObjectStoreSnapshot::open(image(ram->stable,unit),limits);
      require(recovered.info().generation==committed.generation,"append root generation");
      for(const auto& change:changes)require(recovered.read(change.id)==change.bytes,"append readback");
      recovered.verify_all();
      return growth;
    };
    const auto small=put(1);
    const auto can_share=shared && unit>4096;
    require(check_append(std::span(&small,1))==(can_share?2:4)*std::uint64_t{unit},
        "small append did not use the expected number of bank pairs");
    require(ram->stable[8]==static_cast<std::byte>(can_share?3:2),"unexpected bank layout capability");
    if(can_share) {
      const auto bank=get64(ram->stable,104)/unit*unit;
      for(const auto peer:{0U,1U}) {
        auto damaged=ram->stable;
        std::fill_n(damaged.begin()+static_cast<std::ptrdiff_t>(bank+peer*unit),unit,std::byte{});
        auto recovered=PagedObjectStoreSnapshot::open(image(damaged,unit),limits);
        require(recovered.info().degraded_metadata && recovered.read(small.id)==small.bytes,
            "loss of a shared failure unit lost both copies");
        recovered.verify_all();
      }
      auto incompatible=ram->stable;
      incompatible[8]=std::byte{2};
      digest(incompatible,0);
      rejects([&]{(void)PagedObjectStoreSnapshot::open(image(incompatible,unit),limits);},
          "different capabilities in a root pair were accepted");
      incompatible[kStoreCommitUnit+8]=std::byte{2};
      digest(incompatible,kStoreCommitUnit);
      rejects([&]{(void)PagedObjectStoreSnapshot::open(image(incompatible,unit),limits);},
          "separate-bank grammar accepted a shared leaf");
    }
    populate(writer,1024);
    std::vector<StoreMutation> scattered;
    for(std::uint64_t id=1;id<=1024;id+=4)scattered.push_back({id,false,Bytes(21,std::byte{0x62})});
    check_append(scattered);
  }
}

void failure_matrix(std::uint32_t unit,bool mirrored=false,
    PagedStoreBankLayout layout=PagedStoreBankLayout::separate) {
  auto base=std::make_shared<Ram>();base->unit=unit;
  {auto w=mirrored?PagedObjectStoreWriter::create_mirrored(base,identity(),limits,
        {StoreCompressionMode::raw,6,false},layout):create(base);
    populate(w,130);}
  const auto before=base->stable;
  const auto changed_bytes=mirrored?unit+127:912;
  const std::array<StoreMutation,4> changes{{{17,false,Bytes(changed_bytes,std::byte{9})},
      erase(71),put(150),put(151)}};
  auto control=image(before,unit);std::size_t operations=0;std::uint64_t new_generation=0;
  {auto s=PagedObjectStoreSnapshot::open(control,limits);auto w=PagedObjectStoreWriter::resume(control,identity(),s.info().generation,limits);
    const auto begin=control->calls;new_generation=w.commit(changes).generation;operations=control->calls-begin;}
  for(unsigned mode=0;mode<3;++mode)for(std::size_t point=0;point<operations;++point) {
    auto ram=image(before,unit);
    {const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
      auto w=PagedObjectStoreWriter::resume(ram,identity(),generation,limits);
      auto old=w.snapshot();ram->mode=mode;ram->fail_at=ram->calls+point;
      rejects([&]{w.commit(changes);},"failure not injected");require(w.poisoned(),"failed writer reusable");
      require(old.read(17)==payload(17) && old.read(71)==payload(71),"acknowledged snapshot lost");
      rejects([&]{apply(w,put(202));},"poisoned writer accepted retry");}
    auto crash=image(ram->stable,unit);auto recovered=PagedObjectStoreSnapshot::open(crash,limits);
    const auto bytes=recovered.read(17);const bool updated=bytes==changes[0].bytes;
    require(updated || bytes==payload(17),"partial object survived recovery");
    require(recovered.stat(71).has_value()!=updated && recovered.stat(150).has_value()==updated &&
        recovered.stat(151).has_value()==updated,"partial transaction exposed");
    recovered.verify_all();
    // Repair mixed/torn roots, preserve orphan tails, continue on a NEW writer.
    auto resumed=PagedObjectStoreWriter::resume(crash,identity(),recovered.info().generation,limits);
    apply(resumed,put(202));require(resumed.snapshot().read(202)==payload(202),"recovery could not continue");
    require(resumed.snapshot().read(17)==bytes,"continuation lost recovered object");
  }
  require(new_generation>0,"control generation");
  if(mirrored) {
    auto ram=image(before,unit);
    const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
    auto w=PagedObjectStoreWriter::resume(ram,identity(),generation,limits);
    const auto pair_bytes=2ULL*unit;
    const auto first_payload=(ram->extent()+pair_bytes-1)/pair_bytes*pair_bytes;
    ram->damaged_read_at=first_payload+unit;
    rejects([&]{w.commit(changes);},"transient error in new replica was ignored");
    require(ram->damaged_read_at==UINT64_MAX && w.poisoned() && ram->stable==before,
        "failed replica readback reached a publication fence");
  }
  std::cout<<"failure_matrix unit="<<unit<<" mirrored="<<mirrored<<" shared="<<(layout==PagedStoreBankLayout::shared)
      <<" operations="<<operations<<" modes=3\n";
}
void compact_root_layout() {
  constexpr auto layout=PagedStoreBankLayout::compact;
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto marker=[](const Bytes& bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()),8);
  };
  for(const auto unit:{4096U,8192U,16384U,32768U,65536U}) {
    auto compact=std::make_shared<Ram>(),legacy=std::make_shared<Ram>();
    compact->unit=legacy->unit=unit;
    for(const auto& [backing,style]:std::array{
          std::pair{compact,layout},std::pair{legacy,PagedStoreBankLayout::shared}}) {
      auto writer=PagedObjectStoreWriter::create_mirrored(backing,identity(),limits,raw,style);
      if(style==layout)require(backing->extent()==4ULL*unit,"compact empty checkpoint extent");
      apply(writer,put(1));
    }
    require(marker(compact->stable)==(unit<kStoreCommitUnit?"FLTXPG02":"FLTXPG01") &&
        legacy->extent()-compact->extent()==2ULL*(kStoreCommitUnit-unit),
        "root spacing did not match the backend failure unit");
    for(const auto lost:{0U,1U}) {
      auto damaged=compact->stable;
      std::fill_n(damaged.begin()+static_cast<std::ptrdiff_t>(lost*unit),unit,std::byte{});
      auto survived=image(damaged,unit);
      const auto reader=PagedObjectStoreSnapshot::open(survived,limits);
      require(reader.info().degraded_roots && reader.read(1)==payload(1),"isolated root loss changed data");
      auto writable=PagedObjectStoreWriter::resume(survived,identity(),reader.info().generation,limits,raw);
      require(!writable.snapshot().info().degraded_roots,"surviving root did not repair its peer");
      apply(writable,put(2));
      require(writable.snapshot().read(1)==payload(1),"repaired root append changed data");
    }
    const auto original=PagedObjectStoreSnapshot::open(compact,limits);
    auto moved=image(compact->stable,kStoreCommitUnit);
    require(PagedObjectStoreSnapshot::open(moved,limits).read(1)==payload(1),
        "read-only compact image relocation changed data");
    if(unit<kStoreCommitUnit) {
      rejects([&]{(void)PagedObjectStoreWriter::resume(moved,identity(),original.info().generation,limits,raw);},
          "wider failure units acquired the compact writer");
      require(!moved->calls && moved->stable==compact->stable,"incompatible resume touched the image");
    }
    for(const auto target_unit:{unit,static_cast<unsigned>(kStoreCommitUnit)}) {
      auto replacement=std::make_shared<Ram>();replacement->unit=target_unit;
      auto next=identity();next[0]^=std::byte{1};
      auto writer=PagedObjectStoreWriter::rebuild(original,replacement,next,limits,raw);
      const auto checked=writer.snapshot();
      checked.verify_all();
      require(checked.read(1)==payload(1) &&
          marker(replacement->stable)==(target_unit<kStoreCommitUnit?"FLTXPG02":"FLTXPG01"),
          "independent rebuild chose the wrong root spacing or data");
    }

    if(unit==4096) {
      auto ambiguous=compact->stable;
      ambiguous.resize(legacy->stable.size());
      std::fill_n(ambiguous.begin(),unit,std::byte{});
      std::copy_n(legacy->stable.begin()+kStoreCommitUnit,4096,ambiguous.begin()+kStoreCommitUnit);
      rejects([&]{(void)PagedObjectStoreSnapshot::open(image(ambiguous,unit),limits);},
          "conflicting plausible second-root locations were accepted");
    }
  }
}

void damage_and_malformed() {
  auto ram=std::make_shared<Ram>();
  {auto w=create(ram);populate(w,4000);}
  const auto good=ram->stable;const auto root=get64(good,104),peer=get64(good,112);
  // Highest root is slot zero after an even generation.
  require(root>=2*kStoreCommitUnit && peer>root,"root reference fixture");
  for(auto at:{root,peer}) {
    auto broken=good;broken[static_cast<std::size_t>(at)]^=std::byte{1};auto backing=image(broken);
    auto s=PagedObjectStoreSnapshot::open(backing,limits);require(s.info().degraded_metadata,"root replica loss hidden");
    require(s.read(77)==payload(77),"metadata peer failed recovery");
    const auto writes=backing->writes;
    rejects([&]{PagedObjectStoreWriter::resume(backing,identity(),s.info().generation,limits);},"damaged metadata writable");
    require(backing->writes==writes,"unqualified metadata repair wrote");
  }
  {auto broken=good;broken[static_cast<std::size_t>(root)]^=std::byte{1};broken[static_cast<std::size_t>(peer)]^=std::byte{1};
    rejects([&]{PagedObjectStoreSnapshot::open(image(broken),limits);},"both metadata copies lost silently rolled back");}
  for(std::size_t slot=0;slot<2;++slot) {
    auto broken=good;broken[slot*kStoreCommitUnit]^=std::byte{1};auto backing=image(broken);
    auto s=PagedObjectStoreSnapshot::open(backing,limits);require(s.info().degraded_roots,"root loss hidden");
    require(s.read(77)==payload(77),"root loss changed bytes");
    auto w=PagedObjectStoreWriter::resume(backing,identity(),s.info().generation,limits);
    require(!w.snapshot().info().degraded_roots,"root pair not repaired");
  }
  // Checksums are not authentication: recomputed digests must not bypass
  // semantic checks of root format, reserved bits, height, range or overflow.
  for(auto offset:{8U,12U,56U,60U,132U,176U}) {
    auto broken=good;broken[offset]^=std::byte{128};digest(broken,0);
    rejects([&]{PagedObjectStoreSnapshot::open(image(broken),limits);},"intact malformed root admitted");
  }
  {auto broken=good;put64(broken,104,UINT64_MAX-4095);digest(broken,0);
    rejects([&]{PagedObjectStoreSnapshot::open(image(broken),limits);},"overflowing page address admitted");}
  // Lose one descendant copy while the root remains intact; reads recover,
  // report degraded metadata and writable resume refuses without writes.
  const auto child=get64(good,static_cast<std::size_t>(root)+64+40);
  {auto broken=good;broken[static_cast<std::size_t>(child)]^=std::byte{1};auto backing=image(broken);
    auto s=PagedObjectStoreSnapshot::open(backing,limits);s.verify_all();require(s.info().degraded_metadata,"descendant loss hidden");
    const auto writes=backing->writes;
    rejects([&]{PagedObjectStoreWriter::resume(backing,identity(),s.info().generation,limits);},"damaged descendant writable");
    require(backing->writes==writes,"damaged descendant repair wrote");}
  // Corrupt a payload, leaving its descriptor and both index copies intact.
  auto view=PagedObjectStoreSnapshot::open(image(good),limits);
  auto damaged=good;const auto wanted=payload(77);
  auto at=std::search(damaged.begin(),damaged.end(),wanted.begin(),wanted.end());
  require(at!=damaged.end(),"payload corruption fixture absent");*at^=std::byte{1};
  auto s=PagedObjectStoreSnapshot::open(image(damaged),limits);
  rejects([&]{s.read(77);},"corrupt object exposed");rejects([&]{s.verify_all();},"scrub ignored corrupt object");
}
StoreIdentity copy_identity(){auto id=identity();id[1]=std::byte{71};return id;}
void compacted_descriptor_bounds() {
  auto ram=std::make_shared<Ram>();
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,
      {StoreCompressionMode::automatic,3,false});
  writer.commit(std::array{StoreMutation{1,false,Bytes(57344)},put(2)});
  const auto snapshot=writer.snapshot();
  const auto descriptor=*snapshot.stat(1);
  const auto second=*snapshot.stat(2);
  require(descriptor.stored_payload_bytes<descriptor.logical_bytes,"identity descriptor was not compressed");
  const auto lower=snapshot.rebuilt_size(std::array{StoreObjectInfo{1,descriptor.generation,descriptor.logical_bytes,0},second});
  const auto rewritten=snapshot.rebuilt_size(std::array{StoreObjectInfo{1,descriptor.generation,descriptor.logical_bytes,
      descriptor.logical_bytes},second});
  const auto remaining=snapshot.rebuilt_size(std::array{second});
  const auto check=[&](std::span<const StoreMutation> edits,std::uint64_t rebound,
      std::uint64_t before,std::uint64_t after) {
    const auto bounds=snapshot.rebuilt_size_after(edits,rebound);
    require(bounds.before==before && bounds.after_upper==after,"compacted identity descriptor bounds differ");
  };
  check({},1,lower,rewritten);
  constexpr std::uint32_t replacement_bytes=49152;
  const auto replaced=snapshot.rebuilt_size(std::array{StoreObjectInfo{1,descriptor.generation,replacement_bytes,
      replacement_bytes},second});
  check(std::array{StoreMutation{1,false,Bytes(replacement_bytes)}},1,lower,replaced);
  check(std::array{erase(1)},1,lower,remaining);
  const auto prior=snapshot.rebuilt_size(std::array{descriptor,second});
  const auto added=snapshot.rebuilt_size(std::array{descriptor,second,
      StoreObjectInfo{3,descriptor.generation,replacement_bytes,replacement_bytes}});
  check(std::array{StoreMutation{3,false,Bytes(replacement_bytes)}},3,prior,added);
  rejects([&]{(void)snapshot.rebuilt_size_after(std::array{erase(3)},3);},"priced an absent descriptor erase");
}

void rebuild_and_recovery() {
  auto ram=std::make_shared<Ram>();
  {auto w=create(ram);populate(w,4000);for(unsigned i=0;i<40;++i)apply(w,put(i+1));}
  const auto before=ram->stable;auto source=PagedObjectStoreSnapshot::open(ram,limits);
  auto target=std::make_shared<Ram>();
  auto writer=PagedObjectStoreWriter::rebuild(source,target,copy_identity(),limits);
  auto copy=writer.snapshot();require(copy.identity()==copy_identity(),"rebuild identity");
  require(copy.info().objects==4000 && copy.info().physical_end<source.info().physical_end,"rebuild did not discard history");
  for(std::uint64_t id=1;id<=4000;++id) {
    require(copy.read(id)==source.read(id),"rebuild payload comparison");
    require(copy.stat(id)->generation==source.stat(id)->generation,"rebuild changed object generation");
  }
  apply(writer,put(6000));require(writer.snapshot().read(6000)==payload(6000),"rebuilt writer cannot continue");
  require(ram->stable==before && ram->live==before,"rebuild modified source");
  auto untouched=std::make_shared<Ram>();
  rejects([&]{PagedObjectStoreWriter::rebuild(source,untouched,identity(),limits);},"rebuild reused source identity");
  require(untouched->writes==0,"identity refusal wrote target");
  rejects([&]{PagedObjectStoreWriter::rebuild(source,target,copy_identity(),limits);},"rebuild replaced live target");
  // A damaged descendant can be restored from its surviving peer in a NEW
  // image. Never silently rewrite or select that new image on the caller's behalf.
  auto damaged=before;const auto root=get64(damaged,104),child=get64(damaged,static_cast<std::size_t>(root)+104);
  damaged[static_cast<std::size_t>(child)]^=std::byte{1};auto damaged_ram=image(damaged);
  auto damaged_source=PagedObjectStoreSnapshot::open(damaged_ram,limits);auto repaired_ram=std::make_shared<Ram>();
  auto repaired=PagedObjectStoreWriter::rebuild(damaged_source,repaired_ram,copy_identity(),limits);
  repaired.snapshot().verify_all();require(!repaired.snapshot().info().degraded_metadata,"rebuilt metadata degraded");
  require(repaired.snapshot().read(3999)==payload(3999) && damaged_ram->live==damaged,"peer rebuild failed or changed source");
  std::cout<<"rebuild source_bytes="<<source.info().physical_end<<" output_bytes="<<copy.info().physical_end<<" objects=4000\n";
}
void rebuild_failures() {
  auto source_ram=std::make_shared<Ram>();{auto w=create(source_ram);populate(w,7);}
  const auto unchanged=source_ram->stable;auto source=PagedObjectStoreSnapshot::open(source_ram,limits);
  auto control=std::make_shared<Ram>();std::size_t operations=0;
  {auto copy=PagedObjectStoreWriter::rebuild(source,control,copy_identity(),limits);operations=control->calls;}
  for(unsigned mode=0;mode<3;++mode)for(std::size_t point=0;point<operations;++point) {
    auto target=std::make_shared<Ram>();target->mode=mode;target->fail_at=point;
    rejects([&]{PagedObjectStoreWriter::rebuild(source,target,copy_identity(),limits);},"rebuild fault not reached");
    require(source_ram->live==unchanged && source_ram->stable==unchanged,"failed rebuild changed source");
    std::optional<PagedObjectStoreSnapshot> recovered;
    try{recovered.emplace(PagedObjectStoreSnapshot::open(image(target->stable),limits));}catch(const std::runtime_error&){}
    if(recovered) {
      require(recovered->info().objects==7,"partial rebuild published inventory");
      recovered->verify_all();for(std::uint64_t id=1;id<=7;++id)require(recovered->read(id)==payload(id),"partial rebuild payload");
    }
  }
  std::cout<<"rebuild_failure_matrix operations="<<operations<<" modes=3\n";
}
void semantic_pages_and_units() {
  auto ram=std::make_shared<Ram>();{auto w=create(ram);populate(w,7);}
  const auto original=ram->stable;const auto root=get64(original,104),peer=get64(original,112);
  // Recompute every affected digest, so these exercise semantic rejection
  // rather than stopping at the first ordinary checksum mismatch.
  for(auto offset:{8U,12U,24U,40U,44U,64U,72U,80U,88U,92U,512U}) {
    auto broken=original;
    broken[static_cast<std::size_t>(root)+offset]^=std::byte{128};digest(broken,static_cast<std::size_t>(root));
    std::copy_n(broken.begin()+static_cast<std::ptrdiff_t>(root),4096,broken.begin()+static_cast<std::ptrdiff_t>(peer));
    const auto page_digest=std::span<const std::byte>(broken).subspan(static_cast<std::size_t>(root)+4064,32);
    for(std::size_t slot=0;slot<2;++slot) {
      std::copy(page_digest.begin(),page_digest.end(),broken.begin()+static_cast<std::ptrdiff_t>(slot*kStoreCommitUnit+144));
      digest(broken,slot*kStoreCommitUnit);
    }
    rejects([&]{auto s=PagedObjectStoreSnapshot::open(image(broken),limits);s.verify_all();},"intact malformed page admitted");
  }
  for(const auto unit:{4096U,65536U}) {
    auto backing=std::make_shared<Ram>();backing->unit=unit;{auto w=create(backing);populate(w,4000);}
    const auto good=backing->stable;const auto top=get64(good,104),top_peer=get64(good,112);
    for(auto locator:{top,top_peer}) {
      auto lost=good;const auto begin=locator/unit*unit;
      std::fill_n(lost.begin()+static_cast<std::ptrdiff_t>(begin),unit,std::byte{});
      auto s=PagedObjectStoreSnapshot::open(image(lost,unit),limits);s.verify_all();
      require(s.read(3333)==payload(3333) && s.info().degraded_metadata,"whole metadata failure unit not recovered");
    }
  }
  auto tiny=std::make_shared<Ram>();auto w=PagedObjectStoreWriter::create(tiny,identity(),{2*kStoreCommitUnit+8192});
  const auto calls=tiny->calls;rejects([&]{apply(w,put(1));},"capacity overflow admitted");
  require(!w.poisoned() && tiny->calls==calls,"capacity preflight wrote or poisoned");
  rejects([&]{PagedObjectStoreSnapshot::open(ram,{});},"unknown capacity admitted");
  for(std::size_t slot=0;slot<2;++slot) {
    auto unavailable=image(original);unavailable->unreadable_roots[slot]=true;
    auto s=PagedObjectStoreSnapshot::open(unavailable,limits);
    require(s.info().degraded_roots && s.read(7)==payload(7),"root I/O error hid surviving peer");
    unavailable->unreadable_roots[1-slot]=true;
    rejects([&]{PagedObjectStoreSnapshot::open(unavailable,limits);},"both unreadable roots admitted");
  }
}
void codec_modes() {
  std::vector<Bytes> inputs{{},Bytes(65536),Bytes(131071),Bytes(65537),Bytes(kStoreMaxObjectBytes)};
  for(std::size_t i=0;i<inputs[2].size();++i)inputs[2][i]=static_cast<std::byte>(i%31);
  std::uint64_t random=0x9de8a4ef0b19c473ULL;
  for(auto index:{3U,4U})for(auto& byte:inputs[index]) {
    random^=random<<13;random^=random>>7;random^=random<<17;byte=static_cast<std::byte>(random>>23);
  }
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic,StoreCompressionMode::fast}) {
    auto ram=std::make_shared<Ram>();auto writer=create(ram,mode);std::vector<StoreMutation> batch;
    for(std::size_t i=0;i<inputs.size();++i)batch.push_back({i+1,false,inputs[i],{std::byte{255}}});
    require(!writer.commit(batch).stored_lz4_candidates,"invalid candidate was counted as stored");
    auto s=PagedObjectStoreSnapshot::open(ram,limits);
    for(std::size_t i=0;i<inputs.size();++i)require(s.read(i+1)==inputs[i],"codec mode CPU roundtrip");
    s.verify_all();
    if(mode!=StoreCompressionMode::raw)require(s.info().stored_payload_bytes<s.info().logical_bytes,"zero/pattern encoding absent");
  }
  const StoreCompressionOptions fast{StoreCompressionMode::fast,6,false};
  detail::StoreCodecEncoder encoder(fast);
  Bytes repeated(65536);
  for(std::size_t i=0;i<repeated.size();++i)repeated[i]=inputs[3][i%8192];
  const auto reference=encoder.encode(repeated);
  auto changed=repeated;
  changed[0]^=std::byte{1};
  auto ram=std::make_shared<Ram>();
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,fast);
  const StoreMutation batch[]{{1,false,repeated,reference.payload},{2,false,changed,reference.payload}};
  const auto committed=writer.commit(batch);
  require(committed.mirrored_payloads &&
      committed.stored_lz4_candidates==static_cast<std::uint32_t>(reference.codec==detail::kStoreLz4),
      "mirrored commit did not count only the selected logical candidate");
  auto reopened=PagedObjectStoreSnapshot::open(ram,limits);
  reopened.verify_all();
  require(!reopened.info().stored_lz4_candidates && reopened.read(1)==repeated && reopened.read(2)==changed,
      "candidate accounting or stale input affected independent recovery");
}
void scale(std::uint64_t count) {
  const auto started=std::chrono::steady_clock::now();auto ram=std::make_shared<Ram>();auto writer=create(ram);
  populate(writer,count);auto snapshot=writer.snapshot();require(snapshot.info().objects==count,"scale inventory");
  const auto begin=ram->read_bytes;
  auto reopened=PagedObjectStoreSnapshot::open(ram,limits);
  const auto open_bytes=ram->read_bytes-begin;
  require(open_bytes==16384,"open scanned the inventory");
  const auto read_begin=ram->read_bytes;
  for(std::uint64_t i=0;i<1000;++i) {
    const auto id=(i*104729)%count+1;require(reopened.read(id)==payload(id),"scale random read");
  }
  const auto lookup_bytes=ram->read_bytes-read_begin;
  require(lookup_bytes<=1000ULL*((snapshot.info().height+1)*8192ULL+144),"lookup read amplification");
  std::uint64_t seen=0,after=0;
  for(;;) {
    const auto page=reopened.list_page(after,1024);if(page.empty())break;
    for(const auto& e:page) {require(e.id==++seen,"scale pagination gap");after=e.id;}
  }
  require(seen==count,"scale pagination count");reopened.verify_all();
  // A scattered update/deletion must rewrite bounded paths, not the catalog.
  std::vector<StoreMutation> changes;
  for(std::uint64_t i=0;i<128;++i)changes.push_back(erase((i*104729)%count+1));
  const auto writes=ram->write_bytes;writer.commit(changes);const auto update_bytes=ram->write_bytes-writes;
  require(writer.snapshot().info().objects==count-128,"scale delete count");
  require(snapshot.read(1)==payload(1),"scale old snapshot lost");
  const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  std::cout<<"scale objects="<<count<<" image_bytes="<<snapshot.info().physical_end
    <<" height="<<snapshot.info().height<<" open_read_bytes="<<open_bytes
    <<" random_1000_read_bytes="<<lookup_bytes<<" scattered_128_delete_write_bytes="<<update_bytes
    <<" elapsed_seconds="<<elapsed<<"\n";
}
} // namespace
int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string_view(argv[1])=="--million")scale(1000000);
    else if(argc==2 && std::string_view(argv[1])=="--mirrored-append") {
      append_write_accounting();failure_matrix(4096,true);failure_matrix(65536,true);
    }
    else if(argc==2 && std::string_view(argv[1])=="--compact-roots") {
      compact_root_layout();
      failure_matrix(4096,true,PagedStoreBankLayout::compact);
      failure_matrix(16384,true,PagedStoreBankLayout::compact);
    }
    else if(argc==2 && std::string_view(argv[1])=="--shared-banks") {
      append_write_accounting();append_write_accounting(PagedStoreBankLayout::shared);
      failure_matrix(65536,true,PagedStoreBankLayout::shared);
    }
    else if(argc==1) {append_write_accounting();append_write_accounting(PagedStoreBankLayout::shared);
      failure_matrix(65536,true,PagedStoreBankLayout::shared);paged_coherence_catalog();
      coherence_projection_transition(false);coherence_projection_transition(true);
      basic_and_oracle();remove_all_keys();
      failure_matrix(4096);failure_matrix(65536);failure_matrix(4096,true);failure_matrix(65536,true);
      damage_and_malformed();semantic_pages_and_units();codec_modes();compacted_descriptor_bounds();
      rebuild_and_recovery();rebuild_failures();scale(16384);}
    else throw std::runtime_error("unsupported test arguments");
    std::cout<<"paged store checks="<<checks<<"\n";return 0;
  }catch(const std::exception& e){std::cerr<<"after "<<checks<<" checks: "<<e.what()<<"\n";return 1;}
}
