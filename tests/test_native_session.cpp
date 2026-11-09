#include "flitzfs/native_session.hpp"
#include "native_fs_fixture.hpp"
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;std::size_t fault_cases=0;
constexpr NativeFileAccess all{true,true,true};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,NativeSessionErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeSessionError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
template<class F>void rejects_fs(F&& action,NativeFsErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeFsError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
NativeFileSession seeded(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  auto writer=fresh(ram,mode);
  const std::array<NativeFileMutation,4> setup{{file_create("f"),file_write("f",0,data(73,111)),
      file_create("keep"),file_write("keep",0,data(91,112))}};
  writer.apply(setup);return NativeFileSession::create(std::move(writer));
}
void finish(NativeFileSession& session,const NativeFileHandle& handle) {
  session.cleanup(handle,false);session.close(handle);
}
void normal(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);const auto original=data(65530,121);
  const std::array<NativeFileMutation,5> setup{{file_create("D",true),file_create("D/f"),
      file_write("D/f",0,original),file_create("target"),file_write("target",0,data(17,122))}};
  writer.apply(setup);auto session=NativeFileSession::create(std::move(writer));
  const auto a=session.open("D/f",all,NativeOpenKind::file);
  const auto b=session.open("D/f",{true,true,false});
  const auto directory=session.open("D",all,NativeOpenKind::directory);
  const auto inode=session.info(a).info.inode;
  require(session.stats().contexts==3 && !session.poisoned(),"initial native context ownership");
  auto before=session.snapshot();
  session.rename(a,"D/moved");
  require(session.info(b).info.path=="D/moved" && session.read(b,0,100000)==original,
      "rename invalidated another open instance");
  session.create_entry("D/f");const auto reused=session.open("D/f",all);
  session.write(reused,0,data(9,123));
  require(session.info(reused).info.inode!=inode && session.read(a,0,100000)==original,
      "old pathname reuse redirected an owned context");
  auto target=session.open("target",all);const auto calls=ram->calls;
  rejects([&]{session.rename(a,"target",true);},NativeSessionErrorCode::sharing_violation,
      "replacement discarded another owned context");
  require(ram->calls==calls && session.read(target,0,100)==data(17,122),"replacement rejection changed target");
  finish(session,target);session.rename(a,"target",true);
  require(session.info(b).info.path=="target" && before.read("target",0,100)==data(17,122),
      "replacement changed retained target snapshot");
  session.set_delete(a,true);
  rejects([&]{(void)session.open("target");},NativeSessionErrorCode::delete_pending,"new open accepted pending name");
  session.set_delete(a,false);
  auto reopened=session.open("target");finish(session,reopened);
  rejects_fs([&]{session.set_delete(directory,true);},NativeFsErrorCode::not_empty,"nonempty directory marked");
  session.set_delete(a,true);session.cleanup(a,true);
  require(session.stats().cleaned==1 && session.info(b).unlinked &&
      session.read(a,0,100000)==original,"Cleanup destroyed context or detached payload");
  const auto suffix=data(17,124);const auto appended=session.write(b,UINT64_MAX,suffix,{true,false});
  auto content=original;content.insert(content.end(),suffix.begin(),suffix.end());
  require(appended.offset==original.size() && session.read(a,0,100000)==content,
      "paging write after Cleanup lost retained data");
  session.truncate(b,13);session.truncate(b,65550);content.resize(13);content.resize(65550);
  require(session.read(a,0,100000)==content,"cleaned context truncate/regrow exposed old tail");
  session.create_entry("target");auto replacement=session.open("target",all);
  require(session.info(replacement).info.inode!=inode && session.read(replacement,0,10).empty(),
      "detached context redirected to a replacement name");
  session.close(a);
  require(session.info(b).info.inode==inode && session.snapshot().needs_session_recovery(),
      "first Close prematurely retired shared inode");
  auto retained=session.snapshot();session.cleanup(b,false);session.close(b);
  rejects([&]{(void)session.read(a,0,1);},NativeSessionErrorCode::invalid_handle,"closed alias remained usable");
  require(retained.read_inode(inode,0,100000)==content && session.read(replacement,0,10).empty(),
      "last Close damaged retained snapshot or replacement");
  finish(session,directory);finish(session,reused);finish(session,replacement);
  require(!session.stats().contexts && !session.snapshot().needs_session_recovery(),
      "closed context/session left unexplained lifetime state");
  require(session.snapshot().read("D/f",0,100)==data(9,123),"unrelated reused-path data lost");
  NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable))).verify_all();
}
void disposition_and_latches(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto session=seeded(ram,mode);
  auto a=session.open("f",all),b=session.open("f",all),c=session.open("f",all);
  auto reader=session.open("f");
  session.set_delete(a,true);session.set_delete(b,true);
  session.set_delete(a,false);
  require(session.info(c).delete_pending && session.stats().delete_requests==1,
      "one cancel erased another open's delete request");
  session.set_delete(b,false);
  require(!session.info(c).delete_pending && !session.stats().delete_requests,
      "last cancellation did not persist");
  finish(session,a);
  session.set_delete(b,true);session.cleanup(b,false);session.close(b);
  require(session.stats().contexts==2 && session.stats().latched_inodes==1 &&
      !session.stats().delete_requests && session.info(c).delete_pending,
      "Cleaned request owner forgot latched deletion when closed");
  const auto calls=ram->calls;session.set_delete(c,false);
  require(session.info(c).delete_pending && ram->calls==calls,"another context canceled latched intent");
  finish(session,c);
  session.cleanup(reader,true);session.cleanup(reader,true);
  require(session.info(reader).unlinked && !session.stats().latched_inodes &&
      session.read(reader,0,100)==data(73,111),"last Cleanup without delete access failed to retain authorized I/O");
  session.close(reader);require(!session.stats().contexts,"last Close did not release context");
  rejects_fs([&]{(void)session.snapshot().stat("f");},NativeFsErrorCode::not_found,"deleted file still visible");
  require(session.snapshot().read("keep",0,100)==data(91,112),"delete changed unrelated acknowledged bytes");
}
void identity_permissions_and_capacity(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto session=seeded(ram,mode);
  const auto read=session.open("f"),alias=read;
  auto other_ram=std::make_shared<Ram>();auto other=seeded(other_ram,mode);auto foreign=other.open("f");
  const auto calls=ram->calls,reads=ram->read_calls;
  rejects([&]{(void)session.info(foreign);},NativeSessionErrorCode::invalid_handle,"foreign token bound to session");
  rejects([&]{(void)session.info(NativeFileHandle{});},NativeSessionErrorCode::invalid_handle,"empty token admitted");
  rejects([&]{session.write(read,0,data(1,131));},NativeSessionErrorCode::access_denied,"read context wrote");
  rejects([&]{session.truncate(read,0);},NativeSessionErrorCode::access_denied,"read context truncated");
  rejects([&]{session.set_delete(read,true);},NativeSessionErrorCode::access_denied,"read context deleted");
  rejects([&]{session.rename(read,"bad");},NativeSessionErrorCode::access_denied,"read context renamed");
  require(ram->calls==calls && ram->read_calls==reads,"invalid/denied context performed I/O");
  rejects([&]{session.close(read);},NativeSessionErrorCode::invalid_options,"Close skipped required Cleanup");
  require(!session.poisoned() && session.stats().contexts==1,"invalid Close damaged healthy context");
  finish(session,read);
  rejects([&]{(void)session.read(alias,0,1);},NativeSessionErrorCode::invalid_handle,"closed token alias was reused");
  auto empty=session.open("f",{false,false,false});
  rejects([&]{(void)session.read(empty,0,1);},NativeSessionErrorCode::access_denied,"metadata-only context read data");
  rejects([&]{(void)session.list(empty);},NativeSessionErrorCode::access_denied,"metadata-only context enumerated");
  finish(session,empty);
  rejects([&]{(void)session.open("f",{},static_cast<NativeOpenKind>(99));},
      NativeSessionErrorCode::invalid_options,"unknown context kind accepted");
  rejects_fs([&]{(void)session.open("f",{},NativeOpenKind::directory);},
      NativeFsErrorCode::not_directory,"file opened as directory");
  auto moving=session.open("f",all);auto moved=std::move(session);
  require(moved.read(moving,0,100)==data(73,111),"session move invalidated owned token");
  rejects([&]{(void)session.info(moving);},NativeSessionErrorCode::terminal,"moved-from session accepted handle");
  finish(moved,moving);
  std::vector<NativeFileHandle> contexts;contexts.reserve(NativeFileSession::max_contexts);
  for(std::size_t i=0;i<NativeFileSession::max_contexts;++i)contexts.push_back(moved.open("f"));
  const auto writes=ram->calls,read_calls=ram->read_calls;
  rejects([&]{(void)moved.open("f");},NativeSessionErrorCode::limit,"context admission exceeded cap");
  require(ram->calls==writes && ram->read_calls==read_calls,"context-cap rejection reached backing I/O");
  require(moved.stats().contexts==NativeFileSession::max_contexts,"context table lost entries");
  for(const auto& handle:contexts)finish(moved,handle);
  require(!moved.stats().contexts,"context table leaked closed slots");
  auto stale=contexts[0];auto next=moved.open("f");
  rejects([&]{(void)moved.info(stale);},NativeSessionErrorCode::invalid_handle,"new context reused stale serial");
  finish(moved,next);finish(other,foreign);
  NativeFileHandle expired;
  {auto temp=seeded(std::make_shared<Ram>(),mode);expired=temp.open("f");}
  rejects([&]{(void)moved.info(expired);},NativeSessionErrorCode::invalid_handle,"destroyed owner token bound anew");
}
void protocol_fencing(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();
  {
    auto session=seeded(ram,mode);const auto handle=session.open("f",all);
    const auto before=ram->live;const auto calls=ram->calls;
    rejects([&]{session.cleanup(handle,true);},NativeSessionErrorCode::terminal,"unauthorized cleanup deletion accepted");
    require(session.poisoned() && session.stats().contexts==1 && !session.stats().cleaned &&
        ram->live==before && ram->calls==calls,"invalid Cleanup removed data/context");
  }
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
  require(!view.needs_session_recovery() && view.read("f",0,100)==data(73,111),
      "unknown Cleanup destroyed persisted ordinary data");
  auto pending=std::make_shared<Ram>();
  {
    auto session=seeded(pending,mode);auto handle=session.open("f",all);
    session.set_delete(handle,true);session.cleanup(handle,false);
    rejects([&]{session.close(handle);},NativeSessionErrorCode::terminal,"missing delete Cleanup guessed retirement");
    require(session.stats().contexts==1 && session.stats().cleaned==1 &&
        session.stats().latched_inodes==1,"failed Close lost its context/intent");
    const auto calls=pending->calls;
    rejects([&]{session.write(handle,0,data(1,132));},NativeSessionErrorCode::terminal,"protocol-fenced session wrote");
    require(pending->calls==calls,"protocol fence performed I/O");
  } // Controlled native contexts are gone; no driver/process retirement is inferred here.
  auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(pending));
  require(snapshot.needs_session_recovery() && snapshot.read("f",0,100)==data(73,111),
      "fenced pending context discarded recoverable bytes");
  auto recovered=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(pending,identity(),
      snapshot.storage_info().generation,{mode,6,false}));
  require(recovered.snapshot().read("keep",0,100)==data(91,112),"closed-session recovery lost unrelated data");
  auto orphan=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> retained;
  {
    auto writer=fresh(orphan,mode);file_apply(writer,file_create("f"));
    writer.set_delete_pending(2,true);retained.emplace(writer.snapshot());
    const auto before=orphan->live;const auto calls=orphan->calls;
    rejects([&]{(void)NativeFileSession::create(std::move(writer));},NativeSessionErrorCode::delete_pending,
        "session guessed old context retirement at admission");
    require(orphan->live==before && orphan->calls==calls,"unresolved session admission published recovery");
  }
  require(retained->needs_session_recovery(),"unresolved admission fixture lost intent");
}
std::uint64_t get(const Bytes& bytes,std::size_t at,unsigned width=8) {
  std::uint64_t out=0;for(unsigned i=0;i<width;++i)
    out|=std::uint64_t(std::to_integer<unsigned char>(bytes.at(at+i)))<<(8*i);
  return out;
}
std::uint64_t record_offset(const Ram& ram,std::uint64_t id) {
  const auto root=get(ram.live,16)>get(ram.live,kStoreCommitUnit+16)?0U:kStoreCommitUnit;
  const auto manifest=get(ram.live,static_cast<std::size_t>(root+32));
  const auto count=get(ram.live,static_cast<std::size_t>(manifest+12),4);
  for(std::uint64_t i=0;i<count;++i) {
    const auto at=static_cast<std::size_t>(manifest+64+64*i);
    if(get(ram.live,at)==id)return get(ram.live,at+16);
  }
  throw std::runtime_error("owned synthetic record missing");
}
void efficient_verified_reads() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  file_apply(writer,file_create("f"));const auto content=data(32*kNativeExtentBytes,141);
  for(std::size_t at=0;at<content.size();at+=kStoreMaxObjectBytes)
    file_apply(writer,file_write("f",at,Bytes(content.begin()+static_cast<std::ptrdiff_t>(at),
        content.begin()+static_cast<std::ptrdiff_t>(at+kStoreMaxObjectBytes))));
  auto session=NativeFileSession::create(std::move(writer));
  auto start=ram->read_bytes;auto handle=session.open("f",all);auto state=session.info(handle);
  const auto metadata_reads=ram->read_bytes-start;
  require(state.info.logical_bytes==content.size() && metadata_reads<65536,
      "context metadata/open rescanned all file payloads");
  start=ram->read_bytes;const auto result=session.read(handle,65530,128);
  const auto range_reads=ram->read_bytes-start;
  require(result==Bytes(content.begin()+65530,content.begin()+65658) &&
      range_reads<256*1024,"range read rescanned unrelated extents");
  start=ram->read_bytes;auto full=session.snapshot();
  const auto snapshot_reads=ram->read_bytes-start;
  require(full.stat("f").logical_bytes==content.size() && snapshot_reads>=content.size(),
      "public snapshot stopped full payload verification");
  std::cout<<"Native session read evidence: {\"logical_bytes\":"<<content.size()
      <<",\"metadata_read_bytes\":"<<metadata_reads<<",\"range_read_bytes\":"<<range_reads
      <<",\"public_snapshot_read_bytes\":"<<snapshot_reads<<"}\n";
  auto objects=ObjectStoreSnapshot::open(ram);std::uint64_t selected=0;
  const Bytes second(content.begin()+kNativeExtentBytes,content.begin()+2*kNativeExtentBytes);
  for(const auto& object:objects.list())
    if(object.id!=1 && objects.read(object.id)==second)selected=object.id;
  require(selected!=0,"unique corruption fixture block not identified");
  const auto offset=record_offset(*ram,selected);ram->live.at(static_cast<std::size_t>(offset+128))^=std::byte{1};
  require(session.info(handle).info.logical_bytes==content.size() &&
      session.read(handle,0,1)==Bytes(content.begin(),content.begin()+1),"untouched corruption exposed unrelated data or blocked healthy range");
  Bytes output(8,std::byte{0x55});
  rejects_fs([&]{output=session.read(handle,kNativeExtentBytes,1);},NativeFsErrorCode::storage_failure,
      "touched corrupt record exposed unchecked bytes");
  require(output==Bytes(8,std::byte{0x55}) && session.poisoned(),"failed private read changed caller output or failed to fence");
  const auto calls=ram->read_calls;
  rejects([&]{(void)session.info(handle);},NativeSessionErrorCode::terminal,"corrupt session queried after fence");
  require(ram->read_calls==calls,"fenced session retried backing read");
  auto metadata_ram=std::make_shared<Ram>();auto another=seeded(metadata_ram,StoreCompressionMode::raw);
  auto opened=another.open("f");const auto metadata_at=record_offset(*metadata_ram,1);
  metadata_ram->live.at(static_cast<std::size_t>(metadata_at+128))^=std::byte{1};
  rejects_fs([&]{(void)another.info(opened);},NativeFsErrorCode::storage_failure,"cached metadata skipped backing integrity");
  require(another.poisoned(),"metadata verification failure did not fence session");
}
void latest_acknowledgement_repair(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> acknowledged;
  const auto suffix=data(13,161),latest=data(91,162);
  auto expected=data(73,111);expected.insert(expected.end(),suffix.begin(),suffix.end());
  {
    auto session=seeded(ram,mode);auto file=session.open("f",all),keep=session.open("keep",all);
    session.set_delete(file,true);session.cleanup(file,true);
    session.write(file,UINT64_MAX,suffix,{true,false});session.write(keep,0,latest);
    acknowledged.emplace(session.snapshot());
  } // Controlled context owner is gone; model crash before final Close.
  const auto bytes=ram->stable;const auto info=acknowledged->storage_info();
  const auto at=static_cast<std::size_t>((info.generation%2)*kStoreCommitUnit);
  const std::array<std::uint64_t,4> units{{0,kStoreCommitUnit,get(bytes,at+32),get(bytes,at+96)}};
  for(const auto unit:units) {
    auto damaged=image(bytes);
    std::fill_n(damaged->live.begin()+static_cast<std::ptrdiff_t>(unit),
        static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
    damaged->stable=damaged->live;
    auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(damaged));
    require(snapshot.sequence()==acknowledged->sequence() && snapshot.storage_info().degraded_checkpoint &&
        snapshot.read_inode(2,0,1000)==expected && snapshot.read("keep",0,1000)==latest,
        "lifetime metadata loss rolled back later acknowledged data");
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(damaged,identity(),
        snapshot.storage_info().generation,{mode,6,false}));
    auto recovered=writer.snapshot();
    require(!recovered.needs_session_recovery() && !recovered.storage_info().degraded_checkpoint &&
        recovered.read("keep",0,1000)==latest,"repair/closed-session completion lost unrelated latest data");
    auto session=NativeFileSession::create(std::move(writer));auto keep=session.open("keep",all);
    const auto extra=data(3,163);session.write(keep,0,extra,{true,false});finish(session,keep);
    auto continued=latest;continued.insert(continued.end(),extra.begin(),extra.end());
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(damaged->stable))).
        read("keep",0,1000)==continued,"repaired context owner could not continue writes");
  }
  require(acknowledged->read_inode(2,0,1000)==expected &&
      acknowledged->read("keep",0,1000)==latest,"repair changed retained source snapshot");
}
enum class Phase {mark,cleanup,close};
const char* name(Phase phase){switch(phase){case Phase::mark:return "mark";case Phase::cleanup:return "cleanup";case Phase::close:return "close";}throw std::runtime_error("phase");}
void prepare(NativeFileSession& session,const NativeFileHandle& handle,Phase phase) {
  if(phase!=Phase::mark)session.set_delete(handle,true);
  if(phase==Phase::close)session.cleanup(handle,true);
}
void action(NativeFileSession& session,const NativeFileHandle& handle,Phase phase) {
  switch(phase) {
    case Phase::mark:session.set_delete(handle,true);break;
    case Phase::cleanup:session.cleanup(handle,true);break;
    case Phase::close:session.close(handle);break;
  }
}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool complete) {
  require(view.read("keep",0,100)==data(91,112),"context fault lost unrelated acknowledged bytes");
  if(phase==Phase::close && complete) {
    rejects_fs([&]{(void)view.stat_inode(2);},NativeFsErrorCode::not_found,"partial context retirement");
  } else {
    const auto state=view.inode_state(2);
    const bool detached=phase==Phase::close || (phase==Phase::cleanup && complete);
    const bool pending=(phase==Phase::cleanup && !complete) || (phase==Phase::mark && complete);
    require(state.unlinked==detached && state.delete_pending==pending &&
        view.read_inode(2,0,100)==data(73,111),"context fault exposed mixed state or lost retained data");
  }
}
void interruption(StoreCompressionMode mode,Phase phase) {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto session=seeded(probe,mode);auto handle=session.open("f",all);prepare(session,handle,phase);
    probe->calls=0;action(session,handle,phase);steps=probe->calls;}
  require(steps>3 && steps<64,"context fault trace outside planned bound");
  const auto initial=fault_cases;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++fault_cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> retained;
    std::uint64_t sequence=0;
    {
      auto session=seeded(ram,mode);auto handle=session.open("f",all);prepare(session,handle,phase);
      retained.emplace(session.snapshot());sequence=retained->sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects_fs([&]{action(session,handle,phase);},NativeFsErrorCode::storage_failure,"context publication failure hidden");
      const auto stats=session.stats();
      require(stats.fenced && stats.contexts==1 && stats.cleaned==(phase==Phase::close?1U:0U) &&
          stats.delete_requests==(phase==Phase::mark?0U:1U),"failed commit released/advanced context before durable success");
      const auto calls=ram->calls,reads=ram->read_calls;
      rejects([&]{(void)session.read(handle,0,1);},NativeSessionErrorCode::terminal,"failed context retried");
      require(ram->calls==calls && ram->read_calls==reads,"fenced context performed I/O");
    }
    expected(*retained,phase,false);
    auto recovered=image(ram->stable);
    auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(snapshot.sequence()==sequence || snapshot.sequence()==sequence+1,"context fault lost/fabricated namespace sequence");
    expected(snapshot,phase,snapshot.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
        snapshot.storage_info().generation,{mode,6,false}));
    const auto keep=writer.inspect_path("keep").info.inode;const auto suffix=data(3,151);
    writer.write_inode(keep,0,suffix,{true,false});
    auto content=data(91,112);content.insert(content.end(),suffix.begin(),suffix.end());
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable))).
        read("keep",0,100)==content,"context fault recovery could not continue CPU writes");
  }
  const auto completed=fault_cases-initial;require(completed==3*steps,"context fault cases omitted");
  std::cout<<"Native session evidence: {\"schema\":1,\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"phase\":\""<<name(phase)
      <<"\",\"io_steps\":"<<steps<<",\"fault_cases\":"<<completed<<",\"complete\":true}\n"<<std::flush;
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("explicit bounded native context group required");
  const std::string_view group(argv[1]);
  if(group=="--invariants") {
    for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
      normal(mode);disposition_and_latches(mode);identity_permissions_and_capacity(mode);protocol_fencing(mode);
      latest_acknowledgement_repair(mode);
    }
    efficient_verified_reads();
    std::cout<<"Native session invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    Phase phase;
    if(group=="--mark-faults")phase=Phase::mark;
    else if(group=="--cleanup-faults")phase=Phase::cleanup;
    else if(group=="--close-faults")phase=Phase::close;
    else throw std::runtime_error("unknown bounded native context group");
    for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})interruption(mode,phase);
  }
  std::cout<<"PASS: "<<checks<<" native context assertions; "<<fault_cases<<" publication failures in "<<group<<".\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"native context failure after "<<checks<<": "<<error.what()<<'\n';return 1;}}
