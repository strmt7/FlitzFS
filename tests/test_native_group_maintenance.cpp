#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/native_group_anchor.hpp"
#include "../src/native_group_codec.hpp"
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
enum class BatchOutcome {accepted,cpu_fallback,changed_source,memory_pressure,overlap_success,overlap_fallback};
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
  const bool overlaps=outcome==BatchOutcome::overlap_success || outcome==BatchOutcome::overlap_fallback;
  if(overlaps) {
    const auto staged=std::span<const NativeSharedBaseSearchWindow>{windows};
    const auto prefix=staged.first(1);
    const auto unchanged=[&](std::span<const NativeSharedBaseSearchWindow> submitted,bool foreground_busy,Defer expected) {
      auto observed=f.clock.next();
      observed.foreground_pending=foreground_busy;
      const auto writes=f.image->calls, reads=f.image->read_calls;
      require(controller.advance(observed,f.clock.now,{0,nullptr,true,{},submitted}).deferral==expected &&
          f.image->calls==writes && f.image->read_calls==reads,"pending adviser changed a protected group");
    };
    unchanged(staged.last(1),false,Defer::other_maintenance);
    unchanged(staged,false,Defer::other_maintenance);
    unchanged(prefix,true,Defer::foreground);
    for(unsigned tick=0;tick<800 && !progress.published_groups;++tick) {
      const auto observed=f.clock.next();
      progress=controller.advance(observed,f.clock.now,{0,nullptr,true,{},prefix});
      require(progress.phase!=Phase::failed,"CPU suffix failed while the adviser was pending");
    }
    require(progress.published_groups==1 && !progress.advised_members_published &&
        controller.active_group_members()==files && controller.has_retained_search(),
        "unsubmitted work did not finish independently of the adviser");
    unchanged(prefix,false,Defer::other_maintenance);
  }
  const bool accepts=outcome==BatchOutcome::accepted || changed;
  const auto all_replies=std::span<const NativeSharedBaseSearchReply>{replies};
  const auto supplied=accepts?all_replies:outcome==BatchOutcome::overlap_success?all_replies.last(1):
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
  require(progress.advised_members_published==members*(changed?1:supplied.size()),
      "batch published advice for the wrong revision or group");
  const auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().bases==expected,"staged groups lost independent bases");
  snapshot.verify_all();
  for(std::size_t i=0;i<originals.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"batch readback differs");
}

void indexed_batch_across_scan_windows() {
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  constexpr auto expected_members=anchor_search_fixture::indexed_batch_members;
  Fixture f(4096,0,true);
  const auto originals=anchor_search_fixture::indexed_batch_files();
  for(std::size_t i=0;i<originals.size();++i)put(*f.session,"f"+std::to_string(i),originals[i]);
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned tick=0;tick<2000;++tick) {
    const auto observed=f.clock.next();
    // Leave some work unfilled. Reaching the end of the scan must send the
    // partial batch without waiting for more files to appear.
    progress=controller.advance(observed,f.clock.now,{expected_members+families,nullptr,true});
    require(progress.phase!=Phase::failed && !progress.published_groups,
        "cross-window batch published before receiving advice");
    if(progress.search_batch_ready)break;
  }
  auto windows=controller.search_batch();
  require(progress.search_batch_ready && windows.size()==families,
      "batch stopped at the boundary of a scan window");
  std::size_t members=0;
  for(const auto& window:windows) {
    require(window.members().size()==kNativeMaxDirtyExtents-1,"batch changed the transaction work bound");
    members+=window.members().size();
  }
  require(members==expected_members && progress.sampled_extents==families*kNativeMaxDirtyExtents,
      "batch omitted members or rescanned source extents");

  auto pressure=f.clock.next();
  pressure.physical_available=pressure.physical_total/16;
  // The test owns independent references just like the adviser. Retire them
  // before confirming the maintenance owner's memory has been released.
  windows.clear();
  const auto deferred=controller.advance(pressure,f.clock.now,{expected_members+families,nullptr,true});
  require(deferred.deferral==Defer::memory && !deferred.owned_payload_bytes &&
      !controller.active_group() && !controller.has_retained_search(),
      "cross-window batch retained work after its resource grant contracted");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(std::size_t i=0;i<originals.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,originals[i].size())==originals[i],
        "cross-window retirement changed acknowledged data");
  std::cout<<"indexed batch scan windows="<<families<<" admitted members="<<members
      <<" source extents="<<progress.sampled_extents<<" retired under memory pressure\n";
}

void indexed_batch_reclaims_append_space() {
  require(!NativeVolumeSpace{100,25,16,0}.reclamation_worthwhile() &&
      !NativeVolumeSpace{100,25,16,17}.reclamation_worthwhile() &&
      NativeVolumeSpace{100,25,16,16}.reclamation_worthwhile(),
      "Reclamation did not price the proved append-space shortfall");
  constexpr auto members=anchor_search_fixture::indexed_batch_members;
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  Fixture f(kStoreCommitUnit,0,true);
  const auto original=anchor_search_fixture::indexed_batch_files();
  for(std::size_t i=0;i<original.size();++i)put(*f.session,"f"+std::to_string(i),original[i]);
  const auto extent=f.image->extent();
  const auto committed=f.image->calls;
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned i=0;i<2000 && !progress.search_batch_ready;++i) {
    const auto o=f.clock.next();
    progress=controller.advance(o,f.clock.now,{members+families,nullptr,true});
    require(progress.phase!=Phase::failed,"Capacity fixture did not prepare");
  }
  require(controller.search_batch().size()==families,"Capacity fixture did not retain a complete batch");
  for(unsigned i=0;i<2000 && progress.deferral!=Defer::capacity;++i) {
    const auto o=f.clock.next();
    progress=controller.advance(o,f.clock.now,{0,nullptr,true});
    require(progress.phase!=Phase::failed,"Append refusal fenced the indexed volume");
  }
  require(progress.deferral==Defer::capacity && progress.append_shortfall_bytes &&
      progress.published_groups==0 && progress.declined_groups==1 &&
      !controller.pending() && !controller.has_retained_search() && !progress.owned_payload_bytes &&
      f.image->extent()==extent && f.image->calls==committed,
      "Append refusal did not release the batch before publication");
  const auto space=f.session->indexed_volume_space();
  require(space.compression_shortfall_bytes==progress.append_shortfall_bytes && space.reclamation_worthwhile(),
      "The selected image did not offer bounded capacity reclamation");
  const auto reads=f.image->read_calls;
  for(unsigned i=0;i<4;++i) {
    const auto o=f.clock.next();
    require(controller.advance(o,f.clock.now,{members+families,nullptr,true}).deferral==Defer::capacity &&
        !controller.pending(),"Unchanged capacity refusal restarted expensive preparation");
  }
  require(f.image->calls==committed && f.image->read_calls==reads,
      "Paused capacity refusal read or modified the image");

  auto replacement=ram(kStoreCommitUnit);
  require(f.session->begin_idle_compaction(replacement,id(9)),"Capacity fixture could not start a protected rebuild");
  bool ready=false;
  for(unsigned i=0;i<4000 && !ready;++i)
    ready=f.session->advance_recompression()==NativeFileSession::RecompressionProgress::ready;
  require(ready && f.session->publish_idle_recompression().has_value() && f.image->calls==committed &&
      f.session->indexed_volume_space().free_bytes>space.free_bytes &&
      !f.session->indexed_volume_space().compression_shortfall_bytes && controller.pending(),
      "Verified reclamation did not reopen compression on the selected copy");
  // Sanitized operations can require more idle credit. Count admitted work
  // toward the progress bound, and require each idle deferral to pay down debt.
  for(unsigned admitted=0;admitted<1000 && !progress.published_groups;) {
    const auto previous_delay=progress.deferral==Defer::duty?progress.next_opportunity_delay_ms:0;
    progress=f.step(controller);
    require(progress.phase!=Phase::failed,"Compression failed after reclaiming append space");
    if(progress.deferral==Defer::duty) {
      require(progress.next_opportunity_delay_ms &&
          (!previous_delay || progress.next_opportunity_delay_ms<previous_delay),
          "Idle observations did not advance the capacity resume gate");
    } else {
      ++admitted;
    }
  }
  require(progress.published_groups!=0,"Compression did not resume after verified compaction");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(std::size_t i=0;i<original.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,original[i].size())==original[i],
        "Reclamation or resumed compression changed acknowledged bytes");
  std::cout<<"indexed capacity shortfall="<<space.compression_shortfall_bytes
      <<" free_before="<<space.free_bytes<<" source_unchanged=true resumed=true\n";
}

void indexed_partial_overlap_preserves_candidates() {
  const auto bank=data(anchor_search_fixture::indexed_sample_bank_bytes,4839);
  auto original=anchor_search_fixture::indexed_batch_files(bank);
  const auto independent=data(kNativeExtentBytes,15293);
  // Put a small independent cohort in the window containing the larger,
  // partially overlapping cohorts. Its candidate ranks below their conflict.
  for(std::size_t i=0;i<3;++i) {
    auto extent=std::span(original[i]).last(kNativeExtentBytes);
    std::ranges::copy(independent,extent.begin());
    extent[23]^=static_cast<std::byte>(i);
  }
  const auto compare=[&](bool offer_batch) {
    Fixture f(4096,0,true);
    for(std::size_t i=0;i<original.size();++i)put(*f.session,"f"+std::to_string(i),original[i]);
    NativeGroupMaintenance controller(*f.session);
    bool emitted=false;
    for(unsigned i=0;i<5200 && controller.pending();++i) {
      const auto o=f.clock.next();
      const auto grant=offer_batch && !emitted?anchor_search_fixture::indexed_batch_members:0;
      const auto progress=controller.advance(o,f.clock.now,{grant,nullptr,offer_batch});
      require(progress.phase!=Phase::failed && progress.deferral!=Defer::capacity,"Overlap fixture did not settle");
      if(progress.search_batch_ready) {
        const auto windows=controller.search_batch();
        std::cout<<"indexed overlap first_batch_groups="<<windows.size()<<'\n';
        require(std::ranges::any_of(windows,[&](const auto& window) {
          return std::ranges::equal(window.base(),independent) && window.members().size()==2;
        }),"A partial overlap prevented the independent candidate from joining the batch");
        emitted=true;
      }
    }
    require(!controller.pending() && emitted==offer_batch && controller.progress().published_groups,
        "Overlap fixture did not exercise its requested grouping path");
    f.session.reset();
    const auto closed=NativeVolumeHeadView::open(f.head,id(90),[&](const auto& image)->std::shared_ptr<StoreByteIo> {
      return image==id(1)?f.image:nullptr;
    });
    auto compacted=ram(4096);
    (void)NativeFilesystemWriter::compact(closed.filesystem(),compacted,id(2),{StoreCompressionMode::raw,1,false});
    auto verified=NativeFilesystemSnapshot::open_image(compacted,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verified.verify_all();
    for(std::size_t i=0;i<original.size();++i)
      require(verified.read("f"+std::to_string(i),0,original[i].size())==original[i],"Overlap fixture changed source bytes");
    return compacted->extent();
  };
  const auto cpu=compare(false),batched=compare(true);
  require(batched<=cpu,"Staging discarded partly overlapping candidates");
  std::cout<<"indexed overlap cpu_bytes="<<cpu<<" batched_bytes="<<batched<<'\n';
}

void modular_delta_indexed_family(const Bytes& base,StoreCompressionOptions compression,bool require_delta) {
  constexpr unsigned count=9;
  Fixture f(4096,0,true,compression);
  std::vector<Bytes> original;
  for(unsigned i=0;i<count;++i) {
    original.push_back(calibrated_extent(base,i));
    put(*f.session,"f"+std::to_string(i),original.back());
  }
  const auto compact_size=[&](const auto& snapshot,unsigned identity) {
    auto output=ram(4096);
    (void)NativeFilesystemWriter::compact(snapshot,output,id(identity),compression);
    auto reopened=NativeFilesystemSnapshot::open_image(output,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    for(unsigned i=0;i<count;++i)
      require(reopened.read("f"+std::to_string(i),0,kNativeExtentBytes)==original[i],
          "Modular delta compaction changed source bytes");
    return output->extent();
  };
  const auto resolve=[&](const auto& image)->std::shared_ptr<StoreByteIo> {return image==id(1)?f.image:nullptr;};
  f.session.reset();
  const auto baseline=compact_size(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,compression));
  NativeGroupMaintenance controller(*f.session);
  const auto start=std::chrono::steady_clock::now();
  const auto progress=f.finish(controller);
  const auto maintenance_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count();
  controller.cancel();
  f.session.reset();
  const auto closed=NativeVolumeHeadView::open(f.head,id(90),resolve);
  const auto grouped=compact_size(closed.filesystem(),3);
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  std::cout<<"indexed_modular_delta files="<<count<<" groups="<<progress.published_groups
      <<" capability="<<capability<<" input_bytes="<<baseline<<" output_bytes="<<grouped
      <<" maintenance_ns="<<maintenance_ns<<" cpu_readback=true\n";
  if(require_delta)require(progress.published_groups && capability==6 && grouped<baseline,
      "Automatic maintenance missed fully shifted numeric data");
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
void xor_indexed_family(bool require_xor,bool throughout=false) {
  constexpr unsigned count=9,unit=4096;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  Fixture f(unit,0,true,raw);
  const auto base=data(kNativeExtentBytes,714);
  std::array<Bytes,count> originals;
  for(unsigned member=0;member<count;++member) {
    originals[member]=base;
    if(member) {
      const auto variation=data(kNativeExtentBytes,861+member);
      // The projected case varies both middle bit planes in every band.
      const auto start=throughout?0U:kNativeExtentBytes/8;
      const auto mask=throughout?std::byte{0x30}:std::byte{1};
      for(std::size_t i=start;i<base.size();++i)
        originals[member][i]^=variation[i]&mask;
    }
    put(*f.session,"xor"+std::to_string(member),originals[member]);
  }
  const auto prior=f.session->snapshot();
  const auto baseline_source=image(f.image->stable,unit);
  auto baseline=ram(unit);
  (void)NativeFilesystemWriter::compact(NativeFilesystemSnapshot::open_image(baseline_source,indexed),
      baseline,id(2),raw);

  NativeGroupMaintenance maintenance(*f.session);
  const auto workload=f.session->workload_info();
  const auto result=f.finish(maintenance);
  const auto after=f.session->snapshot();
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  require(f.session->workload_info()==workload,"XOR grouping changed user input provenance");
  maintenance.cancel();
  f.session.reset();
  auto compacted=ram(unit);
  (void)NativeFilesystemWriter::compact(after,compacted,id(3),raw);
  const auto reopened=NativeFilesystemSnapshot::open_image(compacted,indexed);
  reopened.verify_all();
  for(unsigned member=0;member<count;++member) {
    const auto path="xor"+std::to_string(member);
    require(reopened.read(path,0,kNativeExtentBytes)==originals[member] &&
        prior.read(path,0,kNativeExtentBytes)==originals[member],"XOR group changed current or prior bytes");
  }
  if(require_xor)
    require(capability==5 && result.published_groups==1 && reopened.shared_base_info().members==count &&
        reopened.shared_base_info().bases==1 && compacted->extent()<baseline->extent()/2,
        "automatic indexed XOR group was absent or missed its expected size");
  std::cout<<"indexed_xor throughout="<<throughout<<" capability="<<capability
      <<" members="<<reopened.shared_base_info().members
      <<" logical_bytes="<<count*kNativeExtentBytes<<" before_bytes="<<baseline->extent()
      <<" after_bytes="<<compacted->extent()<<" groups="<<result.published_groups
      <<" accounted_savings="<<result.minimum_compacted_bytes_saved<<'\n';
}
void xor_reference_selection(anchor_search_fixture::IndexedBatchPattern pattern) {
  using Pattern=anchor_search_fixture::IndexedBatchPattern;
  constexpr unsigned count=9,unit=4096;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const bool full=pattern==Pattern::xor_full_reference;
  std::array<Bytes,count> originals;
  const auto base=data(kNativeExtentBytes,74181);
  for(unsigned i=0;i<count;++i) {
    originals[i]=base;
    if(full)anchor_search_fixture::apply_full_reference_variation(originals[i],base,i,count);
    else if(pattern==Pattern::xor_sparse_reference)anchor_search_fixture::apply_sparse_reference_variation(originals[i],i,count);
    else if(pattern==Pattern::xor_tile_reference)anchor_search_fixture::apply_tile_reference_variation(originals[i],i,count);
    else anchor_search_fixture::apply_reference_variation(originals[i],i);
  }
  const auto run=[&](bool automatic) {
    Fixture f(unit,0,true,raw);
    for(unsigned i=0;i<count;++i)put(*f.session,"r"+std::to_string(i),originals[i]);
    const auto before=f.session->snapshot();
    const auto workload=f.session->workload_info();
    if(automatic) {
      NativeGroupMaintenance controller(*f.session);
      if(full) {
        for(unsigned opportunity=0;opportunity<4*count && !controller.progress().cpu_reference_pairs;++opportunity)
          f.step(controller);
        require(controller.progress().cpu_reference_pairs==1,"CPU pricing did not start one pair at a time");
        auto foreground=f.clock.next();
        foreground.foreground_pending=true;
        const auto reads=f.image->read_calls,writes=f.image->calls;
        const auto deferred=controller.advance(foreground,f.clock.now);
        require(deferred.deferral==Defer::foreground && deferred.cpu_reference_pairs==1 &&
            f.image->read_calls==reads && f.image->calls==writes,"CPU pricing did not yield to foreground work");
      }
      const auto result=f.finish(controller);
      require(result.published_groups==1,"Automatic reference did not publish one group");
      if(full)require(result.cpu_reference_pairs==32 && result.reselected_reference_groups==1,
          "The complete search did not improve the sampled reference");
    }else {
      // Apply the same encoder and publication checks using the first extent.
      std::array<NativeExtentSelection,count> group;
      for(unsigned i=0;i<count;++i)group[i]={before.stat("r"+std::to_string(i)).inode,0};
      auto work=f.session->prepare_shared_base(group.front(),group);
      bool ready=false;
      for(unsigned step=0;step<4*count && !ready;++step)ready=f.session->advance_shared_base(work);
      require(ready && f.session->finish_shared_base(work).applied,"First-reference control failed to publish");
    }
    const auto after=f.session->snapshot();
    require(after.shared_base_info().members==count && after.shared_base_info().bases==1 &&
        f.session->workload_info()==workload,"Reference choice changed the group or its input accounting");
    f.session.reset();
    auto packed=ram(unit);
    (void)NativeFilesystemWriter::compact(after,packed,id(2),raw);
    const auto reopened=NativeFilesystemSnapshot::open_image(packed,indexed);
    reopened.verify_all();
    for(unsigned i=0;i<count;++i) {
      const auto path="r"+std::to_string(i);
      require(reopened.read(path,0,kNativeExtentBytes)==originals[i] &&
          before.read(path,0,kNativeExtentBytes)==originals[i],"Reference choice altered current or prior bytes");
    }
    return packed->extent();
  };
  const auto first=run(false),selected=run(true);
  require(selected<first,"Automatic reference did not improve the complete image");
  const auto label=full?"xor_full_reference":pattern==Pattern::xor_sparse_reference?"xor_sparse_reference":
      pattern==Pattern::xor_tile_reference?"xor_tile_reference":"xor_reference";
  std::cout<<label<<" members="<<count
      <<" first_bytes="<<first<<" selected_bytes="<<selected
      <<" byte_exact=true\n";
}
void xor_reference_decline_fallback() {
  constexpr std::size_t count=3,sites=8,tile_bytes=256,spacing=kNativeExtentBytes/(sites*tile_bytes);
  constexpr std::size_t bands=16,band_bytes=64,band_spacing=kNativeExtentBytes/bands;
  Fixture f(4096,0,true);
  const auto base=data(kNativeExtentBytes,73217);
  std::array<Bytes,count> originals{base,base,base};
  const auto variation=data(kNativeExtentBytes,31804);
  for(std::size_t i=0;i<base.size();++i)
    originals[i<base.size()/2?1:2][i]^=variation[i];
  for(std::size_t band=0;band<bands;++band) {
    // Preserve the shared masked signature without introducing long copies
    // between members whose nonmatching halves contain independent bytes.
    for(std::size_t i=0;i<band_bytes;++i) {
      const auto offset=band*band_spacing+i;
      originals[1][offset]=base[offset]^(i%2?std::byte{0x80}:std::byte{});
      originals[2][offset]=originals[1][offset]^std::byte{0x80};
    }
  }
  const auto second=data(kNativeExtentBytes,89325),third=data(kNativeExtentBytes,61773);
  for(std::size_t site=0;site<sites;++site) {
    // The second member shares one plane with each of the other members in
    // the sampled tiles. Those few matches cannot pay for an encoded frame
    // between the second and third members.
    const auto offset=(site*spacing+2*((site*7)%(spacing/2))+1)*tile_bytes;
    for(std::size_t i=offset;i<offset+tile_bytes;++i) {
      originals[1][i]=(second[i]&~std::byte{2})|(base[i]&std::byte{2});
      originals[2][i]=(third[i]&~std::byte{1})|(originals[1][i]&std::byte{1});
    }
  }
  for(std::size_t i=0;i<count;++i)put(*f.session,"r"+std::to_string(i),originals[i]);
  NativeGroupMaintenance controller(*f.session);
  const auto result=f.finish(controller);
  const auto snapshot=f.session->snapshot();
  require(result.published_groups==1 && result.declined_groups>=1 &&
      snapshot.shared_base_info().members==count,"Declined estimate suppressed the first reference");
  snapshot.verify_all();
  for(std::size_t i=0;i<count;++i)
    require(snapshot.read("r"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
        "Reference fallback changed file contents");
  std::cout<<"xor_reference_fallback declined="<<result.declined_groups<<" groups="<<result.published_groups
      <<" byte_exact=true\n";
}
void indexed_group_with_outlier() {
  constexpr unsigned related=4;
  Fixture f(4096,0,true);
  std::array<Bytes,related+1> originals;
  const auto base=family(0);
  for(unsigned i=0;i<related;++i) {
    originals[i]=base;
    const auto variation=data(base.size(),917+4627*i);
    for(std::size_t at=0;at<base.size();++at)
      originals[i][at]^=static_cast<std::byte>(std::to_integer<unsigned>(variation[at])%3);
  }
  auto& outlier=originals.back();
  outlier=data(kNativeExtentBytes,435187);
  // Match one sampled masked-XOR signature. Outside those 16 short ranges
  // the unrelated extents have no common byte runs or complete bit planes.
  for(std::size_t sample=0;sample<16;++sample) {
    const auto start=sample*kNativeExtentBytes/16;
    for(std::size_t at=start;at<start+64;++at)outlier[at]=base[at]^std::byte{3};
  }
  for(unsigned i=0;i<originals.size();++i)put(*f.session,"outlier-"+std::to_string(i),originals[i]);

  NativeGroupMaintenance controller(*f.session);
  bool saw_full_window=false;
  for(unsigned tick=0;tick<1200 && !controller.progress().published_groups;++tick) {
    const auto observation=f.clock.next();
    const auto result=controller.advance(observation,f.clock.now,{related,nullptr});
    require(result.phase!=Phase::failed,"outlier proposal failed");
    if(const auto window=controller.search_window()) {
      if(window.members().size()==related && std::ranges::any_of(window.members(),[&](const auto& member) {
        return member==outlier;
      }))saw_full_window=true;
    }
  }
  const auto result=f.finish(controller);
  const auto snapshot=f.session->snapshot();
  require(saw_full_window && result.published_groups==1 && result.declined_groups>0 &&
      result.minimum_compacted_bytes_saved>0 && snapshot.shared_base_info().members==related,
      "strong members were lost or the outlier joined the published group");
  snapshot.verify_all();
  for(unsigned i=0;i<originals.size();++i)
    require(snapshot.read("outlier-"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
        "subset publication changed file bytes");
  std::cout<<"indexed_group_outlier members="<<snapshot.shared_base_info().members
      <<" declined="<<result.declined_groups<<" byte_exact=true\n";
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
void continued_preparation_admission() {
  Fixture f(4096,8,true);
  NativeMaintenancePolicy policy;policy.work_share_ppm=50000;
  NativeGroupMaintenance controller(*f.session,policy);
  auto seen=f.clock.next();
  controller.advance(seen,f.clock.now);
  const auto held_reads=f.image->read_calls,held_writes=f.image->calls;
  auto invalid=f.clock.next();invalid.continued=true;
  require(controller.advance(invalid,f.clock.now).deferral==Defer::observation &&
      f.image->read_calls==held_reads && f.image->calls==held_writes,"Continuation advanced its CPU capture");

  unsigned continued=0;
  for(unsigned sample=0;sample<128 && controller.progress().phase!=Phase::publishing;++sample) {
    seen=f.clock.next();
    auto result=controller.advance(seen,f.clock.now);
    require(result.phase!=Phase::failed,"Live group preparation failed");
    for(unsigned step=0;step<16 && result.deferral==Defer::none &&
        result.phase!=Phase::publishing && result.phase!=Phase::idle;++step) {
      auto reuse=seen;reuse.sequence=++f.clock.sequence;reuse.continued=true;
      const auto image_calls=f.image->calls,head_calls=f.head->calls;
      result=controller.advance(reuse,f.clock.now);
      require(result.phase!=Phase::failed && f.image->calls==image_calls && f.head->calls==head_calls,
          "Continued preparation changed backing storage");
      if(result.deferral==Defer::none)++continued;
    }
  }
  require(continued && controller.progress().phase==Phase::publishing,"Continuation did not reach prepared group");
  auto reuse=seen;reuse.sequence=++f.clock.sequence;reuse.continued=true;
  const auto image_calls=f.image->calls,head_calls=f.head->calls;
  require(controller.advance(reuse,f.clock.now).deferral==Defer::observation &&
      f.image->calls==image_calls && f.head->calls==head_calls,"Reused CPU capture published a group");
  reuse.sequence=++f.clock.sequence;reuse.foreground_pending=true;
  require(controller.advance(reuse,f.clock.now).deferral==Defer::foreground &&
      f.image->calls==image_calls && f.head->calls==head_calls,"Continued work ignored foreground activity");
  const auto complete=f.finish(controller);
  require(complete.published_groups==1,"Fresh observation did not publish prepared group");
  f.verify(8);
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
void retain_distant_indexed_candidate(bool projected=false) {
  constexpr auto prefix=2*kNativeMaxDirtyExtents;
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "long-range discovery advice unavailable");
  const auto initial_advice_bytes=advice->write_bytes;
  put(*f.session,"original",family(0));
  for(std::size_t i=1;i<prefix;++i)
    put(*f.session,"unrelated-"+std::to_string(i),data(kNativeExtentBytes,static_cast<unsigned>(3000+i)));
  auto distant=projected?family(0):family(1);
  if(projected) {
    const auto variation=data(kNativeExtentBytes,9121);
    for(std::size_t i=0;i<distant.size();++i)distant[i]^=variation[i]&std::byte{0x30};
  }
  put(*f.session,"distant",distant);
  put(*f.session,"sparse-funding",Bytes(kOptimizationGroupScanInputBytes/2));
  const auto original_inode=f.session->snapshot().stat("original").inode;
  {
    NativeGroupMaintenance controller(*f.session);
    for(unsigned expected=1;expected<=2;++expected) {
      for(unsigned tick=0;tick<3500&&controller.progress().checkpoints_published<expected;++tick)
        require(f.step(controller).phase!=Phase::failed,"long-range discovery failed");
      require(controller.progress().checkpoints_published==expected &&
          controller.progress().attempted_groups==0 && controller.pending(),
          "unrelated windows did not checkpoint before reaching the distant member");
      require(advice->stable.at(7)==std::byte{'5'},"checkpoint did not retain projected samples");
    }
  }
  const auto saved=read_optimization_state(*advice,id(90));
  require(saved.code==OptimizationReadCode::available && !saved.degraded && saved.state.group_scan &&
      !saved.state.group_scan->complete && saved.state.group_scan->representatives.size()==prefix &&
      saved.state.group_scan->representatives.front().inode==original_inode &&
      saved.state.group_scan->xor_sampling && saved.state.group_scan->projections.size()==prefix &&
      saved.state.group_scan->projections.front().representative==0,
      "extended checkpoint lost the early candidate");
  require(advice->extent()==kOptimizationAreaBytes &&
      advice->write_bytes-initial_advice_bytes==2*kOptimizationAreaBytes,
      "retaining more representatives increased the checkpoint footprint or write traffic");
  // A foreground edit after the checkpoint changes one earlier extent.
  const auto replacement=data(kNativeExtentBytes,9001);
  const auto handle=f.session->open("unrelated-1",{true,true,false});
  f.session->write(handle,0,replacement);f.session->cleanup(handle,false);f.session->close(handle);
  reopen_indexed(f,advice,saved.sequence);
  NativeGroupMaintenance controller(*f.session);
  for(unsigned tick=0;tick<2500 && controller.progress().published_groups==0;++tick)
    require(f.step(controller).phase!=Phase::failed,"reopened distant selection failed");
  const auto selected=controller.progress();
  require(selected.checkpoints_restored==1 && selected.published_groups==1 &&
      selected.minimum_compacted_bytes_saved>0 && selected.sampled_extents==1,
      "reopened scanner missed the distant match before revisiting the edited prefix");
  finish_pending(f,controller);
  const auto progress=controller.progress();
  require(progress.reused_features>=prefix-1 && progress.sampled_extents<prefix,
      "unrelated edit discarded the saved scan work");
  const auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().members==2 && snapshot.shared_base_info().bases==1 &&
      snapshot.read("original",0,kNativeExtentBytes)==family(0) &&
      snapshot.read("distant",0,kNativeExtentBytes)==distant &&
      snapshot.read("unrelated-1",0,kNativeExtentBytes)==replacement,
      "distant group reopened with different contents");
  snapshot.verify_all();
  std::cout<<"indexed_distant_match projected="<<projected<<" prefix_representatives="<<prefix
      <<" samples_before_match="<<selected.sampled_extents
      <<" total_resumed_samples="<<progress.sampled_extents<<" reused_features="<<progress.reused_features
      <<" minimum_compacted_bytes_saved="<<progress.minimum_compacted_bytes_saved
      <<" checkpoint_bytes="<<advice->extent()<<'\n';
}
class CatalogReadGate final:public StoreByteIo {
 public:
  explicit CatalogReadGate(std::shared_ptr<Ram> backing):backing_(std::move(backing)) {}
  bool refuse=false;
  std::uint64_t extent() const override{return backing_->extent();}
  StoreIoContract contract() const noexcept override{return backing_->contract();}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(refuse)throw std::runtime_error("injected coherence catalog read failure");
    backing_->read_exact(at,out);
  }
  void admit_write(std::uint64_t) override{throw std::runtime_error("read-only catalog fixture");}
  void write_exact(std::uint64_t,std::span<const std::byte>) override{throw std::runtime_error("read-only catalog fixture");}
  void flush() override{throw std::runtime_error("read-only catalog fixture");}
 private:
  std::shared_ptr<Ram> backing_;
};
enum class CatalogCase {available,unavailable,stale,aligned_only};
void indexed_catalog_discovery(CatalogCase scenario,bool projected=false) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "coherence discovery advice binding");
  auto backing=ram(4096);
  auto catalog=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  f.session->bind_indexed_coherence_writer(std::move(catalog));
  const auto original=family(0);
  put(*f.session,"original",original);
  const bool long_range=projected && (scenario==CatalogCase::available || scenario==CatalogCase::aligned_only);
  const auto noise_count=long_range?kOptimizationGroupRepresentatives+1:4;
  if(long_range) {
    f.session->create_entry("noise");
    const auto noise=f.session->open("noise",{true,true,false});
    for(std::size_t first=0;first<noise_count;first+=kNativeMaxDirtyExtents) {
      const auto count=(std::min)(noise_count-first,kNativeMaxDirtyExtents);
      Bytes batch(count*kNativeExtentBytes);
      for(std::size_t i=0;i<count;++i) {
        const auto bytes=data(kNativeExtentBytes,4000+static_cast<unsigned>(first+i));
        std::ranges::copy(bytes,batch.begin()+i*kNativeExtentBytes);
      }
      f.session->write(noise,first*kNativeExtentBytes,batch);
    }
    f.session->cleanup(noise,false);
    f.session->close(noise);
    // Fund the optional postings and retain a settled checkpoint after the
    // last window, even when the catalog also carries numeric projections.
    for(unsigned i=0;i<6;++i)
      put(*f.session,"funding-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  }else {
    for(unsigned i=0;i<noise_count;++i)
      put(*f.session,"noise-"+std::to_string(i),data(kNativeExtentBytes,590+i));
    put(*f.session,"funding",Bytes(kOptimizationGroupScanInputBytes-5*kNativeExtentBytes));
  }
  put(*f.session,"catalog-input",Bytes(kOptimizationGroupScanInputBytes/2));
  OptimizationGroupFeature original_feature;
  OptimizationXorSignatures original_signatures{};
  {
    NativeGroupMaintenance initial(*f.session);
    for(unsigned tick=0;tick<200 && !initial.progress().checkpoints_published;++tick)
      require(f.step(initial).phase!=Phase::failed,"catalog scanning failed");
    const auto saved=read_optimization_state(*advice,id(90));
    const auto before_generations=f.session->snapshot();
    require(saved.state.group_scan && !saved.state.group_scan->representatives.empty() &&
        saved.state.group_scan->representatives.front().inode==before_generations.stat("original").inode &&
        !saved.state.group_scan->projections.empty() && saved.state.group_scan->projections.front().representative==0,
        "archive lacked an actual native extent binding");
    original_feature=saved.state.group_scan->representatives.front();
    original_signatures=saved.state.group_scan->projections.front().signatures;
    finish_pending(f,initial);
    require(initial.progress().checkpoints_published && initial.progress().published_groups==0,
        "catalog feature source lacked a funded sample");
    if(long_range) {
      const auto latest=f.session->optimization_snapshot().state.group_scan;
      require(latest && std::ranges::find(latest->representatives,original_feature.object,
          &OptimizationGroupFeature::object)==latest->representatives.end(),
          "long-range test still retained the original in checkpoint history");
    }
  }
  constexpr auto masked_band=static_cast<std::uint32_t>(kOptimizationGroupBands)+2;
  const auto archive=IndexedCoherenceSnapshot::open(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  auto expected=original_feature;expected.bands={};
  require(archive.has_projected_keys() && archive.find(0,original_feature.bands[0])==original_feature &&
      archive.find(masked_band,original_signatures[2])==expected,
      "ordinary funded sampling did not populate both paged key families");
  if(scenario==CatalogCase::aligned_only) {
    backing=ram(4096);
    auto control=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
    const IndexedCoherencePosting legacy{0,original_feature};
    control.publish(std::span(&legacy,1),UINT64_MAX);
    require(!control.snapshot().has_projected_keys(),"aligned-only control acquired projections");
  }
  auto distant=original;
  if(projected) {
    const auto variation=data(distant.size(),9121);
    for(std::size_t i=0;i<distant.size();++i)distant[i]^=variation[i]&std::byte{0x30};
  }else distant[distant.size()-91]^=std::byte{0x31};
  put(*f.session,"distant",distant);
  const auto edited=scenario==CatalogCase::stale;
  const auto current_original=edited?data(kNativeExtentBytes,811):original;
  if(edited) {
    const auto file=f.session->open("original",{true,true,false});
    f.session->write(file,0,current_original);f.session->cleanup(file,false);f.session->close(file);
  }
  f.session.reset();
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,
      [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;},
      {StoreCompressionMode::raw,1,false}));
  auto gate=std::make_shared<CatalogReadGate>(backing);
  rejects([&]{f.session->bind_indexed_coherence(f.image,{kStoreMaxPhysicalBytes});},
      "native image accepted as optional catalog backing");
  NativeGroupMaintenance controller(*f.session);
  if(edited)finish_pending(f,controller);
  f.session->bind_indexed_coherence(gate,{kStoreMaxPhysicalBytes});
  gate->refuse=scenario==CatalogCase::unavailable;
  require(controller.pending(),"new catalog did not wake settled maintenance");
  const auto lookup_quanta=(noise_count+2)*(1+kOptimizationXorProjections+kCoherenceModularWidths);
  for(std::size_t tick=0;tick<3000+lookup_quanta && controller.pending();++tick) {
    auto observation=f.clock.next();
    // Reserve just enough modeled headroom for two ordinary members and two
    // cached representatives, forcing the original out of the live window.
    observation.physical_available=observation.physical_total/16+64*(33ULL*kNativeExtentBytes);
    const auto lookups=controller.progress().coherence_lookups;
    const auto progress=controller.advance(observation,f.clock.now);
    require(progress.phase!=Phase::failed && progress.coherence_lookups<=lookups+1,
        "optional catalog exceeded one lookup or affected data maintenance health");
  }
  const auto progress=controller.progress();
  require(!controller.pending() && !f.session->poisoned(),"indexed catalog maintenance did not settle");
  if(scenario==CatalogCase::unavailable)
    require(progress.coherence_lookups==1 && progress.coherence_failures==1 && progress.published_groups==0,
        "failed catalog lookup retried or manufactured a group");
  else if(scenario==CatalogCase::aligned_only)
    require(!progress.coherence_matches && !progress.coherence_failures && !progress.attempted_groups &&
        !progress.published_groups,"aligned-only catalog found an extent with no matching aligned key");
  else
    require(progress.coherence_matches>=1 && !progress.coherence_failures &&
        progress.published_groups==(edited?0U:1U) && progress.attempted_groups==(edited?0U:1U),
        "paged candidate was missed or an edited binding entered preparation");
  const auto view=f.session->snapshot();
  require(view.read("original",0,kNativeExtentBytes)==current_original &&
      view.read("distant",0,kNativeExtentBytes)==distant &&
      view.shared_base_info().members==(scenario==CatalogCase::available?2:0),"catalog integration changed file data");
  view.verify_all();
  if(long_range)std::cout<<"projected_catalog_discovery aligned_only="<<(scenario==CatalogCase::aligned_only)
      <<" intervening_extents="<<noise_count<<" lookups="<<progress.coherence_lookups
      <<" groups="<<progress.published_groups<<" minimum_compacted_bytes_saved="<<progress.minimum_compacted_bytes_saved
      <<" catalog_bytes="<<progress.coherence_catalog_bytes<<'\n';
}
enum class ModularCatalogCase {available,masked_only,stale};
void indexed_modular_catalog_discovery(ModularCatalogCase scenario) {
  Fixture f(4096,0,true);
  const PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  auto advice=ram(4096),backing=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "modular catalog needs a funded advisory journal");
  f.session->bind_indexed_coherence_writer(IndexedCoherenceWriter::create(backing,id(90),id(1),limits));
  const auto original=data(kNativeExtentBytes,19463);
  const auto distant=calibrated_extent(original,1);
  put(*f.session,"original",original);
  put(*f.session,"other",data(kNativeExtentBytes,9137));
  for(unsigned i=0;i<2;++i)put(*f.session,"input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  {
    NativeGroupMaintenance initial(*f.session);
    finish_pending(f,initial);
    require(initial.progress().checkpoints_published==1 && !initial.progress().published_groups,
        "initial numeric sample did not settle");
  }
  const auto checkpoint=f.session->optimization_snapshot().state.group_scan;
  require(checkpoint && checkpoint->representatives.size()==2 && checkpoint->projections.size()==2,
      "numeric source was absent from the settled scan");
  const auto& measured=checkpoint->representatives.front();
  const auto& masked=checkpoint->projections.front().signatures;
  constexpr auto modular_band=kCoherenceModularBand+2;
  const auto key=detail::native_group_delta_signatures(original)[2];
  require(key && key==detail::native_group_delta_signatures(distant)[2],"fixture lacks a shared numeric signature");
  {
    const auto archived=IndexedCoherenceSnapshot::open(backing,id(90),id(1),limits);
    auto expected=measured;
    expected.bands={};
    require(archived.has_modular_keys() && archived.find(modular_band,key)==expected,
        "automatic funded sampling did not persist the numeric signature");
  }
  if(scenario==ModularCatalogCase::masked_only) {
    backing=ram(4096);
    auto control=IndexedCoherenceWriter::create(backing,id(90),id(1),limits);
    std::vector<IndexedCoherencePosting> postings{{0,measured}};
    for(std::uint32_t i=0;i<masked.size();++i)postings.push_back({kCoherenceMaskedBand+i,measured,masked[i]});
    control.publish(postings,UINT64_MAX);
    require(control.snapshot().has_projected_keys() && !control.snapshot().has_modular_keys(),
        "control did not preserve the preceding catalog schema");
  }
  constexpr unsigned intervening=8;
  for(unsigned i=0;i<intervening;++i)
    put(*f.session,"noise-"+std::to_string(i),data(kNativeExtentBytes,4201+i));
  put(*f.session,"distant",distant);
  const auto current=scenario==ModularCatalogCase::stale?data(kNativeExtentBytes,52943):original;
  if(scenario==ModularCatalogCase::stale) {
    const auto file=f.session->open("original",{true,true,false});
    f.session->write(file,0,current);
    f.session->cleanup(file,false);
    f.session->close(file);
  }
  const auto resolve=[&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  const auto compacted_bytes=[&](unsigned identity) {
    const auto closed=NativeVolumeHeadView::open(f.head,id(90),resolve);
    auto output=ram(4096);
    const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
    (void)NativeFilesystemWriter::compact(closed.filesystem(),output,id(identity),raw);
    auto verified=NativeFilesystemSnapshot::open_image(output,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verified.verify_all();
    require(verified.read("original",0,kNativeExtentBytes)==current &&
        verified.read("distant",0,kNativeExtentBytes)==distant,"numeric catalog trial changed a file");
    return output->extent();
  };
  f.session.reset();
  const auto before=compacted_bytes(2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,
      {StoreCompressionMode::raw,1,false}));
  f.session->bind_indexed_coherence(backing,limits);
  NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.pending();++i) {
    auto observed=f.clock.next();
    observed.physical_available=observed.physical_total/16+64*(33ULL*kNativeExtentBytes);
    const auto prior=controller.progress().coherence_lookups;
    const auto next=controller.advance(observed,f.clock.now);
    require(next.phase!=Phase::failed && next.coherence_lookups<=prior+1,
        "numeric catalog exceeded the admitted lookup quantum");
  }
  const auto progress=controller.progress();
  const bool grouped=scenario==ModularCatalogCase::available;
  require(!controller.pending() && !f.session->poisoned() && !progress.coherence_failures &&
      progress.published_groups==(grouped?1U:0U) && progress.attempted_groups==(grouped?1U:0U) &&
      (scenario==ModularCatalogCase::masked_only?!progress.coherence_matches:progress.coherence_matches>0),
      "numeric catalog missed a live nomination or used a stale binding");
  controller.cancel();
  f.session.reset();
  const auto after=compacted_bytes(3);
  const auto overhead=f.head->extent()+advice->extent()+backing->extent();
  require(grouped?after<before:after==before,"numeric catalog compacted size changed unexpectedly");
  std::cout<<"modular_catalog_discovery scenario="<<static_cast<unsigned>(scenario)
      <<" intervening_extents="<<intervening<<" lookups="<<progress.coherence_lookups
      <<" groups="<<progress.published_groups<<" catalog_bytes="<<backing->extent()
      <<" complete_before="<<before+overhead<<" complete_after="<<after+overhead<<" byte_exact=true\n";
}
enum class CatalogFunding {available,checkpoint_only,journal_failure,catalog_failure,earlier_keys_only};
void funded_indexed_catalog(CatalogFunding outcome) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "funded catalog needs a work journal");
  auto backing=ram(4096);
  auto catalog=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  const auto initial_end=catalog.snapshot().info().physical_end;
  f.session->bind_indexed_coherence_writer(std::move(catalog));

  const auto first=data(kNativeExtentBytes,117),second=data(kNativeExtentBytes,846);
  put(*f.session,"first",first);put(*f.session,"second",second);
  put(*f.session,"input-a",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
  if(outcome!=CatalogFunding::checkpoint_only)
    put(*f.session,"input-b",Bytes(kOptimizationGroupScanInputBytes/2));
  const auto before=read_optimization_state(*advice,id(90));
  const auto catalog_writes=backing->write_bytes;
  const auto catalog_calls=backing->calls;
  if(outcome==CatalogFunding::journal_failure)
    advice->fail_at=advice->calls+2; // The first paid slot survives, then the peer fails.
  if(outcome==CatalogFunding::catalog_failure)backing->fail_at=backing->calls;

  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    require(controller.progress().checkpoints_published==(outcome==CatalogFunding::journal_failure?0U:1U),
        "catalog reservation confused an acknowledged checkpoint with a failed one");
    const auto reads=backing->read_calls,calls=backing->calls;
    for(unsigned tick=0;tick<4;++tick)f.step(controller);
    require(backing->calls==calls && backing->read_calls==reads,
        "unchanged workload retried catalog maintenance");
  }
  const auto settled=read_optimization_state(*image(advice->stable,4096),id(90));
  const auto view=IndexedCoherenceSnapshot::open(image(backing->stable,4096),id(90),id(1),{kStoreMaxPhysicalBytes});
  require(settled.code==OptimizationReadCode::available && settled.state.group_scan &&
      settled.sequence==before.sequence+1,"funded catalog lost the checked settlement");
  const auto recorded=*settled.state.settled_input_bytes;
  if(outcome==CatalogFunding::available) {
    const auto spent=backing->write_bytes-catalog_writes;
    require(view.info().objects==1+2*(1+kOptimizationXorProjections+kCoherenceModularWidths) &&
        view.has_modular_keys() && view.info().physical_end>initial_end && spent>0 &&
        recorded>=kOptimizationGroupScanInputBytes+spent*kOptimizationGroupScanInputPerWriteByte,
        "automatic feature publication escaped its durable write charge");
    for(const auto& feature:settled.state.group_scan->representatives)
      require(view.find(0,feature.bands[0])==feature,"catalog lost its sampled representative");
  } else if(outcome==CatalogFunding::checkpoint_only) {
    require(recorded==kOptimizationGroupScanInputBytes && backing->calls==catalog_calls &&
        view.info().objects==1,"checkpoint-only credit permitted catalog writes");
  } else if(outcome==CatalogFunding::journal_failure) {
    require(recorded>kOptimizationGroupScanInputBytes && settled.degraded &&
        backing->calls==catalog_calls && view.info().objects==1 &&
        f.session->optimization_info().phase==NativeOptimizationPhase::terminal,
        "interrupted settlement released catalog writes");
  } else {
    require(recorded>kOptimizationGroupScanInputBytes && backing->calls==catalog_calls+1 &&
        view.info().objects==1 && f.session->optimization_info().phase==NativeOptimizationPhase::ready,
        "a catalog failure discarded the paid checkpoint or retried");
    // Reopening the last valid catalog and charged journal cannot spend the
    // acknowledged input again, even though the optional output was lost.
    reopen_indexed(f,advice,settled.sequence);
    auto recovered=IndexedCoherenceWriter::resume(backing,id(90),id(1),view.info().generation,{kStoreMaxPhysicalBytes});
    f.session->bind_indexed_coherence_writer(std::move(recovered));
    const auto calls=backing->calls;
    NativeGroupMaintenance resumed(*f.session);
    finish_pending(f,resumed);
    require(backing->calls==calls &&
        f.session->optimization_snapshot().state.settled_input_bytes==recorded,
        "catalog restart minted previously charged input");
  }
  const auto user=f.session->snapshot();
  require(!f.session->poisoned() && user.read("first",0,kNativeExtentBytes)==first &&
      user.read("second",0,kNativeExtentBytes)==second,"optional catalog altered acknowledged files");
  user.verify_all();
}
void automatic_indexed_catalog(CatalogFunding outcome) {
  Fixture f(4096,0,true);
  auto advice=ram(4096),backing=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "automatic catalog requires the funded advisory journal");
  unsigned reservations=0;
  const PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  auto storage=std::make_shared<IndexedCoherenceStorage>(limits,[&] {
    ++reservations;
    return backing;
  });
  f.session->configure_indexed_coherence(storage);
  require(!reservations && !backing->extent(),"session attachment allocated an empty catalog");

  const auto first=data(kNativeExtentBytes,214),second=data(kNativeExtentBytes,624);
  put(*f.session,"first",first);put(*f.session,"second",second);
  put(*f.session,"base-input",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
  const bool earlier_only=outcome==CatalogFunding::earlier_keys_only;
  if(earlier_only) {
    // The grant covers the primary and masked rows plus the root writes.
    // The numeric rows require one further 4 KiB page in this model.
    constexpr auto input=37*4096*kOptimizationGroupScanInputPerWriteByte;
    put(*f.session,"more-input-a",Bytes(input/2));
    put(*f.session,"more-input-b",Bytes(input/2));
  }else if(outcome!=CatalogFunding::checkpoint_only) {
    put(*f.session,"more-input-a",Bytes(3*kOptimizationGroupScanInputBytes/4));
    put(*f.session,"more-input-b",Bytes(3*kOptimizationGroupScanInputBytes/4));
  }
  if(outcome==CatalogFunding::journal_failure)advice->fail_at=advice->calls+2;
  if(outcome==CatalogFunding::catalog_failure)backing->fail_at=0;
  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    require(controller.progress().checkpoints_published==(outcome==CatalogFunding::journal_failure?0U:1U),
        "catalog creation changed checkpoint acknowledgment");
  }

  const auto journal=read_optimization_state(*image(advice->stable,4096),id(90));
  require(journal.code==OptimizationReadCode::available && journal.state.group_scan,
      "catalog creation left no recoverable scan checkpoint");
  const auto settled=*journal.state.settled_input_bytes;
  if(outcome==CatalogFunding::checkpoint_only) {
    require(!reservations && !backing->calls && settled==kOptimizationGroupScanInputBytes,
        "checkpoint-only credit allocated or wrote a catalog");
  }else {
    require(reservations==1 && settled>kOptimizationGroupScanInputBytes,
        "a funded catalog creation failed to reserve exactly once");
    if(outcome==CatalogFunding::available || earlier_only) {
      const auto snapshot=IndexedCoherenceSnapshot::open(backing,id(90),id(1),limits);
      const auto projected=kOptimizationXorProjections+(earlier_only?0:kCoherenceModularWidths);
      require(snapshot.info().objects==1+2*(1+projected) &&
          snapshot.has_projected_keys() && snapshot.has_modular_keys()!=earlier_only && snapshot.info().generation==2 &&
          settled==kOptimizationGroupScanInputBytes+backing->write_bytes*kOptimizationGroupScanInputPerWriteByte,
          "initial catalog and postings escaped one complete charged checkpoint");
      if(earlier_only)std::cout<<"modular_catalog_tight_grant catalog_bytes="<<snapshot.info().physical_end
          <<" charged_catalog_writes="<<backing->write_bytes<<" reservations="<<reservations<<'\n';
      for(const auto& feature:journal.state.group_scan->representatives)
        require(snapshot.find(0,feature.bands[0])==feature,"initial catalog lost a real feature");
      reopen_indexed(f,advice,journal.sequence);
      const auto mutations=backing->calls;
      f.session->configure_indexed_coherence(storage);
      NativeGroupMaintenance resumed(*f.session);
      finish_pending(f,resumed);
      require(reservations==1 && backing->calls==mutations &&
          f.session->optimization_snapshot().state.settled_input_bytes==settled,
          "retained catalog was reprovisioned or spent credit after reopen");
    } else if(outcome==CatalogFunding::journal_failure) {
      require(!backing->calls && !backing->extent() && journal.degraded,
          "unverified initial settlement exposed catalog writes");
    } else {
      require(backing->calls==1 && !backing->extent() && !journal.degraded,
          "an interrupted initial catalog write invalidated or replayed its paid journal");
      reopen_indexed(f,advice,journal.sequence);
      f.session->configure_indexed_coherence(storage);
      NativeGroupMaintenance resumed(*f.session);
      finish_pending(f,resumed);
      require(reservations==1 && backing->calls==1 &&
          f.session->optimization_snapshot().state.settled_input_bytes==settled,
          "an interrupted initial catalog retried on unchanged evidence");
    }
  }
  const auto user=f.session->snapshot();
  require(!f.session->poisoned() && user.read("first",0,kNativeExtentBytes)==first &&
      user.read("second",0,kNativeExtentBytes)==second,"automatic catalog creation lost acknowledged data");
  user.verify_all();
}
enum class CoherenceCopyOutcome {completed,charge_cut,target_cut};
std::size_t compact_full_indexed_catalog(CoherenceCopyOutcome outcome,std::size_t target_cut=0) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "catalog copy needs funded advice");
  // The backing holds exactly one incremental page rewrite after creation.
  // This is fixture pressure; Windows uses a headroom-derived layout.
  const PagedStoreLimits limits{39*4096};
  std::vector<std::shared_ptr<Ram>> backings;
  auto owner=std::make_shared<IndexedCoherenceStorage>(limits,[&]() -> std::shared_ptr<StoreByteIo> {
    if(backings.size()==2)throw std::runtime_error("unexpected catalog reservation");
    return backings.emplace_back(ram(4096));
  });
  f.session->configure_indexed_coherence(owner);
  const auto stable=data(kNativeExtentBytes,1207);
  const auto old=data(kNativeExtentBytes,373);
  put(*f.session,"keep",stable);
  put(*f.session,"edited",old);
  unsigned funding=0;
  const auto earn=[&](unsigned count) {
    for(unsigned i=0;i<count;++i)
      put(*f.session,"credit-"+std::to_string(funding++),Bytes(3*kOptimizationGroupScanInputBytes/4));
  };
  const auto edit=[&](const Bytes& bytes) {
    const auto opened=f.session->open("edited",{true,true,false});
    f.session->write(opened,0,bytes);f.session->cleanup(opened,false);f.session->close(opened);
  };
  earn(3);
  NativeGroupMaintenance controller(*f.session);
  finish_pending(f,controller);
  require(backings.size()==1 && controller.progress().coherence_catalog_bytes>0,"fixture failed to create initial catalog");
  const auto data_view=f.session->snapshot();
  const auto first_scan=*f.session->optimization_snapshot().state.group_scan;
  const auto find_feature=[&](std::string_view name) {
    const auto inode=data_view.stat(name).inode;
    const auto found=std::ranges::find(first_scan.representatives,inode,&OptimizationGroupFeature::inode);
    if(found==first_scan.representatives.end())throw std::runtime_error("fixture lost a sampled file");
    return *found;
  };
  const auto keep=find_feature("keep"),stale=find_feature("edited");
  const auto projected_key=[&](const OptimizationGroupFeature& feature) {
    const auto row=std::ranges::find(first_scan.representatives,feature.object,&OptimizationGroupFeature::object)-
        first_scan.representatives.begin();
    const auto projection=std::ranges::find(first_scan.projections,row,&OptimizationGroupProjection::representative);
    if(projection==first_scan.projections.end())throw std::runtime_error("fixture lost its masked signature");
    return projection->signatures[2];
  };
  constexpr auto masked_band=static_cast<std::uint32_t>(kOptimizationGroupBands)+2;
  const auto keep_key=projected_key(keep),stale_key=projected_key(stale);
  auto projected_keep=keep;projected_keep.bands={};
  const auto active=backings.front();
  edit(data(kNativeExtentBytes,57012));
  earn(2);
  finish_pending(f,controller);
  const auto full=active->extent();
  const auto before=f.session->optimization_snapshot();
  require(backings.size()==1 && full==limits.maximum_physical_bytes,"fixture did not fill the selected catalog");

  const auto latest=data(kNativeExtentBytes,27992);
  edit(latest);
  earn(1);
  const auto old_writes=active->calls;
  finish_pending(f,controller);
  require(backings.size()==2 && !backings.back()->calls && !backings.back()->extent() &&
      f.session->optimization_snapshot().sequence==before.sequence,
      "blocked copy spent its charge or wrote before sufficient credit");
  const auto candidate=backings.back();
  const auto probe_reads=active->read_calls+candidate->read_calls;
  for(unsigned i=0;i<10;++i)f.step(controller);
  require(!controller.pending() && active->read_calls+candidate->read_calls==probe_reads &&
      active->calls==old_writes && !candidate->calls,"credit wait polled the old catalog or replacement");

  if(outcome==CoherenceCopyOutcome::charge_cut)advice->fail_at=advice->calls+2;
  if(outcome==CoherenceCopyOutcome::target_cut)candidate->fail_at=target_cut;
  const auto unrelated=data(kNativeExtentBytes,69923);
  put(*f.session,"other",unrelated);
  earn(2); // While the planner pins the previous, immutable file checkpoint.
  finish_pending(f,controller);
  const auto progress=controller.progress();
  const auto copy_calls=candidate->calls;
  require(backings.size()==2 && active->calls==old_writes,"compaction modified the old catalog or restarted on unrelated input");
  const auto original=IndexedCoherenceSnapshot::open(active,id(90),id(1),limits);
  constexpr auto numeric_band=kCoherenceModularBand+2;
  const auto keep_numeric=detail::native_group_delta_signatures(stable)[2];
  const auto stale_numeric=detail::native_group_delta_signatures(old)[2];
  require(original.find(0,keep.bands[0])==keep && original.find(0,stale.bands[0])==stale,
      "compaction altered the existing catalog's live or old bindings");
  require(original.find(numeric_band,keep_numeric)==projected_keep && original.find(numeric_band,stale_numeric),
      "compaction changed the original numeric catalog");
  const auto settled=read_optimization_state(*image(advice->stable,4096),id(90));
  require(settled.code==OptimizationReadCode::available &&
      *settled.state.settled_input_bytes>*before.state.settled_input_bytes,"copy did not settle its write budget");
  if(outcome==CoherenceCopyOutcome::completed) {
    const auto replacement=IndexedCoherenceSnapshot::open(candidate,id(90),id(1),limits);
    require(progress.coherence_compactions==1 && !progress.coherence_discards &&
        progress.coherence_compacted_bytes_saved==full-replacement.info().physical_end &&
        replacement.info().objects==2+kOptimizationXorProjections+kCoherenceModularWidths &&
        replacement.find(0,keep.bands[0])==keep && replacement.find(numeric_band,keep_numeric)==projected_keep &&
        !replacement.find(numeric_band,stale_numeric) &&
        replacement.find(masked_band,keep_key)==projected_keep && !replacement.find(0,stale.bands[0]) &&
        !replacement.find(masked_band,stale_key),"compaction kept stale postings or lost a current representative");
    require(*settled.state.settled_input_bytes-*before.state.settled_input_bytes==
        kOptimizationGroupScanInputBytes+candidate->write_bytes*kOptimizationGroupScanInputPerWriteByte,
        "candidate grew beyond the durable write charge");
    put(*f.session,"after-copy",data(kNativeExtentBytes,40552));
    earn(2);
    finish_pending(f,controller);
    const auto newer=IndexedCoherenceSnapshot::open(candidate,id(90),id(1),limits);
    require(newer.info().generation>replacement.info().generation &&
        newer.find(0,keep.bands[0])==keep,"the compacted catalog did not admit a new write");
  }else {
    require(!progress.coherence_compactions && progress.coherence_discards==1 &&
        progress.coherence_catalog_bytes==full,"interrupted copy displaced the checked old catalog");
    require(outcome==CoherenceCopyOutcome::charge_cut?(!candidate->calls && settled.degraded):
        (candidate->calls==target_cut+1 && (candidate->extent()!=0)==(target_cut!=0) && !settled.degraded),
        "catalog started before the verified charge or retried a failed write");
  }
  const auto view=f.session->snapshot();
  require(!f.session->poisoned() && view.read("keep",0,kNativeExtentBytes)==stable &&
      view.read("edited",0,kNativeExtentBytes)==latest && view.read("other",0,kNativeExtentBytes)==unrelated,
      "catalog copy or recovery affected acknowledged file bytes");
  view.verify_all();
  return copy_calls;
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
    persistent_indexed_scan();
    retain_distant_indexed_candidate();
    retain_distant_indexed_candidate(true);
    indexed_scan_checkpoint_faults();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-projected-coherence") {
    for(const auto scenario:{ModularCatalogCase::available,ModularCatalogCase::masked_only,ModularCatalogCase::stale})
      indexed_modular_catalog_discovery(scenario);
    for(const auto scenario:{CatalogCase::available,CatalogCase::aligned_only,CatalogCase::unavailable,CatalogCase::stale})
      indexed_catalog_discovery(scenario,true);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-coherence") {
    for(const auto scenario:{CatalogCase::available,CatalogCase::unavailable,CatalogCase::stale})
      indexed_catalog_discovery(scenario);
    for(const auto outcome:{CatalogFunding::available,CatalogFunding::checkpoint_only,
        CatalogFunding::journal_failure,CatalogFunding::catalog_failure}) {
      funded_indexed_catalog(outcome);
      automatic_indexed_catalog(outcome);
    }
    automatic_indexed_catalog(CatalogFunding::earlier_keys_only);
    const auto writes_and_barriers=compact_full_indexed_catalog(CoherenceCopyOutcome::completed);
    require(writes_and_barriers>1,"completed catalog copy never reached the final barrier");
    compact_full_indexed_catalog(CoherenceCopyOutcome::charge_cut);
    compact_full_indexed_catalog(CoherenceCopyOutcome::target_cut);
    compact_full_indexed_catalog(CoherenceCopyOutcome::target_cut,writes_and_barriers-1);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-xor") {
    xor_indexed_family(true);
    xor_indexed_family(true,true);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_tile_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_sparse_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_full_reference);
    xor_reference_decline_fallback();
    indexed_group_with_outlier();
  } else if(argc==2 && std::string_view(argv[1])=="--profile-projected-xor") {
    xor_indexed_family(false,true);
  } else if(argc==2 && std::string_view(argv[1])=="--profile-group-xor") {
    xor_indexed_family(false);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-modular-delta") {
    modular_delta_indexed_family(data(kNativeExtentBytes,20561),{StoreCompressionMode::raw,1,false},true);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-modular-delta") {
    auto input=ReadOnlyFile::open(argv[2]);
    require(input.size()==kNativeExtentBytes,"expected one extent of public source data");
    Bytes bytes(kNativeExtentBytes);
    input.read_exact_at(0,bytes.data(),bytes.size());
    modular_delta_indexed_family(bytes,{StoreCompressionMode::automatic,6,true},false);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-advice") {
    advised_indexed_maintenance();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batch") {
    batched_indexed_maintenance(BatchOutcome::accepted);
    batched_indexed_maintenance(BatchOutcome::cpu_fallback);
    batched_indexed_maintenance(BatchOutcome::changed_source);
    batched_indexed_maintenance(BatchOutcome::memory_pressure);
    batched_indexed_maintenance(BatchOutcome::overlap_success);
    batched_indexed_maintenance(BatchOutcome::overlap_fallback);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-multiwindow-batch") {
    indexed_batch_across_scan_windows();
    indexed_partial_overlap_preserves_candidates();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batch-capacity") {
    indexed_batch_reclaims_append_space();
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
    admission();continued_preparation_admission();negative_cache_and_incompressible();ownership_and_pressure();
    foreground_change_during_preparation();
  } else throw std::invalid_argument("unknown group maintenance test mode");
  std::cout<<"PASS: "<<checks<<" automatic group maintenance checks; "<<faults<<" injected publication cases; model resource observations; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
