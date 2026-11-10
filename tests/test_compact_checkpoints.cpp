#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,const char* why){
  ++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
StoreIdentity another(unsigned value){auto id=identity();id[1]=static_cast<std::byte>(value);return id;}
std::uint32_t u32(const Bytes& bytes,std::size_t at){
  std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(std::to_integer<unsigned>(bytes.at(at+i)))<<(8*i);return value;
}
std::uint64_t u64(const Bytes& bytes,std::size_t at){
  std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t(std::to_integer<unsigned>(bytes.at(at+i)))<<(8*i);return value;
}
void put32(Bytes& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.at(at+i)=static_cast<std::byte>(value>>(8*i));}
void put64(Bytes& bytes,std::size_t at,std::uint64_t value){for(unsigned i=0;i<8;++i)bytes.at(at+i)=static_cast<std::byte>(value>>(8*i));}
void root_digest(Bytes& bytes,std::size_t at){
  const auto digest=detail::record_digest(std::span<const std::byte>(bytes).subspan(at,4064));
  std::copy(digest.begin(),digest.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at+4064));
}
std::shared_ptr<Ram> ram(std::uint32_t unit){auto result=std::make_shared<Ram>();result->failure_unit=unit;return result;}
ObjectStoreWriter fresh(const std::shared_ptr<Ram>& io,StoreCompressionMode mode,bool compact=true){
  return ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact});
}
std::array<StoreMutation,2> seed(){return {{{1,false,data(8179,31)},{2,false,data(4099,32)}}};}
Bytes changed(){Bytes bytes(65537);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>(i%7);return bytes;}
std::array<StoreMutation,2> update(){return {{{1,false,changed()},{3,false,Bytes(8192)}}};}
void inventory(const ObjectStoreSnapshot& view,bool newer,bool continued=false){
  require(view.read(1)==(newer?changed():data(8179,31)) && view.read(2)==data(4099,32),
      "compact checkpoint lost acknowledged/replaced bytes");
  if(newer)require(view.read(3)==Bytes(8192),"compact checkpoint omitted candidate object");
  else rejects([&]{(void)view.read(3);},"compact checkpoint fabricated candidate object");
  require(view.list().size()==(newer?3U:2U)+(continued?1U:0U),"compact checkpoint inventory differs");
  if(continued)require(view.read(4)==data(31,33),"compact checkpoint recovery continuation lost");
}
void geometry(const Ram& io,const StoreCommitInfo& info,std::uint32_t unit){
  const auto base=static_cast<std::size_t>(info.generation%2*kStoreCommitUnit);
  const auto index=u64(io.stable,base+32),peer=u64(io.stable,base+96);
  const auto span=(u32(io.stable,base+40)+unit-1)/unit*unit;
  require(u32(io.stable,base+8)==3 && u32(io.stable,base+112)==unit &&
      index%unit==0 && peer%unit==0 && (std::max)(index,peer)-(std::min)(index,peer)==span &&
      (std::max)(index,peer)+span==info.physical_end && info.physical_end%unit==0,
      "compact metadata geometry/root fields differ");
}
void invariants(){
  for(const auto unit:{512U,4096U,8192U,16384U,32768U,65536U}) {
    auto io=ram(unit);auto writer=fresh(io,StoreCompressionMode::raw);
    writer.commit(seed());const auto info=writer.snapshot().info();geometry(*io,info,(std::max)(unit,4096U));
    require(info.replicated_checkpoint && !info.degraded_checkpoint,"compact preference did not publish two current roots");
    auto old=writer.snapshot();writer.commit(update());inventory(old,false);inventory(writer.snapshot(),true);
  }
  // Multi-unit manifests must keep both complete replicas independent.
  auto full=ram(4096);auto writer=fresh(full,StoreCompressionMode::raw);
  std::vector<StoreMutation> all;
  for(std::uint64_t id=1;id<=kStoreMaxObjects;++id)all.push_back({id,false,{static_cast<std::byte>(id)}});
  writer.commit(all);auto view=writer.snapshot();geometry(*full,view.info(),4096);
  const auto slot=static_cast<std::size_t>(view.info().generation%2*kStoreCommitUnit);
  require(u32(full->stable,slot+40)==16448 &&
      (std::max)(u64(full->stable,slot+32),u64(full->stable,slot+96))-
      (std::min)(u64(full->stable,slot+32),u64(full->stable,slot+96))==20480,
      "full compact manifest span was truncated to one failure unit");
  for(const auto& object:view.list())require(view.read(object.id)==Bytes{static_cast<std::byte>(object.id)},
      "full compact manifest redirected an object");
  // Digest-valid unsupported or contradictory metadata is never a torn-root fallback.
  for(unsigned mutation=0;mutation<6;++mutation) {
    auto bytes=full->stable;
    if(mutation==0)put32(bytes,slot+8,99);
    if(mutation==1)put32(bytes,slot+112,2048);
    if(mutation==2)put32(bytes,slot+112,65537);
    if(mutation==3)bytes.at(slot+116)=std::byte{1};
    if(mutation==4)put64(bytes,slot+96,u64(bytes,slot+96)+4096);
    if(mutation==5)put64(bytes,slot+24,u64(bytes,slot+24)-1);
    root_digest(bytes,slot);
    rejects([&]{(void)ObjectStoreSnapshot::open(image(bytes));},"intact malformed compact root rolled back");
  }
  auto wide=ram(65536);{
    auto w=fresh(wide,StoreCompressionMode::raw);w.commit(seed());
  }
  auto bytes=wide->stable;const auto latest=ObjectStoreSnapshot::open(image(bytes)).info();
  const auto root=static_cast<std::size_t>(latest.generation%2*kStoreCommitUnit);
  put32(bytes,root+8,2);put32(bytes,root+112,0);root_digest(bytes,root);
  rejects([&]{(void)ObjectStoreSnapshot::open(image(bytes));},"intact compact root downgraded to fixed layout");
  // Read-only admission does not require a write contract; writable resume does.
  auto small=ram(4096);{auto w=fresh(small,StoreCompressionMode::raw);w.commit(seed());}
  auto wrong=image(small->stable,65536);auto admitted=ObjectStoreSnapshot::open(wrong);
  inventory(admitted,false);const auto calls=wrong->calls,flushes=wrong->flushes;
  rejects([&]{(void)ObjectStoreWriter::resume(wrong,identity(),admitted.info().generation);},
      "incompatible backend resumed compact metadata");
  require(wrong->calls==calls && wrong->flushes==flushes,"incompatible resume wrote/flushed");
  auto drift=ram(4096);auto drifting=fresh(drift,StoreCompressionMode::raw);drifting.commit(seed());
  const auto before=drift->calls;drift->failure_unit=65536;
  rejects([&]{drifting.commit(update());},"changed failure unit admitted");
  require(drifting.poisoned() && drift->calls==before,"changed contract did not fence before writes");
}
void losses(StoreCompressionMode mode,std::uint32_t unit){
  auto source=ram(unit);StoreCommitInfo info;
  {auto w=fresh(source,mode);w.commit(seed());w.commit(update());info=w.snapshot().info();}
  const auto selected=static_cast<std::size_t>(info.generation%2*kStoreCommitUnit);
  const std::array<std::uint64_t,4> victims{{0,kStoreCommitUnit,u64(source->stable,selected+32),u64(source->stable,selected+96)}};
  for(const auto offset:victims) {
    auto damaged=image(source->stable,unit);
    std::fill_n(damaged->live.begin()+static_cast<std::ptrdiff_t>(offset),unit,std::byte{0});
    damaged->stable=damaged->live;
    auto recovered=ObjectStoreSnapshot::open(damaged);inventory(recovered,true);
    require(recovered.info().degraded_checkpoint,"lost metadata copy not reported");
    auto w=ObjectStoreWriter::resume(damaged,identity(),recovered.info().generation,{mode,6,false});
    require(!w.snapshot().info().degraded_checkpoint,"compact repair did not restore redundancy");
    const StoreMutation next{4,false,data(31,33)};w.commit({&next,1});
    inventory(ObjectStoreSnapshot::open(image(damaged->stable)),true,true);
  }
}
void faults(StoreCompressionMode mode,std::uint32_t unit,bool repair){
  auto probe=ram(unit);std::size_t steps=0;
  auto prepare=[&](const std::shared_ptr<Ram>& io) {
    {auto w=fresh(io,mode);w.commit(seed());}
    if(repair) {
      const auto view=ObjectStoreSnapshot::open(io);const auto slot=view.info().generation%2*kStoreCommitUnit;
      const auto index=u64(io->stable,static_cast<std::size_t>(slot+32));
      std::fill_n(io->live.begin()+static_cast<std::ptrdiff_t>(index),unit,std::byte{0});io->stable=io->live;
    }
  };
  prepare(probe);
  if(repair) {
    const auto generation=ObjectStoreSnapshot::open(probe).info().generation;probe->calls=0;
    {auto w=ObjectStoreWriter::resume(probe,identity(),generation,{mode,6,false});}
    steps=probe->calls;
  } else {
    auto w=ObjectStoreWriter::resume(probe,identity(),ObjectStoreSnapshot::open(probe).info().generation,{mode,6,false});
    probe->calls=0;w.commit(update());steps=probe->calls;
  }
  require(steps>3 && steps<100,"compact fault trace out of bound");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto io=ram(unit);prepare(io);auto before=ObjectStoreSnapshot::open(io);const auto generation=before.info().generation;
    if(repair) {
      io->calls=0;io->fail_at=step;io->failure=fault;
      rejects([&]{(void)ObjectStoreWriter::resume(io,identity(),generation,{mode,6,false});},
          "interrupted compact repair returned writer");
    } else {
      {auto w=ObjectStoreWriter::resume(io,identity(),generation,{mode,6,false});
        io->calls=0;io->fail_at=step;io->failure=fault;
        rejects([&]{w.commit(update());},"interrupted compact commit acknowledged");
        require(w.poisoned(),"interrupted compact writer not fenced");
        const auto calls=io->calls;rejects([&]{w.commit(update());},"poisoned compact writer retried");
        require(io->calls==calls,"poisoned compact writer wrote");
      }
    }
    inventory(before,false);
    auto reopened=image(io->stable,unit);auto recovered=ObjectStoreSnapshot::open(reopened);
    const bool newer=!repair && recovered.info().generation!=generation;
    inventory(recovered,newer);
    if(!repair)require(recovered.info().generation>=generation && recovered.info().generation<=generation+2,
        "compact failure fabricated/lost publication generation");
    auto resumed=ObjectStoreWriter::resume(reopened,identity(),recovered.info().generation,{mode,6,false});
    const StoreMutation continuation{4,false,data(31,33)};resumed.commit({&continuation,1});
    inventory(ObjectStoreSnapshot::open(image(reopened->stable)),newer,true);
  }
  require(cases==3*steps,"compact metadata omitted a fault point");
  std::cout<<"Compact checkpoint faults: {\"schema\":1,\"unit\":"<<unit
      <<",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"phase\":\""<<(repair?"repair":"commit")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
Bytes sample(){
  Bytes bytes(192*1024+17);for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i/31+i%251)%256);return bytes;
}
std::shared_ptr<Ram> native_image(StoreCompressionMode mode,bool compact){
  auto io=ram(4096);
  auto writer=NativeFilesystemWriter::initialize(fresh(io,mode,compact));
  const std::array<NativeFileMutation,9> initial{{file_create("discard"),file_create("Data",true),
      file_create("scratch.bin"),file_create("Data/other.txt"),file_create("empty.bin"),file_create("sparse.bin"),
      file_write("scratch.bin",0,sample()),file_truncate("sparse.bin",(1ULL<<32)+65536),
      file_write("sparse.bin",(1ULL<<32)+65536-17,Bytes(17,std::byte{0x5b}))}};
  writer.apply(initial);auto remove=file_remove("discard"),rename=file_rename("scratch.bin","Data/sample.bin");
  const std::array<NativeFileMutation,2> final{{remove,rename}};
  const std::array<NativeBasicMutation,3> basic{{
      {4,0,{133700000000000011ULL,133700000000000022ULL,133700000000000033ULL,133700000000000044ULL,0x2021}},
      {1,0,{133600000000000011ULL,133600000000000022ULL,133600000000000033ULL,133600000000000044ULL,0x10}},
      {5,0,{0,0,0,0,0x22}}}};
  writer.apply_with_basic(final,basic);return io;
}
void native_parity(const NativeFilesystemSnapshot& view){
  require(view.sequence()==3 && view.stat("Data/sample.bin").inode==4 &&
      view.read("Data/sample.bin",0,300000)==sample() && view.read("empty.bin",0,1).empty() &&
      view.stat("sparse.bin").logical_bytes==(1ULL<<32)+65536 &&
      view.read("sparse.bin",(1ULL<<32)+65536-17,17)==Bytes(17,std::byte{0x5b}) &&
      view.basic_metadata(4)==NativeBasicMetadata{133700000000000011ULL,133700000000000022ULL,
          133700000000000033ULL,133700000000000044ULL,0x2021},"compact native profile bytes/identity/basic differs");
}
std::uint64_t file_payload_bytes(const std::shared_ptr<Ram>& io){
  auto objects=ObjectStoreSnapshot::open(io);std::uint64_t bytes=0;
  for(const auto& object:objects.list())if(object.id!=1)bytes+=object.stored_payload_bytes;
  return bytes;
}
void profiles(){
  for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto fixed=native_image(mode,false),dense=native_image(mode,true);
    const auto source_bytes=fixed->stable;
    auto source=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(fixed));
    auto compact=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(dense));
    native_parity(source);native_parity(compact);
    auto old_copy=ram(4096),new_copy=ram(4096);
    auto a=NativeFilesystemWriter::compact(source,old_copy,another(111),{mode,6,false});
    auto b=NativeFilesystemWriter::compact(source,new_copy,another(112),{mode,6,false},{true});
    native_parity(a.snapshot());native_parity(b.snapshot());
    require(dense->stable.size()<fixed->stable.size() && new_copy->stable.size()<old_copy->stable.size() &&
        fixed->stable==source_bytes && source.storage_info().stored_payload_bytes==compact.storage_info().stored_payload_bytes &&
        file_payload_bytes(old_copy)==file_payload_bytes(new_copy),
        "compact metadata saved no physical image bytes or changed payload/source");
    const auto fixed_copy_bytes=old_copy->stable.size(),compact_copy_bytes=new_copy->stable.size();
    const auto fixed_copy_payload=a.snapshot().storage_info().stored_payload_bytes;
    const auto compact_copy_payload=b.snapshot().storage_info().stored_payload_bytes;
    auto moved=std::move(b);const auto op=file_write("Data/other.txt",0,data(31,34));moved.apply({&op,1});
    require(moved.snapshot().read("Data/other.txt",0,40)==data(31,34) && fixed->stable==source_bytes,
        "compact native continuation lost bytes or changed source");
    std::cout<<"Compact checkpoint profile: {\"schema\":1,\"mode\":\""
        <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"unit\":4096,\"fixed_bytes\":"<<fixed->stable.size()
        <<",\"compact_bytes\":"<<dense->stable.size()<<",\"fixed_copy_bytes\":"<<fixed_copy_bytes
        <<",\"compact_copy_bytes\":"<<compact_copy_bytes
        <<",\"fixed_copy_live_payload_bytes\":"<<fixed_copy_payload
        <<",\"compact_copy_live_payload_bytes\":"<<compact_copy_payload<<",\"live_stored_payload_bytes\":"
        <<source.storage_info().stored_payload_bytes<<",\"live_decoded_bytes\":"<<source.storage_info().logical_bytes
        <<",\"source_unchanged\":true,\"complete\":true}\n";
  }
}
}
int main(int argc,char** argv){try{
  if(argc!=2)throw std::runtime_error("one bounded compact-checkpoint group required");
  const std::string_view group=argv[1];
  if(group=="--invariants"){
    invariants();for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(const auto unit:{4096U,16384U,65536U})losses(mode,unit);
    profiles();std::cout<<"Compact checkpoint invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  }else{
    const bool repair=group.starts_with("--repair-");
    const auto unit=group.ends_with("4096")?4096U:group.ends_with("16384")?16384U:group.ends_with("65536")?65536U:
        throw std::runtime_error("unknown compact fault unit");
    require(group.starts_with("--commit-") || repair,"unknown compact fault group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})faults(mode,unit,repair);
  }
  return 0;
}catch(const std::exception& error){std::cerr<<"Compact checkpoint failure: "<<error.what()<<"\n";return 1;}}
