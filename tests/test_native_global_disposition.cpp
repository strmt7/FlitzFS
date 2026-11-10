#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;using namespace native_fixture;namespace sec=native_security_fixture;
unsigned checks=0;
constexpr NativeFileAccess all{true,true,true};
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected global-disposition native rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected global-disposition ownership rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& w) {
  const std::array<NativeFileMutation,4> ops{{file_create("target"),file_write("target",0,data(7001,111)),
      file_create("keep"),file_write("keep",0,data(37,112))}};
  const NativeBasicMutation basic{0,1,{11,22,33,44,2}};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  w.apply_with_metadata(ops,{&basic,1},{&security,1});w.reserve_inode_result(2,8192);
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(37,112),"global disposition lost unrelated acknowledged bytes");
}
void policies(StoreCompressionMode mode,bool compact) {
  for(const auto policy:{NativeDispositionPolicy::aggregate,NativeDispositionPolicy::windows_global}) {
    auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
    auto s=NativeFileSession::create(std::move(writer),policy);auto a=s.open("target",all),b=s.open("target",all);
    const auto before=s.stats();auto found=s.lookup("TARGET");
    require(found.state.info.inode==2&&found.state.reserved_bytes==8192&&found.security==sec::descriptor()&&
        s.stats().contexts==before.contexts,"trusted pre-open lookup changed ownership/lost facts");
    s.set_delete(a,true);s.set_delete(b,true);s.set_delete(a,false);
    const bool global=policy==NativeDispositionPolicy::windows_global;
    require(s.info(b).delete_pending==!global && s.stats().delete_requests==(global?0U:1U),
        "runtime disposition policy did not preserve/select global cancellation");
    if(!global)s.set_delete(b,false);
    s.set_delete(a,true);s.cleanup(a,false);s.close(a);
    require(s.stats().latched_inodes==1&&s.info(b).delete_pending,"explicit Cleanup lost latched intent");
    s.set_delete(b,false);
    require(s.info(b).delete_pending==!global&&s.stats().latched_inodes==(global?0U:1U),
        "global cancellation did not clear prior latched explicit intent or changed aggregate policy");
    if(global) {
      auto peer=s.open("target",{true,false,false});s.cleanup(peer,false);s.close(peer);
      s.cleanup(b,false);s.close(b);
      require(s.snapshot().read("target",0,10000)==data(7001,111),"global cancel removed file");
    }else {s.cleanup(b,true);s.close(b);rejects([&]{s.snapshot().stat("target");},NativeFsErrorCode::not_found);}
    require(!s.stats().contexts,"disposition policy left contexts");keep(s.snapshot());
  }
}
void initial(StoreCompressionMode mode,bool compact,bool cancel_before_cleanup) {
  auto ram=std::make_shared<Ram>();auto w=fresh(ram,mode,compact);seed(w);
  auto s=NativeFileSession::create(std::move(w),NativeDispositionPolicy::windows_global);
  auto a=s.prepare_open("target",{true,false,false},NativeOpenKind::file,{},{},{true});s.adopt_prepared(a);
  auto b=s.open("target",all);
  if(cancel_before_cleanup)s.set_delete(b,false);
  s.cleanup(a,false);
  require(s.lookup("target").state.info.delete_pending,"global cancel incorrectly erased unprocessed initial option");
  s.close(a);s.set_delete(b,false);
  require(!s.info(b).delete_pending&&!s.stats().latched_inodes,"global cancel left processed initial latch");
  s.cleanup(b,false);s.close(b);require(!s.snapshot().needs_session_recovery(),"global cancellation left durable deletion");
  keep(s.snapshot());
}
void nonempty() {
  auto ram=std::make_shared<Ram>();auto w=fresh(ram,StoreCompressionMode::raw,false);
  const std::array<NativeFileMutation,2> ops{{file_create("dir",true),file_create("dir/child")}};w.apply(ops);
  auto s=NativeFileSession::create(std::move(w),NativeDispositionPolicy::windows_global);
  auto a=s.prepare_open("dir",{true,false,false},NativeOpenKind::directory,{},{},{true});s.adopt_prepared(a);
  auto b=s.open("dir",all);s.cleanup(a,false);s.close(a);
  require(s.lookup("dir").state.info.delete_pending&&!s.snapshot().inode_state(2).delete_pending,
      "nonempty global lookup lost volatile/persisted distinction");
  s.set_delete(b,false);s.create_entry("dir/new");
  require(!s.lookup("dir").state.info.delete_pending&&s.snapshot().stat("dir/child").inode==3,
      "global cancel lost directory child or left volatile admission latch");
  s.cleanup(b,false);s.close(b);
}
enum class Phase {arm,cancel};
struct Case{std::shared_ptr<Ram> ram;NativeFileSession session;NativeFileHandle handle;};
Case setup(StoreCompressionMode mode,bool compact,Phase p) {
  auto ram=std::make_shared<Ram>();auto w=fresh(ram,mode,compact);seed(w);
  auto s=NativeFileSession::create(std::move(w),NativeDispositionPolicy::windows_global);
  auto a=s.open("target",all),b=s.open("target",all);
  if(p==Phase::cancel){s.set_delete(a,true);s.cleanup(a,false);s.close(a);}
  else {s.cleanup(a,false);s.close(a);}
  return {ram,std::move(s),b};
}
void action(Case& c,Phase p){c.session.set_delete(c.handle,p==Phase::arm);}
void closure(const NativeFilesystemSnapshot& v,bool pending) {
  require(v.inode_state(2).delete_pending==pending&&v.read("target",0,10000)==data(7001,111)&&
      v.security_descriptor(2)==sec::descriptor()&&v.allocation_info(2).reserved_bytes==8192,
      "global disposition mixed data/security/reservation/intent");keep(v);
}
void faults(StoreCompressionMode mode,bool compact,Phase p) {
  std::size_t steps=0;{auto c=setup(mode,compact,p);c.ram->calls=0;action(c,p);steps=c.ram->calls;}
  require(steps>3&&steps<100,"global disposition trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t cut=0;cut<steps;++cut) {
    ++cases;std::shared_ptr<Ram> ram;std::uint64_t sequence=0;
    {auto c=setup(mode,compact,p);ram=c.ram;auto old=c.session.snapshot();sequence=old.sequence();
      const auto before=c.session.stats();ram->calls=0;ram->fail_at=cut;ram->failure=fault;
      rejects([&]{action(c,p);},NativeFsErrorCode::storage_failure);
      const auto after=c.session.stats();
      require(c.session.poisoned()&&before.contexts==after.contexts&&before.cleaned==after.cleaned&&
          before.latched_inodes==after.latched_inodes&&before.delete_requests==after.delete_requests,
          "failed global disposition changed runtime ownership/intent before durable success");
      const auto calls=ram->calls,reads=ram->read_calls;
      denies([&]{c.session.set_delete(c.handle,false);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls&&ram->read_calls==reads,"fenced global disposition retried");closure(old,p==Phase::cancel);}
    auto recovered=image(ram->stable,ram->failure_unit);auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence||view.sequence()==sequence+1,"global disposition recovery sequence rollback/fabrication");
    const bool changed=view.sequence()!=sequence;const bool pending=(p==Phase::arm)==changed;closure(view,pending);
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,113))}};
    writer.apply(continuation);keep(writer.snapshot());
    if(pending)rejects([&]{writer.snapshot().stat("target");},NativeFsErrorCode::not_found);
    else require(writer.snapshot().read("target",0,10000)==data(7001,111),"canceled deletion recovery lost data");
  }
  std::cout<<"Native global disposition evidence: {\"schema\":1,\"phase\":\""<<(p==Phase::arm?"arm":"cancel")
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one global disposition group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    nonempty();for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      policies(mode,compact);initial(mode,compact,false);initial(mode,compact,true);
    }
    std::cout<<"Native global disposition invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  }else {
    const auto p=group=="--arm-faults"?Phase::arm:group=="--cancel-faults"?Phase::cancel:
        throw std::runtime_error("unknown global disposition group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true})faults(mode,compact,p);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native global disposition failure: "<<e.what()<<"\n";return 1;}}
