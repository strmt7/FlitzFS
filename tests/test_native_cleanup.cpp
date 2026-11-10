#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t now=133800000000000099ULL;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
template<class F>void rejects_session(F&& fn,NativeSessionErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
NativeBasicMetadata basic(bool directory=false) {
  return {133700000000000011ULL,133700000000000022ULL,133700000000000033ULL,
      133700000000000044ULL,directory?0x10U:2U};
}
NativeCleanupMetadata effects(unsigned mask=15,std::uint64_t time=now) {
  NativeCleanupMetadata e;e.set_archive=(mask&1)!=0;e.set_access_time=(mask&2)!=0;
  e.set_write_time=(mask&4)!=0;e.set_change_time=(mask&8)!=0;
  e.utc_time=(mask&14)?time:0;return e;
}
NativeBasicMetadata updated(NativeBasicMetadata value,const NativeCleanupMetadata& e,bool directory=false) {
  if(e.set_archive && !directory)value.attributes|=0x20;
  if(e.set_access_time)value.last_access_time=e.utc_time;
  if(e.set_write_time)value.last_write_time=e.utc_time;
  if(e.set_change_time)value.change_time=e.utc_time;
  return value;
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
void masks(StoreCompressionMode mode) {
  for(unsigned mask=0;mask<16;++mask) {
    auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
    const auto file=file_create("f");const NativeBasicMutation meta{0,1,basic()};
    writer.apply_with_basic({&file,1},{&meta,1});
    auto old=writer.snapshot();const auto before=old.sequence(),calls=ram->calls,barriers=ram->flushes;
    const auto effect=effects(mask);const auto result=writer.cleanup_inode(2,false,effect);
    const auto expected=updated(basic(),effect);
    require(writer.inspect_basic_metadata(2)==expected &&
        writer.inspect_basic_metadata(2)->creation_time==basic().creation_time,
        "Cleanup mask changed wrong field or creation time");
    require(old.basic_metadata(2)==basic(),"automatic Cleanup changed old snapshot");
    require(result.sequence==before+(mask?1:0) && ram->flushes==barriers+(mask?3:0),
        "Cleanup mask did not use one atomic publication/no-op");
    if(!mask)require(ram->calls==calls,"empty Cleanup wrote metadata");
    const auto unchanged=ram->calls,sequence=writer.snapshot().sequence();
    require(writer.cleanup_inode(2,false,effect).sequence==sequence && ram->calls==unchanged,
        "identical automatic Cleanup values republished");
    const auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
    require(reopened.basic_metadata(2)==expected,"independent Cleanup reopen lost fields");
  }
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
  writer.set_basic_metadata(1,basic(true));const auto calls=ram->calls;
  writer.cleanup_inode(1,false,effects(1));
  require(writer.inspect_basic_metadata(1)==basic(true) && ram->calls==calls,"automatic directory archive invented");
  const auto zero=effects(14,0);writer.cleanup_inode(1,false,zero);
  require(writer.inspect_basic_metadata(1)==updated(basic(true),zero,true),
      "explicit zero event time treated as a preserve sentinel");
  // Retain absence rather than fabricate original fields.
  const auto legacy=file_create("legacy");writer.apply({&legacy,1});
  const auto before=ram->live;
  rejects([&]{writer.cleanup_inode(2,false,effects());},NativeFsErrorCode::invalid_operation,
      "Cleanup fabricated original metadata on an unstamped inode");
  require(ram->live==before && !writer.poisoned(),"missing tuple admission changed/fenced backing");
  auto legacy_ram=std::make_shared<Ram>();auto ordinary=fresh(legacy_ram,mode);
  ordinary.cleanup_inode(1,false,effects(1));
  require(!ordinary.inspect_basic_metadata(1) && ordinary.snapshot().sequence()==1,
      "ignored directory archive upgraded legacy namespace");
}
void clock_admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  file_apply(writer,file_create("f"));
  for(const auto bad:std::array<NativeCleanupMetadata,3>{{
      {UINT64_MAX,false,true,false,false},{std::uint64_t{1}<<63,false,false,true,false},
      {1,true,false,false,false}}}) {
    const auto reads=ram->read_calls,calls=ram->calls;
    require(!native_cleanup_metadata_valid(bad),"invalid Cleanup clock admitted");
    rejects([&]{writer.cleanup_inode(2,false,bad);},NativeFsErrorCode::invalid_operation,
        "request/unused Cleanup clock persisted");
    require(ram->read_calls==reads && ram->calls==calls,"invalid Cleanup clock reached I/O");
  }
  rejects([&]{writer.cleanup_inode(99,false,{});},NativeFsErrorCode::not_found,"missing Cleanup inode fabricated");
  require(!writer.poisoned(),"pure Cleanup admission fenced healthy writer");
}
void contexts(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto session=NativeFileSession::create(fresh(ram,mode));
  const auto id=session.create_entry("f",false,basic()).created_inodes.front();
  const auto a=session.open("f",all),b=session.open("f",{true,false,false});
  auto before=session.snapshot();session.set_delete(a,true);auto pending=session.snapshot();
  const auto start=ram->flushes;session.cleanup(a,false,effects());
  require(ram->flushes==start+3 && session.stats().contexts==2 && session.stats().cleaned==1 &&
      session.stats().latched_inodes==1 && session.basic_metadata(b)==updated(basic(),effects()),
      "automatic Cleanup lost latch/context/metadata");
  const auto calls=ram->calls;
  session.cleanup(a,false,effects(15,now+1));
  require(ram->calls==calls && session.basic_metadata(b)==updated(basic(),effects()),
      "duplicate Cleanup sampled a new durable time");
  rejects_session([&]{session.cleanup(a,false,effects(2));},NativeSessionErrorCode::invalid_options,
      "conflicting duplicate Cleanup action accepted");
  session.close(a);
  const auto deleting=ram->flushes;session.cleanup(b,true,effects(14,now+7));
  require(ram->flushes==deleting+3 && session.info(b).unlinked && session.stats().contexts==1 &&
      session.stats().latched_inodes==0 &&
      session.basic_metadata(b)==updated(updated(basic(),effects()),effects(14,now+7)),
      "delete Cleanup split automatic metadata from unlink or lost other-context intent");
  require(before.basic_metadata(id)==basic() && pending.inode_state(id).delete_pending &&
      pending.basic_metadata(id)==basic(),"Cleanup changed earlier acknowledged snapshots");
  rejects([&]{(void)session.snapshot().stat("f");},NativeFsErrorCode::not_found,"automatic unlink left old name");
  // Metadata-only implicit effects require no explicit WRITE_ATTRIBUTES grant.
  // Paging I/O still works after Cleanup and does not invent another timestamp.
  session.create_entry("replacement",false,basic());
  auto r=session.open("replacement",all);
  session.write(r,0,data(9,41));
  session.cleanup(r,false,effects(2,now+9));session.close(r);
  auto detached=session.snapshot();session.close(b);
  rejects([&]{(void)session.snapshot().stat_inode(id);},NativeFsErrorCode::not_found,"last Close retained detached automatic inode");
  require(detached.basic_metadata(id)==updated(updated(basic(),effects()),effects(14,now+7)),
      "last Close destroyed retained metadata snapshot");
}
void late_paging(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto session=NativeFileSession::create(fresh(ram,mode));
  const auto id=session.create_entry("f",false,basic()).created_inodes.front();
  const auto handle=session.open("f",all);session.write(handle,0,data(73,42));
  session.set_delete(handle,true);session.cleanup(handle,true,effects());
  const auto sequence=session.snapshot().sequence();const auto original=session.basic_metadata(handle);
  session.write(handle,UINT64_MAX,data(7,43),{true,false});
  auto expected=data(73,42);const auto suffix=data(7,43);expected.insert(expected.end(),suffix.begin(),suffix.end());
  require(session.read(handle,0,100)==expected && session.basic_metadata(handle)==original &&
      session.snapshot().sequence()==sequence+1,"paging I/O after automatic unlink lost bytes/tuple");
  session.truncate(handle,13);session.truncate(handle,100);expected.resize(13);expected.resize(100);
  require(session.read(handle,0,100)==expected,"post-Cleanup truncate exposed old tail");
  auto retained=session.snapshot();session.close(handle);
  require(retained.read_inode(id,0,100)==expected && retained.basic_metadata(id)==original,
      "automatic last Close changed old data/basic snapshot");
}
void fence() {
  auto ram=std::make_shared<Ram>();auto session=NativeFileSession::create(fresh(ram,StoreCompressionMode::raw));
  session.create_entry("f",false,basic());const auto handle=session.open("f",all);
  auto before=session.snapshot();const auto calls=ram->calls,reads=ram->read_calls;session.fence();session.fence();
  require(session.poisoned() && session.stats().contexts==1 && !session.stats().cleaned,
      "explicit fence released context");
  rejects_session([&]{session.cleanup(handle,false,effects());},NativeSessionErrorCode::terminal,"fenced Cleanup retried");
  rejects_session([&]{session.close(handle);},NativeSessionErrorCode::terminal,"fenced Close retired data");
  require(ram->calls==calls && ram->read_calls==reads && before.basic_metadata(2)==basic(),"explicit fence performed I/O or changed metadata");
}
enum class Phase {retain,latch,unlink};
const char* phase_name(Phase p){return p==Phase::retain?"retain":p==Phase::latch?"latch":"unlink";}
struct Seed {
  NativeFileSession session;
  NativeFileHandle handle,other;
};
Seed seed(std::shared_ptr<Ram> ram,StoreCompressionMode mode,Phase phase) {
  auto writer=fresh(ram,mode);
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(73,51)),
      file_create("keep"),file_write("keep",0,data(91,52))}};
  const std::array<NativeBasicMutation,2> meta{{{2,0,basic()},{3,0,basic()}}};
  writer.apply_with_basic(ops,meta);auto session=NativeFileSession::create(std::move(writer));
  const auto handle=session.open("f",all),other=session.open("f",all);
  if(phase!=Phase::retain)session.set_delete(handle,true);
  return {std::move(session),handle,other};
}
void action(Seed& source,Phase phase) {source.session.cleanup(source.handle,phase==Phase::unlink,effects());}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool changed) {
  require(view.read("keep",0,100)==data(91,52) && view.basic_metadata(3)==basic(),
      "Cleanup interruption lost unrelated acknowledged tuple/data");
  const auto state=view.inode_state(2);
  require(state.unlinked==(changed && phase==Phase::unlink) &&
      state.delete_pending==(phase!=Phase::retain && !(changed && phase==Phase::unlink)),
      "Cleanup interruption mixed metadata and namespace lifetime");
  require(view.basic_metadata(2)==(changed?updated(basic(),effects()):basic()) &&
      view.read_inode(2,0,100)==data(73,51),"Cleanup interruption mixed automatic fields or lost retained data");
  if(state.unlinked)rejects([&]{(void)view.stat("f");},NativeFsErrorCode::not_found,"automatic detached inode still named");
  else require(view.stat("f").inode==2,"automatic Cleanup redirected stable inode");
}
void interruption(StoreCompressionMode mode,Phase phase) {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto source=seed(probe,mode,phase);probe->calls=0;action(source,phase);steps=probe->calls;}
  require(steps>3 && steps<80,"automatic Cleanup trace out of bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> old;
    std::uint64_t sequence=0;
    {
      auto source=seed(ram,mode,phase);old.emplace(source.session.snapshot());sequence=old->sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(source,phase);},NativeFsErrorCode::storage_failure,"failed automatic Cleanup acknowledged");
      const auto state=source.session.stats();
      require(source.session.poisoned() && state.contexts==2 && state.cleaned==0 &&
          state.latched_inodes==0 && state.delete_requests==(phase==Phase::retain?0U:1U),
          "failed automatic Cleanup advanced/released context/latch");
      const auto calls=ram->calls;
      rejects_session([&]{action(source,phase);},NativeSessionErrorCode::terminal,"poisoned automatic Cleanup retried");
      require(ram->calls==calls,"poisoned Cleanup touched backing again");
    }
    expected(*old,phase,false);
    auto recovered=image(ram->stable);auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"automatic Cleanup lost/fabricated acknowledged sequence");
    expected(view,phase,view.sequence()==sequence+1);
    // The original session and every owned descriptor have now retired.
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const auto op=file_write("keep",3,data(7,53));const NativeBasicMutation meta{3,0,basic()};
    writer.apply_with_basic({&op,1},{&meta,1});
    auto bytes=data(91,52);const auto patch=data(7,53);std::copy(patch.begin(),patch.end(),bytes.begin()+3);
    const auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable)));
    require(reopened.read("keep",0,100)==bytes && reopened.basic_metadata(3)==basic() &&
        !reopened.needs_session_recovery(),"automatic Cleanup CPU recovery could not continue");
  }
  require(cases==steps*3,"automatic Cleanup omitted a fault case");
  std::cout<<"Native Cleanup evidence: {\"schema\":1,\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"phase\":\""<<phase_name(phase)
      <<"\",\"io_steps\":"<<steps<<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one bounded Cleanup group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
      masks(mode);contexts(mode);late_paging(mode);
    }
    clock_admission();fence();
    std::cout<<"Native Cleanup invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    const auto phase=group=="--retain-faults"?Phase::retain:group=="--latch-faults"?Phase::latch:
        group=="--unlink-faults"?Phase::unlink:throw std::runtime_error("unknown Cleanup group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})interruption(mode,phase);
  }
  return 0;
}catch(const std::exception& error){std::cerr<<"Native Cleanup failure: "<<error.what()<<"\n";return 1;}}
