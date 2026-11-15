#include "native_fs_fixture.hpp"
#include <functional>
#include <iostream>
#include <optional>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool condition,const char* message) {
  ++checks;if(!condition)throw std::runtime_error(message);
}
template<class F> void rejects(F&& operation,const char* message) {
  ++checks;try{operation();}catch(const NativeFsError&){return;}
  throw std::runtime_error(message);
}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
void populate(NativeFilesystemWriter& fs,const Bytes& bytes) {
  const std::array<NativeFileMutation,4> operations{{
    file_create("a"),file_create("b"),file_write("a",0,bytes),file_write("b",0,bytes)}};
  fs.apply(operations);
}
void contents(const NativeFilesystemSnapshot& fs,const Bytes& a,const Bytes& b) {
  require(fs.read("a",0,kNativeMaxReadBytes)==a,"first shared file changed");
  require(fs.read("b",0,kNativeMaxReadBytes)==b,"second shared file changed");
  fs.verify_all();
}
void sharing_and_mutation(StoreCompressionMode mode) {
  const auto bytes=data(kNativeExtentBytes,77);
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,mode);populate(fs,bytes);
  auto prior=fs.snapshot();const auto old_sequence=prior.sequence();
  const auto first_inode=prior.stat("a").inode,second_inode=prior.stat("b").inode;
  const auto shared=fs.coalesce_equal_extents();
  require(shared.sequence==old_sequence+1 && shared.storage.objects==2,"equal files did not share one payload");
  contents(fs.snapshot(),bytes,bytes);contents(prior,bytes,bytes);
  require(fs.snapshot().stat("a").inode==first_inode && fs.snapshot().stat("b").inode==second_inode,
      "coalescing changed file identity");
  const auto calls=ram->calls,sequence=fs.snapshot().sequence();
  require(fs.coalesce_equal_extents().sequence==sequence && ram->calls==calls,"coalescing an existing group wrote again");
  auto retained=fs.snapshot();auto altered=bytes;altered[19]^=std::byte{1};
  const std::array<NativeFileMutation,3> mutate_and_revert{{
    file_write("a",19,Bytes{altered[19]}),file_write("b",7,Bytes{bytes[7]^std::byte{1}}),
    file_write("b",7,Bytes{bytes[7]})}};
  fs.apply(mutate_and_revert);
  require(fs.snapshot().storage_info().objects==3,"shared write did not detach private payload");
  contents(fs.snapshot(),altered,bytes);contents(retained,bytes,bytes);
  // Reversion in a single transaction may reattach only verified original bytes.
  const std::array<NativeFileMutation,2> revert{{file_write("b",3,Bytes{std::byte{1}}),
                                               file_write("b",3,Bytes{bytes[3]})}};
  fs.apply(revert);contents(fs.snapshot(),altered,bytes);
  file_apply(fs,file_write("a",0,bytes));fs.coalesce_equal_extents();
  require(fs.snapshot().storage_info().objects==2,"restored files did not coalesce");
  file_apply(fs,file_truncate("a",17));
  const Bytes short_bytes(bytes.begin(),bytes.begin()+17);contents(fs.snapshot(),short_bytes,bytes);
  file_apply(fs,file_truncate("a",kNativeExtentBytes));
  auto regrown=short_bytes;regrown.resize(kNativeExtentBytes);contents(fs.snapshot(),regrown,bytes);
  file_apply(fs,file_write("a",0,bytes));fs.coalesce_equal_extents();
  file_apply(fs,file_remove("a"));
  require(fs.snapshot().storage_info().objects==2 && fs.snapshot().read("b",0,kNativeMaxReadBytes)==bytes,
      "deleting first name removed a shared dependency");
  contents(retained,bytes,bytes);contents(prior,bytes,bytes);
  file_apply(fs,file_remove("b"));require(fs.snapshot().storage_info().objects==1,"last reference retained live payload");
}
void within_file_and_noop() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,StoreCompressionMode::raw);
  const auto empty_sequence=fs.snapshot().sequence(),empty_calls=ram->calls;
  require(fs.coalesce_equal_extents().sequence==empty_sequence && ram->calls==empty_calls,
      "empty coalescing changed legacy bytes");
  const auto block=data(kNativeExtentBytes,9);auto repeated=block;
  repeated.insert(repeated.end(),block.begin(),block.end());
  file_apply(fs,file_create("a"));file_apply(fs,file_write("a",0,repeated));
  fs.coalesce_equal_extents();require(fs.snapshot().storage_info().objects==2,"same-file equal extents did not share");
  const auto changed=data(29,2);file_apply(fs,file_write("a",kNativeExtentBytes+11,changed));
  std::copy(changed.begin(),changed.end(),repeated.begin()+kNativeExtentBytes+11);
  require(fs.snapshot().read("a",0,kNativeMaxReadBytes)==repeated,"same-file update changed another block");
  auto unrelated=std::make_shared<Ram>();auto unique=fresh(unrelated,StoreCompressionMode::raw);
  file_apply(unique,file_create("x"));file_apply(unique,file_write("x",0,block));
  const auto before=unrelated->live;const auto calls=unrelated->calls;
  unique.coalesce_equal_extents();
  require(unrelated->live==before && unrelated->calls==calls,"unique data upgraded or wrote metadata");
  file_apply(unique,file_create("y"));file_apply(unique,file_write("y",0,data(kNativeExtentBytes,10)));
  const auto different=unrelated->live;const auto different_calls=unrelated->calls;
  unique.coalesce_equal_extents();
  require(unrelated->live==different && unrelated->calls==different_calls,"different data shared a payload");
}
void compact_and_resume(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();const auto block=data(kNativeExtentBytes,71);
  std::optional<NativeFilesystemSnapshot> source,unshared;
  {auto fs=fresh(ram,mode);populate(fs,block);
    unshared.emplace(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable))));
    fs.coalesce_equal_extents();source.emplace(fs.snapshot());}
  auto target=std::make_shared<Ram>();auto id=identity();id[5]=std::byte{37};
  std::uint64_t generation=0;
  {
    auto copy=NativeFilesystemWriter::compact(*source,target,id,{mode,6,false});
    auto copied=copy.snapshot();contents(copied,block,block);
    require(copied.identity()==id && copied.sequence()==source->sequence() &&
        copied.stat("a").inode==source->stat("a").inode &&
        copied.stat("b").inode==source->stat("b").inode,"compaction changed namespace identity/sequence");
    require(copy.snapshot().storage_info().objects==2,"compaction expanded shared payload");
    auto control=std::make_shared<Ram>();auto control_id=id;control_id[6]=std::byte{1};
    auto ordinary=NativeFilesystemWriter::compact(*unshared,control,control_id,{mode,6,false});
    contents(ordinary.snapshot(),block,block);
    const auto plain=ordinary.snapshot().storage_info(),deduplicated=copied.storage_info();
    require(deduplicated.physical_end<plain.physical_end &&
        deduplicated.stored_payload_bytes<plain.stored_payload_bytes,
        "sharing did not reduce complete freshly compacted image cost");
    std::cout<<"Shared extent compacted size: {\"mode\":\""
        <<(mode==StoreCompressionMode::raw?"raw":"automatic")
        <<"\",\"source_bytes\":"<<2*kNativeExtentBytes
        <<",\"ordinary_image_bytes\":"<<plain.physical_end
        <<",\"shared_image_bytes\":"<<deduplicated.physical_end
        <<",\"ordinary_active_payload_bytes\":"<<plain.stored_payload_bytes
        <<",\"shared_active_payload_bytes\":"<<deduplicated.stored_payload_bytes<<"}\n";
    generation=copy.snapshot().storage_info().generation;
  }
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(target->stable)));
  contents(recovered,block,block);
  {
    auto resumed=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(target,id,generation,{mode,6,false}));
    auto altered=block;altered.back()^=std::byte{3};
    resumed.write_inode(resumed.snapshot().stat("b").inode,kNativeExtentBytes-1,Bytes{altered.back()});
    contents(resumed.snapshot(),block,altered);
  }
  contents(recovered,block,block);contents(*source,block,block);
}
void detached_lifetime() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,StoreCompressionMode::raw);
  const auto bytes=data(kNativeExtentBytes,81);populate(fs,bytes);fs.coalesce_equal_extents();
  const auto inode=fs.snapshot().stat("a").inode;
  fs.set_delete_pending(inode,true);fs.unlink_inode(inode);
  require(fs.snapshot().read_inode(inode,0,kNativeMaxReadBytes)==bytes,"detached shared inode lost data");
  auto changed=bytes;changed[0]^=std::byte{8};fs.write_inode(inode,0,Bytes{changed[0]});
  require(fs.snapshot().read_inode(inode,0,kNativeMaxReadBytes)==changed &&
      fs.snapshot().read("b",0,kNativeMaxReadBytes)==bytes,"detached write damaged visible sibling");
  fs.retire_inode(inode);
  require(fs.snapshot().read("b",0,kNativeMaxReadBytes)==bytes && fs.snapshot().storage_info().objects==2,
      "retirement removed live shared dependency");
}
void smallest_existing_encoding() {
  auto ram=std::make_shared<Ram>();const Bytes bytes(kNativeExtentBytes,std::byte{0x5a});
  {
    auto fs=fresh(ram,StoreCompressionMode::raw);
    const std::array<NativeFileMutation,3> ops{{file_create("a"),file_create("b"),file_write("a",0,bytes)}};
    fs.apply(ops);
  }
  const auto generation=ObjectStoreSnapshot::open(ram).info().generation;
  auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(ram,identity(),generation,
      {StoreCompressionMode::automatic,6,false}));
  file_apply(fs,file_write("b",0,bytes));auto old=fs.snapshot();
  const auto records=ObjectStoreSnapshot::open(ram).list();
  std::optional<StoreObjectInfo> smallest;std::uint32_t largest=0;
  for(const auto& record:records)if(record.id!=1) {
    if(!smallest || record.stored_payload_bytes<smallest->stored_payload_bytes)smallest=record;
    largest=(std::max)(largest,record.stored_payload_bytes);
  }
  require(smallest && smallest->stored_payload_bytes<largest,"fixture lacks different stored encodings");
  fs.coalesce_equal_extents();const auto shared=ObjectStoreSnapshot::open(ram).list();
  require(shared.size()==2,"mixed encodings did not coalesce");
  const auto chosen=std::find_if(shared.begin(),shared.end(),[](const auto& record){return record.id!=1;});
  require(chosen!=shared.end() && chosen->id==smallest->id &&
      chosen->stored_payload_bytes==smallest->stored_payload_bytes,"larger first encoding selected as shared base");
  contents(fs.snapshot(),bytes,bytes);contents(old,bytes,bytes);
}
std::uint64_t integer(const Bytes& bytes,std::size_t at,unsigned width) {
  if(at>bytes.size() || width>bytes.size()-at)throw std::runtime_error("fixture field bounds");
  std::uint64_t result=0;for(unsigned i=0;i<width;++i)
    result|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
  return result;
}
void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::byte>(value>>(8*i));
}
void malicious_namespace() {
  auto ram=std::make_shared<Ram>();const auto block=data(kNativeExtentBytes,3);
  {auto fs=fresh(ram,StoreCompressionMode::raw);populate(fs,block);fs.coalesce_equal_extents();}
  for(unsigned attack=0;attack<4;++attack) {
    auto target=image(ram->stable);auto snapshot=ObjectStoreSnapshot::open(target);auto metadata=snapshot.read(1);
    require(integer(metadata,8,4)==7,"shared namespace capability absent");
    if(attack==0){metadata[7]=std::byte{'6'};put(metadata,8,6,4);} // Legacy formats still forbid aliases.
    if(attack==1){metadata[7]=std::byte{'8'};put(metadata,8,8,4);} // Unknown format.
    if(attack>=2) {
      std::size_t cursor=64;bool edited=false;
      for(std::uint64_t i=0;i<integer(metadata,12,4);++i) {
        const auto extents=integer(metadata,cursor+50,2),names=integer(metadata,cursor+48,2);
        const auto extent_at=cursor+104+static_cast<std::size_t>(names);
        if(integer(metadata,cursor,8)==3) {
          if(attack==2)put(metadata,extent_at+8,0,8); // Missing/invalid dependency.
          else put(metadata,cursor+24,1,8); // Shared nonzero bytes beyond EOF.
          edited=true;break;
        }
        cursor=extent_at+static_cast<std::size_t>(extents)*16;
      }
      require(edited,"malicious fixture inode missing");
    }
    {
      auto writer=ObjectStoreWriter::resume(target,identity(),snapshot.info().generation,{StoreCompressionMode::raw,6,false});
      const StoreMutation mutation{1,false,metadata};writer.commit_replicated({&mutation,1});
    }
    rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(target));},"invalid shared namespace exposed");
  }
}
void interrupted_transactions(StoreCompressionMode mode,bool after_sharing) {
  const auto bytes=data(kNativeExtentBytes,22);auto altered=bytes;altered[7]^=std::byte{4};
  auto seed=std::make_shared<Ram>();
  {auto fs=fresh(seed,mode);populate(fs,bytes);if(after_sharing)fs.coalesce_equal_extents();}
  const auto baseline=seed->stable;
  auto perform=[&](NativeFilesystemWriter& fs){
    if(after_sharing)file_apply(fs,file_write("a",7,Bytes{altered[7]}));else fs.coalesce_equal_extents();
  };
  auto probe=image(baseline);std::size_t steps=0;
  {
    const auto info=ObjectStoreSnapshot::open(probe).info();
    auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(probe,identity(),info.generation,{mode,6,false}));
    probe->calls=0;perform(fs);steps=probe->calls;
  }
  require(steps>0,"fault campaign had no publication operations");
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    auto target=image(baseline);
    {
      const auto info=ObjectStoreSnapshot::open(target).info();
      auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(target,identity(),info.generation,{mode,6,false}));
      target->calls=0;target->fail_at=step;target->failure=fault;
      rejects([&]{perform(fs);},"failed shared transaction acknowledged");
      require(fs.poisoned(),"failed shared writer was not fenced");
      const auto calls=target->calls;
      rejects([&]{fs.coalesce_equal_extents();},"fenced coalescing retried");
      require(target->calls==calls,"fenced writer performed I/O");
    }
    auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(target->stable)));
    const auto actual=recovered.read("a",0,kNativeMaxReadBytes);
    require(actual==bytes || (after_sharing && actual==altered),"interrupted publication exposed partial data");
    require(recovered.read("b",0,kNativeMaxReadBytes)==bytes,"failed detachment damaged sibling");
    recovered.verify_all();
  }
}
}
int main() {
  try {
    within_file_and_noop();detached_lifetime();smallest_existing_encoding();malicious_namespace();
    for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
      sharing_and_mutation(mode);compact_and_resume(mode);
      interrupted_transactions(mode,false);interrupted_transactions(mode,true);
    }
    std::cout<<"PASS: "<<checks<<" shared-extent checks; exact equality, private updates, deletion, compaction, legacy rejection and interrupted publication.\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<"shared-extent failure after "<<checks<<": "<<error.what()<<'\n';return 1;}
}
