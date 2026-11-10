#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;using namespace native_fixture;namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0;
constexpr std::uint64_t stamp=133800000000000131ULL;
constexpr NativeFileAccess all{true,true,true};
NativeBasicMetadata tuple(bool dir=false){return {11,22,33,44,dir?0x10U:2U};}
NativeAutomaticMetadata effects(){return {stamp,true,true,true,true};}
NativeBasicMetadata updated(){return {11,stamp,stamp,stamp,0x22};}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected allocated-event native rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected allocated-event ownership rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer) {
  const std::array<NativeFileMutation,4> ops{{file_create("target"),file_write("target",0,data(7001,81)),
      file_create("keep"),file_write("keep",0,data(37,82))}};
  const NativeBasicMutation basic{0,1,tuple()};const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&basic,1},{&security,1});
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(37,82) && view.stat("keep").inode==3,
      "allocated event changed unrelated acknowledged data");
}
void lifecycle(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));
  NativePreparedCreateOptions options;options.access={true,true,false};options.initial_delete={true};
  const auto previous=session.snapshot().sequence(),flushes=ram->flushes;
  auto created=session.create_prepared_storage("new",false,tuple(),sec::descriptor(2),131072,options);
  require(created.state.reserved_bytes==131072 && !created.state.info.logical_bytes &&
      !created.state.info.delete_pending && created.transaction.sequence==previous+1 && ram->flushes==flushes+3 &&
      session.stats().contexts==1,"Create did not atomically reserve/install provisional ownership");
  auto old=session.snapshot();const auto bytes=ram->live;const auto calls=ram->calls,reads=ram->read_calls;
  session.close(created.handle);
  require(ram->live==bytes && ram->calls==calls && ram->read_calls==reads && !session.stats().contexts &&
      session.snapshot().allocation_info(4).reserved_bytes==131072 && !session.snapshot().needs_session_recovery(),
      "canceled allocated Create released data/reservation or performed I/O");
  auto h=session.prepare_open("new",all,NativeOpenKind::file,{},{},{true});session.adopt_prepared(h);
  auto written=session.write_storage_result(h,70000,data(73,83),{},effects());
  require(written.state.reserved_bytes==131072 && written.state.info.logical_bytes==70073 &&
      written.offset==70000 && written.bytes==73 && written.state.info.basic==updated(),
      "captured write storage state differs");
  auto truncated=session.truncate_storage_result(h,7001);
  require(truncated.state.reserved_bytes==131072 && truncated.state.info.logical_bytes==7001 &&
      !truncated.bytes && !truncated.offset,"captured EOF result lost retained reservation");
  auto noop=session.write_storage_result(h,UINT64_MAX,{}, {false,true},effects());
  require(noop.state.reserved_bytes==131072 && !noop.bytes &&
      noop.transaction.sequence==truncated.transaction.sequence,"constrained no-op storage result published");
  session.cleanup(h,true);require(session.allocation_info(h).info.unlinked,"initial deletion lost allocated identity");
  session.close(h);keep(session.snapshot());
  require(old.allocation_info(4).reserved_bytes==131072 && !old.stat("new").logical_bytes,
      "later allocated mutation changed old snapshot");
  auto dir=session.create_prepared_storage("dir",true,tuple(true),sec::descriptor(),{});
  require(!dir.state.reserved_bytes && dir.state.info.directory,"directory storage creation invented reservation");
  session.close(dir.handle);
  auto target=session.open("target",all);session.reserve_result(target,131072);
  auto before=session.snapshot();const auto before_flushes=ram->flushes;
  session.cleanup_event(target,false,{effects(),true});
  require(ram->flushes==before_flushes+3 && session.allocation_info(target).reserved_bytes==8192 &&
      session.basic_metadata(target)==updated() && session.read(target,0,10000)==data(7001,81),
      "Cleanup trim/basic/data was not one publication");
  const auto duplicate_calls=ram->calls;auto later=effects();later.utc_time++;
  session.cleanup_event(target,false,{later,true});
  require(ram->calls==duplicate_calls && session.basic_metadata(target)==updated(),
      "duplicate trim replaced first successful stamp");
  denies([&]{session.cleanup(target,false,effects());},NativeSessionErrorCode::invalid_options);
  session.close(target);
  require(before.allocation_info(2).reserved_bytes==131072 && before.basic_metadata(2)==tuple(),
      "trim changed an old snapshot");
  auto pending=session.prepare_overwrite("target",{true,false,false},{},{},{true,false},{});
  const auto overwritten=session.overwrite_prepared_storage_result(pending,{stamp,4,false},65536);
  auto peer=session.open("target",{true,false,false},NativeOpenKind::file,{},{true,false});
  require(overwritten.state.reserved_bytes==65536 && !overwritten.state.info.logical_bytes &&
      overwritten.state.info.basic==NativeBasicMetadata{11,stamp,stamp,stamp,0x26} &&
      session.security_descriptor(peer)==sec::descriptor(),
      "Overwrite did not capture reserved EOF/basic/security preservation");
  const auto cancel_bytes=ram->live;const auto cancel_calls=ram->calls,cancel_reads=ram->read_calls;
  session.close(pending);
  require(ram->live==cancel_bytes && ram->calls==cancel_calls && ram->read_calls==cancel_reads,
      "healthy allocated overwrite cancellation performed I/O/rollback");
  session.cleanup(peer,false);session.close(peer);
  require(!session.stats().contexts,"allocated event lifecycle left an owned context");
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));const auto bytes=ram->live;
  const auto calls=ram->calls,reads=ram->read_calls;
  rejects([&]{session.create_prepared_storage("bad",false,tuple(),sec::descriptor(),1);},NativeFsErrorCode::invalid_operation);
  rejects([&]{session.create_prepared_storage("bad",true,tuple(true),sec::descriptor(),0);},NativeFsErrorCode::invalid_operation);
  require(ram->live==bytes && ram->calls==calls && ram->read_calls==reads && !session.stats().contexts &&
      !session.poisoned(),"bad creation reservation touched backing/leaked context");
  auto h=session.open("target",all);
  const auto state=session.stats();
  rejects([&]{session.cleanup_event(h,false,{effects(),true});},NativeFsErrorCode::invalid_operation);
  require(session.stats().cleaned==state.cleaned && !session.poisoned() && ram->calls==calls,
      "untracked trim silently enabled allocation/published metadata");
  denies([&]{session.overwrite_storage_result(h,{stamp,4,false},8192);},NativeSessionErrorCode::access_denied);
  session.reserve_result(h,kNativeAllocationCapacity-kNativeExtentBytes);
  const auto before=ram->live;const auto before_calls=ram->calls;const auto contexts=session.stats().contexts;
  rejects([&]{session.create_prepared_storage("full",false,tuple(),sec::descriptor(),65536);},NativeFsErrorCode::limit);
  require(ram->live==before && ram->calls==before_calls && session.stats().contexts==contexts &&
      !session.poisoned(),"overcommitted creation changed durable state/context");
  session.cleanup(h,false);session.close(h);
}
enum class Phase {create,merge,supersede,trim};
const char* name(Phase p){return p==Phase::create?"create":p==Phase::merge?"merge":p==Phase::supersede?"supersede":"trim";}
struct Case {
  std::shared_ptr<Ram> ram;NativeFileSession session;NativeFileHandle handle,created;
  NativeInodeAllocationInfo captured;
};
Case setup(StoreCompressionMode mode,bool compact,Phase p) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  if(p!=Phase::create)writer.reserve_inode_result(2,131072);
  auto session=NativeFileSession::create(std::move(writer));NativeFileHandle h;
  if(p==Phase::merge || p==Phase::supersede)h=session.prepare_overwrite("target",{true,false,false},{},{},
      {p==Phase::merge,p==Phase::supersede},{});
  else h=session.open("target",all);
  return {ram,std::move(session),h,{},{}};
}
void action(Case& c,Phase p) {
  if(p==Phase::create) {
    NativePreparedCreateOptions options;options.initial_delete={true};
    auto r=c.session.create_prepared_storage("new",false,tuple(),sec::descriptor(2),131072,options);
    c.created=r.handle;c.captured=r.state;
  } else if(p==Phase::trim)c.session.cleanup_event(c.handle,false,{effects(),true});
  else c.captured=c.session.overwrite_prepared_storage_result(c.handle,{stamp,4,p==Phase::supersede},65536).state;
}
void closure(const NativeFilesystemSnapshot& view,Phase p,bool changed) {
  keep(view);const auto target=view.allocation_info(2);
  if(p==Phase::create) {
    require(!target.reserved_bytes && view.read_inode(2,0,10000)==data(7001,81) &&
        view.basic_metadata(2)==tuple(),"allocated creation altered prior inode");
    if(changed)require(view.stat("new").inode==4 && view.allocation_info(4).reserved_bytes==131072 &&
        !view.allocation_info(4).info.logical_bytes && !view.needs_session_recovery() &&
        view.basic_metadata(4)==tuple() && view.security_descriptor(4)==sec::descriptor(2),
        "allocated creation recovery mixed inode/basic/security/reservation");
    else rejects([&]{view.stat("new");},NativeFsErrorCode::not_found);
  } else {
    const bool overwritten=changed && p!=Phase::trim;
    const auto basic=changed?(p==Phase::trim?updated():NativeBasicMetadata{11,stamp,stamp,stamp,p==Phase::merge?0x26U:0x24U}):tuple();
    const auto allocation=changed?(p==Phase::trim?8192:65536):131072;
    require(target.reserved_bytes==allocation && target.info.logical_bytes==(overwritten?0:7001) &&
        view.basic_metadata(2)==basic && view.security_descriptor(2)==sec::descriptor() &&
        view.read_inode(2,0,10000)==(overwritten?Bytes{}:data(7001,81)),
        "allocated event recovery mixed EOF/data/basic/security/reservation");
  }
}
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();bool sealed=false;mutable unsigned blocked=0;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  void allowed() const{if(sealed){++blocked;throw std::runtime_error("post-publication unavailable");}}
  std::uint64_t extent() const override{allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t n) override{allowed();ram->admit_write(n);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{allowed();ram->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override{allowed();ram->write_exact(at,in);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
};
void sealed(StoreCompressionMode mode,bool compact,Phase p) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));seed(writer);
  if(p!=Phase::create)writer.reserve_inode_result(2,131072);
  auto session=NativeFileSession::create(std::move(writer));NativeFileHandle h;
  if(p==Phase::merge || p==Phase::supersede)h=session.prepare_overwrite("target",{true,false,false},{},{},
      {p==Phase::merge,p==Phase::supersede},{});
  else h=session.open("target",all);
  Case c{io->ram,std::move(session),h,{},{}};io->seal_at=io->ram->flushes+3;action(c,p);
  require(io->sealed && !io->blocked && c.session.stats().contexts==(p==Phase::create?2U:1U) &&
      c.session.stats().cleaned==(p==Phase::trim?1U:0U),"allocated event used backing after publication/lost ownership");
  if(p!=Phase::trim)require(c.captured.reserved_bytes==(p==Phase::create?131072:65536),
      "sealed captured reservation missing");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  closure(view,p,true);
  rejects([&]{c.session.allocation_info(c.handle);},NativeFsErrorCode::storage_failure);
  const auto blocked=io->blocked;
  denies([&]{c.session.close(c.handle);},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked,"fenced allocated event retried backing");++sealed_cases;
}
void sealed_data(StoreCompressionMode mode,bool compact,bool truncate) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));seed(writer);
  writer.reserve_inode_result(2,8192);auto session=NativeFileSession::create(std::move(writer));
  auto h=session.open("target",all);io->seal_at=io->ram->flushes+3;
  const auto result=truncate?session.truncate_storage_result(h,70073,effects()):
      session.write_storage_result(h,70000,data(73,84),{},effects());
  require(io->sealed && !io->blocked && result.state.reserved_bytes==73728 &&
      result.state.info.logical_bytes==70073 && result.state.info.basic==updated() &&
      result.bytes==(truncate?0U:73U) && result.offset==(truncate?0U:70000U),
      "data mutation storage return used post-commit I/O or incomplete facts");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  auto bytes=data(7001,81);bytes.resize(70073);
  if(!truncate){const auto extra=data(73,84);std::copy(extra.begin(),extra.end(),bytes.begin()+70000);}
  require(view.read_inode(2,0,100000)==bytes && view.allocation_info(2).reserved_bytes==73728,
      "sealed storage mutation differs from independent recovery");keep(view);
  rejects([&]{session.info(h);},NativeFsErrorCode::storage_failure);const auto blocked=io->blocked;
  denies([&]{session.close(h);},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked && session.stats().contexts==1,"sealed data failure released/retried");++sealed_cases;
}
void faults(StoreCompressionMode mode,bool compact,Phase p) {
  std::size_t steps=0;{auto c=setup(mode,compact,p);c.ram->calls=0;action(c,p);steps=c.ram->calls;}
  require(steps>3 && steps<100,"allocated-event trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;std::shared_ptr<Ram> ram;std::uint64_t sequence=0;
    {auto c=setup(mode,compact,p);ram=c.ram;auto old=c.session.snapshot();sequence=old.sequence();
      const auto before=c.session.stats();ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(c,p);},NativeFsErrorCode::storage_failure);
      const auto after=c.session.stats();
      require(c.session.poisoned() && before.contexts==after.contexts && before.cleaned==after.cleaned &&
          before.latched_inodes==after.latched_inodes,"failed allocated event changed ownership/latches");
      const auto calls=ram->calls,reads=ram->read_calls;
      denies([&]{c.session.close(c.handle);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls && ram->read_calls==reads,"fenced allocated event Close retried");closure(old,p,false);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"allocated event recovery sequence lost/fabricated");
    closure(view,p,view.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,85))}};
    writer.apply(continuation);keep(writer.snapshot());
    require(writer.snapshot().read("continued",0,100)==data(17,85),"allocated event independent CPU continuation failed");
  }
  require(cases==3*steps,"allocated-event fault point omitted");
  std::cout<<"Native allocated-event evidence: {\"schema\":1,\"phase\":\""<<name(p)
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one allocated-event group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    admission();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      lifecycle(mode,compact);
      for(const auto p:{Phase::create,Phase::merge,Phase::supersede,Phase::trim})sealed(mode,compact,p);
      for(bool truncate:{false,true})sealed_data(mode,compact,truncate);
    }
    require(sealed_cases==24,"allocated event sealed case omitted");
    std::cout<<"Native allocated-event invariants: {\"schema\":1,\"checks\":"<<checks
        <<",\"sealed_cases\":"<<sealed_cases<<",\"complete\":true}\n";
  } else {
    const auto p=group=="--create-faults"?Phase::create:group=="--merge-faults"?Phase::merge:
        group=="--supersede-faults"?Phase::supersede:group=="--trim-faults"?Phase::trim:
        throw std::runtime_error("unknown allocated-event group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,p);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native allocated-event failure: "<<e.what()<<"\n";return 1;}}
