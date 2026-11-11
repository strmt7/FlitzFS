#include "flitzfs/native_session.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "store_codec.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0,cases=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected volume-session rejection");}
template<class F>void rejects_code(F&& fn,NativeSessionErrorCode code){
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("unexpected volume-session status");
}
StoreIdentity id(unsigned n){auto value=identity();value[0]=static_cast<std::byte>(n);return value;}
std::shared_ptr<Ram> ram(bool compact){auto r=std::make_shared<Ram>();r->failure_unit=compact?4096:65536;return r;}
NativeFilesystemSnapshot view(std::shared_ptr<Ram> r){return NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(std::move(r)));}
void reset(const std::shared_ptr<Ram>& r){r->calls=0;r->writes=0;r->flushes=0;r->read_calls=0;r->fail_at=(std::numeric_limits<std::size_t>::max)();}
struct Fixture {
  bool compact;std::shared_ptr<Ram> head,source,target;
  NativeFileSession session;NativeFileHandle rw,ro,late;
  explicit Fixture(bool c,StoreCompressionMode mode=StoreCompressionMode::raw):compact(c),head(ram(c)),source(ram(c)),target(ram(c)),
      session(NativeFileSession::create_volume(head,id(90),source,id(1),
          {mode,6,false},{c},NativeDispositionPolicy::windows_global)) {
    session.create_entry("d",true,{1,2,3,4,0x10},native_security_fixture::descriptor());
    session.create_entry("d/file",false,{11,12,13,14,0x20},native_security_fixture::descriptor(2));
    rw=session.open("d/file",{true,true,true},NativeOpenKind::file,{true},{true,true});
    ro=session.open("d/file",{true,false,false},NativeOpenKind::file);
    late=session.open("d/file",{true,true,false},NativeOpenKind::file);
    session.write(rw,0,Bytes(65536,std::byte{81}));session.cleanup(late,false);
    session.create_entry("sparse");
    auto sparse=session.open("sparse",{true,true,false});
    session.write(sparse,kNativeMaxFileBytes-80,data(17,2));session.truncate(sparse,kNativeMaxFileBytes-1);
    session.cleanup(sparse,false);session.close(sparse);
    require(source->live.size()+head->live.size()<=6U*1024*1024,"volume-session seed cap");
  }
  NativeVolumeHeadInfo maintain(){
    return session.recompress_volume(target,id(2),{StoreCompressionMode::automatic,6,false},{compact});
  }
  NativeVolumeImageResolver resolver() {
    return [this](const auto& which)->std::shared_ptr<StoreByteIo>{
      if(which==id(1))return source;if(which==id(2))return target;return {};
    };
  }
  NativeVolumeHeadView selected(){return NativeVolumeHeadView::open(head,id(90),resolver());}
  void expected_session() {
    require(session.stats().contexts==3&&session.stats().cleaned==1&&!session.stats().fenced,"context table changed");
    require(session.info(rw).info.inode==3&&session.read(ro,0,65536)==Bytes(65536,std::byte{81})&&
        session.read(late,0,65536)==Bytes(65536,std::byte{81}),"context identity or paging bytes lost");
    require(session.basic_metadata(rw)==NativeBasicMetadata{11,12,13,14,0x20}&&
        session.security_descriptor(rw)==native_security_fixture::descriptor(2),"context metadata/security changed");
    rejects_code([&]{session.write(ro,0,data(1,1));},NativeSessionErrorCode::access_denied);
  }
  void retire_contexts(){
    session.cleanup(rw,false);session.close(rw);session.cleanup(ro,false);session.close(ro);session.close(late);
    require(session.stats().contexts==0,"contexts not actually retired");
  }
};
void expected(const NativeFilesystemSnapshot& snapshot){
  snapshot.verify_all();
  require(snapshot.stat("d/file").inode==3&&snapshot.read("d/file",0,65536)==Bytes(65536,std::byte{81}),
      "independent volume payload changed");
  require(snapshot.stat("sparse").logical_bytes==kNativeMaxFileBytes-1&&
      snapshot.read("sparse",kNativeMaxFileBytes-80,17)==data(17,2),"sparse state changed");
  require(snapshot.basic_metadata(3)==NativeBasicMetadata{11,12,13,14,0x20}&&
      snapshot.security_descriptor(3)==native_security_fixture::descriptor(2),"independent metadata changed");
}
void staged_recompression(bool compact) {
  using Progress=NativeFileSession::RecompressionProgress;
  const StoreCheckpointOptions checkpoints{compact};
  {
    Fixture f(compact);
    const auto source=f.source->live,head=f.head->live;
    const auto before=f.session.snapshot().storage_info();
    f.session.begin_recompression(f.target,id(2),{},checkpoints);
    rejects([&]{(void)view(f.target);});
    rejects_code([&]{f.session.publish_recompression();},NativeSessionErrorCode::invalid_options);
    rejects_code([&]{f.session.begin_recompression(ram(compact),id(3),{},checkpoints);},
        NativeSessionErrorCode::invalid_options);
    for(std::size_t step=1;step<=before.objects;++step) {
      const auto writes=f.target->writes;
      const auto progress=f.session.advance_recompression();
      require(progress==(step==before.objects?Progress::ready:Progress::copying),"wrong staged copy progress");
      require(f.target->writes>writes&&f.target->writes-writes<=2&&f.target->flushes==0,
          "copy step exceeded one record or published");
      require(f.session.read(f.ro,0,65536)==Bytes(65536,std::byte{81}),"foreground read unavailable during staging");
    }
    const auto prepared=f.target->calls;
    require(f.session.advance_recompression()==Progress::ready&&f.target->calls==prepared&&
        f.source->live==source&&f.head->live==head,"ready stage performed I/O or changed authority");
    auto provisional=f.session.prepare_open("d/file",{true,false,false},NativeOpenKind::file,{},{});
    rejects_code([&]{f.session.publish_recompression();},NativeSessionErrorCode::delete_pending);
    require(f.target->calls==prepared&&f.head->live==head,"deferred publication touched target or head");
    f.session.close(provisional);
    const auto after=f.session.publish_recompression();
    require(after.selection.epoch==2&&after.selection.image_identity==id(2)&&f.source->live==source,
        "staged handoff lost identity or changed source");
    f.expected_session();
    const auto selected=f.selected();
    expected(selected.filesystem());
    require(selected.filesystem().storage_info().stored_payload_bytes<before.stored_payload_bytes,
        "staged maintenance did not reduce live payload");
    f.session.write(f.late,3,data(7,19));
    require(f.session.read(f.ro,3,7)==data(7,19),"live handle could not write after handoff");
    f.retire_contexts();
  }
  for(bool publish:{false,true}) {
    Fixture f(compact);
    f.session.begin_recompression(f.target,id(2),{},checkpoints);
    require(f.session.advance_recompression()==Progress::copying,"fixture needs another live record");
    f.session.write(f.rw,5,data(19,12));
    const auto target=f.target->live,head=f.head->live;
    const auto calls=f.target->calls,reads=f.source->read_calls;
    if(publish)rejects_code([&]{f.session.publish_recompression();},NativeSessionErrorCode::invalid_options);
    else require(f.session.advance_recompression()==Progress::stale,"committed foreground write did not stale candidate");
    require(f.target->calls==calls&&f.source->read_calls==reads&&f.target->live==target&&f.head->live==head,
        "stale candidate accessed backing");
    require(!f.session.poisoned()&&f.session.read(f.ro,5,19)==data(19,12),"stale candidate lost foreground write");
    rejects([&]{(void)view(f.target);});
  }
  for(unsigned phase=0;phase<4;++phase) {
    Fixture f(compact);
    const auto source=f.source->live,head=f.head->live;
    if(phase==0)f.target->fail_at=0;
    if(phase==0)rejects([&]{f.session.begin_recompression(f.target,id(2),{},checkpoints);});
    else {
      f.session.begin_recompression(f.target,id(2),{},checkpoints);
      if(phase==1) {
        f.target->fail_at=f.target->calls;
        rejects([&]{f.session.advance_recompression();});
      } else if(phase==2) {
        while(f.session.advance_recompression()==Progress::copying) {}
        f.target->fail_at=f.target->calls;
        rejects([&]{f.session.publish_recompression();});
      } else f.session.cancel_recompression();
    }
    require(f.source->live==source&&f.head->live==head&&!f.session.poisoned(),
        "unpublished failure or cancellation disturbed the source");
    f.expected_session();
    rejects_code([&]{f.session.advance_recompression();},NativeSessionErrorCode::invalid_options);
    rejects([&]{(void)view(f.target);});
  }
}

void idle_recompression(bool compact) {
  using Progress=NativeFileSession::RecompressionProgress;
  Fixture f(compact,StoreCompressionMode::automatic);
  const auto source=f.source->live,head=f.head->live;
  const auto initial=f.session.snapshot().storage_info();
  require(initial.stored_payload_bytes<initial.logical_bytes,"idle fixture did not compress its payload");
  require(f.session.begin_idle_recompression(f.target,id(2),{StoreCompressionMode::raw,6,false},{compact}),
      "first idle opportunity was suppressed");
  for(std::size_t step=1;step<=initial.objects;++step) {
    const auto progress=f.session.advance_recompression();
    require(progress==(step==initial.objects?Progress::ready:Progress::copying),"idle copy progress");
  }
  rejects_code([&]{(void)f.session.publish_recompression();},NativeSessionErrorCode::invalid_options);
  require(!f.session.publish_idle_recompression()&&f.source->live==source&&f.head->live==head,
      "idle policy published a larger payload");
  rejects([&]{(void)view(f.target);});
  auto next=ram(compact);
  require(!f.session.begin_idle_recompression(next,id(3),{}, {compact})&&next->live.empty(),
      "same acknowledged source was recompressed twice");
  f.session.write(f.rw,0,data(19,12));
  require(f.session.begin_idle_recompression(next,id(3),{}, {compact}),
      "new foreground generation did not permit another attempt");
  f.session.cancel_recompression();
  require(!f.session.begin_idle_recompression(ram(compact),id(4),{}, {compact})&&
      f.session.read(f.ro,0,19)==data(19,12),"cancellation retried or lost foreground bytes");
}

void staged_matches(bool compact) {
  using Progress=NativeFileSession::RecompressionProgress;
  Fixture f(compact);
  const auto prefix=data(8193,43);
  auto input=prefix;
  input.insert(input.end(),prefix.begin(),prefix.end());
  auto extent=input;extent.resize(kNativeExtentBytes);
  f.session.truncate(f.rw,input.size());
  f.session.write(f.rw,0,input);
  f.session.begin_recompression(f.target,id(2),{}, {compact});
  const auto count=f.session.snapshot().storage_info().objects;
  bool offered=false;
  for(std::size_t i=1;i<=count;++i) {
    auto work=f.session.next_recompression_input();
    auto progress=Progress::copying;
    if(work && work->bytes==extent) {
      const StoreMatchProposal match{8193,8193,8193};
      progress=f.session.advance_recompression(work->object_id,{&match,1});
      offered=true;
    } else progress=f.session.advance_recompression();
    require(progress==(i==count?Progress::ready:Progress::copying),"staged match copy progress");
  }
  require(offered==flitzfs::detail::store_match_sequences_available()&&
      !f.session.next_recompression_input(),"staged match input capability mismatch");
  require(f.session.publish_recompression().selection.epoch==2&&
      f.session.read(f.ro,0,65536)==input&&f.selected().filesystem().read("d/file",0,65536)==input,
      "staged match publication changed retained bytes");
  f.retire_contexts();
}

void invariants(bool compact){
  Fixture f(compact);f.expected_session();auto pinned=f.session.snapshot();const auto original=f.source->live;
  const auto before=f.session.stats();const auto alias=f.rw;
  rejects([&]{auto other=NativeFileSession::resume_volume(f.head,id(90),1,f.resolver());});
  rejects_code([&]{auto stale=NativeFileSession::resume_volume(f.head,id(90),2,f.resolver());},
      NativeSessionErrorCode::invalid_options);
  rejects_code([&]{f.session.recompress_volume(f.target,id(1));},NativeSessionErrorCode::invalid_options);
  require(!f.session.poisoned()&&f.target->live.empty(),"admission poisoned healthy source");
  auto provision=f.session.prepare_open("d/file",{true,false,false},NativeOpenKind::file,{},{});
  rejects_code([&]{f.maintain();},NativeSessionErrorCode::delete_pending);
  require(f.target->live.empty()&&!f.session.poisoned(),"provisional deferral wrote or fenced");
  f.session.close(provision);
  auto first=f.maintain();f.expected_session();
  require(first.selection.volume_identity==id(90)&&first.selection.image_identity==id(2)&&
      first.selection.epoch==2&&f.session.info(alias).info.inode==3,"stable volume/handle identity lost");
  require(f.source->live==original&&f.source->stable==original,"handoff modified source");
  require(f.session.stats().contexts==before.contexts&&f.session.stats().cleaned==before.cleaned,"handoff reset contexts");
  expected(pinned);auto selected=f.selected();expected(selected.filesystem());
  require(f.target->live.size()+f.source->live.size()+f.head->live.size()<=6U*1024*1024,"handoff aggregate cap");
  const auto replacement=data(19,23);f.session.write(f.late,3,replacement);
  auto changed=Bytes(65536,std::byte{81});std::copy(replacement.begin(),replacement.end(),changed.begin()+3);
  require(f.session.read(alias,0,65536)==changed&&pinned.read("d/file",0,65536)==Bytes(65536,std::byte{81}),
      "late paging or pinned source changed");
  // Initial-delete authority survives an adopted context across another handoff.
  auto initial=f.session.prepare_open("d/file",{true,false,false},NativeOpenKind::file,{},{},{true});
  f.session.adopt_prepared(initial);
  auto third=ram(compact);auto second=f.session.compact_volume(third,id(3),{StoreCompressionMode::automatic,6,false},{compact});
  require(second.selection.epoch==3&&second.selection.volume_identity==id(90)&&
      f.session.read(alias,0,65536)==changed,"second handoff lost live contexts");
  f.session.cleanup(initial,true);
  require(f.session.info(alias).unlinked&&f.session.read(f.ro,0,65536)==changed,
      "initial delete authority/pinned data lost across handoff");
  auto deferred=ram(compact);
  rejects_code([&]{f.session.recompress_volume(deferred,id(4));},NativeSessionErrorCode::delete_pending);
  require(deferred->live.empty()&&!f.session.poisoned(),"detached-context deferral changed storage");
  f.session.close(initial);f.retire_contexts();
  rejects_code([&]{f.session.read(alias,0,1);},NativeSessionErrorCode::invalid_handle);
  rejects_code([&]{f.session.recover_volume();},NativeSessionErrorCode::invalid_options);
  // Complete real context retirement precedes a separate controller resume.
  auto resume_head=ram(compact),resume_source=ram(compact),resume_target=ram(compact);
  NativeFileHandle stale;
  {
    auto owner=NativeFileSession::create_volume(resume_head,id(80),resume_source,id(10),
        {StoreCompressionMode::raw,6,false},{compact});
    owner.create_entry("persist");stale=owner.open("persist",{true,true,false});
    owner.write(stale,0,data(73,4));
    owner.recompress_volume(resume_target,id(11),{StoreCompressionMode::automatic,6,false},{compact});
    owner.cleanup(stale,false);owner.close(stale);
  }
  auto resolver=[&](const auto& which)->std::shared_ptr<StoreByteIo>{
    if(which==id(10))return resume_source;if(which==id(11))return resume_target;return {};
  };
  auto reopened=NativeFileSession::resume_volume(resume_head,id(80),2,resolver,{StoreCompressionMode::raw,6,false});
  require(reopened.volume_info().selection.volume_identity==id(80),"resumed volume identity changed");
  rejects_code([&]{reopened.read(stale,0,1);},NativeSessionErrorCode::invalid_handle);
  auto handle=reopened.open("persist",{true,true,false});require(reopened.read(handle,0,100)==data(73,4),"resumed bytes changed");
  reopened.write(handle,73,data(11,5));require(reopened.read(handle,73,11)==data(11,5),"resumed writes failed");
  auto moved=std::move(reopened);
  require(reopened.poisoned()&&moved.read(handle,0,73)==data(73,4)&&
      moved.volume_info().selection.volume_identity==id(80),"moving owner invalidated its contexts/volume");
  moved.cleanup(handle,false);moved.close(handle);
  auto unresolved_head=ram(compact),unresolved_image=ram(compact);
  {
    auto owner=NativeFileSession::create_volume(unresolved_head,id(70),unresolved_image,id(20));
    owner.create_entry("detached");auto open=owner.open("detached",{true,true,true});
    owner.write(open,0,data(31,8));owner.set_delete(open,true);owner.cleanup(open,true);
    // Synthetic crash: no final Close or closed-session recovery is invented.
  }
  const auto retained=unresolved_image->live;reset(unresolved_image);reset(unresolved_head);
  rejects([&]{auto owner=NativeFileSession::resume_volume(unresolved_head,id(70),1,
      [&](const auto&)->std::shared_ptr<StoreByteIo>{return unresolved_image;});});
  require(unresolved_image->calls==0&&unresolved_head->calls==0&&unresolved_image->live==retained,
      "startup silently resolved old contexts");
  auto pending=view(unresolved_image);
  require(pending.needs_session_recovery()&&pending.read_inode(2,0,100)==data(31,8),
      "unresolved startup discarded detached data");
  auto invalid_head=ram(compact),untouched=ram(compact);invalid_head->live=Bytes(1);
  rejects_code([&]{auto owner=NativeFileSession::create_volume(invalid_head,id(60),untouched,id(21));},
      NativeSessionErrorCode::invalid_options);
  require(untouched->live.empty(),"nonempty head admission wrote a new image");
  auto unknown=ram(compact);unknown->failure_unit=0;
  require(!valid_store_io_contract(unknown->contract()),"unknown contract admitted");
  rejects_code([&]{auto owner=NativeFileSession::create_volume(ram(compact),id(60),unknown,id(21));},
      NativeSessionErrorCode::invalid_options);
  require(unknown->live.empty(),"unknown contract wrote image");
}
std::uint64_t observations(const Fixture& f) {
  return f.source->calls+f.source->read_calls+f.target->calls+f.target->read_calls+f.head->calls+f.head->read_calls;
}
void terminal_no_io(Fixture& f) {
  const auto before=observations(f);
  rejects_code([&]{f.session.read(f.ro,0,1);},NativeSessionErrorCode::terminal);
  rejects_code([&]{f.session.flush();},NativeSessionErrorCode::terminal);
  rejects_code([&]{f.maintain();},NativeSessionErrorCode::terminal);
  require(observations(f)==before&&f.session.stats().contexts==3&&f.session.stats().cleaned==1,
      "fenced session retried I/O or lost contexts");
}
void maintenance_faults(bool compact) {
  Fixture probe(compact);reset(probe.source);reset(probe.target);reset(probe.head);
  probe.maintain();require(probe.source->calls==0,"maintenance wrote/flushed source");
  const auto image_steps=probe.target->calls,head_steps=probe.head->calls;
  require(image_steps>0&&image_steps<=256&&head_steps==4,"maintenance trace/budget mismatch");
  for(bool on_head:{false,true}) {
    const auto steps=on_head?head_steps:image_steps;
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
      Fixture f(compact);const auto original=f.source->stable;
      reset(f.source);reset(f.target);reset(f.head);
      auto fault=on_head?f.head:f.target;fault->failure=failure;fault->fail_at=step;
      rejects([&]{f.maintain();});require(f.session.poisoned(),"failed maintenance stayed writable");
      require(f.source->live==original&&f.source->stable==original,"failed maintenance changed source");
      terminal_no_io(f);
      // Discard unflushed RAM epochs after the failed transition. No real files,
      // drivers, hardware faults or external contexts participate in this model.
      f.head->live=f.head->stable;f.target->live=f.target->stable;
      auto recovered=f.session.recover_volume();
      require(recovered.selection.epoch==1||recovered.selection.epoch==2,"recovered foreign handoff");
      f.expected_session();expected(f.selected().filesystem());
      require(f.source->live==original&&f.source->stable==original,"recovery changed source data");
      f.session.write(f.rw,0,data(13,9));require(f.session.read(f.late,0,13)==data(13,9),"recovered paging write failed");
      f.retire_contexts();++cases;
    }
  }
}
void fail_transition(Fixture& f,unsigned scenario,std::size_t image_steps) {
  reset(f.source);reset(f.target);reset(f.head);
  if(scenario==0)f.target->fail_at=0; // Old selected image, incomplete candidate.
  else if(scenario==1)f.target->fail_at=image_steps-1; // Both head copies published, adoption fence fails.
  else f.head->fail_at=2; // New head survivor, old peer: recovery must repair peer first.
  rejects([&]{f.maintain();});f.head->live=f.head->stable;f.target->live=f.target->stable;
  reset(f.source);reset(f.target);reset(f.head);
}
void recovery_faults(bool compact) {
  Fixture completed(compact);reset(completed.target);completed.maintain();const auto image_steps=completed.target->calls;
  for(unsigned scenario=0;scenario<3;++scenario) {
    Fixture trace(compact);fail_transition(trace,scenario,image_steps);
    const auto expected_epoch=scenario?2U:1U;
    const auto chosen=scenario?trace.target:trace.source;
    auto restored=trace.session.recover_volume();require(restored.selection.epoch==expected_epoch,"recovery scenario not reached");
    const auto selected_steps=chosen->calls,head_steps=trace.head->calls;
    require(selected_steps==3&&head_steps==(scenario==2?4U:1U),"recovery trace changed");
    for(bool on_head:{false,true}) {
      const auto steps=on_head?head_steps:selected_steps;
      for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
        Fixture f(compact);fail_transition(f,scenario,image_steps);const auto original=f.source->stable;
        auto fault=on_head?f.head:(scenario?f.target:f.source);fault->failure=failure;fault->fail_at=step;
        rejects([&]{f.session.recover_volume();});require(f.session.poisoned(),"failed recovery stayed writable");
        terminal_no_io(f);const auto calls=observations(f);
        rejects_code([&]{f.session.recover_volume();},NativeSessionErrorCode::terminal);
        require(observations(f)==calls,"second recovery attempt performed I/O");
        f.head->live=f.head->stable;f.source->live=f.source->stable;f.target->live=f.target->stable;
        auto current=f.selected();require(current.info().selection.epoch==expected_epoch,"recovery rolled back selection");
        expected(current.filesystem());require(f.source->stable==original,"failed recovery changed acknowledged source");
        ++cases;
      }
    }
  }
}
}
int main(int argc,char** argv){try{
  if(argc!=2)throw std::runtime_error("volume-session phase required");const std::string_view phase=argv[1];
  if(phase=="--invariants")for(bool compact:{false,true}) {
    invariants(compact);staged_recompression(compact);idle_recompression(compact);staged_matches(compact);
  }
  else if(phase=="--maintenance-4096")maintenance_faults(true);
  else if(phase=="--maintenance-65536")maintenance_faults(false);
  else if(phase=="--recovery-4096")recovery_faults(true);
  else if(phase=="--recovery-65536")recovery_faults(false);
  else throw std::runtime_error("unknown volume-session phase");
  std::cout<<"Native volume-session evidence: {\"schema\":1,\"phase\":\""<<phase<<"\",\"checks\":"<<checks
      <<",\"fault_cases\":"<<cases<<",\"files_created\":false,\"mounted\":false,\"gpu\":false,\"complete\":true}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Volume-session failure: "<<e.what()<<"\n";return 1;}}
