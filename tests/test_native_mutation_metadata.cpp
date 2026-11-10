#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t stamp=133800000000000099ULL;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
template<class F>void rejects_session(F&& fn,NativeSessionErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
NativeBasicMetadata basic(){return {11,22,33,44,2};}
NativeAutomaticMetadata effects(unsigned mask=15,std::uint64_t time=stamp) {
  return {(mask&14)?time:0,(mask&1)!=0,(mask&2)!=0,(mask&4)!=0,(mask&8)!=0};
}
NativeBasicMetadata updated(NativeAutomaticMetadata e) {
  auto value=basic();if(e.set_archive)value.attributes|=32;
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
    const auto op=file_create("f");const NativeBasicMutation meta{0,1,basic()};
    writer.apply_with_basic({&op,1},{&meta,1});auto old=writer.snapshot();
    const auto barriers=ram->flushes;
    const auto result=writer.write_inode_with_metadata(2,UINT64_MAX,data(23,71),{true,false},effects(mask));
    require(result.inode==2 && result.offset==0 && result.bytes==23 &&
        result.transaction.sequence==old.sequence()+1 && ram->flushes==barriers+3,
        "automatic data write split publication or accepted count");
    auto view=writer.snapshot();
    require(view.read_inode(2,0,100)==data(23,71) && view.basic_metadata(2)==updated(effects(mask)) &&
        old.read_inode(2,0,100).empty() && old.basic_metadata(2)==basic(),
        "automatic data/basic write changed untouched fields or old snapshot");
    auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
    require(reopened.read_inode(2,0,100)==data(23,71) && reopened.basic_metadata(2)==updated(effects(mask)),
        "independent write/basic reopen mismatch");
    // A nonempty identical write still publishes even with identical effects.
    const auto sequence=view.sequence(),flushes=ram->flushes;
    require(writer.write_inode_with_metadata(2,0,data(23,71),{},effects(mask)).transaction.sequence==sequence+1 &&
        ram->flushes==flushes+3,"identical automatic write weakened barriers");
    const auto zero=effects(14,0);
    writer.truncate_inode_with_metadata(2,13,zero);
    auto zeroed=updated(effects(mask));
    zeroed.last_access_time=zeroed.last_write_time=zeroed.change_time=0;
    require(writer.inspect_basic_metadata(2)==zeroed,
        "truncate zero event time became preserve sentinel or lost archive");
    writer.truncate_inode(2,100);
    auto bytes=data(23,71);bytes.resize(13);bytes.resize(100);
    require(writer.snapshot().read_inode(2,0,100)==bytes,"atomic truncate exposed old tail after regrow");
  }
}
void admission(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
  file_apply(writer,file_create("legacy"));
  auto before=ram->live;const auto calls=ram->calls;
  // Zero accepted bytes preserve absence and do not require a fabricated tuple.
  const auto empty=writer.write_inode_with_metadata(2,UINT64_MAX,data(9,72),{false,true},effects());
  require(!empty.bytes && !writer.inspect_basic_metadata(2) && ram->live==before && ram->calls==calls,
      "clipped automatic write stamped/changed an unstamped inode");
  rejects([&]{writer.write_inode_with_metadata(2,0,data(9,72),{},effects());},
      NativeFsErrorCode::invalid_operation,"nonempty automatic write fabricated original tuple");
  rejects([&]{writer.truncate_inode_with_metadata(2,9,effects());},
      NativeFsErrorCode::invalid_operation,"automatic truncate fabricated original tuple");
  require(ram->live==before && ram->calls==calls && !writer.poisoned(),"missing tuple caused writes/fencing");
  writer.set_basic_metadata(2,basic());writer.write_inode(2,0,data(23,73));
  for(const auto bad:std::array<NativeAutomaticMetadata,3>{{
      {UINT64_MAX,false,true,false,false},{std::uint64_t{1}<<63,false,false,true,false},
      {1,true,false,false,false}}}) {
    const auto reads=ram->read_calls,writes=ram->calls;
    rejects([&]{writer.write_inode_with_metadata(2,0,data(1,74),{},bad);},
        NativeFsErrorCode::invalid_operation,"invalid automatic write clock admitted");
    rejects([&]{writer.truncate_inode_with_metadata(2,1,bad);},
        NativeFsErrorCode::invalid_operation,"invalid automatic truncate clock admitted");
    require(ram->read_calls==reads && ram->calls==writes,"invalid automatic clock reached I/O");
  }
  const auto state=ram->live;const auto barriers=ram->flushes;
  for(const auto options:{NativeInodeWriteOptions{false,true},NativeInodeWriteOptions{true,true}}) {
    const auto result=writer.write_inode_with_metadata(2,UINT64_MAX,data(7,75),options,effects());
    require(!result.bytes && ram->live==state && ram->flushes==barriers &&
        writer.inspect_basic_metadata(2)==basic(),"zero accepted write applied automatic effects");
  }
  require(!writer.write_inode_with_metadata(2,0,{}, {},effects()).bytes &&
      ram->live==state,"empty automatic write published");
  // Full supplied input admission is not bypassed by clipping.
  const auto too_big=data(kStoreMaxObjectBytes+1,76);const auto reads=ram->read_calls;
  rejects([&]{writer.write_inode_with_metadata(2,UINT64_MAX,too_big,{false,true},effects());},
      NativeFsErrorCode::limit,"clipped oversized automatic input admitted");
  require(ram->read_calls==reads && ram->live==state,"oversized automatic input reached backing I/O");
  rejects([&]{writer.write_inode_with_metadata(99,0,data(1,77),{},effects());},
      NativeFsErrorCode::not_found,"missing automatic target redirected");
  rejects([&]{writer.truncate_inode_with_metadata(1,0,effects());},
      NativeFsErrorCode::is_directory,"automatic directory truncate admitted");
  rejects([&]{writer.write_inode_with_metadata(2,kNativeMaxFileBytes,data(1,77),{},effects());},
      NativeFsErrorCode::limit,"automatic write crossed logical size ceiling");
  rejects([&]{writer.truncate_inode_with_metadata(2,kNativeMaxFileBytes+1,effects());},
      NativeFsErrorCode::limit,"automatic truncate crossed logical size ceiling");
  // An explicitly requested metadata-only effect at unchanged EOF is real.
  const auto sequence=writer.snapshot().sequence();
  writer.truncate_inode_with_metadata(2,23,effects(2,stamp+3));
  require(writer.snapshot().sequence()==sequence+1 &&
      writer.inspect_basic_metadata(2)==updated(effects(2,stamp+3)),
      "same-EOF requested truncate effects were discarded");
  require(!writer.poisoned(),"pure mutation metadata admission fenced healthy writer");
}
void owner_and_identity(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto session=NativeFileSession::create(fresh(ram,mode));
  const auto id=session.create_entry("f",false,basic()).created_inodes.front();
  const auto handle=session.open("f",all),reader=session.open("f",{true,false,false});
  const auto writes=ram->calls,reads=ram->read_calls;
  rejects_session([&]{session.set_basic_metadata(handle,basic());},NativeSessionErrorCode::access_denied,
      "data grant implied explicit attribute write");
  rejects_session([&]{session.write_with_metadata(reader,0,data(3,78),{},effects());},
      NativeSessionErrorCode::access_denied,"read-only context changed data/basic tuple");
  rejects_session([&]{session.truncate_with_metadata(reader,0,effects());},
      NativeSessionErrorCode::access_denied,"read-only context truncated data/basic tuple");
  require(ram->calls==writes && ram->read_calls==reads,"denied grant performed I/O");
  session.rename(handle,"renamed");session.create_entry("f",false,basic());
  const auto replacement=session.open("f",all);
  session.write(replacement,0,data(7,79));
  auto old=session.snapshot();
  session.write_with_metadata(handle,UINT64_MAX,data(11,80),{true,false},effects(1));
  require(session.read(handle,0,100)==data(11,80) && session.read(replacement,0,100)==data(7,79) &&
      session.basic_metadata(handle)==updated(effects(1)) && session.basic_metadata(replacement)==basic(),
      "automatic stable-inode operation followed reused name");
  session.set_delete(handle,true);session.cleanup(handle,true);
  session.write_with_metadata(handle,UINT64_MAX,data(5,81),{true,false},effects());
  auto bytes=data(11,80);const auto suffix=data(5,81);bytes.insert(bytes.end(),suffix.begin(),suffix.end());
  require(session.info(handle).unlinked && session.read(handle,0,100)==bytes &&
      session.basic_metadata(handle)==updated(effects()) &&
      old.read_inode(id,0,100).empty() && old.basic_metadata(id)==basic(),
      "late paging automatic effects lost detached identity/snapshot");
  session.truncate_with_metadata(handle,3,effects(14,stamp+9));session.truncate(handle,20);
  bytes.resize(3);bytes.resize(20);
  require(session.read(handle,0,100)==bytes,"detached atomic truncate leaked old bytes");
  auto retained=session.snapshot();session.close(handle);
  require(session.stats().contexts==2 && retained.read_inode(id,0,100)==bytes,
      "first Close discarded another context");
  session.cleanup(reader,false);session.close(reader);
  rejects([&]{(void)session.snapshot().stat_inode(id);},NativeFsErrorCode::not_found,"last Close failed to retire detached target");
  require(retained.read_inode(id,0,100)==bytes,"retirement changed retained data");
  session.cleanup(replacement,false);session.close(replacement);
  const auto calls=ram->calls;
  rejects_session([&]{session.write_with_metadata(handle,0,data(1,82),{},effects());},
      NativeSessionErrorCode::invalid_handle,"stale automatic mutation context admitted");
  require(ram->calls==calls,"stale automatic context wrote backing data");
}
enum class Phase{append,constrained,truncate};
const char* name(Phase p){return p==Phase::append?"append":p==Phase::constrained?"constrained":"truncate";}
struct Seed{NativeFileSession session;NativeFileHandle handle,other;};
Seed seed(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  auto writer=fresh(ram,mode);
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(65530,83)),
      file_create("keep"),file_write("keep",0,data(91,84))}};
  const std::array<NativeBasicMutation,2> meta{{{2,0,basic()},{3,0,basic()}}};
  writer.apply_with_basic(ops,meta);auto session=NativeFileSession::create(std::move(writer));
  const auto handle=session.open("f",all),other=session.open("f",all);
  session.set_delete(handle,true);session.cleanup(handle,true);
  return {std::move(session),handle,other};
}
Bytes changed(Phase phase) {
  auto bytes=data(65530,83);
  if(phase==Phase::append){const auto addition=data(29,85);bytes.insert(bytes.end(),addition.begin(),addition.end());}
  else if(phase==Phase::constrained){const auto patch=data(100,86);std::copy_n(patch.begin(),7,bytes.end()-7);}
  else bytes.resize(13);
  return bytes;
}
void action(Seed& s,Phase phase) {
  const auto result=phase==Phase::truncate?s.session.truncate_result(s.handle,13,effects()):
      phase==Phase::append?s.session.write_result(s.handle,UINT64_MAX,data(29,85),{true,false},effects()):
      s.session.write_result(s.handle,65523,data(100,86),{false,true},effects());
  require(result.bytes==(phase==Phase::truncate?0U:phase==Phase::append?29U:7U) &&
      result.offset==(phase==Phase::truncate?0U:phase==Phase::append?65530U:65523U),
      "successful mutation result escaped wrong count/EOF");
  require(result.info.inode==2 && result.info.logical_bytes==changed(phase).size() &&
      result.info.modified_sequence==result.transaction.sequence && !result.info.directory &&
      result.info.unlinked && !result.info.delete_pending && result.info.basic==updated(effects()),
      "successful mutation result did not describe the SAME durable detached inode/tuple");
}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool newer) {
  require(view.inode_state(2).unlinked && !view.inode_state(2).delete_pending &&
      view.read_inode(2,0,100000)==(newer?changed(phase):data(65530,83)) &&
      view.basic_metadata(2)==(newer?updated(effects()):basic()),
      "interrupted automatic data/basic mutation exposed a partial state");
  require(view.read("keep",0,100)==data(91,84) && view.basic_metadata(3)==basic(),
      "automatic mutation lost unrelated acknowledged bytes/tuple");
}
void faults(StoreCompressionMode mode,Phase phase) {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto s=seed(probe,mode);probe->calls=0;action(s,phase);steps=probe->calls;}
  require(steps>3 && steps<80,"automatic data/basic trace out of bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> old;std::uint64_t sequence=0;
    {
      auto s=seed(ram,mode);old.emplace(s.session.snapshot());sequence=old->sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(s,phase);},NativeFsErrorCode::storage_failure,"failed automatic data/basic commit acknowledged");
      const auto stats=s.session.stats();
      require(s.session.poisoned() && stats.contexts==2 && stats.cleaned==1 &&
          stats.delete_requests==1 && !stats.latched_inodes,"failed automatic mutation released/changed owned context");
      const auto calls=ram->calls;
      rejects_session([&]{action(s,phase);},NativeSessionErrorCode::terminal,"failed automatic data/basic commit retried");
      require(ram->calls==calls,"poisoned automatic data/basic operation reached backing");
    }
    expected(*old,phase,false);
    auto recovered=image(ram->stable);auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"automatic mutation lost acknowledged sequence");
    expected(view,phase,view.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    // Closing the original owning session permits detached retirement only now.
    rejects([&]{(void)writer.snapshot().stat_inode(2);},NativeFsErrorCode::not_found,"closed-session recovery retained detached inode");
    writer.write_inode_with_metadata(3,UINT64_MAX,data(7,87),{true,false},effects(14,stamp+3));
    auto keep=data(91,84);const auto addition=data(7,87);keep.insert(keep.end(),addition.begin(),addition.end());
    auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable)));
    require(reopened.read("keep",0,100)==keep && reopened.basic_metadata(3)==updated(effects(14,stamp+3)) &&
        !reopened.needs_session_recovery(),"independent CPU recovery could not continue combined data/basic writes");
  }
  require(cases==steps*3,"automatic data/basic fault point omitted");
  std::cout<<"Native mutation metadata evidence: {\"schema\":1,\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"phase\":\""<<name(phase)
      <<"\",\"io_steps\":"<<steps<<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  static_assert(std::is_same_v<NativeAutomaticMetadata,NativeCleanupMetadata>);
  if(argc!=2)throw std::runtime_error("one bounded mutation metadata group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}){masks(mode);admission(mode);owner_and_identity(mode);}
    std::cout<<"Native mutation metadata invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    const auto phase=group=="--append-faults"?Phase::append:group=="--constrained-faults"?Phase::constrained:
        group=="--truncate-faults"?Phase::truncate:throw std::runtime_error("unknown mutation metadata group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})faults(mode,phase);
  }
  return 0;
}catch(const std::exception& error){std::cerr<<"Native mutation metadata failure: "<<error.what()<<"\n";return 1;}}
