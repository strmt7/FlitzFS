#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
#include <iostream>
#include <set>
namespace {
using namespace flitzfs;using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,unsigned count=8) {
  if(at>bytes.size() || count>bytes.size()-at)throw std::runtime_error("fixture framing");
  std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(i*8);return value;
}
void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned count=8){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::byte>(value>>(i*8));}
StoreIdentity next_id(){auto id=identity();id[7]=std::byte{98};return id;}
struct Location{std::uint64_t id=0,primary=0,peer=0,stored=0;};
std::vector<Location> locations(const Ram& ram) {
  require(get(ram.live,8,4)==2,"missing mirrored root capability");std::vector<Location> result;
  auto visit=[&](auto&& self,std::uint64_t page,unsigned depth)->void {
    require(depth<=16,"fixture tree depth");const auto count=get(ram.live,page+40,4),height=get(ram.live,page+12,4);
    for(std::uint64_t i=0;i<count;++i) {
      if(height)self(self,get(ram.live,page+64+i*112+40),depth+1);
      else {const auto at=page+64+i*72;result.push_back({get(ram.live,at),get(ram.live,at+16),get(ram.live,at+64),get(ram.live,at+28,4)});}
    }
  };
  visit(visit,get(ram.live,104),0);return result;
}
void lose_unit(Ram& ram,std::uint64_t unit) {
  const auto at=unit*ram.failure_unit;require(at<=ram.live.size() && ram.failure_unit<=ram.live.size()-at,"fixture unit bounds");
  std::fill_n(ram.live.begin()+static_cast<std::ptrdiff_t>(at),ram.failure_unit,std::byte{0});ram.stable=ram.live;
}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,unsigned layout) {
  auto objects=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  if(layout==1)return NativeFilesystemWriter::initialize_inode_records(std::move(objects));
  if(layout==2)return NativeFilesystemWriter::initialize_indexed_names(std::move(objects));
  return NativeFilesystemWriter::initialize(std::move(objects));
}
NativeFilesystemSnapshot view(const std::shared_ptr<Ram>& ram){return NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,limits));}
void packing_and_boundaries(std::uint32_t unit) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  std::vector<StoreMutation> changes;
  for(unsigned i=1;i<=96;++i)changes.push_back({i,false,data(i==1?kStoreMaxObjectBytes:(i==2?unit-257:i),i)});
  writer.commit(changes);auto snapshot=writer.snapshot();snapshot.verify_all();
  std::uint64_t stored=0;for(const auto& change:changes){require(snapshot.read(change.id)==change.bytes,"striped record boundary mismatch");stored+=change.bytes.size();}
  require(snapshot.info().mirrored_payloads && !snapshot.info().degraded_payloads && snapshot.info().stored_payload_bytes==2*stored,
      "mirror policy/accounting differs");
  const auto entries=locations(*ram);require(entries.size()==96,"mirrored leaf split inventory");
  require(entries[2].id==3 && entries[2].primary%unit==unit-1,"header-straddling boundary fixture missing");
  for(const auto& entry:entries)require(entry.peer==entry.primary+unit && (entry.primary/unit)%2==0,"payload units not independently paired");
  require(ram->extent()<2*stored+2*kStoreCommitUnit+16ULL*unit,"record packing retained per-object padding");
  std::cout<<"mirror_packing unit="<<unit<<" objects=96 encoded_bytes_one_copy="<<stored<<" image_bytes="<<ram->extent()<<'\n';
}
void survivor_and_rebuild(std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;const auto bytes=data(kNativeExtentBytes,42);
  {auto writer=PagedObjectStoreWriter::create_mirrored(original,identity(),limits,{StoreCompressionMode::raw,1,false});
    const StoreMutation change{7,false,bytes};writer.commit({&change,1});}
  const auto entry=locations(*original).front();
  const auto chunks=(entry.primary%unit+128+entry.stored+unit-1)/unit;
  for(bool peer:{false,true})for(std::uint64_t chunk=0;chunk<chunks;++chunk) {
    auto ram=image(original->stable,unit);lose_unit(*ram,(peer?entry.peer:entry.primary)/unit+2*chunk);
    const auto before=ram->live;auto source=PagedObjectStoreSnapshot::open(ram,limits);
    require(source.read(7)==bytes,"one payload unit loss lost bytes");source.verify_all();
    require(source.info().degraded_payloads,"payload loss not reported by scrub");
    rejects([&]{PagedObjectStoreWriter::resume(ram,identity(),source.info().generation,limits);},"degraded payload writable resume accepted");
    require(ram->live==before && ram->calls==0,"read/scrub/refused resume wrote source");
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto repaired=PagedObjectStoreWriter::rebuild(source,target,next_id(),limits,{StoreCompressionMode::raw,1,false});
    auto healed=repaired.snapshot();healed.verify_all();
    require(healed.read(7)==bytes && healed.info().mirrored_payloads && !healed.info().degraded_payloads,"new-image repair failed");
    require(ram->live==before && ram->calls==0,"repair modified source");
  }
  auto both=image(original->stable,unit);lose_unit(*both,entry.primary/unit);lose_unit(*both,entry.peer/unit);
  auto failed=PagedObjectStoreSnapshot::open(both,limits);
  rejects([&]{failed.read(7);},"both lost copies read as good data");rejects([&]{failed.verify_all();},"both lost copies scrubbed as healthy");
  auto empty=std::make_shared<Ram>();empty->failure_unit=unit;
  rejects([&]{PagedObjectStoreWriter::rebuild(failed,empty,next_id(),limits);},"unrecoverable source repaired with fabricated data");
  std::cout<<"mirror_survivor unit="<<unit<<" single_unit_losses="<<2*chunks<<'\n';
}
void native_recovery(unsigned layout,std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  {auto writer=fresh(original,layout);writer.apply(baseline());}
  const auto records=locations(*original);std::set<std::uint64_t> units;
  for(const auto& entry:records){units.insert(entry.primary/unit);units.insert(entry.peer/unit);}
  for(const auto lost:units) {
    auto ram=image(original->stable,unit);lose_unit(*ram,lost);const auto before=ram->live;
    auto source=view(ram);expected(source,false);source.verify_all();require(source.storage_info().degraded_checkpoint,"native payload degradation hidden");
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto repaired=NativeFilesystemWriter::compact(source,target,next_id(),{StoreCompressionMode::raw,1,false});
    expected(repaired.snapshot(),false);auto independent=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(target,limits));
    independent.verify_all();require(!independent.storage_info().degraded_checkpoint,"native repair stayed degraded");
    require(ram->calls==0 && ram->live==before,"native repair wrote source");
  }
  auto ram=image(original->stable,unit);auto source=PagedObjectStoreSnapshot::open(ram,limits);
  auto writer=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),source.info().generation,limits));
  auto target=std::make_shared<Ram>();target->failure_unit=unit;
  auto candidate=writer.prepare_paged_recompression(target,next_id());
  while(!writer.advance_paged_recompression(candidate)){}
  auto rebuilt=writer.finish_paged_recompression(candidate);expected(rebuilt.snapshot(),false);
  const auto pg=PagedObjectStoreSnapshot::open(target,limits);require(pg.info().mirrored_payloads,"staged native copy dropped protection");
  // A live writer can retain CPU-readable source views while a separately
  // owned staged candidate restores a subsequently lost payload bank.
  const auto lost=locations(*ram).front().primary/unit;lose_unit(*ram,lost);
  const auto damaged=ram->live;const auto calls=ram->calls;
  auto healing=std::make_shared<Ram>();healing->failure_unit=unit;auto recovery_id=next_id();recovery_id[6]=std::byte{44};
  auto repair=writer.prepare_paged_recompression(healing,recovery_id);
  while(!writer.advance_paged_recompression(repair)){}
  auto recovered=writer.finish_paged_recompression(repair);expected(recovered.snapshot(),false);
  auto checked=PagedObjectStoreSnapshot::open(healing,limits);checked.verify_all();
  require(!checked.info().degraded_payloads && checked.info().mirrored_payloads,"live staged repair retained bad copies");
  require(ram->live==damaged && ram->calls==calls,"live staged repair modified damaged source");
  expected(writer.snapshot(),false);
  std::cout<<"mirror_native layout="<<layout<<" unit="<<unit<<" damaged_start_units="<<units.size()<<'\n';
}
void fault_matrix(unsigned layout,std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  {auto writer=fresh(original,layout);writer.apply(baseline());}
  auto resume_native=[&](const std::shared_ptr<Ram>& ram) {
    const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
    return NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),generation,limits,{StoreCompressionMode::raw,1,false}));
  };
  auto probe=image(original->stable,unit);std::size_t cuts=0;
  {auto writer=resume_native(probe);const auto start=probe->calls;writer.apply(update());cuts=probe->calls-start;}
  require(cuts>3 && cuts<512,"mirrored transaction failure envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto ram=image(original->stable,unit);
    {auto writer=resume_native(ram);ram->failure=mode;ram->fail_at=ram->calls+cut;
      rejects([&]{writer.apply(update());},"faulted mirror commit acknowledged");require(writer.poisoned(),"indeterminate mirror writer remained active");}
    ram->live=ram->stable;ram->fail_at=(std::numeric_limits<std::size_t>::max)();
    auto recovered=view(ram);expected(recovered,recovered.sequence()==3);recovered.verify_all();
    auto continued=resume_native(ram);file_apply(continued,file_create("continue"));
    require(continued.snapshot().stat("continue").inode!=0,"mirrored recovery could not continue");++faults;
  }
  std::cout<<"mirror_faults layout="<<layout<<" unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
void forged_descriptors() {
  auto original=std::make_shared<Ram>();original->failure_unit=4096;
  {auto writer=PagedObjectStoreWriter::create_mirrored(original,identity(),limits,{StoreCompressionMode::raw,1,false});
    const StoreMutation object{7,false,data(65536,19)};writer.commit({&object,1});}
  for(unsigned variant=0;variant<5;++variant) {
    auto ram=image(original->stable,4096);const auto page=get(ram->live,104),peer=get(ram->live,112);
    auto pointer=get(ram->live,page+64+16);const auto at=page+64+64;
    if(variant==0)put(ram->live,at,0);
    if(variant==1)put(ram->live,at,pointer);
    if(variant==2)put(ram->live,at,pointer+8192);
    if(variant==3)put(ram->live,at,ram->extent()+4096);
    if(variant==4)put(ram->live,page+64+16,pointer+4096);
    const auto hash=detail::record_digest(std::span<const std::byte>(ram->live).subspan(page,4064));
    std::copy(hash.begin(),hash.end(),ram->live.begin()+page+4064);
    std::copy_n(ram->live.begin()+page,4096,ram->live.begin()+peer);
    for(const auto root:{std::uint64_t{0},kStoreCommitUnit}) {
      std::copy(hash.begin(),hash.end(),ram->live.begin()+root+144);
      const auto digest=detail::record_digest(std::span<const std::byte>(ram->live).subspan(root,4064));
      std::copy(digest.begin(),digest.end(),ram->live.begin()+root+4064);
    }
    ram->stable=ram->live;rejects([&]{PagedObjectStoreSnapshot::open(ram,limits);},"intact invalid mirror descriptor accepted");
    require(ram->calls==0,"malformed mirror reader wrote bytes");
  }
}
void fresh_replica_readback(std::uint32_t unit) {
  for(bool header:{false,true}) {
    auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
    auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
    const auto before=writer.snapshot().info();
    ram->corrupt_read_offset=before.physical_end+(header?unit:128);ram->corrupt_read_number=1;
    const StoreMutation object{7,false,data(65536,9)};
    rejects([&]{writer.commit({&object,1});},"fresh bad replica acknowledged");
    require(writer.poisoned(),"fresh replica failure did not fence writer");
    ram->corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)();
    auto selected=PagedObjectStoreSnapshot::open(ram,limits);
    require(selected.info().generation==before.generation && selected.info().objects==0,"bad replica published a root");
  }
}
void native_shared_lifetime() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram,2);const auto bytes=data(65536,54);
  writer.apply(std::array{file_create("a"),file_write("a",0,bytes),file_create("b"),file_write("b",0,bytes)});
  writer.coalesce_equal_extents();auto retained=writer.snapshot();
  file_apply(writer,file_remove("a"));file_apply(writer,file_write("b",19,Bytes{std::byte{11}}));
  require(retained.read("a",0,65536)==bytes && retained.read("b",0,65536)==bytes,"mirrored shared snapshot damaged");
  auto expected_bytes=bytes;expected_bytes[19]=std::byte{11};
  require(writer.snapshot().read("b",0,65536)==expected_bytes,"mirrored private update differs");
  PagedObjectStoreSnapshot::open(ram,limits).verify_all();
}
}
int main(){try {
  forged_descriptors();native_shared_lifetime();
  for(auto unit:{4096U,65536U}) {
    packing_and_boundaries(unit);survivor_and_rebuild(unit);fresh_replica_readback(unit);
    for(unsigned layout=0;layout<3;++layout){native_recovery(layout,unit);fault_matrix(layout,unit);}
  }
  std::cout<<"PASS: "<<checks<<" mirrored payload checks; "<<faults<<" injected write/barrier cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
