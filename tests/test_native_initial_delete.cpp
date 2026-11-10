#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0;
constexpr std::uint64_t stamp=133800000000000123ULL;
constexpr NativeFileAccess all{true,true,true};
NativeBasicMetadata tuple(bool dir=false){return {11,22,33,44,dir?0x10U:2U};}
NativeCleanupMetadata effects(){return {stamp,true,true,true,true};}
NativeBasicMetadata updated(bool dir=false){return {11,stamp,stamp,stamp,dir?0x10U:0x22U};}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected initial-delete native rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected initial-delete owned rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer,bool dir=false) {
  std::vector<NativeFileMutation> ops{file_create("target",dir),file_create("keep"),
      file_write("keep",0,data(37,51))};
  if(dir){ops.push_back(file_create("target/child"));ops.push_back(file_write("target/child",0,data(19,52)));}
  else ops.push_back(file_write("target",0,data(73,53)));
  const NativeBasicMutation basic{0,1,tuple(dir)};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&basic,1},{&security,1});
}
NativeFileHandle prepare(NativeFileSession& s,bool dir=false,bool initial=true) {
  return s.prepare_open("target",{true,true,false},dir?NativeOpenKind::directory:NativeOpenKind::file,
      {},{true,false},{initial});
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(37,51) && view.stat("keep").inode==3,
      "initial cleanup lost unrelated acknowledged bytes");
}
void prepared_routes(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));
  const auto baseline=ram->live;
  auto h=prepare(s);const auto calls=ram->calls,reads=ram->read_calls;
  denies([&]{s.read(h,0,1);},NativeSessionErrorCode::invalid_options);
  s.close(h);
  require(ram->live==baseline && ram->calls==calls && ram->read_calls==reads && !s.stats().contexts,
      "canceled initial Open persisted deletion or performed Close I/O");
  h=prepare(s);s.adopt_prepared(h);
  require(!s.info(h).delete_pending && !s.snapshot().needs_session_recovery(),
      "adoption made initial delete effective before Cleanup");
  denies([&]{s.set_delete(h,true);},NativeSessionErrorCode::access_denied);
  s.cleanup(h,true,effects());
  require(s.info(h).unlinked && s.read(h,0,100)==data(73,53) &&
      s.basic_metadata(h)==updated(),"single initial Cleanup did not atomically unlink/stamp/retain paging bytes");
  s.write(h,7,data(13,54));s.close(h);
  rejects([&]{s.snapshot().stat("target");},NativeFsErrorCode::not_found);
  for(bool dir:{false,true})for(bool cancel:{false,true}) {
    NativePreparedCreateOptions options;options.access={true,true,false};
    options.security_access={true,false};options.initial_delete={true};
    auto created=s.create_prepared("new",dir,tuple(dir),sec::descriptor(),options);
    require(!created.info.delete_pending && !s.snapshot().needs_session_recovery(),
        "prepared Create persisted its volatile initial option");
    if(cancel) {
      const auto bytes=ram->live;const auto cancel_calls=ram->calls;
      s.close(created.handle);
      require(ram->live==bytes && ram->calls==cancel_calls && s.snapshot().stat("new").directory==dir,
          "canceled prepared Create deleted its inode");
      auto cleanup=s.prepare_open("new",all,NativeOpenKind::any,{},{},{true});
      s.adopt_prepared(cleanup);s.cleanup(cleanup,true);s.close(cleanup);
    } else {
      s.adopt_prepared(created.handle);s.cleanup(created.handle,true,effects());s.close(created.handle);
    }
    rejects([&]{s.snapshot().stat("new");},NativeFsErrorCode::not_found);
  }
  for(bool replace:{false,true})for(bool cancel:{false,true}) {
    s.create_entry("target",false,tuple(),sec::descriptor());
    auto prepared=s.prepare_overwrite("target",{true,false,false},{},{true,false},{!replace,replace},{true});
    auto result=s.overwrite_prepared_result(prepared,{stamp,4,replace});
    require(!result.info.delete_pending && !s.snapshot().needs_session_recovery(),"overwrite persisted initial intent too early");
    if(cancel) {
      s.close(prepared);require(s.snapshot().stat("target").logical_bytes==0,"canceled overwritten inode vanished");
      auto cleanup=prepare(s);s.adopt_prepared(cleanup);s.cleanup(cleanup,true);s.close(cleanup);
    } else {s.adopt_prepared(prepared);s.cleanup(prepared,true);s.close(prepared);}
    rejects([&]{s.snapshot().stat("target");},NativeFsErrorCode::not_found);
  }
  require(!s.stats().contexts && !s.poisoned(),"prepared initial routes leaked contexts");keep(s.snapshot());
}
void pair(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s),peer=s.open("target",all);
  s.adopt_prepared(h);auto old=s.snapshot();const auto flushes=ram->flushes;
  s.cleanup(h,false,effects());
  require(s.info(peer).delete_pending && s.snapshot().inode_state(2).delete_pending &&
      s.basic_metadata(peer)==updated() && ram->flushes==flushes+3 &&
      s.stats().latched_inodes==1,"initial first Cleanup did not arm pending/stamp in ONE publication");
  auto bytes=ram->live;const auto calls=ram->calls;
  s.cleanup(h,false,{stamp+1,true,true,true,true});
  require(ram->live==bytes && ram->calls==calls && s.basic_metadata(h)==updated(),
      "duplicate initial Cleanup resampled/staged a second event");
  denies([&]{s.open("target",all);},NativeSessionErrorCode::delete_pending);
  s.close(h);
  require(s.stats().contexts==1 && s.stats().latched_inodes==1,"initial authority disappeared with first Close");
  s.cleanup(peer,true);require(s.info(peer).unlinked && !s.stats().latched_inodes,
      "last peer Cleanup lost initial authorization");
  require(old.read("target",0,100)==data(73,53) && !old.inode_state(2).delete_pending,"old snapshot changed");
  s.close(peer);keep(s.snapshot());
}
void directories(StoreCompressionMode mode,bool compact,bool becomes_empty) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer,true);
  auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s,true),peer=s.open("target",all);
  auto child=s.open("target/child",all);s.adopt_prepared(h);
  s.cleanup(h,false,effects());auto disk=s.snapshot();
  require(s.info(peer).delete_pending && !disk.inode_state(2).delete_pending &&
      !disk.needs_session_recovery() && disk.read("target/child",0,100)==data(19,52) &&
      s.stats().latched_inodes==1,"nonempty initial directory acquired unsafe durable deletion");
  denies([&]{s.open("target",all);},NativeSessionErrorCode::delete_pending);
  denies([&]{s.create_entry("target/new");},NativeSessionErrorCode::delete_pending);
  denies([&]{s.create_prepared("target/new",false,tuple(),sec::descriptor());},NativeSessionErrorCode::delete_pending);
  s.create_entry("outside");
  auto outside=s.open("outside",all);
  denies([&]{s.rename(outside,"target/new");},NativeSessionErrorCode::delete_pending);
  s.cleanup(outside,false);s.close(outside);
  s.close(h);
  if(becomes_empty) {s.set_delete(child,true);s.cleanup(child,true);s.close(child);}
  else {s.cleanup(child,false);s.close(child);}
  s.cleanup(peer,true,effects());
  if(becomes_empty)require(s.info(peer).unlinked,"now-empty initial directory was not unlinked");
  else require(!s.info(peer).delete_pending && !s.info(peer).unlinked &&
      s.list(peer).size()==1,"nonempty final Cleanup removed contents or left pending stuck");
  s.close(peer);
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable,ram->failure_unit)));
  keep(recovered);
  if(becomes_empty)rejects([&]{recovered.stat("target");},NativeFsErrorCode::not_found);
  else require(recovered.read("target/child",0,100)==data(19,52) && !recovered.needs_session_recovery(),
      "nonempty directory was not independently recoverable");
}
void cancellation_policy(bool dir,bool after_cleanup) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,true);seed(writer,dir);
  auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s,dir),peer=s.open("target",all);
  s.adopt_prepared(h);
  if(after_cleanup) {
    s.cleanup(h,false);s.close(h);s.set_delete(peer,false);
    require(!s.info(peer).delete_pending && !s.stats().latched_inodes &&
        !s.snapshot().needs_session_recovery(),"explicit false failed to cancel current initial pending state");
    if(dir)s.create_entry("target/added");
    s.cleanup(peer,false);s.close(peer);
    require(dir?s.snapshot().read("target/child",0,100)==data(19,52):
        s.snapshot().read("target",0,100)==data(73,53),"canceled initial pending lost contents");
  } else {
    s.set_delete(peer,false); // Does not clear the still-open descriptor's option.
    s.cleanup(h,false);
    require(s.info(peer).delete_pending,"initial option did not re-arm at its later Cleanup");
    s.close(h);s.cleanup(peer,true);s.close(peer);
    if(dir)require(s.snapshot().read("target/child",0,100)==data(19,52),"nonempty canceled/rearmed directory lost child");
    else rejects([&]{s.snapshot().stat("target");},NativeFsErrorCode::not_found);
  }
  keep(s.snapshot());
}
void mixed_latch_provenance() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,true);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s),explicit_owner=s.open("target",all),peer=s.open("target",all);
  s.adopt_prepared(h);s.set_delete(explicit_owner,true);s.cleanup(explicit_owner,false);s.close(explicit_owner);
  s.cleanup(h,false);s.close(h);s.set_delete(peer,false);
  require(s.info(peer).delete_pending && s.stats().latched_inodes==1,
      "canceling initial origin also canceled independent explicit latch");
  s.cleanup(peer,true);s.close(peer);keep(s.snapshot());
  rejects([&]{s.snapshot().stat("target");},NativeFsErrorCode::not_found);
}
void interrupted_runtime(bool dir,bool cleanup) {
  auto ram=std::make_shared<Ram>();std::uint64_t generation=0;
  {auto writer=fresh(ram,StoreCompressionMode::raw,true);seed(writer,dir);
    auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s,dir),peer=s.open("target",all);
    (void)peer;s.adopt_prepared(h);
    if(cleanup)s.cleanup(h,false,effects());
    generation=s.snapshot().storage_info().generation;}
  auto recovered=image(ram->stable,ram->failure_unit);
  auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),generation));
  auto view=writer.snapshot();keep(view);
  if(cleanup && !dir)rejects([&]{view.stat("target");},NativeFsErrorCode::not_found);
  else require(dir?view.read("target/child",0,100)==data(19,52):
      view.read("target",0,100)==data(73,53),"canceled/unarmed or nonempty directory recovery lost bytes");
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));const auto calls=ram->calls;
  denies([&]{s.prepare_open("",all,NativeOpenKind::directory,{},{},{true});},NativeSessionErrorCode::invalid_options);
  require(ram->calls==calls && !s.stats().contexts,"root initial delete admitted context");
  auto h=prepare(s);s.adopt_prepared(h);
  auto unresolved=s.prepare_open("target",all,NativeOpenKind::file,{},{});
  const auto reads=ram->read_calls;
  denies([&]{s.cleanup(h,false);},NativeSessionErrorCode::sharing_violation);
  require(ram->calls==calls && ram->read_calls==reads && !s.stats().cleaned,
      "initial deletion armed while another Open was provisional");
  s.close(unresolved);s.cleanup(h,true);s.close(h);
  auto plain=s.create_prepared("plain",false,tuple(),sec::descriptor());s.adopt_prepared(plain.handle);
  try{s.cleanup(plain.handle,true);throw std::runtime_error("unauthorized Cleanup deletion accepted");}
  catch(const NativeSessionError& e){require(e.code()==NativeSessionErrorCode::terminal,"wrong unauthorized-delete error");}
  require(s.poisoned() && s.stats().contexts==1,"unauthorized Cleanup did not retain fenced ownership");
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
void sealed(StoreCompressionMode mode,bool compact,bool dir) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));
  seed(writer,dir);auto s=NativeFileSession::create(std::move(writer));
  auto h=prepare(s,dir),peer=s.open("target",all);s.adopt_prepared(h);io->seal_at=io->ram->flushes+3;
  s.cleanup(h,false,effects());
  require(io->sealed && !io->blocked && s.stats().contexts==2 && s.stats().cleaned==1 &&
      s.stats().latched_inodes==1,"initial Cleanup queried after publication or lost latch/context");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  keep(view);require(view.basic_metadata(2)==updated(dir) && view.inode_state(2).delete_pending==!dir,
      "sealed initial result/latch differs from independent bytes");
  rejects([&]{s.info(peer);},NativeFsErrorCode::storage_failure);
  const auto blocked=io->blocked;
  denies([&]{s.close(h);},NativeSessionErrorCode::terminal);
  require(s.stats().contexts==2 && io->blocked==blocked,"fenced initial Close retried/released");++sealed_cases;
}
enum class Phase {arm,unlink,retain,final_dir};
const char* name(Phase p){return p==Phase::arm?"arm":p==Phase::unlink?"unlink":p==Phase::retain?"retain":"final-dir";}
struct Case {
  std::shared_ptr<Ram> ram;
  NativeFileSession session;
  NativeFileHandle handle;
  bool dir=false;
};
Case setup(StoreCompressionMode mode,bool compact,Phase p) {
  auto ram=std::make_shared<Ram>();const bool dir=p==Phase::retain || p==Phase::final_dir;
  auto writer=fresh(ram,mode,compact);seed(writer,dir);auto s=NativeFileSession::create(std::move(writer));
  auto h=prepare(s,dir);s.adopt_prepared(h);
  if(p==Phase::arm || p==Phase::retain)(void)s.open("target",all);
  if(p==Phase::final_dir) {
    auto peer=s.open("target",all),child=s.open("target/child",all);
    s.cleanup(h,false);s.close(h);
    s.set_delete(child,true);s.cleanup(child,true);s.close(child);h=peer;
  }
  return {ram,std::move(s),h,dir};
}
void action(Case& c,Phase p){c.session.cleanup(c.handle,p==Phase::unlink || p==Phase::final_dir,effects());}
void closure(const NativeFilesystemSnapshot& view,Phase p,bool changed) {
  keep(view);const auto state=view.inode_state(2);const bool dir=p==Phase::retain || p==Phase::final_dir;
  require(view.basic_metadata(2)==(changed?updated(dir):tuple(dir)) && view.security_descriptor(2)==sec::descriptor() &&
      state.delete_pending==(changed && p==Phase::arm) &&
      state.unlinked==(changed && (p==Phase::unlink || p==Phase::final_dir)),
      "initial Cleanup fault mixed data/basic/security/intent");
  if(p==Phase::retain)require(view.read("target/child",0,100)==data(19,52),"retained directory fault lost child");
  if(!dir)require(view.read_inode(2,0,100)==data(73,53),"initial Cleanup fault discarded retained paging data");
}
void faults(StoreCompressionMode mode,bool compact,Phase p) {
  std::size_t steps=0;
  {auto c=setup(mode,compact,p);c.ram->calls=0;action(c,p);steps=c.ram->calls;}
  require(steps>3 && steps<100,"initial-delete trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;std::shared_ptr<Ram> ram;std::uint64_t sequence=0;
    {auto c=setup(mode,compact,p);ram=c.ram;auto old=c.session.snapshot();sequence=old.sequence();
      const auto before=c.session.stats();ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(c,p);},NativeFsErrorCode::storage_failure);
      const auto after=c.session.stats();
      require(c.session.poisoned() && before.contexts==after.contexts && before.cleaned==after.cleaned &&
          before.latched_inodes==after.latched_inodes,"failed initial Cleanup changed ownership/latches");
      const auto calls=ram->calls,reads=ram->read_calls;
      denies([&]{c.session.close(c.handle);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls && ram->read_calls==reads,"fenced initial Cleanup Close retried");closure(old,p,false);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"initial recovery lost/fabricated sequence");
    const bool changed=view.sequence()==sequence+1;closure(view,p,changed);
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,55))}};
    writer.apply(continuation);auto continued=writer.snapshot();keep(continued);
    require(continued.read("continued",0,100)==data(17,55),"CPU continuation failed after initial Cleanup");
    if(p==Phase::retain)require(continued.read("target/child",0,100)==data(19,52),"nonempty recovery removed child");
    else if(changed)rejects([&]{continued.stat("target");},NativeFsErrorCode::not_found);
  }
  require(cases==3*steps,"initial fault point omitted");
  std::cout<<"Native initial-delete evidence: {\"schema\":1,\"phase\":\""<<name(p)
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one initial-delete group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    admission();mixed_latch_provenance();for(bool dir:{false,true})for(bool cleanup:{false,true}) {
      interrupted_runtime(dir,cleanup);cancellation_policy(dir,cleanup);
    }
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      prepared_routes(mode,compact);pair(mode,compact);
      for(bool empty:{false,true})directories(mode,compact,empty);
      for(bool dir:{false,true})sealed(mode,compact,dir);
    }
    require(sealed_cases==8,"initial sealed case omitted");
    std::cout<<"Native initial-delete invariants: {\"schema\":1,\"checks\":"<<checks
        <<",\"sealed_cases\":"<<sealed_cases<<",\"complete\":true}\n";
  } else {
    const auto p=group=="--arm-faults"?Phase::arm:group=="--unlink-faults"?Phase::unlink:
        group=="--retain-faults"?Phase::retain:group=="--final-dir-faults"?Phase::final_dir:
        throw std::runtime_error("unknown initial-delete group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,p);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native initial-delete failure: "<<e.what()<<"\n";return 1;}}
