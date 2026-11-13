#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "record_digest.hpp"
#include <iostream>
#include <optional>
namespace {
using namespace flitzfs;using namespace native_fixture;
using Phase=NativeOptimizationPhase;using Progress=NativeOptimizationBudgetProgress;
unsigned checks=0,cuts=0;
void require(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void rejects(F&& f,const char* why){
  ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
StoreIdentity id(unsigned n){auto x=identity();x[0]=std::byte(n);return x;}
void reset(const std::shared_ptr<Ram>& r){
  r->calls=r->writes=r->flushes=0;r->read_calls=r->read_bytes=0;r->write_bytes=0;
  r->fail_at=(std::numeric_limits<std::size_t>::max)();r->failure=0;
}
struct Fixture {
  std::shared_ptr<Ram> head=std::make_shared<Ram>(),image=std::make_shared<Ram>(),area=std::make_shared<Ram>();
  std::unique_ptr<NativeFileSession> session;
  NativeFileHandle rw,cleaned;
  Bytes expected=data(7,1);unsigned seed=2;
  Fixture(){
    session=std::make_unique<NativeFileSession>(NativeFileSession::create_volume(head,id(70),image,id(71),
        {StoreCompressionMode::raw}));
    session->enable_workload_accounting();session->create_entry("keep");
    open();session->write(rw,0,expected);session->flush(rw);
  }
  void open(){
    rw=session->open("keep",{true,true,false},NativeOpenKind::file);
    cleaned=session->open("keep",{true,false,false},NativeOpenKind::file);
    session->cleanup(cleaned,false);
  }
  void close(){
    session->cleanup(rw,false);session->close(rw);session->close(cleaned);
    require(session->stats().contexts==0,"real context retirement");session.reset();
  }
  void resume(){
    session=std::make_unique<NativeFileSession>(NativeFileSession::resume_volume(head,id(70),1,
        [&](const auto& which)->std::shared_ptr<StoreByteIo>{return which==id(71)?image:nullptr;}));
    open();
  }
  void earn(std::size_t n){
    auto bytes=data(n,seed++);session->write(rw,0,bytes);session->flush(rw);
    if(expected.size()<bytes.size())expected.resize(bytes.size());
    std::copy(bytes.begin(),bytes.end(),expected.begin());
  }
  void data_ok(){
    require(!session->poisoned()&&session->stats().contexts==2&&session->stats().cleaned==1&&
        session->read(cleaned,0,1024*1024)==expected,"budget changed data or live cleaned contexts");
    auto copy=native_fixture::image(image->stable);
    auto read=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(copy));
    require(read.read("keep",0,1024*1024)==expected&&read.workload_info()==session->workload_info(),
        "independent CPU data/provenance mismatch");
  }
  NativeOptimizationInfo create(unsigned limit=4){return session->create_budgeted_optimization_area(area,limit);}
  OptimizationState proposal(){
    auto state=session->optimization_snapshot().state;
    if(state.profiles.empty())state.profiles.push_back({1,64,32,1,0,1,1,1000000});
    else ++state.profiles[0].cpu_ns;
    return state;
  }
  void damage(unsigned count){
    for(unsigned i=0;i<count;++i){const auto at=i*kStoreCommitUnit+96;
      area->live.at(static_cast<std::size_t>(at))^=std::byte{1};}
    area->stable=area->live;
  }
};
void admission_and_binding_limit(){
  auto head=std::make_shared<Ram>(),image=std::make_shared<Ram>(),area=std::make_shared<Ram>();
  auto plain=NativeFileSession::create_volume(head,id(80),image,id(81));
  reset(head);reset(image);
  rejects([&]{plain.create_budgeted_optimization_area(area);},"untracked input admitted budget");
  require(head->calls==0&&image->read_calls==0&&area->calls==0&&
      !plain.optimization_info().budgeted,"missing provenance did I/O or selected budget mode");
  Fixture f;f.create();
  for(unsigned replacement=1;replacement<NativeFileSession::max_optimization_bindings;++replacement){
    const auto state=f.session->optimization_snapshot().state;f.damage(2);
    rejects([&]{f.session->publish_optimization_state(state);},"lost binding audit accepted");
    auto target=std::make_shared<Ram>();
    f.session->rebuild_budgeted_optimization_area(target,4); // Establish current loss baseline only.
    f.earn(2*kOptimizationAreaBytes);
    const auto info=f.session->rebuild_budgeted_optimization_area(target,4);
    require(info.phase==Phase::ready&&info.binding_attempts==replacement+1&&
        info.owned_area_bytes==(replacement+1)*kOptimizationAreaBytes,
        "funded binding history/accounting");
    f.area=std::move(target);
  }
  const auto state=f.session->optimization_snapshot().state;f.damage(2);
  rejects([&]{f.session->publish_optimization_state(state);},"fourth-loss audit accepted");
  auto target=std::make_shared<Ram>();reset(f.image);
  rejects([&]{f.session->rebuild_budgeted_optimization_area(target,4);},"funding escaped binding ceiling");
  require(!f.session->prepare_budgeted_optimization_rebuild()&&
      target->calls==0&&target->read_calls==0&&f.image->read_calls==0,
      "exhausted binding ceiling retried I/O");
  f.data_ok();
}
void rebuild_preflight(){
  Fixture f;f.create();reset(f.image);reset(f.area);
  rejects([&]{f.session->prepare_budgeted_optimization_rebuild();},"healthy preflight reset allowance");
  require(f.image->read_calls==0&&f.area->calls==0,"healthy preflight performed I/O");
  const auto state=f.session->optimization_snapshot().state;f.damage(2);
  rejects([&]{f.session->publish_optimization_state(state);},"preflight loss audit accepted");
  reset(f.image);reset(f.area);
  require(!f.session->prepare_budgeted_optimization_rebuild()&&
      f.session->optimization_info().loss_input_baseline==f.session->workload_info().committed_input_bytes&&
      f.image->read_calls>0&&f.area->read_calls==0&&f.area->calls==0,"preflight failed to establish checked baseline");
  reset(f.image);
  for(unsigned i=0;i<8;++i)
    require(!f.session->prepare_budgeted_optimization_rebuild()&&f.image->read_calls==0&&
        f.area->read_calls==0&&f.area->calls==0&&f.session->optimization_info().binding_attempts==1,
        "unchanged preflight repeated I/O/admission");
  auto staged=std::make_shared<Ram>();
  f.session->begin_recompression(staged,id(73),{StoreCompressionMode::raw});
  reset(f.image);reset(f.area);
  rejects([&]{f.session->prepare_budgeted_optimization_rebuild();},"staged maintenance admitted rebuild preflight");
  require(f.image->read_calls==0&&f.area->read_calls==0&&f.area->calls==0,"staged preflight performed I/O");
  f.session->cancel_recompression();
  f.earn(2*kOptimizationAreaBytes);reset(f.image);
  require(f.session->prepare_budgeted_optimization_rebuild()&&f.image->read_calls>0&&
      f.session->optimization_info().binding_attempts==1,"fresh input did not admit read-only preflight");
  const auto old=f.area->stable;auto target=std::make_shared<Ram>();reset(f.image);
  const auto rebuilt=f.session->rebuild_budgeted_optimization_area(target,4);
  require(rebuilt.phase==Phase::ready&&f.image->read_calls>0&&f.area->stable==old,
      "rebuild trusted preflight without its own current-source verification");
  f.data_ok();
  Fixture legacy;legacy.session->create_optimization_area(legacy.area,4);
  const auto prior=legacy.session->optimization_snapshot().state;legacy.damage(2);
  rejects([&]{legacy.session->publish_optimization_state(prior);},"legacy damage audit accepted");
  reset(legacy.image);reset(legacy.area);
  rejects([&]{legacy.session->prepare_budgeted_optimization_rebuild();},"legacy owner received funded preflight");
  require(legacy.image->read_calls==0&&legacy.area->read_calls==0&&legacy.area->calls==0,
      "legacy preflight rejection performed I/O");
  legacy.data_ok();
}
void normal(){
  Fixture f;const auto source=f.image->stable,head=f.head->stable;
  auto info=f.create();require(info.phase==Phase::ready&&info.budgeted&&info.sequence==1&&
      info.settled_input_bytes==7&&info.remaining_publications==3&&
      f.area->write_bytes==2*kOptimizationAreaBytes,"one NEW-volume bootstrap");
  require(f.image->stable==source&&f.head->stable==head,"bootstrap mutated native data/head");
  const auto initial=f.session->optimization_snapshot().state;
  auto result=f.session->publish_budgeted_optimization_state(initial);
  require(result.progress==Progress::unchanged&&result.state.sequence==1,"identical audit consumed budget");
  auto proposal=f.proposal();reset(f.area);
  result=f.session->publish_budgeted_optimization_state(proposal);
  require(result.progress==Progress::deferred&&f.area->calls==0&&
      result.state.settled_input_bytes==7,"unearned policy wrote records");
  rejects([&]{f.session->publish_optimization_state(proposal);},"legacy policy bypassed funded owner");
  require(f.area->calls==0,"legacy rejection touched advice");
  for(unsigned i=0;i<3;++i){
    f.earn(kOptimizationAreaBytes);proposal=f.proposal();
    const auto before=f.image->stable;reset(f.area);
    result=f.session->publish_budgeted_optimization_state(proposal);
    require(result.progress==Progress::published&&result.state.sequence==i+2&&
        result.state.settled_input_bytes==7+(i+1)*kOptimizationAreaBytes&&
        f.area->write_bytes==kOptimizationAreaBytes&&f.image->stable==before,
        "funded publication charge or source preservation");
  }
  f.earn(kOptimizationAreaBytes);reset(f.area);reset(f.image);
  result=f.session->publish_budgeted_optimization_state(f.proposal());
  require(result.progress==Progress::deferred&&result.state.remaining_publications==0&&
      f.area->calls==0,"funded quota silently renewed");
  auto target=std::make_shared<Ram>();
  rejects([&]{f.session->rebuild_budgeted_optimization_area(target);},"healthy quota rebuilt");
  rejects([&]{f.session->rebuild_optimization_area(target);},"legacy binding escaped budget mode");
  require(target->calls==0&&target->read_calls==0,"rejected target queried");
  f.data_ok();
}
void controller_resume(){
  Fixture f;f.create();f.earn(2*kOptimizationAreaBytes);
  auto result=f.session->publish_budgeted_optimization_state(f.proposal());
  require(result.progress==Progress::published,"resume seed publication");
  auto seq=result.state.sequence;const auto spent=*result.state.settled_input_bytes;
  f.close();f.resume();reset(f.area);
  auto info=f.session->resume_budgeted_optimization_area(f.area,seq,4);
  require(info.phase==Phase::ready&&info.settled_input_bytes==spent&&f.area->writes==0,
      "healthy resume reset spending or rewrote policy");
  result=f.session->publish_budgeted_optimization_state(f.proposal());
  require(result.progress==Progress::published&&
      result.state.settled_input_bytes==7+2*kOptimizationAreaBytes,"unspent credit did not survive resume");
  seq=result.state.sequence;
  for(unsigned repeat=0;repeat<3;++repeat){
    f.close();f.resume();f.session->resume_budgeted_optimization_area(f.area,seq,4);
    reset(f.area);result=f.session->publish_budgeted_optimization_state(f.proposal());
    require(result.progress==Progress::deferred&&f.area->calls==0&&
        result.state.sequence==seq,"reopen minted old input");
  }
  f.data_ok();
}
void resumed_provision(){
  Fixture f;f.close();f.resume();reset(f.area);
  auto info=f.session->create_budgeted_optimization_area(f.area,4);
  require(info.phase==Phase::unbound&&info.binding_attempts==0&&info.loss_input_baseline==7&&
      f.area->calls==0,"resume received NEW-volume bootstrap");
  reset(f.image);info=f.session->create_budgeted_optimization_area(f.area,4);
  require(f.image->read_calls==0&&f.area->calls==0&&info.binding_attempts==0,
      "unchanged input repeated provisioning I/O");
  f.earn(2*kOptimizationAreaBytes);info=f.session->create_budgeted_optimization_area(f.area,4);
  require(info.phase==Phase::ready&&info.settled_input_bytes==7+2*kOptimizationAreaBytes,
      "fresh input did not fund creation");
  f.data_ok();
}
void lost_state(){
  Fixture f;f.create();f.earn(kOptimizationAreaBytes);
  f.session->publish_budgeted_optimization_state(f.proposal());
  const auto state=f.session->optimization_snapshot().state;f.damage(2);
  rejects([&]{f.session->publish_optimization_state(state);},"lost advice audit accepted");
  const auto old=f.area->stable;auto target=std::make_shared<Ram>();
  auto info=f.session->rebuild_budgeted_optimization_area(target,4);
  require(info.phase==Phase::terminal&&info.binding_attempts==1&&target->calls==0&&
      info.loss_input_baseline==f.session->workload_info().committed_input_bytes,
      "lost advice reused historical input");
  reset(f.image);info=f.session->rebuild_budgeted_optimization_area(target,4);
  require(f.image->read_calls==0&&target->calls==0,"lost advice retried unchanged evidence");
  f.earn(2*kOptimizationAreaBytes);info=f.session->rebuild_budgeted_optimization_area(target,4);
  require(info.phase==Phase::ready&&info.binding_attempts==2&&info.sequence==1&&
      info.settled_input_bytes==f.session->workload_info().committed_input_bytes&&
      f.area->stable==old,"funded replacement reset history or mutated old bytes");
  f.data_ok();
}
void repair(){
  Fixture f;f.create();f.earn(kOptimizationAreaBytes);f.close();f.damage(1);f.resume();
  reset(f.area);auto info=f.session->resume_budgeted_optimization_area(f.area,1,4);
  require(info.phase==Phase::unavailable&&info.repair_pending&&f.area->writes==0&&
      f.session->optimization_snapshot().code!=OptimizationReadCode::available,
      "degraded resume wrote/unlocked old entitlement");
  const auto baseline=*info.loss_input_baseline;reset(f.image);reset(f.area);
  auto result=f.session->repair_budgeted_optimization_area();
  require(result.progress==Progress::deferred&&f.image->read_calls==0&&f.area->calls==0,
      "unfunded repair repeated I/O");
  auto fresh=std::make_shared<Ram>();
  rejects([&]{f.session->rebuild_budgeted_optimization_area(fresh);},"pending repair replaced area");
  f.earn(kOptimizationAreaBytes);reset(f.area);
  result=f.session->repair_budgeted_optimization_area();
  require(result.progress==Progress::published&&result.state.phase==Phase::ready&&
      !result.state.repair_pending&&result.state.sequence==2&&
      result.state.settled_input_bytes==baseline+kOptimizationAreaBytes&&
      f.area->write_bytes==kOptimizationAreaBytes,"repair not paired and input-funded");
  f.data_ok();
}
void creation_faults(){
  for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<6;++cut){
    Fixture f;f.area->failure=mode;f.area->fail_at=cut;
    const auto info=f.create();require(info.phase==Phase::unavailable&&info.binding_attempts==1,
        "creation fault not reached");++cuts;
    require(f.session->optimization_snapshot().code!=OptimizationReadCode::available,
        "failed creation exposed policy");
    auto target=std::make_shared<Ram>();reset(f.image);
    const auto held=f.session->rebuild_budgeted_optimization_area(target,4);
    require(target->calls==0&&held.binding_attempts==1&&f.image->read_calls==0,
        "failed creation repeated bootstrap");
    const auto partial=f.area->stable;f.earn(2*kOptimizationAreaBytes);
    const auto repaired=f.session->rebuild_budgeted_optimization_area(target,4);
    require(repaired.phase==Phase::ready&&repaired.binding_attempts==2&&f.area->stable==partial,
        "creation recovery reused source");
    f.data_ok();
  }
}
void publication_faults(bool repairing){
  for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<4;++cut){
    Fixture f;f.create();
    if(repairing){f.close();f.damage(1);f.resume();f.session->resume_budgeted_optimization_area(f.area,1,4);}
    f.earn(kOptimizationAreaBytes);
    auto proposed=repairing?OptimizationState{}:f.proposal();
    reset(f.area);f.area->failure=mode;f.area->fail_at=cut;
    if(repairing)rejects([&]{f.session->repair_budgeted_optimization_area();},"repair cut not reached");
    else rejects([&]{f.session->publish_budgeted_optimization_state(proposed);},"publication cut not reached");
    ++cuts;require(f.session->optimization_info().phase==Phase::terminal,"failed writer not terminal");
    auto target=std::make_shared<Ram>();reset(f.image);
    const auto held=f.session->rebuild_budgeted_optimization_area(target,4);
    require(target->calls==0&&held.binding_attempts==1&&f.image->read_calls==0,
        "failed debit reused old input");
    f.data_ok();
  }
}
void put(Bytes& b,std::size_t at,std::uint64_t value,unsigned width=8){
  for(unsigned i=0;i<width;++i)b[at+i]=std::byte(value>>(8*i));
}
void seal(Bytes& bytes,unsigned slot){
  const auto begin=slot*kStoreCommitUnit;
  const auto digest=detail::record_digest(std::span<const std::byte>(bytes).subspan(begin,kStoreCommitUnit-32));
  std::copy(digest.begin(),digest.end(),bytes.begin()+static_cast<std::ptrdiff_t>(begin+kStoreCommitUnit-32));
}
void malicious(){
  for(unsigned variant=0;variant<9;++variant){
    Fixture f;f.create();f.close();
    auto& b=f.area->live;const auto slot=kStoreCommitUnit;
    if(variant==0){b[7]=std::byte{'1'};put(b,8,1,4);std::fill(b.begin()+52,b.begin()+64,std::byte{});seal(b,0);}
    if(variant==1||variant==2){put(b,slot+16,2);put(b,slot+56,variant==1?6:7);seal(b,1);}
    if(variant==3){put(b,slot+56,8);seal(b,1);}
    if(variant==4){put(b,slot+52,2,4);seal(b,1);}
    if(variant==5){b[slot+7]=std::byte{'3'};put(b,slot+8,3,4);seal(b,1);}
    if(variant==6){b[slot+24]^=std::byte{1};seal(b,1);}
    if(variant==7){put(b,slot+16,2);put(b,slot+56,7+kOptimizationAreaBytes);seal(b,1);}
    if(variant==8){for(unsigned i=0;i<2;++i){put(b,i*kStoreCommitUnit+56,UINT64_MAX);seal(b,i);}}
    f.area->stable=b;
    const auto observed=read_optimization_state(*f.area,id(70));
    if(variant<7)require(observed.code!=OptimizationReadCode::available,"intact invalid peer supplied policy");
    else require(observed.code==OptimizationReadCode::available,"future funded diagnostic fixture invalid");
    f.resume();reset(f.area);
    const auto info=f.session->resume_budgeted_optimization_area(f.area,variant==7?2:1,4);
    require(info.phase==Phase::unavailable&&f.area->writes==0&&
        f.session->optimization_snapshot().code!=OptimizationReadCode::available,
        "malformed/future funding became writable authority");
    f.data_ok();
  }
}
void readback_failures(){
  {
    Fixture f;f.area->corrupt_read_offset=0;f.area->corrupt_read_number=1;
    const auto info=f.create();
    require(info.phase==Phase::unavailable&&f.area->writes==4,"creation readback fault not reached");
    f.data_ok();
  }
  {
    Fixture f;f.create();f.earn(kOptimizationAreaBytes);
    auto proposed=f.proposal();reset(f.area);
    f.area->corrupt_read_offset=0;f.area->corrupt_read_number=2;f.area->matching_reads=0;
    rejects([&]{f.session->publish_budgeted_optimization_state(proposed);},"final readback fault not reached");
    require(f.session->optimization_info().phase==Phase::terminal&&f.area->writes==2,
        "ambiguous final publication supplied authority");
    f.close();f.resume();
    const auto resumed=f.session->resume_budgeted_optimization_area(f.area,2,4);
    require(resumed.phase==Phase::ready&&resumed.settled_input_bytes==f.session->workload_info().committed_input_bytes,
        "completed unacknowledged spending was lost");
    const auto result=f.session->publish_budgeted_optimization_state(f.proposal());
    require(result.progress==Progress::deferred,"ambiguous committed spending reused credit");
    f.data_ok();
  }
}
void saturated_input(){
  Fixture f;f.create();f.close();
  {
    auto snapshot=ObjectStoreSnapshot::open(f.image);auto metadata=snapshot.read(1);
    put(metadata,56,UINT64_MAX);
    auto writer=ObjectStoreWriter::resume(f.image,id(71),snapshot.info().generation,{StoreCompressionMode::raw});
    const StoreMutation changed{1,false,std::move(metadata)};writer.commit_replicated({&changed,1});
  }
  for(unsigned i=0;i<2;++i){put(f.area->live,i*kStoreCommitUnit+56,UINT64_MAX-15);seal(f.area->live,i);}
  f.area->stable=f.area->live;f.resume();
  auto info=f.session->resume_budgeted_optimization_area(f.area,1,4);
  require(info.phase==Phase::ready&&info.settled_input_bytes==UINT64_MAX-15,"saturation fixture admission");
  const auto result=f.session->publish_budgeted_optimization_state(f.proposal());
  require(result.progress==Progress::deferred&&result.state.settled_input_bytes==UINT64_MAX-15,
      "funding addition wrapped at UINT64_MAX");
  f.earn(3);f.data_ok();
  require(f.session->workload_info().committed_input_bytes==UINT64_MAX,"saturation blocked input semantics");
  f.close();f.damage(1);f.resume();f.session->resume_budgeted_optimization_area(f.area,1,4);
  reset(f.area);const auto repaired=f.session->repair_budgeted_optimization_area();
  require(repaired.progress==Progress::deferred&&f.area->writes==0,"saturated input reminted repair credit");
}
void competing_and_legacy(){
  Fixture owner;owner.create();Fixture other;reset(owner.area);
  const auto info=other.session->resume_budgeted_optimization_area(owner.area,1,4);
  require(info.phase==Phase::unavailable&&owner.area->read_calls==0&&owner.area->writes==0,
      "competing funded owner read before exclusive lease");
  owner.data_ok();other.data_ok();owner.close();other.close();
  reset(owner.area);
  rejects([&]{auto old=OptimizationStateWriter::resume(owner.area,id(70),1,4);},
      "legacy journal repaired budgeted records");
  require(owner.area->writes==0&&owner.area->flushes==0,"legacy journal bypass wrote records");
  owner.resume();const auto refused=owner.session->resume_optimization_area(owner.area,1,4);
  require(refused.phase==Phase::unavailable&&refused.budgeted,"legacy session accepted funded records");
  auto target=std::make_shared<Ram>();
  rejects([&]{owner.session->rebuild_optimization_area(target);},"legacy downgrade lost budget mode");
  owner.data_ok();
}
}
int main(){
  try{admission_and_binding_limit();rebuild_preflight();normal();controller_resume();resumed_provision();lost_state();repair();creation_faults();
    publication_faults(false);publication_faults(true);malicious();readback_failures();saturated_input();competing_and_legacy();
    std::cout<<"Optimization budget assertions: "<<checks<<"; actual journal faults="<<cuts
        <<"; CPU RAM model only, no files/mount/GPU.\n";return 0;
  }catch(const std::exception& e){std::cerr<<"Optimization budget failure after "<<checks<<": "<<e.what()<<"\n";return 1;}
}
