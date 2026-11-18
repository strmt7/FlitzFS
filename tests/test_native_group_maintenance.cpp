#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/native_group_anchor.hpp"
#include "anchor_search_fixture.hpp"
#include "native_fs_fixture.hpp"
#include <iostream>
namespace {
using namespace flitzfs;using namespace native_fixture;
using Phase=NativeMaintenancePhase;using Defer=NativeMaintenanceDeferral;
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);}
StoreIdentity id(unsigned n){auto result=identity();result[0]=static_cast<std::byte>(n);return result;}
auto ram(std::uint32_t unit){auto result=std::make_shared<Ram>();result->failure_unit=unit;return result;}
Bytes family(unsigned n){auto bytes=data(kNativeExtentBytes,714);if(n){bytes[n*719%bytes.size()]^=std::byte{41};bytes[(n*1409+170)%bytes.size()]^=std::byte{77};}return bytes;}
void put(NativeFileSession& session,std::string path,const Bytes& bytes){session.create_entry(path);auto handle=session.open(path,{true,true,false});
  session.write(handle,0,bytes);session.cleanup(handle,false);session.close(handle);}
struct Clock {
  std::uint64_t sequence=0,now=1000;
  NativeMaintenanceObservation next(){NativeMaintenanceObservation o;o.sequence=++sequence;o.captured_ms=now+=1000;
    o.physical_total=16ULL<<30;o.physical_available=12ULL<<30;o.commit_limit=32ULL<<30;o.commit_available=24ULL<<30;
    o.cpu_total_ticks=1000;o.cpu_busy_ticks=0;o.cpu_window_ms=1000;o.valid=true;return o;}
};
struct Fixture {
  std::shared_ptr<Ram> head,image;std::optional<NativeFileSession> session;Clock clock;
  explicit Fixture(std::uint32_t unit,unsigned count=8,bool indexed=false,
      StoreCompressionOptions compression={StoreCompressionMode::raw,1,false}):head(ram(unit)),image(ram(unit)) {
    if(indexed)session.emplace(NativeFileSession::create_indexed_volume(head,id(90),image,id(1),{kStoreMaxPhysicalBytes},compression));
    else session.emplace(NativeFileSession::create_paged_volume(head,id(90),image,id(1),{kStoreMaxPhysicalBytes},compression));
    for(unsigned i=0;i<count;++i)put(*session,"f"+std::to_string(i),family(i));
  }
  auto step(NativeGroupMaintenance& controller){const auto observed=clock.next();return controller.advance(observed,clock.now);}
  auto finish(NativeGroupMaintenance& controller){for(unsigned i=0;i<6000;++i){const auto result=step(controller);
    require(result.phase!=Phase::failed,"controller failed");if(result.phase==Phase::idle)return result;}throw std::runtime_error("controller did not quiesce");}
  void verify(unsigned count){const auto snapshot=session->snapshot();for(unsigned i=0;i<count;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"automatic grouping changed bytes");snapshot.verify_all();}
};
void advised_indexed_maintenance() {
  constexpr unsigned count=5,band_bytes=kNativeExtentBytes/8;
  Fixture f(4096,0,true);
  const auto base=family(0);
  std::vector<Bytes> original;
  for(unsigned i=0;i<count;++i) {
    auto bytes=i?anchor_search_fixture::fragmented(base,i):base;
    std::copy_n(base.begin(),band_bytes,bytes.begin());
    put(*f.session,"f"+std::to_string(i),bytes);
    original.push_back(std::move(bytes));
  }
  NativeGroupMaintenance controller(*f.session);
  detail::AnchorMatchAdvice hints;
  std::optional<NativeSharedBaseSearchReply> reply;
  for(unsigned tick=0;tick<1000;++tick) {
    const auto observed=f.clock.next();
    const auto progress=controller.advance(observed,f.clock.now,{
        count-1,reply?&*reply:nullptr});
    require(progress.phase!=Phase::failed,"advised maintenance failed");
    if(progress.published_groups)break;
    auto window=controller.search_window();
    if(!window || reply)continue;
    require(progress.search_window_ready && window.members().size()==count-1 && std::ranges::equal(window.base(),base),
        "automatic candidate was not staged for search");
    const std::array<std::uint32_t,1> counts{count-1};
    const auto quantum=anchor_match::max_members;
    const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),count+1);
    detail::AnchorMatchBatchBuilder builder(1,plan);
    builder.append(1,window);
    const auto request=builder.finish();
    const auto& group=request.groups().front();
    hints=detail::verify_anchor_match_records(group,detail::cpu_anchor_match_records(group));
    reply.emplace(NativeSharedBaseSearchReply{std::move(window),hints});

    auto busy=f.clock.next();
    busy.foreground_pending=true;
    const auto writes=f.image->calls,reads=f.image->read_calls;
    const auto deferred=controller.advance(busy,f.clock.now,{0,&*reply});
    require(deferred.deferral==Defer::foreground && f.image->calls==writes && f.image->read_calls==reads,
        "completed search bypassed foreground admission");
  }
  const auto result=f.finish(controller);
  require(reply && result.published_groups==1 && result.advised_members_published>0 &&
      result.owned_payload_bytes==0 && !controller.search_window(),"advised automatic group did not publish and retire");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(unsigned i=0;i<count;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==original[i],
        "automatic search advice changed committed bytes");
}
enum class BatchOutcome {accepted,cpu_fallback,changed_source,memory_pressure};
void batched_indexed_maintenance(BatchOutcome outcome) {
  constexpr unsigned members=4,files=members+1,cohorts=2;
  Fixture f(4096,0,true);
  std::vector<Bytes> originals;
  for(unsigned cohort=0;cohort<cohorts;++cohort) {
    const auto base=data(kNativeExtentBytes,714+cohort*5731);
    for(unsigned i=0;i<files;++i) {
      auto bytes=i?anchor_search_fixture::fragmented(base,i):base;
      std::copy_n(base.begin(),kNativeExtentBytes/8,bytes.begin());
      put(*f.session,"f"+std::to_string(originals.size()),bytes);
      originals.push_back(std::move(bytes));
    }
  }
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned tick=0;tick<800;++tick) {
    const auto observed=f.clock.next();
    progress=controller.advance(observed,f.clock.now,{cohorts*members,nullptr,true});
    require(progress.phase!=Phase::failed && !progress.published_groups,"batch published before receiving advice");
    if(progress.search_batch_ready)break;
  }
  const auto windows=controller.search_batch();
  require(windows.size()==cohorts && controller.active_group_members()==originals.size(),
      "independent indexed candidates did not fill the admitted batch");
  const auto selection=f.session->snapshot().stat("f0").inode;
  rejects([&]{(void)f.session->prepare_shared_base({selection,0},{});},
      "exclusive writer work entered an active maintenance batch");

  const std::array counts{members,members};
  const auto quantum=anchor_match::max_members;
  const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),cohorts*files);
  detail::AnchorMatchBatchBuilder builder(1,plan);
  for(std::size_t i=0;i<windows.size();++i)builder.append(i+1,windows[i]);
  const auto request=builder.finish();
  std::array<detail::AnchorMatchAdvice,cohorts> hints;
  std::array<NativeSharedBaseSearchReply,cohorts> replies;
  for(std::size_t i=0;i<windows.size();++i) {
    const auto& group=request.groups()[i];
    hints[i]=detail::verify_anchor_match_records(group,detail::cpu_anchor_match_records(group));
    replies[cohorts-1-i]={windows[i],hints[i]};
  }
  const bool changed=outcome==BatchOutcome::changed_source;
  if(changed) {
    originals.front()=data(kNativeExtentBytes,32771);
    const auto h=f.session->open("f0",{true,true,false});
    f.session->write(h,0,originals.front());f.session->cleanup(h,false);f.session->close(h);
    require(controller.search_batch().size()==1,"foreground edit did not invalidate exactly one staged group");
  }
  auto foreground=f.clock.next();
  foreground.foreground_pending=true;
  const auto io=f.image->calls,reads=f.image->read_calls;
  require(controller.advance(foreground,f.clock.now,{0,nullptr,true,replies}).deferral==Defer::foreground &&
      f.image->calls==io && f.image->read_calls==reads,"batch advice bypassed foreground admission");
  if(outcome==BatchOutcome::memory_pressure) {
    auto pressure=f.clock.next();
    pressure.physical_available=pressure.physical_total/16;
    const auto deferred=controller.advance(pressure,f.clock.now,{0,nullptr,true,replies});
    require(deferred.deferral==Defer::memory && !deferred.owned_payload_bytes &&
        !controller.active_group() && !controller.has_retained_search(),
        "batch kept preparations after losing its memory grant");
  }
  const bool accepts=outcome==BatchOutcome::accepted || changed;
  const auto supplied=accepts?std::span<const NativeSharedBaseSearchReply>{replies}:
      std::span<const NativeSharedBaseSearchReply>{};
  const auto expected=changed?1U:cohorts;
  for(unsigned tick=0;tick<800;++tick) {
    const auto observed=f.clock.next();
    progress=controller.advance(observed,f.clock.now,{0,nullptr,true,supplied});
    require(progress.phase!=Phase::failed,"staged batch failed after optional adviser completion");
    if(!controller.active_group() && progress.published_groups==expected)break;
  }
  require(progress.published_groups==expected && !controller.active_group() && !controller.has_retained_search() &&
      progress.owned_payload_bytes==0,"staged batch did not publish and retire its independent groups");
  require(progress.advised_members_published==(supplied.empty()?0:members*expected),
      "batch published advice for the wrong revision or group");
  const auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().bases==expected,"staged groups lost independent bases");
  snapshot.verify_all();
  for(std::size_t i=0;i<originals.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"batch readback differs");
}

void shifted_indexed_family(const Bytes& base,StoreCompressionOptions compression,bool corpus) {
  constexpr unsigned count=12;
  constexpr std::uint32_t unit=4096;
  Fixture f(unit,0,true,compression);
  std::vector<Bytes> originals;
  for(unsigned i=0;i<count;++i) {
    auto input=base;
    if(i) {
      const auto prefix=7*i+1;
      std::copy_n(base.begin(),base.size()-prefix,input.begin()+prefix);
      std::fill_n(input.begin(),prefix,static_cast<std::byte>(i));
    }
    put(*f.session,"shifted"+std::to_string(i),input);
    originals.push_back(std::move(input));
  }
  const auto before=f.session->snapshot();
  const auto workload=f.session->workload_info();
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto baseline_source=image(f.image->stable,unit);
  auto baseline=ram(unit);
  auto copied=NativeFilesystemWriter::compact(NativeFilesystemSnapshot::open_image(baseline_source,indexed),
      baseline,id(2),compression);
  copied.snapshot().verify_all();
  NativeGroupMaintenance controller(*f.session);
  const auto result=f.finish(controller);
  const auto after=f.session->snapshot();
  require(result.published_groups==1 && after.shared_base_info().members==count &&
      after.shared_base_info().bases==1 && f.session->workload_info()==workload,"shifted extents were not grouped");
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  require(root[8]==std::byte{4},"shifted group did not promote the indexed capability");
  controller.cancel();
  f.session.reset();
  auto compacted=ram(unit);
  (void)NativeFilesystemWriter::compact(after,compacted,id(3),compression);
  const auto reopened=NativeFilesystemSnapshot::open_image(compacted,indexed);
  reopened.verify_all();
  for(unsigned i=0;i<count;++i) {
    const auto path="shifted"+std::to_string(i);
    require(reopened.read(path,0,kNativeExtentBytes)==originals[i] &&
        before.read(path,0,kNativeExtentBytes)==originals[i],"shifted group changed current or prior bytes");
  }
  require(compacted->extent()<baseline->extent(),"complete shifted image did not shrink");
  std::cout<<"shifted_indexed corpus="<<corpus<<" members="<<count<<" before_bytes="<<baseline->extent()
      <<" after_bytes="<<compacted->extent()<<" minimum_compacted_saved="<<result.minimum_compacted_bytes_saved<<'\n';
}
void automatic_and_extension(std::uint32_t unit,bool indexed=false) {
  Fixture f(unit,8,indexed);const auto workload=f.session->workload_info();NativeGroupMaintenance original(*f.session);
  rejects([&]{NativeGroupMaintenance duplicate(*f.session);},"duplicate controller admitted");
  auto controller=std::move(original);require(original.progress().phase==Phase::failed,"moved controller stayed live");
  auto result=f.finish(controller);f.verify(8);
  require(result.published_groups==1 && result.minimum_compacted_bytes_saved>0 && result.retained_history_bytes_added>0,
      "related files not grouped as one beneficial group");
  require(f.session->snapshot().shared_base_info().members==8 && f.session->snapshot().shared_base_info().bases==1 &&
      f.session->workload_info()==workload,"grouping changed provenance or fragmented family");
  const auto reads=f.image->read_calls,calls=f.image->calls,attempts=result.attempted_groups;
  for(unsigned i=0;i<100;++i)require(f.step(controller).phase==Phase::idle,"unchanged source did not stay idle");
  require(f.image->read_calls==reads && f.image->calls==calls,"unchanged opportunities performed backing I/O");
  put(*f.session,"f8",family(8));const auto input=f.session->workload_info();result=f.finish(controller);f.verify(9);
  require(result.published_groups==2 && f.session->snapshot().shared_base_info().members==9 && f.session->snapshot().shared_base_info().bases==1,
      "new related file did not merge into protected existing group");
  require(f.session->workload_info()==input && result.attempted_groups>attempts,"regrouping minted input");
  std::cout<<"automatic_groups indexed="<<indexed<<" unit="<<unit<<" published="<<result.published_groups<<" sampled="<<result.sampled_extents
      <<" reused="<<result.reused_features<<" minimum_compacted_saved="<<result.minimum_compacted_bytes_saved<<'\n';
}
void admission() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  const auto reads=f.image->read_calls,calls=f.image->calls;
  for(unsigned variant=0;variant<12;++variant) {
    auto o=f.clock.next();const auto now=f.clock.now;
    if(variant==0)o.valid=false;if(variant==1)o.captured_ms=now+1;if(variant==2)o.captured_ms=now-251;
    if(variant==3)o.physical_total=0;if(variant==4)o.physical_available=o.physical_total+1;
    if(variant==5)o.commit_limit=0;if(variant==6)o.commit_available=o.commit_limit+1;
    if(variant==7)o.cpu_total_ticks=0;if(variant==8)o.cpu_busy_ticks=1001;
    if(variant==9)o.cpu_window_ms=0;if(variant==10)o.cpu_window_ms=2001;if(variant==11)o.captured_ms=0;
    require(controller.advance(o,now).deferral==Defer::observation,"bad observation admitted");
  }
  auto o=f.clock.next();o.foreground_pending=true;require(controller.advance(o,f.clock.now).deferral==Defer::foreground,"foreground queue ignored");
  o=f.clock.next();o.cpu_busy_ticks=126;require(controller.advance(o,f.clock.now).deferral==Defer::cpu,"busy CPU admitted");
  o=f.clock.next();o.physical_available=0;require(controller.advance(o,f.clock.now).deferral==Defer::memory,"pressure admitted");
  require(f.image->read_calls==reads && f.image->calls==calls,"refused observations performed I/O");
  o=f.clock.next();controller.advance(o,f.clock.now);const auto sampled=controller.progress().sampled_extents;
  require(controller.advance(o,f.clock.now).deferral==Defer::observation && controller.progress().sampled_extents==sampled,"replayed observation admitted");
  const auto snapshot=f.session->snapshot();std::vector<NativeExtentSelection> keys;
  for(unsigned i=0;i<4;++i)keys.push_back({snapshot.stat("f"+std::to_string(i)).inode,0});
  auto explicit_work=f.session->prepare_shared_base(keys.front(),keys);const auto before=f.image->read_calls;
  require(f.step(controller).deferral==Defer::other_maintenance && f.image->read_calls==before,"competing work was not deferred");
  explicit_work.cancel();f.finish(controller);f.verify(4);
}
void negative_cache_and_incompressible() {
  Fixture f(4096,2);const auto snapshot=f.session->snapshot();
  const std::array<NativeExtentSelection,2> keys{{{snapshot.stat("f0").inode,0},{snapshot.stat("f1").inode,0}}};
  auto optimized=f.session->prepare_shared_base(keys.front(),keys);
  while(!f.session->advance_shared_base(optimized)){}
  require(f.session->finish_shared_base(optimized).applied,"negative fixture not already optimized");
  NativeGroupMaintenance controller(*f.session);const auto state=f.finish(controller);
  require(state.published_groups==0 && state.declined_groups>0,"unprofitable pair published");
  const auto reads=f.image->read_calls,calls=f.image->calls;
  for(unsigned i=0;i<50;++i)f.step(controller);
  require(f.image->read_calls==reads && f.image->calls==calls,"negative source repeated I/O");
  f.session->create_entry("empty");const auto before=controller.progress();const auto after=f.finish(controller);
  require(after.sampled_extents==before.sampled_extents && after.attempted_groups==before.attempted_groups,
      "metadata-only change repeated content sampling/failed group");
  Fixture other(4096,0);put(*other.session,"a",data(kNativeExtentBytes,1));put(*other.session,"b",data(kNativeExtentBytes,2));
  NativeGroupMaintenance no_match(*other.session);const auto result=other.finish(no_match);
  require(result.published_groups==0 && result.attempted_groups==0 && result.sampled_extents==2,"unrelated data incorrectly nominated");
}
void ownership_and_pressure() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().owned_payload_bytes==0;++i)f.step(controller);
  require(controller.progress().owned_payload_bytes>0,"no private work to pressure");
  auto observation=f.clock.next();observation.commit_available=0;const auto writes=f.image->writes;
  const auto refused=controller.advance(observation,f.clock.now);
  require(refused.deferral==Defer::memory && refused.owned_payload_bytes==0 && f.image->writes==writes,"pressure retained buffers or wrote data");
  NativeFileSession moved=std::move(*f.session);f.session.reset();f.session.emplace(std::move(moved));f.finish(controller);f.verify(4);
  const auto reads=f.image->read_calls;f.session.reset();require(f.step(controller).phase==Phase::failed && f.image->read_calls==reads,"expired session accessed data");
  Fixture next(4096,2);NativeGroupMaintenance cancelled(*next.session);cancelled.cancel();
  require(next.step(cancelled).phase==Phase::failed,"cancelled controller restarted");NativeGroupMaintenance replacement(*next.session);next.finish(replacement);
}
void foreground_change_during_preparation() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().owned_payload_bytes==0;++i)f.step(controller);
  require(controller.progress().owned_payload_bytes>0,"no prepared content to invalidate");
  const auto replacement=data(kNativeExtentBytes,919);
  auto handle=f.session->open("f0",{true,true,false});f.session->write(handle,0,replacement);
  f.session->cleanup(handle,false);f.session->close(handle);
  const auto sampled=controller.progress().sampled_extents;
  const auto result=f.finish(controller);const auto snapshot=f.session->snapshot();
  require(result.sampled_extents>sampled && result.published_groups==1,"changed content was not resampled and reconciled");
  require(snapshot.read("f0",0,kNativeExtentBytes)==replacement,"stale prepared group overwrote foreground content");
  for(unsigned i=1;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"reconciled group changed other data");
  require(snapshot.shared_base_info().members==3,"unrelated replacement entered stale group");snapshot.verify_all();
}
void indexed_scan_windows() {
  Fixture f(4096,0,true);NativeGroupMaintenance controller(*f.session);
  require(!controller.pending(),"empty indexed volume requested discovery");
  constexpr unsigned count=2*kNativeMaxDirtyExtents+3;
  constexpr std::uint64_t sparse_offset=1ULL<<38;
  f.session->create_entry("churn");
  auto churn=f.session->open("churn",{true,true,true});
  Bytes expected{std::byte{0x40}};
  f.session->write(churn,sparse_offset,expected);
  require(!controller.pending(),"one sparse extent requested a group");
  for(unsigned i=0;i<count;++i)put(*f.session,"f"+std::to_string(i),family(i));
  require(controller.pending(),"indexed volume did not request discovery");
  std::uint64_t max_scan_bytes=0;unsigned rewrites=0;
  for(unsigned i=0;i<1000 && controller.progress().published_groups==0;++i) {
    if(i%11==0) {
      expected[0]^=std::byte{1};f.session->write(churn,sparse_offset,expected);++rewrites;
    }
    const auto reads=f.image->read_bytes;const auto prior=controller.progress();
    const auto state=f.step(controller);
    require(state.phase!=Phase::failed,"indexed discovery failed while advancing a live cursor");
    if(prior.phase==Phase::scanning || prior.phase==Phase::candidates)
      max_scan_bytes=(std::max)(max_scan_bytes,f.image->read_bytes-reads);
  }
  require(controller.progress().published_groups!=0 && rewrites>1,"indexed cursor did not reach a family under foreground writes");
  const auto final=f.finish(controller);
  require(final.published_groups>=3 && final.minimum_compacted_bytes_saved>0 && max_scan_bytes<1024*1024,
      "indexed batching, sizing or one-extent scan bound failed");
  auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().members==count && snapshot.shared_base_info().bases==1,
      "indexed windows failed to extend one immutable base");
  require(snapshot.read("churn",sparse_offset,1)==expected,"background scan changed sparse foreground bytes");
  f.verify(count);
  f.session->rename(churn,"renamed");
  f.session->cleanup(churn,false);f.session->close(churn);
  f.session->create_entry("empty");
  const auto reads=f.image->read_calls,calls=f.image->calls;
  require(!controller.pending() && f.step(controller).phase==Phase::idle &&
      f.image->read_calls==reads && f.image->calls==calls,"indexed metadata changes restarted payload discovery");
  std::cout<<"indexed_scan members="<<count<<" published_groups="<<final.published_groups<<" foreground_rewrites="<<rewrites
      <<" max_scan_read_bytes="<<max_scan_bytes<<" lower_bound_savings="<<final.minimum_compacted_bytes_saved<<'\n';
}
void indexed_stale_binding_and_padding() {
  Fixture f(4096,4,true);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().phase!=Phase::candidates;++i)f.step(controller);
  require(controller.progress().phase==Phase::candidates,"indexed sample window was not ready");
  auto handle=f.session->open("f0",{true,true,false});
  const auto changed=data(kNativeExtentBytes,449);
  f.session->write(handle,0,changed);f.session->cleanup(handle,false);f.session->close(handle);
  for(unsigned i=0;i<1000 && !controller.active_group();++i)f.step(controller);
  require(controller.active_group(),"saved candidate did not reach its generation check");
  const auto reads=f.image->read_bytes,calls=f.image->calls;
  for(unsigned i=0;i<1000 && controller.active_group();++i)f.step(controller);
  require(!controller.active_group() && f.image->read_bytes-reads<kNativeExtentBytes && f.image->calls==calls &&
      !f.session->poisoned(),"a changed sampled base was decoded or published");
  (void)f.finish(controller);
  auto view=f.session->snapshot();
  require(view.shared_base_info().members==3 && view.read("f0",0,kNativeExtentBytes)==changed,
      "stale sample caused incomplete recovery or changed the acknowledged bytes");
  view.verify_all();
  Fixture small(4096,0,true);
  put(*small.session,"first",data(128,1));put(*small.session,"second",data(192,2));
  NativeGroupMaintenance padding(*small.session);const auto result=small.finish(padding);
  require(result.sampled_extents==2 && result.attempted_groups==0,"zero-padded bands nominated unrelated small files");
}
void publication_faults(std::uint32_t unit) {
  Fixture original(unit,4);const auto bytes=original.image->stable,head=original.head->stable;original.session.reset();
  auto setup=[&](const auto& data_io,const auto& control){return NativeFileSession::resume_volume(control,id(90),1,
      [&](const auto&)->std::shared_ptr<StoreByteIo>{return data_io;},{StoreCompressionMode::raw,1,false});};
  auto advance_ready=[&](auto& controller,Clock& clock){for(unsigned i=0;i<1000;++i){const auto o=clock.next();const auto result=controller.advance(o,clock.now);
    if(result.phase==Phase::publishing)return;}throw std::runtime_error("publication phase not reached");};
  auto probe=image(bytes,unit),probe_head=image(head,unit);std::size_t cuts=0;
  {auto session=setup(probe,probe_head);NativeGroupMaintenance controller(session);Clock clock;advance_ready(controller,clock);const auto start=probe->calls;
    for(unsigned i=0;i<1000 && controller.progress().published_groups==0;++i){const auto o=clock.next();controller.advance(o,clock.now);}
    require(controller.progress().published_groups==1,"probe did not publish");cuts=probe->calls-start;}
  require(cuts>3 && cuts<256,"automatic publication fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto data_io=image(bytes,unit),control=image(head,unit);
    {auto session=setup(data_io,control);NativeGroupMaintenance controller(session);Clock clock;advance_ready(controller,clock);
      data_io->failure=mode;data_io->fail_at=data_io->calls+cut;
      for(unsigned i=0;i<1000 && controller.progress().phase!=Phase::failed;++i){const auto o=clock.next();controller.advance(o,clock.now);}
      const auto result=controller.progress();
      require(result.phase==Phase::failed && result.owned_payload_bytes==0 && session.poisoned(),"failed automatic publication stayed active");
      const auto calls=data_io->calls,reads=data_io->read_calls;
      for(unsigned i=0;i<5;++i){const auto next=clock.next();controller.advance(next,clock.now);}
      require(data_io->calls==calls && data_io->read_calls==reads,"failed source retried I/O");}
    data_io->live=data_io->stable;
    auto snapshot=NativeFilesystemSnapshot::open_image(data_io,{NativeImageKind::paged,kStoreMaxPhysicalBytes});
    for(unsigned i=0;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"automatic fault lost file data");
    snapshot.verify_all();++faults;
  }
  std::cout<<"automatic_group_faults unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
void reopen_indexed(Fixture& fixture,const std::shared_ptr<Ram>& advice,std::uint64_t sequence) {
  fixture.session.reset();
  fixture.session.emplace(NativeFileSession::resume_closed_indexed_volume(fixture.head,id(90),1,
      [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?fixture.image:nullptr;},
      {StoreCompressionMode::raw,1,false}));
  require(fixture.session->resume_budgeted_optimization_area(advice,sequence).phase==NativeOptimizationPhase::ready,
      "indexed scan advice could not be rebound after closing");
}
void finish_pending(Fixture& fixture,NativeGroupMaintenance& controller) {
  for(unsigned i=0;i<6000&&controller.pending();++i)
    require(fixture.step(controller).phase!=Phase::failed,"indexed scan stopped");
  require(!controller.pending(),"indexed scan did not finish");
}
void persistent_indexed_scan() {
  constexpr unsigned count=static_cast<unsigned>(kNativeMaxDirtyExtents)+9;
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "reserved indexed scan advice unavailable");
  for(unsigned i=0;i<count;++i)put(*f.session,"f"+std::to_string(i),family(i));
  // Sparse all-zero data supplies genuine accepted input without changing the
  // similarity window. Each operation is below the native transaction limit.
  put(*f.session,"sparse-a",Bytes(kOptimizationGroupScanInputBytes/2));
  put(*f.session,"sparse-b",Bytes(kOptimizationGroupScanInputBytes/2));
  {
    NativeGroupMaintenance controller(*f.session);
    for(unsigned i=0;i<3500&&controller.progress().checkpoints_published==0;++i)
      require(f.step(controller).phase!=Phase::failed,"initial indexed scan failed");
    require(controller.progress().checkpoints_published==1 && controller.pending(),
        "indexed scan did not durably checkpoint its first window");
  }
  const auto partial=read_optimization_state(*advice,id(90));
  require(partial.code==OptimizationReadCode::available && !partial.degraded && partial.state.group_scan &&
      !partial.state.group_scan->complete && !partial.state.group_scan->revisit &&
      partial.state.settled_input_bytes==kOptimizationGroupScanInputBytes &&
      std::ranges::any_of(partial.state.group_scan->representatives,[](const auto& item){return item.exact_base;}),
      "first scan checkpoint lacks a checked reusable representation");
  auto old_checkpoint=image(advice->stable,4096);
  reopen_indexed(f,advice,partial.sequence);
  std::uint64_t resumed_samples=0;
  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    const auto state=controller.progress();resumed_samples=state.sampled_extents;
    require(state.checkpoints_restored==1 && state.checkpoints_published==1 &&
        state.published_groups==1 && resumed_samples<count-kNativeMaxDirtyExtents+3,
        "reopened discovery repeated the old window or lost its shared base");
    const auto actual=f.session->snapshot().shared_base_info();
    require(actual.bases==1&&actual.members==count,"reopened discovery did not extend the existing group");
  }
  f.verify(count);
  const auto completed=read_optimization_state(*advice,id(90));
  require(completed.state.group_scan && completed.state.group_scan->complete &&
      completed.state.settled_input_bytes==2*kOptimizationGroupScanInputBytes &&
      completed.sequence==partial.sequence+1,"completion or durable write charge is missing");
  reopen_indexed(f,advice,completed.sequence);
  {
    NativeGroupMaintenance controller(*f.session);
    const auto writes=advice->writes;finish_pending(f,controller);
    require(controller.progress().checkpoints_restored==1 && controller.progress().sampled_extents==0 &&
        advice->writes==writes,"completed unchanged scan repeated sampling or wrote state");
  }
  const auto replacement=data(kNativeExtentBytes,809);
  auto handle=f.session->open("f0",{true,true,false});f.session->write(handle,0,replacement);
  f.session->cleanup(handle,false);f.session->close(handle);
  reopen_indexed(f,old_checkpoint,partial.sequence);
  {
    NativeGroupMaintenance controller(*f.session);finish_pending(f,controller);
    require(controller.progress().checkpoints_restored==1 && controller.progress().sampled_extents>=count,
        "changed prefix was skipped after resuming an older cursor");
    const auto snapshot=f.session->snapshot();
    require(snapshot.read("f0",0,kNativeExtentBytes)==replacement,"older checkpoint changed replacement bytes");
    for(unsigned i=1;i<count;++i)
      require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"older checkpoint changed a peer");
    snapshot.verify_all();
  }
  std::cout<<"persistent_indexed_scan total="<<count<<" resumed_samples="<<resumed_samples
      <<" checkpoint_store_bytes="<<advice->extent()<<" write_cost="<<kOptimizationAreaBytes<<'\n';
}
void indexed_scan_checkpoint_faults() {
  for(const auto unit:{4096U,65536U})for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<4;++cut) {
    Fixture f(4096,0,true);auto advice=ram(unit);
    require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
        "checkpoint fault fixture advice ownership");
    put(*f.session,"f0",family(0));put(*f.session,"f1",family(1));
    put(*f.session,"funding",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
    advice->fail_at=advice->calls+cut;advice->failure=mode;
    NativeGroupMaintenance controller(*f.session);finish_pending(f,controller);
    require(!f.session->poisoned() && controller.progress().published_groups==1 &&
        controller.progress().checkpoints_published==0 &&
        f.session->optimization_info().phase==NativeOptimizationPhase::terminal,
        "optional checkpoint failure fenced data or acknowledged an incomplete write");
    const auto writes=advice->writes;
    f.step(controller);require(advice->writes==writes,"failed checkpoint retried");
    const auto recovered=read_optimization_state(*image(advice->stable,unit),id(90));
    require(recovered.code==OptimizationReadCode::available && (recovered.sequence==1 || recovered.sequence==2) &&
        (recovered.sequence==1?!recovered.state.group_scan:
         recovered.state.group_scan && recovered.state.group_scan->complete),
        "power cut exposed a partial scan checkpoint");
    f.verify(2);++faults;
  }
}
}
int main(int argc,char** argv){try{
  if(argc==2 && std::string_view(argv[1])=="--indexed-resume") {
    persistent_indexed_scan();indexed_scan_checkpoint_faults();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-advice") {
    advised_indexed_maintenance();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batch") {
    batched_indexed_maintenance(BatchOutcome::accepted);
    batched_indexed_maintenance(BatchOutcome::cpu_fallback);
    batched_indexed_maintenance(BatchOutcome::changed_source);
    batched_indexed_maintenance(BatchOutcome::memory_pressure);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed") {
    for(auto unit:{4096U,65536U})automatic_and_extension(unit,true);
    indexed_scan_windows();indexed_stale_binding_and_padding();
    shifted_indexed_family(data(kNativeExtentBytes,714),{StoreCompressionMode::raw,1,false},false);
  } else if(argc==3 && std::string_view(argv[1])=="--shifted-corpus") {
    auto input=ReadOnlyFile::open(argv[2]);
    require(input.size()==kNativeExtentBytes,"expected one extent of public source data");
    Bytes bytes(kNativeExtentBytes);
    input.read_exact_at(0,bytes.data(),bytes.size());
    shifted_indexed_family(bytes,{StoreCompressionMode::automatic,6,true},true);
  } else if(argc==1) {
    for(auto unit:{4096U,65536U}){automatic_and_extension(unit);publication_faults(unit);}
    admission();negative_cache_and_incompressible();ownership_and_pressure();foreground_change_during_preparation();
  } else throw std::invalid_argument("unknown group maintenance test mode");
  std::cout<<"PASS: "<<checks<<" automatic group maintenance checks; "<<faults<<" injected publication cases; model resource observations; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
