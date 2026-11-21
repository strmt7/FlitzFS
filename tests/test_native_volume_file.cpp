#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/read_only_file.hpp"
#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
#include "volume_range_fixture.hpp"
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <chrono>
#include <iostream>
#include <string_view>

namespace {
using namespace flitzfs;
using namespace native_fixture;
constexpr PagedStoreLimits limits{8ULL*1024*1024};
constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
unsigned checks=0;
StoreIdentity id(unsigned value){auto result=identity();result[0]=static_cast<std::byte>(value);return result;}
void require(bool test,const char* message){++checks;if(!test)throw std::runtime_error(message);}
template<class Action>void rejects(Action&& action,const char* message) {
  ++checks;
  try{action();}catch(const std::exception&){return;}
  throw std::runtime_error(message);
}
std::shared_ptr<Ram> empty(std::uint32_t unit) {
  auto io=std::make_shared<Ram>();
  io->failure_unit=unit;
  return io;
}
void put(NativeFileSession& session,std::string_view path,const Bytes& bytes) {
  session.create_entry(path);
  const auto handle=session.open(path,{true,true,false});
  session.write(handle,0,bytes);
  session.cleanup(handle,false);session.close(handle);
}
void update(NativeFileSession& session,const Bytes& bytes) {
  const auto handle=session.open("payload",{true,true,false});
  session.write(handle,0,bytes);
  session.cleanup(handle,false);session.close(handle);
}
void cold_relocation(const Bytes& payload,bool require_reduction) {
  if(!store_zstd_available())return;
  const StoreCompressionOptions foreground=require_reduction?
      StoreCompressionOptions{StoreCompressionMode::fast,3,false}:
      StoreCompressionOptions{StoreCompressionMode::automatic,3,true};
  const auto noise=data(2*kNativeExtentBytes,102019);
  struct Result {std::uint64_t image,stored,read_bytes,written;};
  const auto trial=[&](PagedRewriteMode mode,bool late_edit) {
    auto head=empty(4096),input=empty(4096),target=empty(4096);
    auto expected=payload;
    const auto resolver=[&](const StoreIdentity& requested)->std::shared_ptr<StoreByteIo> {
      return requested==id(71)?input:requested==id(72)?target:nullptr;
    };
    std::uint64_t before=0;
    {
      auto session=NativeFileSession::create_indexed_volume(head,id(70),input,id(71),limits,foreground);
      put(session,"payload",payload);
      put(session,"noise",noise);
      before=input->extent();
      require(session.begin_idle_compaction(target,id(72),mode),"cold copy admission");
      const auto advance=[&] {
        for(unsigned step=0;step<4000;++step) {
          const auto progress=session.advance_recompression();
          if(progress==NativeFileSession::RecompressionProgress::ready)return;
          require(progress==NativeFileSession::RecompressionProgress::copying,"staged cold copy became stale");
        }
        throw std::runtime_error("cold copy did not settle");
      };
      advance();
      if(late_edit) {
        const auto file=session.open("payload",{true,true,false});
        expected[0]^=std::byte{0x42};
        session.write(file,0,{expected.data(),1});
        session.cleanup(file,false);
        session.close(file);
        rejects([&]{(void)session.publish_idle_recompression();},"stale cold candidate published without replay");
        advance();
      }
      require(session.publish_idle_recompression().has_value(),"cold candidate did not publish");
      if(!late_edit)require(input->extent()==before,"cold copy changed its source backing");
    }
    const auto recovered=NativeVolumeHeadView::open(head,id(70),resolver);
    const auto& view=recovered.filesystem();
    view.verify_all();
    const auto reads=target->read_bytes;
    require(view.read("payload",0,static_cast<std::uint32_t>(expected.size()))==expected &&
        view.read("noise",0,static_cast<std::uint32_t>(noise.size()))==noise,"CPU cold relocation changed acknowledged bytes");
    return Result{target->extent(),view.storage_info().stored_payload_bytes,target->read_bytes-reads,target->write_bytes};
  };
  const auto copy=trial(PagedRewriteMode::exact_checkpoint,false);
  const auto cold=trial(PagedRewriteMode::reconcile_foreground,false);
  require(cold.image<=copy.image && cold.stored<=copy.stored,"optional encoding grew a compacted image");
  if(require_reduction) {
    require(cold.image<copy.image && cold.stored<copy.stored,"cold relocation did not reduce the model image");
    (void)trial(PagedRewriteMode::reconcile_foreground,true);
  }
  std::cout<<"cold_relocation payload="<<payload.size()<<" copy_image="<<copy.image<<" cold_image="<<cold.image
      <<" copy_stored="<<copy.stored<<" cold_stored="<<cold.stored
      <<" copy_reads="<<copy.read_bytes<<" cold_reads="<<cold.read_bytes
      <<" copy_writes="<<copy.written<<" cold_writes="<<cold.written
      <<" independent_cpu_readback=1 replay_checked="<<(require_reduction?1:0)<<'\n';
}
Bytes acknowledged(std::uint32_t unit,const Bytes& original,const Bytes& witness) {
  auto io=empty(unit);
  {
    auto session=NativeVolumeFile::create(io,id(70),id(71),limits,raw);
    put(session,"payload",original);
    put(session,"witness",witness);
    session.flush();
  }
  return io->stable;
}
void retired_and_reopened() {
  auto io=empty(4096);
  const auto first=data(8831,17),later=data(first.size(),38),orphan=data(625,47);
  NativeFilesystemSnapshot retained=[&] {
    auto session=NativeVolumeFile::create(io,id(70),id(71),limits,raw);
    require(session.optimization_info().phase==NativeOptimizationPhase::ready,
        "volume file did not create its funded advice pair");
    put(session,"payload",first);
    const auto reads=io->read_calls,writes=io->calls;
    rejects([&]{(void)NativeVolumeFile::resume(io);},"another writer leased the same volume file");
    require(io->read_calls==reads && io->calls==writes,"competing lease reached backing I/O");
    return session.snapshot();
  }();
  rejects([&]{(void)NativeVolumeFile::resume(io);},"retained writer snapshot released its backing owner");
  require(retained.read("payload",0,static_cast<std::uint32_t>(first.size()))==first,
      "old session snapshot lost its file bytes");
  // Model a process restart from a separately recovered file stream.
  auto reopened=image(io->stable,4096);
  std::uint64_t orphan_inode=0;
  {
    auto user=NativeVolumeFile::resume(reopened,id(70),raw);
    require(user.workload_info().committed_input_bytes==first.size() &&
        user.optimization_info().phase==NativeOptimizationPhase::ready,"reopen lost the native input/advice state");
    update(user,later);
    user.create_entry("removed");
    auto handle=user.open("removed",{true,true,true});
    user.write(handle,0,orphan);
    orphan_inode=user.info(handle).info.inode;
    user.set_delete(handle,true);user.cleanup(handle,true);
    require(user.snapshot().needs_session_recovery(),"closed-session recovery fixture did not retain an orphan");
  }

  auto after=image(reopened->stable,4096);
  const auto writes=after->calls;
  {
    const auto reader=NativeVolumeFile::inspect(after);
    require(reader.info().selection.volume_identity==id(70) && reader.info().selection.image_identity==id(71) &&
        reader.info().selection.storage.maximum_physical_bytes==limits.maximum_physical_bytes &&
        reader.filesystem().read("payload",0,10000)==later && reader.filesystem().needs_session_recovery() &&
        reader.filesystem().read_inode(orphan_inode,0,1000)==orphan,"CPU inspection did not bind the durable selected image");
    require(after->calls==writes,"read-only inspection performed recovery writes");
  }
  {
    auto user=NativeVolumeFile::resume(after,{},raw);
    require(!user.snapshot().needs_session_recovery() && user.snapshot().read("payload",0,10000)==later,
        "reopen did not complete retired file lifetimes");
    rejects([&]{(void)user.snapshot().stat("removed");},"reopen revived a durably deleted name");
  }
  require(!NativeVolumeFile::inspect(image(after->stable,4096)).filesystem().needs_session_recovery(),
      "retired lifetime recovery was not durable");
}

void geometry_and_advice() {
  const auto payload=data(3588,16),witness=data(337,33);
  const auto checked=acknowledged(4096,payload,witness);
  auto io=image(checked,4096);
  rejects([&]{(void)NativeVolumeFile::create(io,id(90),id(91),limits,raw);},
      "creation accepted nonempty backing");
  rejects([&]{(void)NativeVolumeFile::resume(io,id(69),raw);},"writable reopen accepted a foreign volume identity");
  require(!io->calls && io->stable==checked,"rejected creation or identity modified acknowledged data");
  {auto wrong=image(checked,4096);wrong->failure_unit=0;
    rejects([&]{(void)NativeVolumeFile::resume(wrong);},"unknown durability contract admitted a writer");
    require(!wrong->calls && NativeVolumeFile::inspect(wrong).filesystem().read("payload",0,4000)==payload,
        "invalid write contract affected independent reads");}

  auto bad_head=image(checked,4096);
  std::fill_n(bad_head->live.begin(),kStoreCommitUnit,std::byte{});
  std::fill_n(bad_head->live.begin()+kVolumeHeadBytes,kOptimizationAreaBytes,std::byte{});
  bad_head->stable=bad_head->live;
  {
    const auto read=NativeVolumeFile::inspect(bad_head);
    require(read.info().degraded && read.filesystem().read("payload",0,4000)==payload,
        "one damaged head and unavailable advice lost CPU data");
  }
  {
    auto mounted=NativeVolumeFile::resume(bad_head,{},raw);
    require(!mounted.volume_info().degraded &&
        mounted.optimization_info().phase==NativeOptimizationPhase::unbound &&
        mounted.snapshot().read("witness",0,1000)==witness,"head repair read or replaced damaged advice");
  }
  require(!NativeVolumeFile::inspect(image(bad_head->stable,4096)).info().degraded,"repaired head did not survive reopen");

  auto other=empty(4096);
  {auto session=NativeVolumeFile::create(other,id(72),id(73),limits,raw);}
  auto inconsistent=image(checked,4096);
  std::copy_n(other->stable.begin()+kStoreCommitUnit,kStoreCommitUnit,inconsistent->live.begin()+kStoreCommitUnit);
  rejects([&]{(void)NativeVolumeFile::inspect(inconsistent);},"bootstrap accepted two intact foreign peers");
  require(!inconsistent->calls,"bootstrap wrote inconsistent metadata");

  auto truncated=checked;truncated.resize(kNativeVolumeFileImageOffset);
  rejects([&]{(void)NativeVolumeFile::inspect(image(truncated,4096));},"truncated volume image was exposed");
  auto oversized=checked;oversized.resize(kNativeVolumeFileImageOffset+limits.maximum_physical_bytes+1);
  rejects([&]{(void)NativeVolumeFile::inspect(image(oversized,4096));},"image beyond the declared capacity was exposed");
}
void publication_cuts() {
  const auto original=data(4811,374),changed=data(original.size(),806),witness=data(377,932);
  for(const auto unit:{4096U,65536U}) {
    const auto baseline=acknowledged(unit,original,witness);
    std::size_t cuts=0;
    {
      auto probe=image(baseline,unit);
      auto session=NativeVolumeFile::resume(probe,{},raw);
      const auto before=probe->calls;
      update(session,changed);
      cuts=probe->calls-before;
      require(NativeVolumeFile::inspect(image(probe->stable,unit)).filesystem().read("payload",0,6000)==changed,
          "acknowledged update did not survive a real reopen of the model");
    }
    require(cuts>4 && cuts<=320,"unbounded volume file cut matrix");
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
      auto torn=image(baseline,unit);
      {
        auto session=NativeVolumeFile::resume(torn,{},raw);
        torn->failure=mode;torn->fail_at=torn->calls+cut;
        rejects([&]{update(session,changed);},"injected file update cut was acknowledged");
      }
      const auto recovered=NativeVolumeFile::inspect(image(torn->stable,unit));
      const auto actual=recovered.filesystem().read("payload",0,6000);
      require(actual==original || actual==changed,"interruption exposed mixed payload bytes");
      require(recovered.filesystem().read("witness",0,1000)==witness && !recovered.info().degraded &&
          std::equal(torn->stable.begin(),torn->stable.begin()+kNativeVolumeFileImageOffset,baseline.begin()),
          "image write damaged adjacent metadata or a previously acknowledged object");
    }
    std::cout<<"volume_file_cuts unit="<<unit<<" transaction_boundaries="<<cuts<<" modes=3\n";
  }
}
using Reclaim=NativeVolumeReclaimProgress;
const auto witness_bytes=data(613,678);
Bytes establish_history(NativeFileSession& session) {
  auto latest=data(12289,81);
  put(session,"payload",latest);put(session,"witness",witness_bytes);
  for(unsigned i=0;i<28 && !session.persistent_reclamation_pending();++i) {
    latest=data(latest.size(),140+i);
    update(session,latest);
  }
  require(session.persistent_reclamation_pending(),"persistent history never admitted a smaller image");
  return latest;
}

Reclaim stage_compaction(NativeFileSession& session,unsigned next=93) {
  for(unsigned steps=0;steps<900;++steps) {
    const auto step=session.advance_persistent_reclamation([next]{return id(next);});
    if(step!=Reclaim::copying)return step;
  }
  throw std::runtime_error("persistent compaction never reached publication");
}

void release_inactive(NativeFileSession& session) {
  for(unsigned steps=0;session.persistent_reclamation_pending();++steps) {
    require(steps<5,"unbounded retirement steps");
    const auto step=session.advance_persistent_reclamation({});
    if(step==Reclaim::idle)break;
    require(step==Reclaim::retiring,"inactive range did not retire");
  }
}

void durable_cold_coverage() {
  if(!store_zstd_available())return;
  const StoreCompressionOptions foreground{StoreCompressionMode::fast,3,false};
  const auto backing=std::make_shared<SparseVolumeRam>(4096);
  // Keep per-extent redundancy while giving each extent its own vocabulary.
  // The real scan should save its cursor without grouping the coverage inputs.
  const auto payload=cold_test_payload(0,7717);
  auto late=cold_test_payload(99173);
  late.resize(kNativeExtentBytes);
  const auto witness=data(2*kNativeExtentBytes,811);
  const auto advice_bytes=[](const SparseVolumeRam& io) {
    return std::span(io.io.stable).subspan(kVolumeHeadBytes,kOptimizationAreaBytes);
  };
  Bytes prior;
  OptimizationColdCoverage saved;
  {
    auto session=NativeVolumeFile::create(backing,id(70),id(71),limits,foreground);
    put(session,"payload",data(payload.size(),799));
    update(session,payload);
    put(session,"witness",witness);
    put(session,"quiet",Bytes(kOptimizationGroupScanInputBytes/2));
    const auto previous=advice_bytes(*backing);
    prior.assign(previous.begin(),previous.end());
    const auto captured=session.snapshot().storage_info().generation;
    require(stage_compaction(session,72)==Reclaim::ready,"initial cold copy did not stage");
    put(session,"late",late);
    const auto replayed=session.snapshot().storage_info().generation;
    require(replayed>captured && stage_compaction(session,72)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,"cold copy did not replay and publish");
    const auto selection=session.volume_info().selection;
    require(std::ranges::equal(advice_bytes(*backing),prior),"selector handoff wrote optional cold advice");
    release_inactive(session);
    NativeGroupMaintenance groups(session);
    NativeMaintenanceObservation observed;
    observed.physical_total=16ULL<<30;observed.physical_available=12ULL<<30;
    observed.commit_limit=32ULL<<30;observed.commit_available=24ULL<<30;
    observed.cpu_total_ticks=1000;observed.cpu_window_ms=1000;observed.valid=true;
    for(unsigned i=0;i<1200 && !groups.progress().checkpoints_published;++i) {
      ++observed.sequence;observed.captured_ms+=1000;
      require(groups.advance(observed,observed.captured_ms).phase!=NativeMaintenancePhase::failed,
          "funded group scan failed to save cold coverage");
    }
    const auto checkpoint=session.optimization_snapshot();
    require(groups.progress().checkpoints_published==1,"group scan did not publish its funded checkpoint");
    require(checkpoint.sequence==2 && checkpoint.state.settled_input_bytes==kOptimizationGroupScanInputBytes,
        "group scan checkpoint did not account for the expected input credit");
    require(checkpoint.state.group_scan.has_value(),"funded checkpoint did not save the group scan cursor");
    require(checkpoint.state.cold_coverage.has_value(),"funded checkpoint did not save cold coverage");
    require(!session.snapshot().shared_base_info().members,"group scan unexpectedly rewrote the cold coverage fixture");
    saved=*checkpoint.state.cold_coverage;
    require(saved.image==selection.image_identity && saved.selector_epoch==selection.epoch &&
        saved.minimum_checkpoint==selection.minimum_checkpoint && saved.through_generation==captured &&
        saved.through_generation<replayed && saved.zstd_version==store_zstd_version() &&
        saved.foreground_level==3,"saved cold coverage included unevaluated replayed data");
  }
  require(advice_bytes(*backing)[7]==std::byte{'7'} &&
      advice_bytes(*backing)[kStoreCommitUnit+7]==std::byte{'7'},"cold coverage capability was not mirrored");

  // A re-signed, wider hint models an old optimizer overstating coverage.
  // A valid matching hint can only skip compression, even for replayed bytes.
  enum class Advice {matching,wider,wrong_image,wrong_epoch,wrong_checkpoint,wrong_codec,wrong_policy,
      malformed,torn,mixed};
  const auto run=[&](Advice scenario,StoreCompressionOptions options) {
    auto copy=backing->replay();
    auto slots=std::span(copy->io.stable).subspan(kVolumeHeadBytes,kOptimizationAreaBytes);
    if(scenario==Advice::torn || scenario==Advice::mixed) {
      std::ranges::copy(std::span(prior).last(kStoreCommitUnit),slots.last(kStoreCommitUnit).begin());
      if(scenario==Advice::torn)slots[kStoreCommitUnit-1]^=std::byte{1};
    }else if(scenario!=Advice::matching) {
      for(unsigned i=0;i<2;++i) {
        auto bytes=slots.subspan(i*kStoreCommitUnit,kStoreCommitUnit);
        constexpr std::size_t cold=1904,digest_at=kStoreCommitUnit-32;
        const auto put_field=[&](std::size_t at,std::uint64_t value,unsigned width) {
          for(unsigned j=0;j<width;++j)bytes[cold+at+j]=static_cast<std::byte>(value>>(8*j));
        };
        put_field(32,saved.minimum_checkpoint-1,8);
        if(scenario==Advice::wrong_image)bytes[cold]^=std::byte{1};
        if(scenario==Advice::wrong_epoch)put_field(16,saved.selector_epoch+1,8);
        if(scenario==Advice::wrong_checkpoint)put_field(24,saved.minimum_checkpoint+1,8);
        if(scenario==Advice::wrong_codec)put_field(44,saved.zstd_version+1,4);
        if(scenario==Advice::wrong_policy)put_field(40,saved.policy_revision+1,4);
        if(scenario==Advice::malformed)put_field(32,saved.minimum_checkpoint,8);
        const auto digest=detail::record_digest(bytes.first(digest_at));
        std::ranges::copy(digest,bytes.begin()+digest_at);
      }
    }
    copy->io.live=copy->io.stable;
    const auto before=NativeVolumeFile::inspect(copy,id(70)).info().selection;
    Bytes observed_witness=witness;
    {
      auto session=NativeVolumeFile::resume(copy,id(70),options);
      require(session.volume_info().selection==before,"advisory interruption changed volume selection");
      const auto reopened=session.optimization_snapshot();
      if(scenario==Advice::matching)
        require(reopened.state.cold_coverage==saved,"reopen lost the selected cold coverage");
      if(scenario==Advice::malformed)
        require(!reopened.state.cold_coverage,"invalid cold generation supplied progress");
      if(scenario==Advice::torn || scenario==Advice::mixed)
        require(session.optimization_info().phase==NativeOptimizationPhase::unavailable,
            "unfunded reopen rewrote an interrupted advisory pair");
      for(unsigned seed=881;seed<895 && !session.persistent_reclamation_pending();++seed) {
        observed_witness=data(witness.size(),seed);
        const auto file=session.open("witness",{true,true,false});
        session.write(file,0,observed_witness);session.cleanup(file,false);session.close(file);
      }
      require(session.persistent_reclamation_pending(),"foreground history did not fund range rotation");
      require(stage_compaction(session,73)==Reclaim::ready &&
          session.advance_persistent_reclamation({})==Reclaim::published,"reopened indexed copy did not publish");
    }
    const auto recovered=NativeVolumeFile::inspect(copy,id(70));
    const auto& view=recovered.filesystem();
    view.verify_all();
    require(view.read("payload",0,static_cast<std::uint32_t>(payload.size()))==payload &&
        view.read("late",0,static_cast<std::uint32_t>(late.size()))==late &&
        view.read("witness",0,static_cast<std::uint32_t>(observed_witness.size()))==observed_witness,
        "cold hint changed independently reopened bytes");
    return view.storage_info().stored_payload_bytes;
  };
  const auto matched=run(Advice::matching,foreground);
  const auto wider=run(Advice::wider,foreground);
  require(matched<wider,"restart ignored the cutoff or failed to reconsider replayed data");
  for(const auto scenario:{Advice::wrong_image,Advice::wrong_epoch,Advice::wrong_checkpoint,Advice::wrong_codec,
      Advice::wrong_policy,Advice::malformed,Advice::torn,Advice::mixed})
    require(run(scenario,foreground)==matched,"foreign or interrupted cold hint excluded eligible data");
  auto different_level=foreground;
  different_level.zstd_level=foreground.zstd_level+1;
  require(run(Advice::wider,different_level)<wider,"changed foreground level reused a prior encoder cutoff");
  std::cout<<"cold_coverage persisted_sequence=2 captured="<<saved.through_generation
      <<" matched_stored="<<matched<<" wider_stored="<<wider
      <<" selected_data_independent=1 binding_mismatches=6 malformed_pairs=1 interrupted_pairs=2\n";
}

void screened_cold_compaction() {
  if(!store_zstd_available())return;
  const StoreCompressionOptions foreground{StoreCompressionMode::fast,1,false};
  const auto retained=cold_test_payload();
  const auto additional=cold_test_payload(130011);
  const Bytes sparse(640*1024);
  enum class Scenario {copy,noise,foreground_commit,journal_fault};
  unsigned successful=0;
  for(const auto scenario:{Scenario::copy,Scenario::noise,Scenario::foreground_commit,Scenario::journal_fault}) {
    auto io=std::make_shared<SparseVolumeRam>(4096);
    auto expected=scenario==Scenario::noise?data(retained.size(),2382):retained;
    const auto sibling=scenario==Scenario::noise?Bytes(1024*1024):additional;
    std::uint64_t old_size=0,old_writes=0,old_settlement=0,new_settlement=0,new_size=0;
    {
      auto session=NativeVolumeFile::create(io,id(70),id(71),limits,foreground);
      put(session,"payload",expected);
      put(session,"sibling",sibling);
      put(session,"sparse",sparse);
      old_size=session.snapshot().storage_info().physical_end;
      old_writes=io->io.write_bytes;
      old_settlement=*session.optimization_info().settled_input_bytes;
      if(scenario==Scenario::copy)io->admitted_growth=3*kStoreCommitUnit;
      require(!session.persistent_reclamation_pending() && session.persistent_reclamation_pending(true),
          "append-mostly data did not qualify for a screened cold pass");
      require(session.advance_persistent_reclamation([]{return id(72);},true)==Reclaim::copying &&
          io->io.write_bytes==old_writes,"starting a compression screen wrote data");
      if(scenario==Scenario::foreground_commit) {
        expected.front()^=std::byte{1};
        const auto file=session.open("payload",{true,true,false});
        session.write(file,0,std::span(expected).first(1));session.cleanup(file,false);session.close(file);
        old_writes=io->io.write_bytes;
      }
      if(scenario==Scenario::journal_fault)io->io.fail_at=io->io.calls+1;
      bool charged_before_copy=false;
      auto progress=Reclaim::copying;
      for(unsigned step=0;step<2000 && progress==Reclaim::copying;++step) {
        progress=session.advance_persistent_reclamation({},true);
        if(!charged_before_copy &&
            io->allocated_extent(kNativeVolumeFileImageOffset+limits.maximum_physical_bytes,limits.maximum_physical_bytes)) {
          const auto funding=session.optimization_info();
          require(funding.sequence==2 && funding.settled_input_bytes &&
              *funding.settled_input_bytes>old_settlement,"candidate wrote before its durable budget charge");
          charged_before_copy=true;
        }
      }
      if(scenario==Scenario::copy) {
        require(progress==Reclaim::ready && charged_before_copy &&
            session.advance_persistent_reclamation({},true)==Reclaim::published,"funded cold candidate did not publish");
        new_size=session.snapshot().storage_info().physical_end;
        new_settlement=*session.optimization_info().settled_input_bytes;
        require(new_size<old_size && io->io.write_bytes-old_writes<=2*(new_settlement-old_settlement),
            "cold copy failed its space benefit or exceeded its charge");
        release_inactive(session);
        require(!session.persistent_reclamation_pending(true),"successful cold copy immediately offered another pass");
        ++successful;
      }else {
        require(!charged_before_copy && session.volume_info().selection.epoch==1,
            "refused cold candidate crossed the image boundary");
        if(scenario==Scenario::journal_fault) {
          require(progress==Reclaim::failed && session.optimization_info().phase==NativeOptimizationPhase::terminal,
              "journal fault did not fence further optional writes");
        }else {
          require((progress==Reclaim::idle || progress==Reclaim::retiring) &&
              io->io.write_bytes==old_writes && session.optimization_info().settled_input_bytes==old_settlement,
              "inconclusive or stale proof wrote storage");
          release_inactive(session);
          require(!session.persistent_reclamation_pending(true),"unchanged funding retried a rejected screen");
        }
      }
    }
    auto replayed=io->replay();
    {
      const auto view=NativeVolumeFile::inspect(replayed,id(70));
      const auto& files=view.filesystem();
      files.verify_all();
      require(files.read("payload",0,static_cast<std::uint32_t>(expected.size()))==expected &&
          files.read("sibling",0,static_cast<std::uint32_t>(sibling.size()))==sibling &&
          files.read("sparse",0,static_cast<std::uint32_t>(sparse.size()))==sparse,
          "screened compaction changed acknowledged file bytes");
    }
    if(scenario==Scenario::copy) {
      auto reopened=NativeVolumeFile::resume(replayed,id(70),foreground);
      require(reopened.optimization_info().settled_input_bytes==new_settlement,
          "reopening restored spent rewrite credit");
      std::cout<<"screened_cold old_image="<<old_size<<" new_image="<<new_size
          <<" input_charge="<<new_settlement-old_settlement<<" incremental_growth=1\n";
    }
  }
  std::cout<<"screened_cold published="<<successful
      <<" negative_screen=1 foreground_cancellation=1 journal_failure=1 exact_readback=4\n";
}

void reclaimed_ranges() {
  auto io=std::make_shared<SparseVolumeRam>(4096);
  Bytes expected;
  {
    auto session=NativeVolumeFile::create(io,id(90),id(91),limits,raw);
    expected=establish_history(session);
    const auto original=io->occupied();
    rejects([&]{(void)session.begin_idle_compaction(empty(4096),id(94));},
        "file authority accepted an unrelated replacement image");
    {
      const auto old=session.snapshot();
      require(stage_compaction(session)==Reclaim::ready &&
          session.advance_persistent_reclamation({})==Reclaim::published,"verified range switch did not publish");
      require(session.volume_info().selection.epoch==2 && !session.volume_info().degraded &&
          !io->range_reclaims && io->occupied()>original,"source range reclaimed before selection or pin release");
      require(session.advance_persistent_reclamation({})==Reclaim::pinned &&
          old.read("payload",0,50000)==expected && !io->range_reclaims,"live old snapshot failed to pin its physical range");
      rejects([&]{(void)NativeVolumeFile::inspect(io);},"paired CPU read bypassed the exclusive live lease");
    }
    release_inactive(session);
    require(io->occupied()<original && io->range_reclaims &&
        !io->allocated_extent(kNativeVolumeFileImageOffset,limits.maximum_physical_bytes) &&
        session.snapshot().read("witness",0,1000)==witness_bytes,"retired range did not reduce total occupied bytes");
  }
  auto stable=io->replay();
  {
    const auto checked=NativeVolumeFile::inspect(stable,id(90));
    require(checked.info().selection.placement==NativeVolumePlacement::paired_file_ranges &&
        checked.filesystem().read("payload",0,50000)==expected,"independent paired CPU reopen lost bytes");
    rejects([&]{(void)NativeVolumeFile::resume(stable);},"active CPU view failed to pin the paired file");
  }
  auto linear=image(stable->io.stable,4096);
  rejects([&]{(void)NativeVolumeFile::resume(linear);},"paired volume admitted an unqualified allocator");
  require(NativeVolumeFile::inspect(linear).filesystem().read("payload",0,50000)==expected,
      "CPU inspection depended on the sparse writer");
  {
    auto second=NativeVolumeFile::resume(stable,id(90),raw);
    // Retain the same staged physical range after a late foreground edit.
    for(unsigned i=0;i<12 && !second.persistent_reclamation_pending();++i) {
      expected=data(12289,310+i);update(second,expected);
    }
    require(stage_compaction(second,94)==Reclaim::ready,"second range did not stage");
    expected=data(12289,440);update(second,expected);
    require(second.advance_persistent_reclamation({})==Reclaim::copying && second.volume_info().selection.epoch==2,
        "a foreground edit allowed stale selection");
    require(stage_compaction(second,95)==Reclaim::ready && second.advance_persistent_reclamation({})==Reclaim::published &&
        second.volume_info().selection.image_identity==id(94),
        "reconciled first range did not retain the originally staged identity");
    release_inactive(second);
    require(second.volume_info().selection.epoch==3 &&
        !stable->allocated_extent(kNativeVolumeFileImageOffset+limits.maximum_physical_bytes,limits.maximum_physical_bytes),
        "third epoch did not reclaim its inactive range");
  }
  require(NativeVolumeFile::inspect(stable).filesystem().read("payload",0,50000)==expected,
      "recycled first range lost the latest acknowledged foreground data");
}

void reclamation_after_reopen() {
  for(const auto unit:{4096U,65536U}) {
    auto io=std::make_shared<SparseVolumeRam>(unit);
    auto expected=data(12289,810);
    std::uint64_t original_extent=0;
    {
      auto session=NativeVolumeFile::create(io,id(90),id(91),limits,raw);
      put(session,"payload",expected);
      put(session,"witness",witness_bytes);
      for(unsigned revision=0;revision<12;++revision) {
        expected=data(expected.size(),811+revision);
        update(session,expected);
      }
      require(session.persistent_reclamation_pending(),"known history could not qualify before reopen");
      original_extent=session.snapshot().storage_info().physical_end;
    }
    const auto occupied=io->occupied();
    io=io->replay();
    std::uint64_t lower_bound=0,replacement_extent=0;
    {
      auto session=NativeVolumeFile::resume(io,id(90),raw);
      const auto reads=io->io.read_calls,writes=io->io.writes;
      const auto space=session.indexed_volume_space();
      lower_bound=space.obsolete_bytes;
      const auto pending=session.persistent_reclamation_pending();
      std::cout<<"reopened_reclamation unit="<<unit<<" original_extent="<<original_extent
          <<" lower_bound="<<lower_bound<<" pending="<<pending<<'\n';
      require(pending && space.reclamation_worthwhile() && session.volume_info().selection.epoch==1 &&
          session.advance_persistent_reclamation({})==Reclaim::idle && session.persistent_reclamation_pending() &&
          io->io.read_calls==reads && io->io.writes==writes,
          "reopened history failed to qualify for reclamation without more I/O");
      {
        const auto old=session.snapshot();
        require(stage_compaction(session)==Reclaim::ready &&
            session.advance_persistent_reclamation({})==Reclaim::published,
            "reopened history was not compacted");
        replacement_extent=session.snapshot().storage_info().physical_end;
        require(replacement_extent<original_extent && lower_bound<=original_extent-replacement_extent,
            "reopened reclaimable span exceeded the observed reduction");
        require(session.advance_persistent_reclamation({})==Reclaim::pinned && !io->range_reclaims &&
            old.read("payload",0,50000)==expected && old.read("witness",0,1000)==witness_bytes,
            "reopen reclamation retired a pinned generation");
      }
      release_inactive(session);
      require(io->occupied()<occupied && session.volume_info().selection.epoch==2,
          "reopen reclamation failed to release the old allocation");
    }
    const auto settled=io->replay();
    {
      const auto selected=NativeVolumeFile::inspect(settled,id(90));
      selected.filesystem().verify_all();
      require(selected.filesystem().read("payload",0,50000)==expected &&
          selected.filesystem().read("witness",0,1000)==witness_bytes,
          "reclaimed image lost acknowledged data on independent CPU reopen");
    }
    auto session=NativeVolumeFile::resume(settled,id(90),raw);
    require(!session.indexed_volume_space().obsolete_bytes && !session.persistent_reclamation_pending(),
        "reopen scheduled another compaction of the settled image");
    std::cout<<"reopened_reclamation_verified unit="<<unit<<" replacement_extent="<<replacement_extent
        <<" occupied_before="<<occupied<<" occupied_after="<<io->occupied()
        <<" exact_cpu_readback=1 pin_blocks_retirement=1 settled_idle=1\n";
  }
}

void capacity_repair_rotation() {
  const auto run=[&](std::optional<std::size_t> cut,unsigned mode,bool transient=false) {
    auto io=std::make_shared<SparseVolumeRam>(4096);
    Bytes expected;
    std::size_t operations=0;
    {
      auto session=NativeVolumeFile::create(io,id(90),id(91),limits,raw);
      expected=establish_history(session);
      require(stage_compaction(session)==Reclaim::ready && session.advance_persistent_reclamation({})==Reclaim::published,
          "repair fixture could not pack its selected source");
      release_inactive(session);
      require(session.volume_info().selection.epoch==2 && !session.indexed_volume_space().obsolete_bytes &&
          !session.persistent_reclamation_pending(),"packed source unnecessarily scheduled ordinary compaction");
      const auto handle=session.open("payload",{true,true,true});
      const auto pinned=session.snapshot();
      const auto packed=pinned.storage_info().physical_end;
      const auto& view=io->io.live;
      constexpr std::size_t marker=40;
      const auto start=kNativeVolumeFileImageOffset+limits.maximum_physical_bytes;
      const auto data=std::search(view.begin()+start,view.end(),expected.begin(),expected.begin()+marker);
      require(data!=view.end(),"repair fixture could not identify its active payload");
      const auto offset=static_cast<std::size_t>(data-view.begin());
      const auto peer=offset+io->io.failure_unit;
      require(peer+marker<=view.size() &&
          std::equal(expected.begin(),expected.begin()+marker,view.begin()+peer),"repair fixture peer differs");
      const auto correct=view[offset];
      io->admitted_end=io->extent();
      io->io.live[offset]^=std::byte{1};
      io->io.stable[offset]=io->io.live[offset];
      const auto before=io->io.calls;
      const auto previous_reclaims=io->range_reclaims;
      require(session.read(handle,0,50000)==expected &&
          session.repair_indexed_payload()==PagedPayloadRepairProgress::capacity &&
          session.indexed_capacity_repair_generation() && session.persistent_reclamation_pending() &&
          io->io.calls==before,"verified repair did not escalate on append refusal");

      if(transient) {
        io->io.live[offset]=io->io.stable[offset]=correct;
        require(stage_compaction(session,94)==Reclaim::failed && io->io.calls==before &&
            session.volume_info().selection.epoch==2,"a historical fault authorized a non-saving switch");
        release_inactive(session);
        require(!session.persistent_reclamation_pending(),"unchanged repair evidence retried");
      } else {
        require(stage_compaction(session,94)==Reclaim::ready,"equal-size repair did not verify its candidate");
        const auto handoff_start=io->io.calls;
        if(cut){io->io.failure=mode;io->io.fail_at=handoff_start+*cut;}
        try {
          const auto result=session.advance_persistent_reclamation({});
          require(!cut && result==Reclaim::published,"repair selector did not publish the verified candidate");
        }catch(const NativeFsError&){if(!cut)throw;}
        operations=io->io.calls-handoff_start;
        require(io->range_reclaims==previous_reclaims,"repair retired an old range before checking its readers");
        if(!cut) {
          const auto repaired=session.snapshot();
          repaired.verify_all();
          require(session.volume_info().selection.epoch==3 && repaired.storage_info().physical_end==packed &&
              !repaired.storage_info().degraded_checkpoint && session.read(handle,0,50000)==expected &&
              pinned.read("payload",0,50000)==expected && session.advance_persistent_reclamation({})==Reclaim::pinned,
              "equal-size repair lost bytes, failed to heal, or released a pinned reader");
        }
      }
    }
    const auto reopened=NativeVolumeFile::inspect(io->replay(),id(90));
    const auto epoch=reopened.info().selection.epoch;
    require((cut?(epoch==2 || epoch==3):epoch==(transient?2U:3U)) &&
        reopened.filesystem().read("payload",0,50000)==expected &&
        reopened.filesystem().read("witness",0,1000)==witness_bytes,
        "repair rotation interrupted acknowledged data");
    if(epoch==3)require(!reopened.filesystem().storage_info().degraded_checkpoint,
        "selected repair left an invalid replica");
    return operations;
  };
  const auto boundaries=run({},0);
  require(boundaries==4,"repair selector write and barrier count changed");
  (void)run({},0,true);
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut)(void)run(cut,mode);
  std::cout<<"capacity_repair_rotation equal_size=1 transient_refused=1 selector_interruptions="<<boundaries*3<<'\n';
}

void full_startup_repair_rotation() {
  const auto start=kNativeVolumeFileImageOffset+limits.maximum_physical_bytes;
  const auto make=[&](std::uint32_t unit,bool retired_context) {
    auto io=std::make_shared<SparseVolumeRam>(unit);
    Bytes expected;
    {
      auto session=NativeVolumeFile::create(io,id(90),id(91),limits,raw);
      expected=establish_history(session);
      require(stage_compaction(session)==Reclaim::ready && session.advance_persistent_reclamation({})==Reclaim::published,
          "startup repair fixture did not pack into its second range");
      release_inactive(session);
      if(retired_context) {
        session.create_entry("orphan");
        const auto handle=session.open("orphan",{true,true,true});
        session.write(handle,0,data(225,612));
        session.set_delete(handle,true);session.cleanup(handle,true);
        require(session.snapshot().needs_session_recovery(),"startup fixture did not retain a file lifetime");
      }
    }
    const auto& body=io->io.live;
    const auto marker=std::span(expected).first(40);
    const auto location=std::search(body.begin()+start,body.end(),marker.begin(),marker.end());
    require(location!=body.end(),"startup repair fixture has no selected payload");
    const auto offset=static_cast<std::size_t>(location-body.begin());
    const auto peer=offset+unit;
    require(peer+marker.size()<=body.size() && std::equal(marker.begin(),marker.end(),body.begin()+peer),
        "startup repair fixture has no payload peer");
    io->io.live[peer]^=std::byte{1};
    io->io.stable[peer]=io->io.live[peer];
    return std::pair{io,expected};
  };
  const auto [seed,expected]=make(4096,false);
  const auto verify=[&](const NativeFilesystemSnapshot& source,const Bytes& bytes) {
    require(source.read("payload",0,50000)==bytes && source.read("witness",0,1000)==witness_bytes,
        "startup rotation lost acknowledged bytes");
  };
  unsigned advisory_failures=0;
  const auto run=[&](std::optional<std::size_t> cut,unsigned mode) {
    auto io=seed->replay();io->admitted_end=io->extent();
    if(cut){io->io.fail_at=*cut;io->io.failure=mode;}
    bool attached=false;
    try {
      auto session=NativeVolumeFile::resume(io,id(90),raw,NativeDispositionPolicy::aggregate,[]{return id(94);});
      attached=true;
      const auto selected=session.volume_info().selection;
      require(selected.epoch==3 && selected.image_identity==id(94) && selected.parent_identity==id(93) &&
          !session.snapshot().storage_info().degraded_checkpoint,"startup repair did not select a verified alternate");
      verify(session.snapshot(),expected);
      if(cut) {
        require(io->io.calls>*cut && session.optimization_info().phase==NativeOptimizationPhase::unavailable &&
            session.optimization_snapshot().code==OptimizationReadCode::io_failure,
            "startup rotation accepted an interrupted authority or image operation");
        ++advisory_failures;
      }
    }catch(const std::exception&){if(!cut || attached || io->io.calls<=*cut)throw;}
    require(cut || attached,"startup rotation did not complete in the uninterrupted fixture");
    require(!io->range_reclaims,"startup rotation reclaimed a range before publication");
    const auto old=std::span(seed->io.stable).subspan(start);
    const auto after=std::span(io->io.stable).subspan(start);
    if(!std::ranges::equal(old,after)) {
      const auto differing=std::ranges::mismatch(old,after);
      std::cerr<<"startup source difference at "<<differing.in1-old.begin()
          <<" old bytes="<<old.size()<<" current bytes="<<after.size()<<" mode="<<mode
          <<" operation="<<cut.value_or(io->io.calls)<<'\n';
    }
    require(std::ranges::equal(old,after),"startup rotation changed the selected source range before retirement");
    const auto operations=io->io.calls;
    auto durable=io->replay();durable->admitted_end=durable->extent();
    std::uint64_t observed_epoch=0;
    {
      const auto recovered=NativeVolumeFile::inspect(durable,id(90));
      observed_epoch=recovered.info().selection.epoch;
      require(observed_epoch==3 || (cut && observed_epoch==2),"startup interruption exposed an unrelated selector");
      verify(recovered.filesystem(),expected);
    }
    if(cut) {
      const auto retry_id=observed_epoch==2?id(95):id(94);
      unsigned allocated=0;
      auto session=NativeVolumeFile::resume(durable,id(90),raw,NativeDispositionPolicy::aggregate,[&] {
        ++allocated;return retry_id;
      });
      require(session.volume_info().selection.epoch==3 && session.volume_info().selection.image_identity==retry_id &&
          !session.snapshot().storage_info().degraded_checkpoint && allocated==(observed_epoch==2?1U:0U),
          "startup retry did not recover the verified selection");
      verify(session.snapshot(),expected);
      release_inactive(session);
    }
    return operations;
  };
  const auto operations=run({},0);
  require(operations>6 && operations<180,"startup fallback exceeded its fault fixture bounds");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<operations;++cut)(void)run(cut,mode);

  {
    auto rejected=seed->replay();rejected->admitted_end=rejected->extent();rejected->admitted_growth=0;
    rejects([&]{(void)NativeVolumeFile::resume(rejected,id(90),raw,NativeDispositionPolicy::aggregate,[]{return id(94);});},
        "startup repair wrote without room for its alternate");
    require(!rejected->io.writes && !rejected->range_reclaims && rejected->io.stable==seed->io.stable,
        "alternate admission refusal changed active or spare bytes");
  }
  {
    // Measure space for the first append, then leave another damaged record.
    // Rotation must use the newer checkpoint produced by that first repair.
    auto partial=seed->replay(),probe=seed->replay();
    const auto original=NativeVolumeFile::inspect(seed,id(90)).filesystem().storage_info().generation;
    {
      auto fit=NativeVolumeFile::resume(probe,id(90),raw);
      require(!fit.snapshot().storage_info().degraded_checkpoint && probe->extent()>seed->extent(),
          "startup append probe did not grow the active range");
    }
    auto& bytes=partial->io.live;
    const auto witness=std::search(bytes.begin()+start,bytes.end(),witness_bytes.begin(),witness_bytes.end());
    require(witness!=bytes.end(),"repair fixture lost its witness");
    const auto offset=static_cast<std::size_t>(witness-bytes.begin());
    bytes[offset]^=std::byte{1};partial->io.stable[offset]=bytes[offset];
    partial->admitted_end=probe->extent();
    auto repaired=NativeVolumeFile::resume(partial,id(90),raw,NativeDispositionPolicy::aggregate,[]{return id(94);});
    require(repaired.volume_info().selection.epoch==3,"partial append repair did not rotate");
    verify(repaired.snapshot(),expected);
    const auto old=image(Bytes(bytes.begin()+start,bytes.end()),4096);
    const auto retained=NativeFilesystemSnapshot::open_image(old,{NativeImageKind::indexed_paged,limits.maximum_physical_bytes});
    retained.verify_all();
    require(retained.storage_info().generation>original && retained.storage_info().degraded_checkpoint,
        "startup rotation did not retain the partially repaired current source");
    verify(retained,expected);
  }

  auto [retired,remaining]=make(65536,true);
  retired->admitted_end=retired->extent();
  auto session=NativeVolumeFile::resume(retired,id(90),raw,NativeDispositionPolicy::aggregate,[]{return id(94);});
  require(session.volume_info().selection.epoch==3 && !session.snapshot().needs_session_recovery(),
      "startup rotation did not finish a detached lifetime after selection");
  verify(session.snapshot(),remaining);
  rejects([&]{(void)session.open("orphan",{true,false,false});},"startup rotation restored a detached filename");
  std::cout<<"full_startup_rotation interruptions="<<operations*3
      <<" advisory_only_failures="<<advisory_failures
      <<" interrupted_candidate_recovered=1 selected_source_preserved=1 retired_context_recovered=1"
      <<" target_refusal_write_free=1 partial_append_reconciled=1\n";
}

void range_publication_cuts() {
  for(const auto unit:{4096U,65536U})for(const bool reconcile:{false,true}) {
    const auto run=[&](std::optional<std::size_t> cut,unsigned mode) {
      std::size_t boundaries=0;
      auto io=std::make_shared<SparseVolumeRam>(unit);
      Bytes expected;
      {
        auto session=NativeVolumeFile::create(io,id(90),id(91),limits,raw);
        expected=establish_history(session);
        require(stage_compaction(session)==Reclaim::ready,"fault matrix could not stage a verified successor");
        if(reconcile) {
          expected=data(expected.size(),170);
          update(session,expected);
        }
        const auto before=io->io.calls;
        if(cut) {
          io->io.fail_at=before+*cut;io->io.failure=mode;
        }
        try {
          const bool ready=!reconcile || stage_compaction(session)==Reclaim::ready;
          const auto published=ready?session.advance_persistent_reclamation({}):Reclaim::failed;
          if(published==Reclaim::published) {
            for(unsigned i=0;i<4 && session.persistent_reclamation_pending();++i)
              (void)session.advance_persistent_reclamation({});
          }
        }catch(const NativeFsError&){if(!cut)throw;}
        boundaries=io->io.calls-before;
        if(!cut)require(boundaries>=6 && boundaries<160 && io->range_reclaims>0,
            "fault matrix did not cross durable selection and reclamation");
      }
      auto stable=io->replay();
      {
        const auto observed=NativeVolumeFile::inspect(stable);
        require(observed.info().selection.epoch>=1 && observed.info().selection.epoch<=2 &&
            observed.filesystem().read("payload",0,50000)==expected &&
            observed.filesystem().read("witness",0,1000)==witness_bytes,
            "interrupted physical reclamation lost acknowledged data");
      }
      {
        auto recovery=NativeVolumeFile::resume(stable,{},raw);
        release_inactive(recovery);
        const auto recovered=recovery.volume_info();
        const auto inactive=kNativeVolumeFileImageOffset+
            (recovered.selection.epoch%2?limits.maximum_physical_bytes:0);
        require(!recovered.degraded && !stable->allocated_extent(inactive,limits.maximum_physical_bytes) &&
            recovery.snapshot().read("payload",0,50000)==expected,
            "interrupted range retirement did not finish after independent reopen");
      }
      return boundaries;
    };
    const auto cuts=run({},0);
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut)(void)run(cut,mode);
    std::cout<<"range_retirement_cuts unit="<<unit<<" reconciled="<<reconcile<<" operation_boundaries="<<cuts<<" modes=3\n";
  }
}

#ifdef _WIN32
void disposable_file(const std::filesystem::path& base) {
  // Only the explicit integration fixture takes this path. Retain its new,
  // uniquely owned directory and files for disposal by that environment.
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  auto root=std::filesystem::canonical(base)/("flitzfs-volume-file-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"could not exclusively create the file fixture directory");
  const auto path=root/"volume.flfs";
  const auto alias=root/"alias.flfs";
  constexpr StoreIoContract model{65536,true};
  constexpr PagedStoreLimits large{128ULL*1024*1024};
  const auto first=data(67539,181),second=data(first.size(),677);
  {
    auto session=NativeVolumeFile::create(path,model,id(61),id(62),large,raw);
    put(session,"payload",first);session.flush();
    rejects([&]{(void)NativeVolumeFile::resume(path,model);},"second Windows write owner acquired the file");
  }
  const auto size=std::filesystem::file_size(path);
  require(size>kNativeVolumeFileImageOffset && size<2*1024*1024,"file creation preallocated its declared capacity");
  rejects([&]{(void)NativeVolumeFile::create(path,model,id(64),id(65),large,raw);},
      "CREATE_NEW modified an existing file");
  {
    const auto cpu=NativeVolumeFile::inspect(path);
    require(cpu.info().selection.volume_identity==id(61) &&
        cpu.info().selection.storage.maximum_physical_bytes==large.maximum_physical_bytes &&
        cpu.filesystem().read("payload",0,100000)==first,"read-only Windows reopen lost acknowledged data");
    rejects([&]{(void)NativeVolumeFile::resume(path,model);},"pinned CPU reader allowed a Windows writer");
  }
  {
    auto session=NativeVolumeFile::resume(path,model,id(61),raw);
    update(session,second);session.flush();
  }
  rejects([&]{(void)NativeVolumeFile::resume(path,model,id(99));},"foreign volume identity accepted on Windows");
  std::filesystem::create_hard_link(path,alias);
  rejects([&]{(void)NativeVolumeFile::resume(alias,model);},"hard-linked backing admitted a Windows writer");
  const auto after=NativeVolumeFile::inspect(path);
  require(after.filesystem().read("payload",0,100000)==second,"Windows reopen altered the durable update");
  std::cout<<"windows_volume_file bytes_before_update="<<size<<" bytes_after_update="<<std::filesystem::file_size(path)<<'\n';
}
std::uint64_t allocated_bytes(const std::filesystem::path& file) {
  DWORD high=0;
  SetLastError(ERROR_SUCCESS);
  const auto low=GetCompressedFileSizeW(file.c_str(),&high);
  require(low!=INVALID_FILE_SIZE || GetLastError()==ERROR_SUCCESS,"Cannot query Windows allocated bytes");
  return (static_cast<std::uint64_t>(high)<<32)|low;
}
void disposable_sparse_file(const std::filesystem::path& base) {
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root=std::filesystem::canonical(base)/("flitzfs-sparse-volume-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"Could not exclusively create the sparse volume fixture");
  const auto path=root/"reclaimable.flfs";
  constexpr StoreIoContract isolated{65536,true,true};
  Bytes expected;
  {
    auto session=NativeVolumeFile::create(path,isolated,id(90),id(91),limits,raw);
    expected=data(12289,81);
    put(session,"payload",expected);put(session,"witness",witness_bytes);
    for(unsigned i=0;i<8;++i) {
      expected=data(12289,100+i);update(session,expected);
    }
  }
  auto previous=allocated_bytes(path);
  rejects([&]{(void)NativeVolumeFile::resume(path,{65536,true});},"Sparse file accepted an unqualified overwrite path");
  for(unsigned epoch=2;epoch<=3;++epoch) {
    {
      auto session=NativeVolumeFile::resume(path,isolated,id(90),raw);
      for(unsigned i=0;i<32 && !session.persistent_reclamation_pending();++i) {
        expected=data(12289,epoch*200+i);update(session,expected);
      }
      require(session.persistent_reclamation_pending(),"Windows sparse history did not admit a rewrite");
      require(stage_compaction(session,91+epoch)==Reclaim::ready,"Windows sparse replacement did not stage");
      require(session.advance_persistent_reclamation({})==Reclaim::published,
          "Windows sparse replacement did not publish");
      release_inactive(session);
      require(session.volume_info().selection.epoch==epoch,"Unexpected selected sparse file generation");
    }
    const auto selected=NativeVolumeFile::inspect(path,id(90));
    require(selected.info().selection.epoch==epoch && selected.filesystem().read("payload",0,50000)==expected &&
        selected.filesystem().read("witness",0,1000)==witness_bytes,"Windows sparse CPU reopen lost acknowledged bytes");
    const auto current=allocated_bytes(path);
    if(epoch==2)require(current<previous,"Old image retirement did not release Windows disk allocation");
    else require(current<=previous,"Recycled range retained superseded Windows allocations");
    previous=current;
    std::cout<<"windows_sparse_epoch verified="<<epoch<<" retired_previous=1\n";
  }
  std::cout<<"windows_sparse_reclamation both_slots_reused=1 independent_cpu_reopen=1 allocation_released=1\n";
}
#endif
}
int main(int argc,char** argv){
  try {
    if(argc==2 && std::string_view(argv[1])=="--cold-relocation") {
      cold_relocation(cold_test_payload(),true);
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--cold-coverage") {
      durable_cold_coverage();
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--screened-cold") {
      screened_cold_compaction();
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--reopened-reclamation") {
      reclamation_after_reopen();
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--cold-corpus") {
      auto source=ReadOnlyFile::open(argv[2]);
      require(source.size()>0 && source.size()<=2*1024*1024,"public cold fixture byte budget");
      Bytes payload(static_cast<std::size_t>(source.size()));
      source.read_exact_at(0,payload.data(),payload.size());
      cold_relocation(payload,false);
      return 0;
    }
#ifdef _WIN32
    if(argc==3 && std::string_view(argv[1])=="--disposable-file") {
      disposable_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows file-handle checks\n";
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-sparse-file") {
      disposable_sparse_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows sparse volume checks\n";
      return 0;
    }
#else
    (void)argv;
#endif
    require(argc==1,"unexpected volume file fixture mode");
    retired_and_reopened();
    geometry_and_advice();
    publication_cuts();
    reclaimed_ranges();
    reclamation_after_reopen();
    capacity_repair_rotation();full_startup_repair_rotation();range_publication_cuts();
    std::cout<<"PASS: "<<checks<<" memory-backed volume-file checks\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
