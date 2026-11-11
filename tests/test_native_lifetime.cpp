#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;std::size_t fault_cases=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,NativeFsErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeFsError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
std::uint64_t get(const Bytes& bytes,std::size_t at,unsigned width=8) {
  std::uint64_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes.at(at+i)))<<(8*i);
  return value;
}
void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned width=8) {
  for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::byte>(value>>(8*i));
}
std::map<std::uint64_t,std::size_t> fields(const Bytes& bytes) {
  std::map<std::uint64_t,std::size_t> result;std::size_t cursor=64;
  for(std::uint64_t i=0;i<get(bytes,12,4);++i) {
    result.emplace(get(bytes,cursor),cursor);
    cursor+=64+static_cast<std::size_t>(get(bytes,cursor+48,2))+16*static_cast<std::size_t>(get(bytes,cursor+50,2));
  }
  return result;
}
void lifetime(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);const auto content=data(65543,71);
  const std::array<NativeFileMutation,5> setup{{file_create("D",true),file_create("D/f"),
      file_write("D/f",0,content),file_create("keep"),file_write("keep",0,data(91,72))}};
  writer.apply(setup);auto named=writer.snapshot();const auto inode=named.stat("D/f").inode;
  const auto directory=named.stat("D").inode;
  auto metadata=ObjectStoreSnapshot::open(ram).read(1);
  require(get(metadata,8,4)==1 && metadata[7]==std::byte{'1'} && !named.needs_session_recovery(),
      "ordinary namespace changed legacy capability");
  auto calls=ram->calls;const auto bytes=ram->live;const auto sequence=named.sequence();
  writer.set_delete_pending(inode,false);
  require(ram->calls==calls && ram->live==bytes && writer.snapshot().sequence()==sequence,
      "clearing absent delete intent published/changed legacy bytes");
  const auto barriers=ram->flushes;writer.set_delete_pending(inode,true);
  auto pending=writer.snapshot();const auto state=pending.inode_state(inode);
  require(state.delete_pending && !state.unlinked && state.info.path=="D/f" &&
      pending.read_inode(inode,0,100000)==content && pending.needs_session_recovery(),
      "delete eligibility removed name/data instead of persisting intent");
  require(ram->flushes==barriers+3,"delete intent weakened publication barriers");
  metadata=ObjectStoreSnapshot::open(ram).read(1);
  require(get(metadata,8,4)==2 && metadata[7]==std::byte{'2'},"lifetime capability was not explicit");
  calls=ram->calls;writer.set_delete_pending(inode,true);
  require(ram->calls==calls,"duplicate delete intent was not a no-op");
  const auto untouched=ram->live;
  rejects([&]{file_apply(writer,file_rename("D/f","g"));},NativeFsErrorCode::invalid_operation,
      "pending inode renamed outside lifetime protocol");
  rejects([&]{file_apply(writer,file_remove("D/f"));},NativeFsErrorCode::invalid_operation,
      "pending inode discarded outside cleanup");
  rejects([&]{writer.retire_inode(inode);},NativeFsErrorCode::invalid_operation,"linked inode was retired");
  rejects([&]{writer.set_delete_pending(1,true);},NativeFsErrorCode::invalid_operation,"root delete intent accepted");
  rejects([&]{writer.set_delete_pending(directory,true);},NativeFsErrorCode::not_empty,"nonempty directory became pending");
  require(ram->live==untouched && ram->calls==calls && !writer.poisoned(),"lifetime admission performed writes");
  writer.set_delete_pending(inode,false);
  require(!writer.snapshot().needs_session_recovery() &&
      writer.snapshot().read("D/f",0,100000)==content,"intent cancellation lost file");
  // Retirement cannot be inferred from time, loss of a name or a CPU snapshot.
  rejects([&]{writer.unlink_inode(inode);},NativeFsErrorCode::invalid_operation,"unlink without intent");
  writer.set_delete_pending(inode,true);writer.unlink_inode(inode);
  auto detached=writer.snapshot();const auto gone=detached.inode_state(inode);
  require(gone.unlinked && !gone.delete_pending && gone.info.parent==0 && gone.info.path.empty() &&
      gone.info.inode==inode && detached.list("D").empty() && detached.needs_session_recovery(),
      "detached inode retained a reachable name or lost its identity");
  rejects([&]{(void)detached.stat("D/f");},NativeFsErrorCode::not_found,"detached name remained visible");
  require(detached.read_inode(inode,0,100000)==content &&
      detached.storage_info().objects==pending.storage_info().objects,"unlink discarded retained extent objects");
  calls=ram->calls;writer.unlink_inode(inode);
  require(ram->calls==calls,"repeated cleanup discarded or republished detached inode");
  rejects([&]{writer.set_delete_pending(inode,false);},NativeFsErrorCode::invalid_operation,"detached inode was resurrected");
  const auto replacement=file_apply(writer,file_create("D/f")).created_inodes[0];
  file_apply(writer,file_write("D/f",0,data(7,73)));
  const auto suffix=data(17,74);const auto result=writer.write_inode(inode,UINT64_MAX,suffix,{true,false});
  auto expected=content;expected.insert(expected.end(),suffix.begin(),suffix.end());
  require(result.offset==content.size() && result.bytes==suffix.size() &&
      writer.snapshot().read_inode(inode,0,100000)==expected &&
      writer.snapshot().read("D/f",0,100)==data(7,73),"detached inode I/O redirected to reused pathname");
  writer.truncate_inode(inode,13);writer.truncate_inode(inode,65550);
  expected.resize(13);expected.resize(65550);
  require(writer.snapshot().read_inode(inode,0,100000)==expected,"detached truncate/regrow exposed old bytes");
  auto before_close=writer.snapshot();writer.retire_inode(inode);
  auto closed=writer.snapshot();
  rejects([&]{(void)closed.inode_state(inode);},NativeFsErrorCode::not_found,"retired inode still live");
  require(!closed.needs_session_recovery() && closed.stat("D/f").inode==replacement &&
      closed.read("keep",0,100)==data(91,72),"retirement changed replacement/unrelated data");
  require(named.read_inode(inode,0,100000)==content && detached.read_inode(inode,0,100000)==content &&
      before_close.read_inode(inode,0,100000)==expected,"retirement damaged retained snapshots");
  rejects([&]{(void)writer.write_inode(inode,0,suffix);},NativeFsErrorCode::not_found,"retired write redirected");
  // Empty directory lifetime and blocked creation while deletion is pending.
  const auto empty=file_apply(writer,file_create("empty",true)).created_inodes[0];
  writer.set_delete_pending(empty,true);calls=ram->calls;
  rejects([&]{file_apply(writer,file_create("empty/child"));},NativeFsErrorCode::invalid_operation,
      "child created under pending directory");
  require(ram->calls==calls,"pending directory rejection wrote backing bytes");
  writer.set_delete_pending(empty,false);file_apply(writer,file_create("empty/child"));
  file_apply(writer,file_remove("empty/child"));writer.set_delete_pending(empty,true);writer.unlink_inode(empty);
  require(writer.snapshot().list_inode(empty).empty(),"detached directory has children");
  writer.retire_inode(empty);
  require(get(ObjectStoreSnapshot::open(ram).read(1),8,4)==2,"cleared lifecycle capability silently downgraded");
  NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable))).verify_all();
}
void recovery_admission(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();std::uint64_t a=0,b=0,keep=0,seq=0;std::optional<NativeFilesystemSnapshot> old;
  {
    auto writer=fresh(ram,mode);
    const std::array<NativeFileMutation,6> setup{{file_create("a"),file_write("a",0,data(17,81)),
        file_create("b"),file_write("b",0,data(23,82)),file_create("keep"),file_write("keep",0,data(31,83))}};
    const auto ids=writer.apply(setup).created_inodes;a=ids[0];b=ids[1];keep=ids[2];
    writer.set_delete_pending(a,true);writer.set_delete_pending(b,true);writer.unlink_inode(b);
    old.emplace(writer.snapshot());seq=old->sequence();
  } // No native writer survives; CPU snapshots are immutable retained readers.
  {
    auto objects=ObjectStoreWriter::resume(ram,identity(),old->storage_info().generation,{mode,6,false});
    const auto before=ram->live;const auto calls=ram->calls;
    rejects([&]{(void)NativeFilesystemWriter::resume(std::move(objects));},NativeFsErrorCode::invalid_operation,
        "ordinary admission guessed that old file contexts had retired");
    require(ram->live==before && ram->calls==calls,"ordinary lifetime rejection published deletion");
  }
  auto id=identity();id[7]=std::byte{1};auto target=std::make_shared<Ram>();
  rejects([&]{(void)NativeFilesystemWriter::compact(*old,target,id,{mode,6,false});},
      NativeFsErrorCode::invalid_operation,"unresolved file lifetimes compacted");
  require(target->live.empty(),"unresolved compaction touched destination");
  {
    const auto current=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(ram,identity(),
        current.storage_info().generation,{mode,6,false}));
    auto recovered=writer.snapshot();
    require(recovered.sequence()==seq+1 && !recovered.needs_session_recovery() &&
        recovered.list().size()==1 && recovered.stat("keep").inode==keep &&
        recovered.read_inode(keep,0,100)==data(31,83),"closed-session recovery deleted unrelated data");
    rejects([&]{(void)recovered.stat_inode(a);},NativeFsErrorCode::not_found,"pending deletion survived retired session");
    rejects([&]{(void)recovered.stat_inode(b);},NativeFsErrorCode::not_found,"detached inode survived retired session");
    const auto suffix=data(3,84);writer.write_inode(keep,0,suffix,{true,false});
  }
  require(old->read_inode(a,0,100)==data(17,81) && old->read_inode(b,0,100)==data(23,82),
      "closed-session completion destroyed retained CPU snapshot bytes");
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  auto content=data(31,83);const auto suffix=data(3,84);content.insert(content.end(),suffix.begin(),suffix.end());
  require(reopened.read("keep",0,100)==content && !reopened.needs_session_recovery(),"continued closed-session recovery failed");
  auto plain=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(ram,identity(),
      reopened.storage_info().generation,{mode,6,false}));
  plain.snapshot().verify_all();
}
void detached_capacity(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
  const auto held=file_apply(writer,file_create("held")).created_inodes[0];
  file_apply(writer,file_write("held",0,data(17,87)));
  writer.set_delete_pending(held,true);writer.unlink_inode(held);auto retained=writer.snapshot();
  for(std::size_t count=0;count<kNativeMaxNodes-2;) {
    std::vector<NativeFileMutation> batch;
    for(unsigned i=0;i<16 && count<kNativeMaxNodes-2;++i,++count)
      batch.push_back(file_create("n"+std::to_string(count)));
    writer.apply(batch);
  }
  auto view=writer.snapshot();
  require(view.list().size()==kNativeMaxNodes-2 && view.inode_state(held).unlinked &&
      view.storage_info().objects==2,"detached inode/payload escaped aggregate accounting");
  const auto before=ram->live;const auto calls=ram->calls;
  rejects([&]{file_apply(writer,file_create("overflow"));},NativeFsErrorCode::limit,
      "detached inode did not consume namespace capacity");
  require(ram->live==before && ram->calls==calls && !writer.poisoned(),"detached capacity rejection wrote backing");
  writer.retire_inode(held);const auto created=file_apply(writer,file_create("now-fits")).created_inodes[0];
  require(created>held && writer.snapshot().list().size()==kNativeMaxNodes-1 &&
      retained.read_inode(held,0,100)==data(17,87),"retirement did not release logical slot or damaged snapshot");
}
void malformed_metadata() {
  auto ram=std::make_shared<Ram>();
  {
    auto writer=fresh(ram,StoreCompressionMode::raw);
    const std::array<NativeFileMutation,5> setup{{file_create("D",true),file_create("D/f"),
        file_write("D/f",0,data(13,91)),file_create("empty",true),file_create("outside")}};
    writer.apply(setup);writer.set_delete_pending(3,true);writer.unlink_inode(3);
    writer.set_delete_pending(4,true);writer.unlink_inode(4);
  }
  const auto original=ram->stable;
  const auto objects=ObjectStoreSnapshot::open(image(original));const auto metadata=objects.read(1);
  const auto positions=fields(metadata);const auto orphan=positions.at(3),directory=positions.at(2);
  auto corrupt=[&](std::function<void(Bytes&)> edit) {
    auto disk=image(original);
    {
      auto writer=ObjectStoreWriter::resume(disk,identity(),objects.info().generation);
      auto changed=metadata;edit(changed);const StoreMutation mutation{1,false,std::move(changed)};
      writer.commit({&mutation,1});
    }
    ++checks;bool rejected=false;
    try{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(disk));}
    catch(const NativeFsError&){rejected=true;}
    require(rejected,"digest-valid malformed lifetime metadata admitted");
  };
  corrupt([&](auto& b){put(b,orphan+20,4,4);});
  corrupt([&](auto& b){put(b,orphan+20,3,4);});
  corrupt([&](auto& b){put(b,64+20,1,4);});
  corrupt([&](auto& b){put(b,64+20,2,4);});
  corrupt([&](auto& b){put(b,orphan+8,2);});
  corrupt([&](auto& b){put(b,orphan+20,0,4);});
  corrupt([&](auto& b){put(b,positions.at(5)+8,4);}); // Even a detached DIRECTORY cannot parent a linked child.
  corrupt([](auto& b){put(b,8,1,4);});
  corrupt([](auto& b){b[7]=std::byte{'1'};});
  corrupt([](auto& b){put(b,8,3,4);});
  // A named inode cannot merely claim the detached flag; names must be absent.
  corrupt([&](auto& b){put(b,directory+20,2,4);put(b,directory+8,0);});
  auto legacy=original;
  for(unsigned slot=0;slot<2;++slot) {
    const auto at=static_cast<std::size_t>(slot*kStoreCommitUnit);
    put(legacy,at+8,1,4);
    std::fill_n(legacy.begin()+static_cast<std::ptrdiff_t>(at+96),16,std::byte{0});
    const auto digest=detail::record_digest(std::span<const std::byte>(legacy).subspan(at,4064));
    std::copy(digest.begin(),digest.end(),legacy.begin()+static_cast<std::ptrdiff_t>(at+4064));
  }
  require(!ObjectStoreSnapshot::open(image(legacy)).info().replicated_checkpoint,
      "legacy-root negative fixture was not independently valid");
  rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(legacy)));},
      NativeFsErrorCode::corrupt,"lifetime capability admitted historical single-root publication");
  // A pending directory cannot have a linked child. Use a nonempty namespace.
  auto named=std::make_shared<Ram>();
  {
    auto writer=fresh(named,StoreCompressionMode::raw);
    const std::array<NativeFileMutation,2> setup{{file_create("D",true),file_create("D/f")}};
    writer.apply(setup);writer.set_delete_pending(3,true);writer.set_delete_pending(3,false);
  }
  auto snapshot=ObjectStoreSnapshot::open(named);auto bytes=snapshot.read(1);const auto map=fields(bytes);
  put(bytes,map.at(2)+20,1,4);
  {auto writer=ObjectStoreWriter::resume(named,identity(),snapshot.info().generation);
    const StoreMutation mutation{1,false,std::move(bytes)};writer.commit({&mutation,1});}
  rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(named));},
      NativeFsErrorCode::corrupt,"pending directory with linked child admitted");
}
enum class Phase {mark,cancel,detach,write,retire,finish};
NativeFilesystemWriter seed(std::shared_ptr<Ram> ram,StoreCompressionMode mode,Phase phase) {
  auto writer=fresh(ram,mode);
  const std::array<NativeFileMutation,6> setup{{file_create("f"),file_write("f",0,data(73,101)),
      file_create("keep"),file_write("keep",0,data(91,102)),
      file_create("second"),file_write("second",0,data(17,103))}};
  writer.apply(setup);
  if(phase!=Phase::mark)writer.set_delete_pending(2,true);
  if(phase==Phase::write || phase==Phase::retire || phase==Phase::finish)writer.unlink_inode(2);
  if(phase==Phase::finish)writer.set_delete_pending(4,true);
  return writer;
}
void action(NativeFilesystemWriter& writer,Phase phase) {
  switch(phase) {
    case Phase::mark:writer.set_delete_pending(2,true);break;
    case Phase::cancel:writer.set_delete_pending(2,false);break;
    case Phase::detach:writer.unlink_inode(2);break;
    case Phase::write:writer.write_inode(2,UINT64_MAX,data(13,104),{true,false});break;
    case Phase::retire:writer.retire_inode(2);break;
    case Phase::finish:throw std::runtime_error("finish needs separate retired-session admission");
  }
}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool complete) {
  require(view.read("keep",0,1000)==data(91,102) && view.stat("keep").inode==3,
      "lifetime fault lost unrelated acknowledged bytes/identity");
  const bool missing=complete && (phase==Phase::retire || phase==Phase::finish);
  const bool detached=phase==Phase::write || phase==Phase::retire || phase==Phase::finish ||
      (phase==Phase::detach && complete);
  const bool pending=(phase==Phase::mark && complete) || (phase==Phase::cancel && !complete) ||
      (phase==Phase::detach && !complete);
  if(missing)rejects([&]{(void)view.stat_inode(2);},NativeFsErrorCode::not_found,"retirement left partial inode");
  else {
    const auto state=view.inode_state(2);
    require(state.delete_pending==pending && state.unlinked==detached,
        "lifetime recovery exposed mixed intent/name state");
    auto bytes=data(73,101);if(phase==Phase::write && complete) {
      const auto suffix=data(13,104);bytes.insert(bytes.end(),suffix.begin(),suffix.end());
    }
    require(view.read_inode(2,0,1000)==bytes,"lifetime fault lost or partially appended retained payload");
    if(detached)rejects([&]{(void)view.stat("f");},NativeFsErrorCode::not_found,"detached inode still named");
    else require(view.stat("f").inode==2,"linked intent lost namespace identity");
  }
  if(phase==Phase::finish && complete)
    rejects([&]{(void)view.stat_inode(4);},NativeFsErrorCode::not_found,"session finish retained partial deletion set");
  else require(view.read_inode(4,0,100)==data(17,103),"session finish lost nonselected acknowledged bytes");
}
const char* phase_name(Phase phase) {
  switch(phase) {
    case Phase::mark:return "mark";case Phase::cancel:return "cancel";
    case Phase::detach:return "detach";case Phase::write:return "write";
    case Phase::retire:return "retire";case Phase::finish:return "finish";
  }
  throw std::runtime_error("unknown lifetime phase");
}
void interruption(StoreCompressionMode mode,std::span<const Phase> phases) {
  for(const auto phase:phases) {
    const auto initial_cases=fault_cases;
    auto probe=std::make_shared<Ram>();std::size_t steps=0;
    {
      auto writer=seed(probe,mode,phase);
      if(phase!=Phase::finish) {
        probe->calls=0;action(writer,phase);steps=probe->calls;
      }
    }
    if(phase==Phase::finish) {
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(probe));
      auto objects=ObjectStoreWriter::resume(probe,identity(),snapshot.storage_info().generation,{mode,6,false});
      probe->calls=0;auto writer=NativeFilesystemWriter::resume_closed_session(std::move(objects));steps=probe->calls;
    }
    require(steps>3 && steps<64,"lifetime fault trace exceeded planned bound");
    for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
      ++fault_cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> retained;
      std::uint64_t sequence=0;
      {
        auto writer=seed(ram,mode,phase);retained.emplace(writer.snapshot());sequence=retained->sequence();
        if(phase!=Phase::finish) {
          ram->calls=0;ram->fail_at=step;ram->failure=fault;
          rejects([&]{action(writer,phase);},NativeFsErrorCode::storage_failure,"lifetime I/O failure hidden");
          require(writer.poisoned(),"failed lifetime writer not terminal");
          const auto calls=ram->calls;
          rejects([&]{writer.set_delete_pending(3,true);},NativeFsErrorCode::storage_failure,"poisoned lifetime writer retried");
          require(ram->calls==calls,"poisoned lifetime writer touched backing I/O");
        }
      }
      if(phase==Phase::finish) {
        auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
        auto objects=ObjectStoreWriter::resume(ram,identity(),snapshot.storage_info().generation,{mode,6,false});
        ram->calls=0;ram->fail_at=step;ram->failure=fault;
        rejects([&]{(void)NativeFilesystemWriter::resume_closed_session(std::move(objects));},
            NativeFsErrorCode::storage_failure,"closed-session I/O failure hidden");
      }
      expected(*retained,phase,false);
      auto recovered=image(ram->stable);
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
      require(snapshot.sequence()==sequence || snapshot.sequence()==sequence+1,
          "lifetime fault lost/fabricated acknowledged sequence");
      expected(snapshot,phase,snapshot.sequence()==sequence+1);
      // Old contexts are gone in this independent recovery fixture. Finish only
      // their persisted intents/orphans; never a still-linked ordinary inode.
      const auto kept=snapshot.stat("keep").inode;
      auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
          snapshot.storage_info().generation,{mode,6,false}));
      require(!writer.snapshot().needs_session_recovery(),"closed-session continuation left unresolved states");
      const auto suffix=data(3,105);writer.write_inode(kept,0,suffix,{true,false});
      auto content=data(91,102);content.insert(content.end(),suffix.begin(),suffix.end());
      auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable)));
      require(reopened.read("keep",0,1000)==content,"lifetime recovery could not continue independent CPU writes");
    }
    const auto completed=fault_cases-initial_cases;
    require(completed==3*steps,"lifetime campaign omitted a planned fault case");
    std::cout<<"Native lifetime evidence: {\"schema\":1,\"mode\":\""
        <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"phase\":\""<<phase_name(phase)
        <<"\",\"io_steps\":"<<steps<<",\"fault_cases\":"<<completed<<",\"complete\":true}\n"<<std::flush;
  }
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("explicit bounded lifetime test group required");
  const std::string_view group(argv[1]);
  if(group=="--invariants") {
    for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
      lifetime(mode);recovery_admission(mode);detached_capacity(mode);
    }
    malformed_metadata();
    std::cout<<"Native lifetime invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    std::array<Phase,1> phases{};
    if(group=="--mark-faults")phases={Phase::mark};
    else if(group=="--cancel-faults")phases={Phase::cancel};
    else if(group=="--detach-faults")phases={Phase::detach};
    else if(group=="--write-faults")phases={Phase::write};
    else if(group=="--retire-faults")phases={Phase::retire};
    else if(group=="--finish-faults")phases={Phase::finish};
    else throw std::runtime_error("unknown bounded lifetime test group");
    for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})interruption(mode,phases);
  }
  std::cout<<"PASS: "<<checks<<" native lifetime assertions; "<<fault_cases
      <<" injected publication failures in "<<group<<".\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"native lifetime failure after "<<checks<<": "<<error.what()<<'\n';return 1;}}
