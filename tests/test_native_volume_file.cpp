#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/read_only_file.hpp"
#include "dense_volume_backing.hpp"
#include "dense_file_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "native_volume_file_reclaimer.hpp"
#include "record_digest.hpp"
#include "volume_range_fixture.hpp"
#ifdef _WIN32
#define NOMINMAX
#include "windows/positioned_file_io.hpp"
#include "windows/volume_file_lifetime.hpp"
#include "native_capture.hpp"
#include "native_lz4_fixture.hpp"
#include "store_codec.hpp"
#include <future>
#include <thread>
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

void overwrite_and_rename_publication_cuts() {
  const auto original=data(2*kNativeExtentBytes+137,751);
  const auto shortened=data(kNativeExtentBytes+997,752),witness=data(377,753);
  auto written=original;
  std::copy(shortened.begin(),shortened.end(),written.begin());
  const std::array<Bytes,4> contents{original,std::move(written),shortened,shortened};
  const auto continuation=data(907,754);
  for(const auto unit:{4096U,65536U}) {
    const auto baseline=acknowledged(unit,original,witness);
    const auto before=NativeVolumeFile::inspect(image(baseline,unit),id(70));
    const auto sequence=before.filesystem().sequence();
    const auto inode=before.filesystem().stat("payload").inode;
    std::array<std::size_t,4> recovered_states{};
    std::array<bool,4> continued{};
    const auto trial=[&](std::optional<std::size_t> cut,unsigned failure) {
      auto backing=image(baseline,unit);
      std::size_t boundaries=0,completed=0;
      {
        auto session=NativeVolumeFile::resume(backing,id(70),raw);
        const auto handle=session.open("payload",{true,true,true});
        const auto start=backing->calls;
        if(cut){backing->failure=failure;backing->fail_at=start+*cut;}
        try {
          session.write(handle,0,shortened);
          ++completed;
          session.truncate(handle,shortened.size());
          ++completed;
          session.rename(handle,"renamed");
          ++completed;
        }catch(const NativeFsError& error) {
          if(!cut || error.code()!=NativeFsErrorCode::storage_failure)throw;
          require(session.poisoned() && backing->calls==start+*cut+1,
              "Overwrite or rename continued issuing writes after its backing failure");
        }
        boundaries=backing->calls-start;
        require(cut?completed<3:completed==3,"Overwrite or rename acknowledgement differed from the fault schedule");
      }
      const auto recovered=NativeVolumeFile::inspect(image(backing->stable,unit),id(70));
      const auto& snapshot=recovered.filesystem();
      const auto recovered_sequence=snapshot.sequence();
      require(recovered_sequence>=sequence && recovered_sequence-sequence<contents.size(),
          "Overwrite or rename recovered an unrelated sequence");
      const auto state=static_cast<std::size_t>(recovered_sequence-sequence);
      require(state>=completed && state<=completed+(cut?1U:0U),
          "Overwrite or rename lost an acknowledged call or exposed a later call");
      const std::string_view path=state==3?"renamed":"payload";
      const auto info=snapshot.stat(path);
      require(info.inode==inode && info.logical_bytes==contents[state].size() && snapshot.list().size()==2 &&
          snapshot.read(path,0,static_cast<std::uint32_t>(original.size()+1))==contents[state] &&
          snapshot.read("witness",0,1000)==witness,
          "Overwrite or rename recovery mixed names, inode identities, EOF or payload bytes");
      require(recovered.info().selection==before.info().selection && !recovered.info().degraded &&
          std::equal(baseline.begin(),baseline.begin()+kNativeVolumeFileImageOffset,backing->stable.begin()),
          "Ordinary overwrite or rename changed the volume selector or adjacent advisory state");
      if(cut)++recovered_states[state];
      if(!continued[state]) {
        auto reopened=image(backing->stable,unit);
        {
          auto session=NativeVolumeFile::resume(reopened,id(70),raw);
          put(session,"continuation",continuation);
          session.flush();
        }
        const auto verified=NativeVolumeFile::inspect(image(reopened->stable,unit),id(70));
        const auto& next=verified.filesystem();
        require(next.list().size()==3 && next.stat(path).inode==inode && next.read("witness",0,1000)==witness &&
            next.read("continuation",0,1000)==continuation &&
            next.read(path,0,static_cast<std::uint32_t>(original.size()+1))==contents[state],
            "Writable continuation failed after overwrite or rename recovery");
        continued[state]=true;
      }
      return boundaries;
    };
    const auto boundaries=trial(std::nullopt,0);
    require(boundaries>9 && boundaries<=320,"Unbounded overwrite and rename cut matrix");
    for(unsigned failure=0;failure<3;++failure)
      for(std::size_t cut=0;cut<boundaries;++cut)(void)trial(cut,failure);
    require(std::ranges::all_of(recovered_states,[](auto count){return count>0;}),
        "Overwrite and rename cuts did not exercise every complete state");
    std::cout<<"volume_file_overwrite_rename_cuts unit="<<unit<<" boundaries="<<boundaries
        <<" failure_modes=3 states="<<recovered_states[0]<<','<<recovered_states[1]<<','
        <<recovered_states[2]<<','<<recovered_states[3]<<" writable_continuations=4\n";
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

void managed_catalog_ranges() {
  constexpr PagedStoreLimits data_range{16ULL*1024*1024};
  constexpr auto catalog_bytes=data_range.maximum_physical_bytes/32;
  constexpr auto catalog_base=kNativeVolumeFileImageOffset+2*data_range.maximum_physical_bytes;
  constexpr PagedStoreLimits catalog_limit{catalog_bytes};
  const auto volume=id(90),native_image=id(91);
  const auto witness=data(kNativeExtentBytes,27377);
  auto current=data(kNativeExtentBytes,32393);
  Bytes first_switch_payload;
  auto io=std::make_shared<SparseVolumeRam>(4096);
  std::shared_ptr<SparseVolumeRam> before,after;
  OptimizationCatalogSelection old_selection,new_selection;
  std::uint64_t old_bytes=0,new_bytes=0;
  NativeMaintenanceObservation observed;
  observed.physical_total=16ULL<<30;observed.physical_available=12ULL<<30;
  observed.commit_limit=32ULL<<30;observed.commit_available=24ULL<<30;
  observed.cpu_total_ticks=1000;observed.cpu_window_ms=1000;observed.valid=true;
  const auto step=[&](NativeGroupMaintenance& groups) {
    ++observed.sequence;observed.captured_ms+=1000;
    require(groups.advance(observed,observed.captured_ms).phase!=NativeMaintenancePhase::failed,
        "managed catalog maintenance failed");
  };
  const auto extent=[](const SparseVolumeRam& backing,unsigned slot) {
    return backing.allocated_extent(catalog_base+slot*catalog_bytes,catalog_bytes);
  };
  const auto stable_catalog=[&](const SparseVolumeRam& backing,unsigned slot) -> std::optional<IndexedCoherenceSnapshot> {
    const auto offset=catalog_base+slot*catalog_bytes;
    const auto end=(std::min)(offset+extent(backing,slot),std::uint64_t(backing.io.stable.size()));
    if(end<=offset)return {};
    const auto stable=std::span(backing.io.stable).subspan(static_cast<std::size_t>(offset),
        static_cast<std::size_t>(end-offset));
    auto bytes=image(Bytes(stable.begin(),stable.end()),4096);
    try {
      const auto checked=PagedObjectStoreSnapshot::open(bytes,catalog_limit);
      if(checked.info().degraded_roots || checked.info().degraded_metadata)return {};
      checked.verify_all();
      return IndexedCoherenceSnapshot::open(bytes,volume,native_image,catalog_limit,checked.identity());
    }catch(const std::exception&) {return {};}
  };
  const auto verify_files=[&](const NativeFilesystemSnapshot& snapshot,const Bytes& expected) {
    snapshot.verify_all();
    require(snapshot.read("payload",0,kNativeExtentBytes)==expected &&
        snapshot.read("witness",0,kNativeExtentBytes)==witness,"catalog management changed acknowledged files");
  };
  {
    auto session=NativeVolumeFile::create(io,volume,native_image,data_range,raw);
    require(session.volume_info().selection.placement==NativeVolumePlacement::paired_file_catalog_ranges,
        "qualified managed backing did not reserve paired catalog ranges");
    put(session,"payload",current);
    put(session,"witness",witness);
    unsigned earned=0;
    const Bytes zero_credit(3*kOptimizationGroupScanInputBytes/4);
    const auto earn=[&](unsigned count) {
      for(unsigned i=0;i<count;++i)put(session,"catalog-credit-"+std::to_string(earned++),zero_credit);
    };
    earn(5);
    NativeGroupMaintenance groups(session);
    for(unsigned i=0;i<1200 && groups.pending();++i)step(groups);
    const auto initial=session.optimization_snapshot().state.catalog_selection;
    require(initial && extent(*io,0) && !extent(*io,1),"managed scanner did not create its first catalog in the reservation");
    old_selection=*initial;
    const auto initial_catalog=stable_catalog(*io,0);
    require(initial_catalog && initial_catalog->backing_identity()==old_selection.backing &&
        initial_catalog->info().generation>=old_selection.minimum_generation,
        "first selection and independently read reserved catalog differ");
    unsigned revision=0;
    const auto rotate=[&](const OptimizationCatalogSelection& prior,bool capture_cut) {
      for(unsigned edit=0;edit<50;++edit) {
        current=data(kNativeExtentBytes,41011+revision++);update(session,current);earn(3);
        for(unsigned i=0;i<1200 && groups.pending();++i) {
          step(groups);
          const auto selected=session.optimization_snapshot().state.catalog_selection;
          require(selected.has_value(),"catalog selection vanished during append or rotation");
          if(selected->backing!=prior.backing)return *selected;
          if(capture_cut && !before && extent(*io,1) && stable_catalog(*io,1)) {
            const auto count=io->range_reclaims;
            require(session.advance_persistent_reclamation({})==Reclaim::idle && io->range_reclaims==count,
                "reclamation touched an unselected candidate with a live copy pin");
            before=io->replay();
          }
        }
      }
      throw std::runtime_error("catalog rotation never published a replacement");
    };
    new_selection=rotate(old_selection,true);
    first_switch_payload=current;
    require(before && extent(*io,0) && extent(*io,1),
        "normal maintenance never reached a complete reserved replacement and selector switch");
    require(groups.progress().coherence_compactions==1 && !io->range_reclaims,
        "catalog switch unexpectedly reclaimed backing within the publication");
    auto old_catalog=stable_catalog(*before,0),new_catalog=stable_catalog(*io,1);
    require(old_catalog && new_catalog && old_catalog->backing_identity()==old_selection.backing &&
        new_catalog->backing_identity()==new_selection.backing &&
        new_catalog->info().physical_end<old_catalog->info().physical_end,
        "the two durable selections do not name different checked catalog ranges");
    old_bytes=old_catalog->info().physical_end;new_bytes=new_catalog->info().physical_end;
    after=io->replay();
    const auto occupied=io->occupied();
    require(session.advance_persistent_reclamation({})==Reclaim::retiring && !extent(*io,0) &&
        extent(*io,1) && io->occupied()<occupied && session.volume_info().selection.epoch==1,
        "old catalog range was not reclaimed independently of the filesystem image");
    verify_files(session.snapshot(),current);
    const auto reused=rotate(new_selection,false);
    const auto reused_catalog=stable_catalog(*io,0);
    require(reused.backing!=old_selection.backing && reused_catalog &&
        reused_catalog->backing_identity()==reused.backing && groups.progress().coherence_compactions==2,
        "a released range was not reused with a fresh durable backing ID");
    require(session.advance_persistent_reclamation({})==Reclaim::retiring && !extent(*io,1) && extent(*io,0),
        "the previous replacement could not be retired after reuse");
    const auto retained_catalog_bytes=extent(*io,0);
    require(stage_compaction(session,92)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published && session.volume_info().selection.epoch==2,
        "catalog reservations interfered with a data image switch");
    release_inactive(session);
    require(extent(*io,0)==retained_catalog_bytes && !extent(*io,1) &&
        !io->allocated_extent(kNativeVolumeFileImageOffset,data_range.maximum_physical_bytes),
        "data image retirement changed a separately reserved catalog range");
    verify_files(session.snapshot(),current);
  }
  const auto verify_recovery=[&](const std::shared_ptr<SparseVolumeRam>& reopened,
      const OptimizationCatalogSelection& selection,unsigned slot,std::uint64_t bytes,bool torn) {
    auto session=NativeVolumeFile::resume(reopened,volume,raw);
    if(torn) {
      require(session.optimization_info().repair_pending && session.advance_persistent_reclamation({})==Reclaim::idle &&
          extent(*reopened,0) && extent(*reopened,1),"degraded advice authorized catalog retirement");
      verify_files(session.snapshot(),first_switch_payload);
      put(session,"catalog-repair-credit",Bytes(kOptimizationAreaBytes));
      require(session.repair_budgeted_optimization_area().state.phase==NativeOptimizationPhase::ready,
          "fresh input did not repair the catalog selector pair");
    }
    require(session.optimization_snapshot().state.catalog_selection==selection && extent(*reopened,1-slot),
        "independent reopen discarded an occupied catalog before worker admission");
    NativeGroupMaintenance groups(session);
    for(unsigned i=0;i<40 && !groups.progress().coherence_catalog_bytes;++i)step(groups);
    require(groups.progress().coherence_catalog_bytes==bytes && !reopened->range_reclaims,
        "restart did not recover the exact committed catalog");
    require(session.advance_persistent_reclamation({})==Reclaim::retiring && !extent(*reopened,1-slot) &&
        extent(*reopened,slot),"restart reclaimed the wrong catalog reservation");
    verify_files(session.snapshot(),first_switch_payload);
  };
  {
    const auto damaged=before->replay();
    const auto at=kVolumeHeadBytes+kStoreCommitUnit-1;
    damaged->io.stable[at]^=std::byte{1};damaged->io.live[at]^=std::byte{1};
    verify_recovery(damaged,old_selection,0,old_bytes,true);
  }
  verify_recovery(before,old_selection,0,old_bytes,false);
  verify_recovery(after,new_selection,1,new_bytes,false);
  auto cpu=image(after->io.stable,4096);
  verify_files(NativeVolumeFile::inspect(cpu,volume).filesystem(),first_switch_payload);
  std::cout<<"managed_catalog_ranges old_bytes="<<old_bytes<<" new_bytes="<<new_bytes
      <<" preselection_reopen=1 postselection_reopen=1 staged_pin_preserved=1 torn_selector_repair=1 reused_slot=1"
      <<" independent_data_rotation=1\n";
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

void dense_replacement_admission() {
  constexpr auto unit=kStoreCommitUnit;
  const auto file=std::make_shared<DenseTailRam>();
  file->bytes.failure_unit=4096;
  const auto existing=data(unit,214);
  const auto added=data(2*unit,215);
  {
    auto dense=detail::DenseVolumeBacking::create(file,id(79),32*unit,16*unit);
    dense->write_exact(0,existing);
    dense->flush();
    // A later smaller provider grant replaces the earlier grant.
    dense->admit_range_write(8*unit,6*unit);
    dense->admit_range_write(8*unit,0);
    dense->write_exact(4*unit,added);
    dense->flush();
  }
  const auto reopened=detail::DenseVolumeBacking::open(DenseTailRam::replay(*file),false);
  Bytes prior(existing.size()),latest(added.size());
  reopened->read_exact(0,prior);
  reopened->read_exact(4*unit,latest);
  require(prior==existing && latest==added,"replacement admission changed acknowledged bytes");
  std::cout<<"dense_replacement_admission fresh_growth=1 cpu_readback=exact\n";
}

void dense_allocation() {
  constexpr auto unit=kStoreCommitUnit;
  constexpr std::uint64_t logical_limit=8ULL<<40;
  constexpr auto remote=logical_limit-4*unit;
  auto io=empty(4096);
  {
    auto dense=detail::DenseVolumeBacking::create(io,id(70),logical_limit,8*unit);
    const auto early=data(3*unit,62),late=data(337,63);
    dense->write_exact(0,early);
    dense->write_exact(remote+31,late);
    dense->flush();
    const auto occupied=io->extent();
    require(occupied==8*unit && dense->extent()==remote+unit,
        "logical gaps grew the ordinary backing");
    dense->admit_contiguous_growth(dense->extent(),remote+32,remote+33);
    rejects([&]{dense->admit_contiguous_growth(remote+2*unit,remote+unit,remote+2*unit);},
        "growth beyond the physical cap was admitted");
    dense->reclaim_range(unit,unit);
    require(dense->allocated_extent(unit,unit)==0 && dense->allocated_extent(0,3*unit)==3*unit,
        "removing a middle slice damaged the surrounding runs");
    // Exercise allocation before the caller's flush. The retired mapping
    // must settle first, and the reused private slot must be zero-initialized.
    dense->admit_contiguous_growth(dense->extent(),5*unit,6*unit);
    dense->write_exact(5*unit+19,late);
    dense->flush();
    require(io->extent()==occupied,"reuse extended an already full ordinary backing");
    Bytes actual(3*unit),range(unit);
    dense->read_exact(0,actual);
    dense->read_exact(5*unit,range);
    auto expected=early;
    std::fill_n(expected.begin()+unit,unit,std::byte{});
    Bytes replacement(unit);
    std::copy(late.begin(),late.end(),replacement.begin()+19);
    require(actual==expected && range==replacement,"dense split or partial reuse changed logical bytes");
  }
  auto recovered=detail::DenseVolumeBacking::open(image(io->stable,4096),false);
  const auto late=data(337,63);
  Bytes actual(late.size()),hole(unit);
  recovered->read_exact(remote+31,actual);
  recovered->read_exact(unit,hole);
  require(actual==late && hole==Bytes(unit),"recovered directory lost its mappings");
  rejects([&]{recovered->reclaim_range(0,unit);},"read-only dense inspection retired a range");

  // A single damaged copy is independent at both the anchor and the
  // directory. A writable reopen converges them before publishing reuse.
  auto damaged=image(io->stable,4096);
  damaged->live[0]^=std::byte{1};
  damaged->live[2*unit]^=std::byte{1};
  damaged->stable=damaged->live;
  require(detail::DenseVolumeBacking::recognizes(*damaged),"one lost anchor hid a dense backing");
  {
    auto dense=detail::DenseVolumeBacking::open(damaged,true);
    dense->read_exact(remote+31,actual);
    require(actual==late && !damaged->calls,"dense inspection changed the damaged peers");
    dense->reclaim_range(remote,unit);
    dense->write_exact(6*unit,late);
    dense->flush();
  }
  auto each=image(damaged->stable,4096);
  each->live[unit]^=std::byte{1};
  each->live[3*unit]^=std::byte{1};
  auto repaired=detail::DenseVolumeBacking::open(each,false);
  repaired->read_exact(6*unit,actual);
  require(actual==late && !each->calls,"independently recovered map did not retain repaired peers");
  each->live[2*unit]^=std::byte{1};
  rejects([&]{(void)detail::DenseVolumeBacking::open(each,false);},
      "two unusable directories were treated as reusable space");
}

// Store unchanged zero units implicitly to exercise the on-disk group stride
// with bounded RAM. The reported physical extent still includes those units.
class DenseStrideRam final:public StoreByteIo,public detail::DenseFileTailIo {
 public:
  explicit DenseStrideRam(std::uint64_t limit):limit_(limit){}
  std::size_t calls=0,cut=(std::numeric_limits<std::size_t>::max)();
  std::uint32_t failure_unit=4096;
  unsigned failure=0;
  bool hit=false;
  std::uint64_t extent() const override {return live_.extent;}
  StoreIoContract contract() const noexcept override {return {failure_unit,true,false,true};}
  void admit_write(std::uint64_t end) override {if(end>limit_)throw std::runtime_error("stride fixture capacity");}
  void flush() override {
    const auto fail=interruption();
    if(!fail || failure)stable_=live_;
    if(fail)throw std::runtime_error("stride fixture interrupted flush");
  }
  std::shared_ptr<DenseStrideRam> replay() const {
    auto output=std::make_shared<DenseStrideRam>(limit_);
    output->failure_unit=failure_unit;
    output->live_=output->stable_=stable_;
    return output;
  }
  std::optional<std::uint64_t> allocated_file_bytes(std::uint64_t expected) const override {
    return expected==extent()?std::optional{expected}:std::nullopt;
  }
  void shorten_file(std::uint64_t expected,std::uint64_t retained) override {
    if(expected!=extent() || retained>=expected || retained%kStoreCommitUnit)
      throw std::runtime_error("stride fixture truncation boundary");
    const auto fail=interruption();
    if(!fail || failure) {
      const auto end=fail && failure==2?retained+(expected-retained)/2:retained;
      shorten(live_,end);
      if(fail)shorten(stable_,(std::min)(stable_.extent,end));
    }
    if(fail)throw std::runtime_error("stride fixture interrupted truncate");
  }
  void write_exact(std::uint64_t offset,std::span<const std::byte> input) override {
    admit_write(offset+input.size());
    if(interruption()) {
      if(failure==1) {
        write(stable_,offset,input.first(input.size()/2));
      } else if(failure==2) {
        const auto begin=offset-offset%failure_unit;
        write(stable_,begin,Bytes(failure_unit));
      }
      live_=stable_;
      throw std::runtime_error("stride fixture interrupted write");
    }
    write(live_,offset,input);
  }
  void read_exact(std::uint64_t offset,std::span<std::byte> output) const override {
    if(offset>extent() || output.size()>extent()-offset)throw std::runtime_error("stride fixture read");
    while(!output.empty()) {
      const auto slot=offset/kStoreCommitUnit;
      const auto start=offset%kStoreCommitUnit;
      const auto count=(std::min)(output.size(),kStoreCommitUnit-start);
      const auto found=live_.pages.find(slot);
      if(found==live_.pages.end())std::fill_n(output.begin(),count,std::byte{});
      else std::copy_n(found->second->begin()+static_cast<std::ptrdiff_t>(start),count,output.begin());
      offset+=count;
      output=output.subspan(count);
    }
  }
 private:
  struct Image {
    std::uint64_t extent=0;
    std::map<std::uint64_t,std::shared_ptr<Bytes>> pages;
  };
  bool interruption() noexcept {
    if(calls++!=cut)return false;
    return hit=true;
  }
  static void shorten(Image& image,std::uint64_t end) {
    const auto slot=end/kStoreCommitUnit;
    const auto from=end%kStoreCommitUnit;
    image.pages.erase(image.pages.lower_bound(slot+(from!=0)),image.pages.end());
    if(from) {
      const auto last=image.pages.find(slot);
      if(last!=image.pages.end()) {
        if(last->second.use_count()!=1)last->second=std::make_shared<Bytes>(*last->second);
        std::fill(last->second->begin()+static_cast<std::ptrdiff_t>(from),last->second->end(),std::byte{});
      }
    }
    image.extent=end;
  }
  static void write(Image& image,std::uint64_t offset,std::span<const std::byte> input) {
    image.extent=(std::max)(image.extent,offset+input.size());
    while(!input.empty()) {
      const auto slot=offset/kStoreCommitUnit;
      const auto start=offset%kStoreCommitUnit;
      const auto count=(std::min)(input.size(),kStoreCommitUnit-start);
      const auto portion=input.first(count);
      const auto entry=image.pages.find(slot);
      if(entry!=image.pages.end() || std::any_of(portion.begin(),portion.end(),[](auto b){return b!=std::byte{};})) {
        auto& page=image.pages[slot];
        if(!page)page=std::make_shared<Bytes>(kStoreCommitUnit);
        else if(page.use_count()!=1)page=std::make_shared<Bytes>(*page);
        std::copy(portion.begin(),portion.end(),page->begin()+static_cast<std::ptrdiff_t>(start));
      }
      offset+=count;
      input=input.subspan(count);
    }
  }
  std::uint64_t limit_;
  Image live_,stable_;
};

void dense_group_boundary() {
  constexpr auto unit=kStoreCommitUnit;
  constexpr std::uint64_t group_slots=4091;
  constexpr auto cap=(2+2+group_slots+2+1)*unit;
  const auto first=data(unit,167),second=data(unit,168);
  auto file=std::make_shared<DenseStrideRam>(cap);
  {
    auto dense=detail::DenseVolumeBacking::create(file,id(70),cap,cap);
    const Bytes zeros(unit);
    for(std::uint64_t i=0;i<group_slots-1;++i)dense->write_exact(i*unit,zeros);
    auto pair=first;
    pair.insert(pair.end(),second.begin(),second.end());
    dense->write_exact((group_slots-1)*unit,pair);
    dense->flush();
    require(file->extent()==cap,"group transition miscounted its metadata or payload");
  }
  {
    auto dense=detail::DenseVolumeBacking::open(file,true);
    Bytes pair(2*unit);
    dense->read_exact((group_slots-1)*unit,pair);
    require(std::equal(first.begin(),first.end(),pair.begin()) &&
        std::equal(second.begin(),second.end(),pair.begin()+unit),"read crossed a physical directory boundary");
    dense->reclaim_range((group_slots-1)*unit,2*unit);
    dense->flush();
    dense->write_exact((group_slots+1)*unit,second);
    dense->flush();
    require(file->extent()==cap && !dense->allocated_extent((group_slots-1)*unit,2*unit),
        "cross-group retire and reuse changed its outer extent or retained a retired owner");
    require(dense->advance_tail(4*unit) && file->extent()==cap-3*unit,
        "empty outer group did not release its directory units and data suffix");
    dense->write_exact((group_slots+2)*unit,first);
    dense->flush();
    require(file->extent()==cap,"discarded group did not rebuild durable ownership before reuse");
  }
  auto recovered=detail::DenseVolumeBacking::open(file,false);
  Bytes checked(unit);
  recovered->read_exact((group_slots+1)*unit,checked);
  require(checked==second,"reopened two-group volume could not address a reused run");
  recovered->read_exact((group_slots+2)*unit,checked);
  require(checked==first,"newly rebuilt directory did not bind its recovered data");
}

void paired_image_admission() {
  constexpr auto unit=kStoreCommitUnit;
  constexpr auto minimum_bytes=kNativeVolumeFileImageOffset+2*(kVolumeHeadBytes+2*unit);
  require(!NativeVolumeFile::paired_image_limits(0,false) &&
      !NativeVolumeFile::paired_image_limits(0,true),"an empty backing admitted paired ranges");
  require(NativeVolumeFile::paired_image_limits(minimum_bytes,false).has_value() &&
      !NativeVolumeFile::paired_image_limits(minimum_bytes,true),
      "paired capacity did not charge packed anchors and directories");
  for(const bool packed:{false,true}) {
    // Exercise both sides of catalog admission, allocation-group rollover and
    // large file limits without allocating the planned volume.
    for(const auto image_units:{4ULL,7ULL,255ULL,256ULL,257ULL,2048ULL,67108864ULL}) {
      const auto catalog_units=image_units/32>=8?image_units/32:0;
      const auto virtual_units=kNativeVolumeFileImageOffset/unit+2*(image_units+catalog_units);
      const auto directories=2*((virtual_units+4090)/4091);
      const auto backing=(virtual_units+(packed?2+directories:0))*unit;
      const auto exact=NativeVolumeFile::paired_image_limits(backing,packed);
      const auto short_backing=NativeVolumeFile::paired_image_limits(backing-1,packed);
      require(exact && exact->maximum_physical_bytes==image_units*unit &&
          (!short_backing || short_backing->maximum_physical_bytes<exact->maximum_physical_bytes),
          "paired capacity did not respect its exact physical boundary");
    }
    const auto largest=NativeVolumeFile::paired_image_limits(UINT64_MAX,packed);
    require(largest && largest->maximum_physical_bytes%unit==0 &&
        largest->maximum_physical_bytes<UINT64_MAX/2,"paired capacity overflowed the file extent");
  }
}

void dense_cross_group_moves() {
  constexpr auto unit=kStoreCommitUnit;
  constexpr std::uint64_t group_slots=4091;
  constexpr auto group_end=(4+group_slots)*unit;
  constexpr auto source=group_end+2*unit, destination=17*unit;
  constexpr auto capacity=source+2*unit;
  constexpr auto logical=(group_slots-1)*unit;
  constexpr auto quantum=4*unit;
  const auto payload=data(3*unit,6037);
  const Bytes zero(unit);
  std::uint64_t move_cuts=0,recovery_cuts=0;
  for(const auto unit_bytes:{4096U,65536U}) {
    auto seed=std::make_shared<DenseStrideRam>(capacity);
    seed->failure_unit=unit_bytes;
    {
      auto dense=detail::DenseVolumeBacking::create(seed,id(70),capacity,capacity);
      for(std::uint64_t i=0;i<group_slots-1;++i)dense->write_exact(i*unit,zero);
      dense->write_exact(logical,payload);
      dense->flush();
      dense->reclaim_range(13*unit,2*unit);
      dense->flush();
      require(dense->file_space().interior_reusable_bytes==2*unit && seed->extent()==capacity,
          "cross-group fixture did not retain an older hole and a live tail group");
    }
    const auto verify=[&](const detail::DenseVolumeBacking& view) {
      Bytes actual(payload.size());
      view.read_exact(logical,actual);
      require(actual==payload,"cross-group transition changed acknowledged bytes");
      view.read_exact(13*unit,std::span(actual).first(2*unit));
      require(std::all_of(actual.begin(),actual.begin()+2*unit,[](auto b){return b==std::byte{};}),
          "cross-group recovery reassigned retired virtual offsets");
    };
    const auto finish=[&](const std::shared_ptr<DenseStrideRam>& trial) {
      auto durable=trial->replay();
      {
        const auto reader=detail::DenseVolumeBacking::open(durable,false);
        verify(*reader);
        require(!durable->calls,"independent cross-group inspection wrote to its source");
      }
      {
        auto writer=detail::DenseVolumeBacking::open(durable,true);
        for(unsigned i=0;writer->tail_work_pending(quantum);++i) {
          require(i<3 && writer->advance_tail(quantum),"recovered cross-group retirement did not settle");
          verify(*writer);
        }
        require(durable->extent()==group_end && !writer->file_space().interior_reusable_bytes,
            "cross-group move did not release the complete outer group");
        Bytes copied(2*unit);
        durable->read_exact(destination,copied);
        require(std::equal(copied.begin(),copied.end(),payload.begin()+unit),
            "released tail did not leave its live data in the earlier hole");
        writer->write_exact(logical+payload.size(),std::span(payload).first(unit));
        writer->flush();
      }
      auto reopened=detail::DenseVolumeBacking::open(durable->replay(),false);
      verify(*reopened);
      Bytes appended(unit);
      reopened->read_exact(logical+payload.size(),appended);
      require(std::equal(appended.begin(),appended.end(),payload.begin()),"cross-group recovery did not continue writing");
    };
    const auto pair=[&](const std::shared_ptr<DenseStrideRam>& trial) {
      auto dense=detail::DenseVolumeBacking::open(trial,true);
      try {
        require(dense->advance_tail(quantum),"cross-group move was not admitted");
        verify(*dense);
        require(dense->advance_tail(quantum) && trial->extent()==group_end,"cross-group tail did not shorten");
        return true;
      }catch(const std::runtime_error&) {
        if(!trial->hit)throw;
        rejects([&]{dense->flush();},"interrupted cross-group move admitted writes before reopen");
        verify(*dense);
        return false;
      }
    };
    auto control=seed->replay();
    require(pair(control),"cross-group control failed");
    const auto boundaries=control->calls;
    require(boundaries==21,"cross-group control missed a data, anchor, directory or length boundary");
    {
      auto repeated=control->replay();
      auto dense=detail::DenseVolumeBacking::open(repeated,true);
      dense->write_exact(logical+payload.size(),std::span(payload).first(unit));
      dense->reclaim_range(20*unit,unit);
      dense->flush();
      require(dense->advance_tail(quantum) && dense->advance_tail(quantum) && repeated->extent()==group_end,
          "settled version-2 allocation could not execute a second move");
      std::byte sequence{};
      repeated->read_exact(80,{&sequence,1});
      require(sequence==std::byte{4},"repeated cross-group relocation lost its authority sequence");
      verify(*detail::DenseVolumeBacking::open(repeated->replay(),false));
    }
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut) {
      auto trial=seed->replay();
      trial->cut=cut;
      trial->failure=mode;
      require(!pair(trial),"cross-group interruption was not observed");
      finish(trial);
      ++move_cuts;
    }

    auto mixed=seed->replay();
    mixed->cut=5; // One published version-2 peer with the other still version 1.
    require(!pair(mixed),"mixed-version restart fixture did not interrupt");
    auto partial=mixed->replay();
    std::array<std::byte,80> old_anchor{},new_anchor{};
    partial->read_exact(0,old_anchor);
    partial->read_exact(unit,new_anchor);
    const auto digest=detail::record_digest(std::span(new_anchor).first(48));
    require(old_anchor[8]==std::byte{1} && new_anchor[8]==std::byte{2} &&
        std::equal(digest.begin(),digest.end(),new_anchor.begin()+48),
        "legacy readers could miss the newer format marker during upgrade");
    finish(partial);

    auto committed=seed->replay();
    committed->cut=7; // Both intent peers have settled and directories are about to change.
    require(!pair(committed),"pending recovery fixture did not interrupt");
    auto foreign=committed->replay();
    std::array<std::byte,96> directory{};
    foreign->read_exact(group_end,directory);
    directory[80]^=std::byte{1};
    const auto changed=detail::record_digest(std::span(directory).first(48),std::span(directory).subspan(80));
    std::copy(changed.begin(),changed.end(),directory.begin()+48);
    foreign->write_exact(group_end,directory);
    foreign->flush();
    rejects([&]{(void)detail::DenseVolumeBacking::open(foreign,true);},
        "a valid but unrelated directory was accepted under a relocation intent");

    auto damaged=committed->replay();
    const std::array wrong{payload[unit]^std::byte{1}};
    damaged->write_exact(destination,wrong);
    damaged->flush();
    verify(*detail::DenseVolumeBacking::open(damaged,false));
    auto lost=damaged->replay();
    lost->write_exact(source,wrong);
    lost->flush();
    rejects([&]{(void)detail::DenseVolumeBacking::open(lost,false);},"recovery accepted two damaged relocation copies");

    const auto repair=[&](const std::shared_ptr<DenseStrideRam>& trial) {
      auto dense=detail::DenseVolumeBacking::open(trial,true);
      try {
        require(dense->advance_tail(quantum) && trial->extent()==capacity,
            "intent repair tried to shorten before settling");
        return true;
      }catch(const std::runtime_error&) {
        if(!trial->hit)throw;
        verify(*dense);
        return false;
      }
    };
    auto repaired=damaged->replay();
    require(repair(repaired),"verified original did not repair its interrupted destination");
    const auto repair_boundaries=repaired->calls;
    require(repair_boundaries==15,"repair control missed durable recovery boundaries");
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<repair_boundaries;++cut) {
      auto trial=damaged->replay();
      trial->cut=cut;
      trial->failure=mode;
      require(!repair(trial),"cross-group recovery interruption was not observed");
      finish(trial);
      ++recovery_cuts;
    }
  }
  std::cout<<"dense_cross_group move_cuts="<<move_cuts<<" repair_cuts="<<recovery_cuts
      <<" released_bytes="<<capacity-group_end<<" continued_writes=1 legacy_transition_checked=1\n";
}

constexpr PagedStoreLimits cross_fixture_limits{384ULL*1024*1024};
constexpr auto cross_fixture_group_end=(4+4091ULL)*kStoreCommitUnit;
constexpr auto cross_fixture_second_payload=cross_fixture_group_end+2*kStoreCommitUnit;
constexpr auto cross_fixture_payload_bytes=2U*1024*1024;

struct NativeCrossFixture {
  Bytes expected;
  NativeDenseFileSpace before,after;
  std::uint64_t peak=0;
  unsigned overwrites=0,steps=0;
};

NativeCrossFixture advance_native_cross_group(NativeFileSession& session) {
  NativeCrossFixture result;
  auto& [expected,before,after,peak,overwrites,steps]=result;
  expected=data(cross_fixture_payload_bytes,16043);
  put(session,"payload",expected);
  put(session,"witness",witness_bytes);
  auto size=session.dense_file_space().value().file_end;
  while(size<=cross_fixture_second_payload) {
    require(++overwrites<100,"bounded native write history did not reach a second group");
    for(std::size_t at=0;at<expected.size();at+=kNativeExtentBytes)
      expected[at]^=std::byte{1};
    update(session,expected);
    size=session.dense_file_space().value().file_end;
  }
  require(session.persistent_reclamation_pending(),"native history did not admit a replacement image");
  {
    const auto old=session.snapshot();
    require(stage_compaction(session)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,"native image switch failed across groups");
    require(session.advance_persistent_reclamation({})==Reclaim::pinned &&
        old.read("payload",0,cross_fixture_payload_bytes)==expected,"old image was retired while its snapshot was pinned");
  }
  peak=session.dense_file_space().value().file_end;
  require(peak>size,"replacement image was not appended beyond the first group's history");
  for(unsigned i=0;session.persistent_reclamation_pending();++i) {
    require(i<cross_fixture_limits.maximum_physical_bytes/kStoreMaxTransactionBytes &&
        session.advance_persistent_reclamation({})==Reclaim::retiring,"native source retirement did not settle");
  }
  before=session.dense_file_space().value();
  require(before.allocated_file_bytes && before.file_end>cross_fixture_second_payload &&
      before.interior_reusable_bytes>cross_fixture_group_end/2,
      "native rotation did not release reusable holes before the selected image");
  const auto held=session.snapshot();
  while(session.persistent_reclamation_pending(true)) {
    const auto progress=session.advance_persistent_reclamation({},true);
    if(progress==Reclaim::idle)break;
    require(progress==Reclaim::retiring && ++steps<2*cross_fixture_limits.maximum_physical_bytes/kStoreMaxTransactionBytes,
        "cross-group maintenance exceeded the fixture's volume bounds");
    require(held.read("witness",0,1000)==witness_bytes,"held witness changed during cross-group retirement");
  }
  after=session.dense_file_space().value();
  require(after.file_end<cross_fixture_group_end && after.owned_payload_bytes==before.owned_payload_bytes &&
      after.allocated_file_bytes && *after.allocated_file_bytes<*before.allocated_file_bytes &&
      held.read("payload",0,cross_fixture_payload_bytes)==expected,"native cross-group retirement changed data or kept the old allocation");
  return result;
}

void append_cross_fixture_edit(NativeFileSession& writer,Bytes& expected) {
  const auto handle=writer.open("payload",{true,true,false});
  expected.front()^=std::byte{2};
  writer.write(handle,0,std::span(expected).first(1));
  writer.cleanup(handle,false);
  writer.close(handle);
  writer.flush();
}

void verify_cross_fixture(const NativeVolumeHeadView& reopened,const Bytes& expected) {
  reopened.filesystem().verify_all();
  require(reopened.info().selection.epoch==2 && reopened.filesystem().read("payload",0,cross_fixture_payload_bytes)==expected &&
      reopened.filesystem().read("witness",0,1000)==witness_bytes,"upgraded backing lost data after independent reopen");
}

void report_cross_fixture(const char* label,const NativeCrossFixture& state) {
  const auto& before=state.before;
  const auto& after=state.after;
  std::cout<<label<<" peak_eof="<<state.peak<<" before_eof="<<before.file_end<<" after_eof="<<after.file_end
      <<" before_allocation="<<*before.allocated_file_bytes<<" after_allocation="<<*after.allocated_file_bytes
      <<" overwritten="<<state.overwrites<<" steps="<<state.steps
      <<" independent_reopen=1 continued_write=1 held_reader=1 upgraded_anchors=2\n";
}

void modeled_native_dense_cross_file() {
  constexpr auto limit=cross_fixture_limits.maximum_physical_bytes;
  auto backing=std::make_shared<DenseStrideRam>(limit);
  NativeCrossFixture result;
  {
    auto session=NativeVolumeFile::create_dense(backing,limit,id(61),id(62),cross_fixture_limits,raw);
    result=advance_native_cross_group(session);
  }
  std::byte a{},b{};
  backing->read_exact(8,{&a,1});
  backing->read_exact(kStoreCommitUnit+8,{&b,1});
  require(a==std::byte{2} && b==a,"modeled native backing did not settle both upgraded anchors");
  verify_cross_fixture(NativeVolumeFile::inspect(backing->replay(),id(61)),result.expected);
  {
    auto writer=NativeVolumeFile::resume(backing,id(61),raw);
    append_cross_fixture_edit(writer,result.expected);
  }
  verify_cross_fixture(NativeVolumeFile::inspect(backing->replay(),id(61)),result.expected);
  report_cross_fixture("modeled_native_dense_cross",result);
}

void dense_directory_cuts() {
  constexpr auto unit=kStoreCommitUnit;
  const auto first=data(3*unit,99),replacement=data(1231,118);
  auto seed=empty(4096);
  {
    auto io=detail::DenseVolumeBacking::create(seed,id(70),16*unit,8*unit);
    io->write_exact(0,first);
    io->flush();
  }
  const auto exchange=[&](const std::shared_ptr<Ram>& backing) {
    auto io=detail::DenseVolumeBacking::open(backing,true);
    try {
      io->reclaim_range(unit,unit);
      io->write_exact(8*unit+54,replacement);
      io->flush();
      return true;
    } catch(const std::runtime_error&) {
      rejects([&]{io->flush();},"uncertain directory publication admitted another flush");
      return false;
    }
  };
  auto trial=image(seed->stable,4096);
  require(exchange(trial) && trial->extent()==seed->extent(),"control transaction did not reuse its range");
  const auto boundaries=trial->calls;
  require(boundaries>=10 && boundaries<32,"directory exchange did not cover its publication barriers");
  for(unsigned failure=0;failure<3;++failure) {
    for(std::size_t cut=0;cut<boundaries;++cut) {
      auto backing=image(seed->stable,4096);
      backing->fail_at=cut;
      backing->failure=failure;
      require(!exchange(backing),"directory cut was not observed");
      auto reopened=image(backing->stable,4096);
      auto reader=detail::DenseVolumeBacking::open(reopened,false);
      Bytes actual(3*unit);
      reader->read_exact(0,actual);
      require(std::equal(actual.begin(),actual.begin()+unit,first.begin()) &&
          std::equal(actual.begin()+2*unit,actual.end(),first.begin()+2*unit),
          "directory interruption lost a live extent");
      if (reader->allocated_extent(unit,unit)) {
        require(actual==first && !reader->allocated_extent(8*unit,unit),
            "old directory owner aliased reused data");
      } else {
        require(std::all_of(actual.begin()+unit,actual.begin()+2*unit,[](std::byte b){return b==std::byte{};}),
            "retired logical data remained reachable");
      }
      if (reader->allocated_extent(8*unit,unit)) {
        Bytes checked(unit),expected(unit);
        std::copy(replacement.begin(),replacement.end(),expected.begin()+54);
        reader->read_exact(8*unit,checked);
        require(checked==expected,"directory became durable before its replacement data");
      }
      require(!reopened->calls,"fault recovery wrote during CPU-only inspection");
    }
  }
  std::cout<<"dense_directory_cuts boundaries="<<boundaries<<" injected_failures="<<3*boundaries<<'\n';
}

void dense_session_reuse() {
  auto backing=empty(4096);
  Bytes expected;
  {
    auto session=NativeVolumeFile::create_dense(backing,backing->capacity,id(90),id(91),limits,raw);
    expected=establish_history(session);
    const auto original=backing->extent();
    require(original<limits.maximum_physical_bytes,"dense initial image did not pack into its file");
    {
      auto captured=session.snapshot();
      require(stage_compaction(session)==Reclaim::ready &&
          session.advance_persistent_reclamation({})==Reclaim::published,
          "dense session could not publish a compacted image");
      require(backing->extent()<limits.maximum_physical_bytes &&
          session.advance_persistent_reclamation({})==Reclaim::pinned &&
          captured.read("payload",0,50000)==expected,"dense storage lost the retained image or allocated a logical gap");
      rejects([&]{(void)NativeVolumeFile::inspect(backing,id(90));},"dense CPU reopen bypassed the live lease");
    }
    release_inactive(session);
  }
  auto restarted=image(backing->stable,4096);
  const auto high_water=restarted->extent();
  {
    auto session=NativeVolumeFile::resume(restarted,id(90),raw);
    for(unsigned i=0;i<12 && !session.persistent_reclamation_pending();++i) {
      expected=data(12289,698+i);
      update(session,expected);
    }
    require(stage_compaction(session,94)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,
        "dense session could not recycle retired physical slots");
    release_inactive(session);
    require(session.volume_info().selection.epoch==3 &&
        restarted->extent()<=high_water+kStoreCommitUnit,
        "recycled dense session kept extending its ordinary backing");
  }
  auto final=image(restarted->stable,4096);
  const auto independent=NativeVolumeFile::inspect(final,id(90));
  independent.filesystem().verify_all();
  require(independent.filesystem().read("payload",0,50000)==expected &&
      independent.filesystem().read("witness",0,1000)==witness_bytes && !final->calls,
      "independent dense reopen lost acknowledged data");
}

void dense_seal_workspace_alignment() {
  if(!store_zstd_available())return;
  constexpr std::uint64_t capacity=96ULL*1024*1024;
  const auto file=std::make_shared<DenseTailRam>();
  file->bytes.failure_unit=4096;
  file->bytes.capacity=capacity;
  const StoreCompressionOptions codec{StoreCompressionMode::fast,1,false};
  auto expected=cold_test_payload();
  const auto sibling=cold_test_payload(130011);
  const Bytes sparse(640*1024);
  using Mode=NativeVolumeReclaimMode;
  std::uint64_t initial=0,settled=0;
  {
    auto session=NativeVolumeFile::create_dense(file,capacity,id(70),id(71),{32ULL*1024*1024},codec);
    put(session,"payload",expected);
    put(session,"sibling",sibling);
    put(session,"sparse",sparse);
    for(unsigned i=0;i<60;++i) {
      const auto change=data(3072,9300+i);
      const auto offset=i*4096;
      const auto h=session.open("payload",{true,true,false});
      session.write(h,offset,change);
      session.cleanup(h,false);
      session.close(h);
      std::copy(change.begin(),change.end(),expected.begin()+offset);
    }
    initial=file->extent();
    unsigned next=72,published=0;
    for(unsigned step=0;step<6000 && session.persistent_reclamation_pending(Mode::include_cold);++step) {
      const auto mode=session.persistent_reclamation_pending(Mode::shorten_packed_tail)?
          Mode::shorten_packed_tail:Mode::include_cold;
      const auto progress=session.advance_persistent_reclamation([&]{return id(next++);},mode);
      require(progress!=Reclaim::failed,"fitted cold-seal workspace did not admit its checked footprint");
      published+=progress==Reclaim::published;
    }
    settled=file->extent();
    require(published==2 && session.volume_info().selection.sealed_range &&
        session.volume_info().selection.epoch==3 && settled<initial &&
        !session.persistent_reclamation_pending(Mode::include_cold),"packed cold sealing did not complete and retire");
  }
  const auto checked=NativeVolumeFile::inspect(DenseTailRam::replay(*file),id(70));
  const auto& view=checked.filesystem();
  view.verify_all();
  require(view.read("payload",0,static_cast<std::uint32_t>(expected.size()))==expected &&
      view.read("sibling",0,static_cast<std::uint32_t>(sibling.size()))==sibling &&
      view.read("sparse",0,static_cast<std::uint32_t>(sparse.size()))==sparse,
      "aligned sealing changed acknowledged files");
  std::cout<<"dense_aligned_seal before="<<initial<<" settled="<<settled<<" epoch=3 cpu_readback=exact\n";
}

void dense_session_cuts() {
  unsigned interrupted=0;
  for(const auto unit:{4096U,65536U}) {
    auto seed=empty(unit);
    Bytes expected;
    {
      auto session=NativeVolumeFile::create_dense(seed,limits.maximum_physical_bytes,id(90),id(91),limits,raw);
      expected=establish_history(session);
    }
    const auto rotate=[&](std::optional<std::size_t> cut,unsigned failure) {
      auto file=image(seed->stable,unit);
      std::size_t boundary=0;
      {
        auto session=NativeVolumeFile::resume(file,id(90),raw);
        boundary=file->calls;
        if(cut) {
          file->fail_at=boundary+*cut;
          file->failure=failure;
        }
        try {
          const bool ready=stage_compaction(session)==Reclaim::ready;
          const bool published=ready && session.advance_persistent_reclamation({})==Reclaim::published;
          if(published)release_inactive(session);
          if(!cut)require(published,"dense control failed to publish its alternate image");
        } catch(const std::runtime_error&) {
          if(!cut)throw;
        }
      }
      if(cut)require(file->calls>file->fail_at,"dense candidate did not reach the requested failure cut");
      auto durable=image(file->stable,unit);
      {
        const auto reader=NativeVolumeFile::inspect(durable,id(90));
        require((reader.info().selection.epoch==1 || reader.info().selection.epoch==2) &&
            reader.filesystem().read("payload",0,50000)==expected &&
            reader.filesystem().read("witness",0,1000)==witness_bytes,
            "dense recovery lost acknowledged bytes during image publication");
      }
      {
        auto resumed=NativeVolumeFile::resume(durable,id(90),raw);
        const auto replacement=data(expected.size(),492);
        update(resumed,replacement);
        resumed.flush();
        require(resumed.snapshot().read("payload",0,50000)==replacement,
            "dense recovery could not continue writing");
      }
      return file->calls-boundary;
    };
    const auto operations=rotate({},0);
    require(operations>16 && operations<150,"dense rotation did not cover its bounded directory and selector I/O");
    for(unsigned failure=0;failure<3;++failure)
      for(std::size_t at=0;at<operations;++at) {
        (void)rotate(at,failure);
        ++interrupted;
      }
    std::cout<<"dense_session_cuts unit="<<unit<<" boundaries="<<operations<<'\n';
  }
  std::cout<<"dense_session_interruption_cases "<<interrupted<<'\n';
}

void dense_tail_retirement() {
  constexpr auto unit=kStoreCommitUnit;
  const auto live=data(6*unit,401),witness=data(unit,402);
  auto source=std::make_shared<DenseTailRam>();
  source->bytes.failure_unit=4096;
  {
    auto dense=detail::DenseVolumeBacking::create(source,id(90),32*unit,16*unit);
    dense->write_exact(0,live);
    dense->write_exact(24*unit,witness);
    dense->flush();
    dense->reclaim_range(unit,2*unit);
    require(!dense->tail_work_pending(unit),"uncommitted directory retirement authorized physical reuse");
    dense->flush();
    const auto space=dense->file_space();
    require(space.file_end==11*unit && space.owned_payload_bytes==5*unit &&
        space.interior_reusable_bytes==2*unit && !space.retired_tail_bytes &&
        space.allocated_file_bytes==space.file_end,"dense accounting confused interior holes and outer extent");
  }
  const auto check=[&](const detail::DenseVolumeBacking& backing) {
    Bytes content(6*unit),last(unit),expected=live;
    std::fill_n(expected.begin()+unit,2*unit,std::byte{});
    backing.read_exact(0,content);
    backing.read_exact(24*unit,last);
    require(content==expected && last==witness,"dense tail work changed owned data or reclaimed holes");
  };
  const auto turn=[&](const std::shared_ptr<DenseTailRam>& file,bool verify_progress) {
    auto dense=detail::DenseVolumeBacking::open(file,true);
    try {
      require(dense->tail_work_pending(unit) && dense->advance_tail(unit),"tail copy was not admitted");
      const auto moved=dense->file_space();
      require(moved.file_end==11*unit && moved.retired_tail_bytes==unit &&
        moved.interior_reusable_bytes==unit,"relocating one unit did not release the correct tail");
      if(verify_progress)check(*dense);
      require(dense->advance_tail(unit),"published move did not admit suffix release");
      const auto shortened=dense->file_space();
      require(shortened.file_end==10*unit && !shortened.retired_tail_bytes &&
          shortened.allocated_file_bytes==10*unit && shortened.owned_payload_bytes==5*unit,
          "prefix-preserving truncate did not reduce the backing file");
      return true;
    }catch(const std::runtime_error&) {
      if(verify_progress)throw;
      rejects([&]{dense->flush();},"uncertain tail operation admitted another write");
      check(*dense);
      return false;
    }
  };
  const auto control=DenseTailRam::replay(*source);
  require(turn(control,true),"dense tail control failed");
  const auto boundaries=control->bytes.calls;
  require(boundaries>=9 && boundaries<16,"tail recovery matrix missed publication or shortening boundaries");
  for(unsigned mode=0;mode<3;++mode)
    for(std::size_t cut=0;cut<boundaries;++cut) {
      auto trial=DenseTailRam::replay(*source);
      trial->bytes.fail_at=cut;
      trial->bytes.failure=mode;
      require(!turn(trial,false),"dense tail cut was not observed");
      auto recovered=DenseTailRam::replay(*trial);
      check(*detail::DenseVolumeBacking::open(recovered,false));
      {
        auto dense=detail::DenseVolumeBacking::open(recovered,true);
        for(unsigned i=0;i<6 && dense->tail_work_pending(unit);++i)
          require(dense->advance_tail(unit),"recovered ownership did not continue tail work");
        require(dense->file_space().file_end==9*unit,"continued evacuation did not pack the live prefix");
        check(*dense);
        dense->write_exact(25*unit,witness);
        dense->flush();
      }
      check(*detail::DenseVolumeBacking::open(DenseTailRam::replay(*recovered),false));
    }
  {
    auto neutral=DenseTailRam::replay(*source);
    neutral->truncate_qualified=false;
    auto dense=detail::DenseVolumeBacking::open(neutral,true);
    require(!dense->tail_work_pending(unit) && !dense->advance_tail(unit) && !neutral->bytes.calls,
        "ordinary flush support alone authorized dense shortening");
  }
  {
    auto mismatched=DenseTailRam::replay(*source);
    mismatched->bytes.corrupt_read_offset=5*unit;
    mismatched->bytes.corrupt_read_number=1;
    auto dense=detail::DenseVolumeBacking::open(mismatched,true);
    rejects([&]{(void)dense->advance_tail(unit);},"different readback published a new physical owner");
    require(mismatched->bytes.writes==1,"readback disagreement rewrote ownership metadata");
    check(*detail::DenseVolumeBacking::open(DenseTailRam::replay(*mismatched),false));
  }
  std::cout<<"dense_tail_interruption boundaries="<<boundaries<<" cuts="<<boundaries*3
      <<" retained_prefix_verified=1\n";
}

unsigned scratch_leases=0;
class ModelSealScratch final:public detail::NativeSealScratch {
 public:
  ModelSealScratch(std::uint64_t capacity,StoreIoContract storage) {
    auto model=std::make_shared<Ram>();
    model->capacity=capacity;
    model->failure_unit=storage.write_failure_unit_bytes;
    store_=std::move(model);
    ++scratch_leases;
  }
  ~ModelSealScratch() override{--scratch_leases;}
  const std::shared_ptr<StoreByteIo>& store() const noexcept override{return store_;}
  bool retire_step() override {
    if(store_.use_count()!=1)return false;
    store_.reset();
    return true;
  }
 private:
  std::shared_ptr<StoreByteIo> store_;
};
std::unique_ptr<detail::NativeSealScratch> model_seal_scratch(std::uint64_t bytes,StoreIoContract storage) noexcept {
  try{return std::make_unique<ModelSealScratch>(bytes,storage);}
  catch(const std::bad_alloc&){return {};}
}

void stage_file_seal(NativeFileSession& session,StoreIdentity next,detail::NativeSealScratchOffer scratch={}) {
  const auto reclaim=session.persistent_reclamation_pending();
  const auto optional=session.persistent_reclamation_pending(true);
  if(reclaim || !optional) {
    const auto space=session.indexed_volume_space();
    const auto input=session.workload_info();
    const auto advice=session.optimization_info();
    std::cerr<<"paired seal admission reclaim="<<reclaim<<" optional="<<optional
        <<" occupied="<<space.total_bytes-space.free_bytes<<" obsolete="<<space.obsolete_bytes
        <<" input_enabled="<<input.enabled<<" input_bytes="<<input.committed_input_bytes
        <<" advice_phase="<<static_cast<unsigned>(advice.phase)<<" advice_sequence="<<advice.sequence<<'\n';
  }
  require(!reclaim && optional,
      "append-only input did not fund optional paired sealing");
  require(session.advance_persistent_reclamation([&]{return next;},true,scratch)==Reclaim::copying,
      "paired sealing did not reserve private preparation");
  for(unsigned turn=0;turn<4096;++turn) {
    const auto progress=session.advance_persistent_reclamation({},true);
    if(progress==Reclaim::ready)return;
    require(progress==Reclaim::copying,"paired sealing did not reach its checked selection");
  }
  throw std::runtime_error("paired sealing exceeded the fixture's advance limit");
}

void sealed_ranges(bool volatile_workspace) {
  const auto payload=data(2'861'348,167);
  const Bytes sparse(2*1024*1024);
  const auto size=static_cast<std::uint32_t>(payload.size());
  const auto volume=id(70),first=id(71),second=id(72),third=id(73);
  for(const auto unit:{4096U,65536U}) {
    const PagedStoreLimits plan{(unit==4096?8ULL:16ULL)*1024*1024};
    const auto slot=plan.maximum_physical_bytes;
    const auto offer=volatile_workspace?detail::NativeSealScratchOffer{slot,model_seal_scratch}:
        detail::NativeSealScratchOffer{};
    const auto older=kNativeVolumeFileImageOffset,newer=older+slot;
    auto seed=std::make_shared<SparseVolumeRam>(unit,2*slot+2*1024*1024);
    {
      auto session=NativeVolumeFile::create(seed,volume,first,plan,raw);
      put(session,"payload",payload);
      put(session,"witness",witness_bytes);
      require(!session.volume_info().selection.sealed_range,"initial layout was not the historical mirror");
      require(!session.persistent_reclamation_pending(true),"unfunded sealing started a persistent rewrite");
      put(session,"sparse",sparse);
    }
    const auto before=seed->occupied();
    auto io=seed->replay();
    std::size_t selection_calls=0;
    std::uint64_t after=0,backing_writes=0;
    {
      auto session=NativeVolumeFile::resume(io,volume,raw);
      const auto handle=session.open("payload",{true,true,false});
      std::optional<NativeFilesystemSnapshot> held(session.snapshot());
      stage_file_seal(session,second,offer);
      require(bool(io->range_reclaims)!=volatile_workspace && scratch_leases==0 &&
          session.volume_info().selection.epoch==1 &&
          NativeVolumeFile::inspect(io->replay(),volume).filesystem().read("payload",0,size)==payload,
          "private workspace retirement affected the selected source");
      const auto start=io->io.calls;
      require(session.advance_persistent_reclamation({},true)==Reclaim::published,"sealed file was not selected");
      selection_calls=io->io.calls-start;
      backing_writes=io->io.write_bytes;
      const auto selected=session.volume_info();
      require(selected.selection.epoch==2 && selected.selection.image_identity==second &&
          selected.selection.sealed_range && !selected.degraded && selection_calls==5,
          "paired publication did not flush the candidate and then bind it in two ordered records");
      require(session.read(handle,0,size)==payload && held->read("payload",0,size)==payload,
          "selection changed a live handle or retained snapshot");
      require(session.advance_persistent_reclamation({})==Reclaim::pinned &&
          io->allocated_extent(older,slot)!=0,"old image retired while its reader was pinned");
      held.reset();
      release_inactive(session);
      after=io->occupied();
      require(!io->allocated_extent(older,slot) && after<before &&
          session.snapshot().read("witness",0,5000)==witness_bytes,
          "sealing did not return physical model allocation or preserve the witness");
      session.cleanup(handle,false);
      session.close(handle);
    }
    // The ordinary byte reader can reopen the selected file without a sparse
    // allocation interface, even though the reported EOF includes discarded workspace.
    const auto portable=image(io->io.stable,unit);
    const auto read_only=NativeVolumeFile::inspect(portable,volume);
    require(read_only.info().selection.sealed_range &&
        read_only.filesystem().read("payload",0,size)==payload,"independent reader did not resolve the sealed pair");
    auto mismatched=io->replay();
    const auto roundtrip_base=read_only.info().selection.sealed_range->indexed_end;
    for(unsigned index=0;index<2;++index) {
      auto record=std::span(mismatched->io.stable).subspan(index*kStoreCommitUnit,kStoreCommitUnit);
      const auto changed=roundtrip_base+unit;
      for(unsigned byte=0;byte<8;++byte)record[120+byte]=static_cast<std::byte>(255U&(changed>>(8*byte)));
      const auto digest=detail::record_digest(record.first(record.size()-detail::Digest{}.size()));
      std::ranges::copy(digest,record.end()-digest.size());
    }
    mismatched->io.live=mismatched->io.stable;
    rejects([&]{(void)NativeVolumeFile::inspect(mismatched,volume);},
        "an independently valid selector disagreed with the sealed tail binding");
    require(!mismatched->io.writes,"placement verification changed the backing");

    auto damaged=io->replay();
    const auto stripe=static_cast<std::size_t>(newer+kVolumeHeadBytes);
    damaged->io.stable.at(stripe)^=std::byte{0x80};
    damaged->io.live=damaged->io.stable;
    {
      const auto recovered=NativeVolumeFile::inspect(damaged->replay(),volume);
      require(recovered.info().selection.sealed_range &&
          recovered.filesystem().read("payload",0,size)==payload,"sealed parity did not recover the read-only view");
      auto refused=damaged->replay();
      rejects([&]{(void)NativeVolumeFile::resume(refused,volume,raw);},
          "degraded sealed layout acquired a writer without a repair range");
      require(!refused->io.writes && !refused->range_reclaims,"refused repair changed the source");
    }
    {
      const auto old_span=std::span(damaged->io.stable).subspan(newer,
          static_cast<std::size_t>((std::min)(slot,damaged->io.extent()-newer)));
      const Bytes damaged_source(old_span.begin(),old_span.end());
      auto restored=NativeVolumeFile::resume(damaged,volume,raw,NativeDispositionPolicy::aggregate,[]{return id(74);});
      require(restored.volume_info().selection.epoch==3 && restored.volume_info().selection.image_identity==id(74) &&
          !restored.volume_info().selection.sealed_range &&
          restored.snapshot().read("payload",0,size)==payload &&
          std::ranges::equal(damaged_source,std::span(damaged->io.stable).subspan(newer,damaged_source.size())),
          "sealed startup repair modified the source or lost reconstructed bytes");
      release_inactive(restored);
      require(!damaged->allocated_extent(newer,slot),"repaired volume kept the damaged range allocated");
    }
    require(NativeVolumeFile::inspect(damaged->replay(),volume).filesystem().read("payload",0,size)==payload,
        "independent reader could not reopen the repaired mirror");

    auto obsolete=seed->replay();
    {
      auto session=NativeVolumeFile::resume(obsolete,volume,raw);
      const auto handle=session.open("payload",{true,true,false});
      require(session.advance_persistent_reclamation([&]{return second;},true,offer)==Reclaim::copying,
          "stale source fixture did not admit its workspace");
      for(unsigned turn=0;!obsolete->allocated_extent(newer,slot) && turn<1024;++turn)
        require(session.advance_persistent_reclamation({},true)==Reclaim::copying,
            "stale source fixture did not reach private writes");
      require(obsolete->allocated_extent(newer,slot)!=0,"stale source fixture has no private allocation");
      const std::byte edited_first=payload.front()^std::byte{1};
      session.write(handle,0,{&edited_first,1});
      require(session.advance_persistent_reclamation({},true)==Reclaim::retiring,
          "changed source allowed private preparation to continue");
      release_inactive(session);
      require(session.volume_info().selection.epoch==1 && !obsolete->allocated_extent(newer,slot) && !scratch_leases &&
          session.read(handle,0,1)==Bytes(1,edited_first) && !session.persistent_reclamation_pending(true),
          "retired stale preparation changed selection, data or funding");
      session.cleanup(handle,false);
      session.close(handle);
    }
    if(volatile_workspace) {
      auto pressure=seed->replay();
      {
        auto session=NativeVolumeFile::resume(pressure,volume,raw);
        require(session.advance_persistent_reclamation([&]{return second;},true,offer)==Reclaim::copying &&
            scratch_leases==1,"paired seal pressure fixture did not acquire its workspace");
        for(unsigned turn=0;!pressure->allocated_extent(newer,slot) && turn<1024;++turn)
          require(session.advance_persistent_reclamation({},true)==Reclaim::copying,
              "paired seal pressure fixture did not create a partial durable candidate");
        require(pressure->allocated_extent(newer,slot) && session.shed_persistent_seal_scratch(0) &&
            !scratch_leases && !session.shed_persistent_seal_scratch(0) &&
            session.volume_info().selection.epoch==1,
            "pressure did not abandon the unselected build and release its immediate memory lease");
        release_inactive(session);
        require(!pressure->allocated_extent(newer,slot),"pressure left an inactive disk allocation");
      }
      require(NativeVolumeFile::inspect(pressure,volume).filesystem().read("payload",0,size)==payload,
          "pressure changed the selected bytes");
    }
    auto resumed=io->replay();
    auto edited=payload;
    const auto edit=data(3119,547);
    const auto edit_at=kNativeExtentBytes-19;
    std::ranges::copy(edit,edited.begin()+edit_at);
    const auto shortened=data(117'372,47);
    {
      auto session=NativeVolumeFile::resume(resumed,volume,raw);
      const auto handle=session.open("payload",{true,true,false});
      session.write(handle,edit_at,edit);
      require(session.read(handle,0,size)==edited &&
          NativeVolumeFile::inspect(resumed->replay(),volume).filesystem().read("payload",0,size)==edited,
          "writable reopen did not persist the changed tail");
      std::optional<NativeFilesystemSnapshot> held(session.snapshot());
      session.truncate(handle,0);
      session.write(handle,0,shortened);
      require(stage_compaction(session,73)==Reclaim::ready &&
          session.advance_persistent_reclamation({})==Reclaim::published,"ordinary reclamation did not rotate a sealed source");
      require(session.volume_info().selection.image_identity==third &&
          !session.volume_info().selection.sealed_range && held->read("payload",0,size)==edited &&
          session.advance_persistent_reclamation({})==Reclaim::pinned,"old sealed components lost their common range pin");
      held.reset();
      release_inactive(session);
      require(!resumed->allocated_extent(newer,slot) &&
          session.read(handle,0,static_cast<std::uint32_t>(shortened.size()))==shortened,
          "two-part image retirement changed the new selection");
      session.cleanup(handle,false);
      session.close(handle);
    }
    require(NativeVolumeFile::inspect(resumed->replay(),volume).filesystem().read("payload",0,size)==shortened,
        "mirrored successor after sealed retirement did not reopen");

    std::array<unsigned,2> recovered{};
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<selection_calls;++cut) {
      auto torn=seed->replay();
      auto session=NativeVolumeFile::resume(torn,volume,raw);
      stage_file_seal(session,second,offer);
      const auto start=torn->io.calls;
      torn->io.failure=mode;
      torn->io.fail_at=start+cut;
      if(!cut)
        require(session.advance_persistent_reclamation({},true)==Reclaim::failed &&
            session.volume_info().selection.epoch==1,"candidate flush failure affected the source authority");
      else rejects([&]{(void)session.advance_persistent_reclamation({},true);},"interrupted selector was acknowledged");
      require(session.poisoned()==(cut!=0) && torn->io.calls==start+cut+1,
          "selector continued writing after its uncertain boundary");
      auto stable=torn->replay();
      const auto checked=NativeVolumeFile::inspect(stable->replay(),volume);
      const auto select=checked.info().selection;
      require((select.epoch==1 || select.epoch==2) &&
          select.image_identity==(select.epoch==1?first:second) &&
          select.sealed_range.has_value()==(select.epoch==2) &&
          checked.filesystem().read("payload",0,size)==payload &&
          checked.filesystem().read("witness",0,5000)==witness_bytes &&
          checked.filesystem().read("sparse",sparse.size()-4096,4096)==Bytes(4096),
          "selector cut mixed layouts or lost acknowledged files");
      ++recovered[select.epoch-1];
      auto reopened=NativeVolumeFile::resume(stable,volume,raw);
      release_inactive(reopened);
      require(!reopened.volume_info().degraded && reopened.volume_info().selection==select &&
          !stable->allocated_extent(select.epoch==1?newer:older,slot),"closed recovery did not retire the unselected slot");
      if(mode==0 && (cut==1 || cut==4)) {
        torn->io.live=torn->io.stable; // Reopen the modeled durable bytes on the same owned handle.
        require(session.recover_volume().selection==select,"in-place CPU recovery disagreed with independent selection");
        release_inactive(session);
        require(!torn->allocated_extent(select.epoch==1?newer:older,slot),
            "in-place recovery kept the reclaimer on the wrong slot");
      }
    }
    require(recovered[0] && recovered[1],"selector cuts did not reach both durable outcomes");
    std::cout<<"paired_sealed_range unit="<<unit<<" original_bytes="<<before<<" settled_bytes="<<after
        <<" old_selector="<<recovered[0]<<" new_selector="<<recovered[1]
        <<" original_handle=1 cpu_reopen=1 tail_write=1 previous_snapshot_pinned=1 mirror_successor=1"
        <<" damaged_base_repaired=1 stale_workspace_retired=1 volatile_workspace="<<volatile_workspace
        <<" attempted_backing_write_bytes="<<backing_writes<<"\n";
  }
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
void volume_file_lifetime() {
  detail::VolumeFileLifetime healthy;
  for(const DWORD status:{ERROR_NOT_READY,ERROR_BUS_RESET,ERROR_DISK_FULL,
      ERROR_OPERATION_ABORTED,ERROR_IO_DEVICE}) {
    healthy.check(status);
    require(healthy.current(),"A transient request result retired the backing volume");
  }
  const auto observed=[](const detail::VolumeFileLifetime& lifetime,DWORD status) {
    try {
      lifetime.check(status);
    }catch(const std::system_error& error) {
      require(error.code().category()==std::system_category(),"Backing loss lost its Win32 error category");
      return static_cast<DWORD>(error.code().value());
    }
    return static_cast<DWORD>(ERROR_SUCCESS);
  };
  constexpr DWORD loss[]={ERROR_DEV_NOT_EXIST,ERROR_DEVICE_NOT_CONNECTED,
      ERROR_FILE_INVALID,ERROR_MEDIA_CHANGED,ERROR_NO_MEDIA_IN_DRIVE};
  for(const DWORD status:loss) {
    detail::VolumeFileLifetime attachment;
    auto capture=std::async(std::launch::async,[&]{return observed(attachment,status);});
    const auto error=capture.get();
    require(error==status && !attachment.current(),"Capture did not retire its backing volume");
    require(observed(attachment,ERROR_SUCCESS)==status,"Later success reopened a lost backing volume");
    require(observed(attachment,ERROR_FILE_INVALID)==status,"A later error replaced the first media failure");
    require(detail::VolumeFileLifetime{}.current(),"A fresh attachment inherited a previous handle's error");
  }
  std::cout<<"windows_backing_lifetime terminal_statuses=5 transient_statuses=5 shared_capture_fence=1"
      <<" first_error_retained=1 fresh_attachment_independent=1\n";
}

void positioned_request_retirement() {
  struct PipeHandle {
    HANDLE value;
    ~PipeHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
  };
  const auto name=L"\\\\.\\pipe\\flitzfs-positioned-"+std::to_wstring(GetCurrentProcessId())+L"-"+
      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
  const PipeHandle server{CreateNamedPipeW(name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|
      FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
      1,0,4096,0,nullptr)};
  require(server.value!=INVALID_HANDLE_VALUE,"create disposable positioned request pipe");
  const PipeHandle client{CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)};
  require(client.value!=INVALID_HANDLE_VALUE,"open disposable positioned request pipe");
  OVERLAPPED connection{};
  if(!ConnectNamedPipe(server.value,&connection)) {
    const auto status=GetLastError();
    DWORD transferred=0;
    require(status==ERROR_PIPE_CONNECTED || (status==ERROR_IO_PENDING &&
        GetOverlappedResult(server.value,&connection,&transferred,TRUE)),"positioned request pipe connection");
  }

  const detail::PositionedFileIo request;
  std::array<std::byte,8> received{};
  auto cancelled_buffer=received;
  unsigned observations=0;
  const std::function<bool()> cancel=[&]{return ++observations>=3;};
  {
    const detail::StoreCaptureReadScope capture(cancel);
    const auto cancelled=request.read(server.value,0,cancelled_buffer.data(),
        static_cast<DWORD>(cancelled_buffer.size()),&capture);
    require(cancelled.error==ERROR_OPERATION_ABORTED && !cancelled.bytes && observations>=3,
        "a pending positioned request did not retire on cancellation");
  }
  const std::array pattern{std::byte{11},std::byte{22},std::byte{33},std::byte{44},
      std::byte{55},std::byte{66},std::byte{77},std::byte{88}};
  DWORD written=0;
  require(WriteFile(client.value,pattern.data(),static_cast<DWORD>(pattern.size()),&written,nullptr) &&
      written==pattern.size(),"fill pipe after retired request");
  const auto next=request.read(server.value,0,received.data(),static_cast<DWORD>(received.size()));
  require(next.error==ERROR_SUCCESS && next.bytes==pattern.size() && received==pattern &&
      cancelled_buffer==std::array<std::byte,8>{},"retired request retained its buffer or consumed later bytes");
  std::cout<<"windows_positioned_request pending_cancelled=1 event_reused=1 retired_buffer_unchanged=1\n";
}

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

void disposable_dense_file(const std::filesystem::path& base) {
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root=std::filesystem::canonical(base)/("flitzfs-dense-file-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"could not exclusively create the dense fixture directory");
  const auto path=root/"volume.flfs";
  constexpr StoreIoContract model{4096,true};
  constexpr auto physical_limit=limits.maximum_physical_bytes;
  Bytes expected;
  {
    auto session=NativeVolumeFile::create_dense(path,model,physical_limit,id(61),id(62),limits,raw);
    expected=establish_history(session);
    require(stage_compaction(session)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,"Windows dense rotation did not publish");
    release_inactive(session);
  }
  const auto high_water=std::filesystem::file_size(path);
  const auto attributes=GetFileAttributesW(path.c_str());
  require(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_SPARSE_FILE) &&
      high_water<physical_limit,"Windows dense file reserved the sparse image gap");
  rejects([&]{(void)NativeVolumeFile::create_dense(path,model,physical_limit,id(64),id(65),limits,raw);},
      "Windows dense creation modified an existing file");
  {
    auto session=NativeVolumeFile::resume(path,model,id(61),raw);
    for(unsigned i=0;i<12 && !session.persistent_reclamation_pending();++i) {
      expected=data(12289,798+i);
      update(session,expected);
    }
    require(stage_compaction(session,94)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,"Windows dense second rotation did not publish");
    release_inactive(session);
  }
  const auto after=std::filesystem::file_size(path);
  const auto cpu=NativeVolumeFile::inspect(path,id(61));
  cpu.filesystem().verify_all();
  require(cpu.info().selection.epoch==3 && cpu.filesystem().read("payload",0,50000)==expected &&
      cpu.filesystem().read("witness",0,1000)==witness_bytes && after<=high_water+kStoreCommitUnit,
      "Windows dense restart lost bytes or extended beyond reclaimed capacity");
  std::cout<<"windows_dense_file first_rotation_bytes="<<high_water<<" second_rotation_bytes="<<after
      <<" independent_cpu_readback=1 sparse_attribute=0\n";
}

void disposable_dense_tail_file(const std::filesystem::path& base) {
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root=std::filesystem::canonical(base)/("flitzfs-dense-tail-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"could not exclusively create the dense tail fixture directory");
  const auto path=root/"volume.flfs";
  constexpr StoreIoContract model{4096,true,false,true};
  Bytes expected;
  NativeDenseFileSpace before,after;
  unsigned steps=0;
  {
    auto session=NativeVolumeFile::create_dense(path,model,limits.maximum_physical_bytes,id(61),id(62),limits,raw);
    expected=establish_history(session);
    {
      auto captured=session.snapshot();
      require(stage_compaction(session)==Reclaim::ready &&
          session.advance_persistent_reclamation({})==Reclaim::published,"dense tail fixture could not rotate its image");
      require(session.advance_persistent_reclamation({},true)==Reclaim::pinned &&
          captured.read("payload",0,50000)==expected,"dense file released a pinned prior image");
    }
    release_inactive(session);
    before=session.dense_file_space().value();
    require(before.allocated_file_bytes && before.interior_reusable_bytes && !before.retired_tail_bytes,
        "dense tail fixture has no interior holes or qualified allocation");
    const auto capture=session.snapshot();
    require(session.persistent_reclamation_pending(true),"ordinary volume did not schedule physical retirement");
    while(session.persistent_reclamation_pending(true)) {
      require(++steps<32 && session.advance_persistent_reclamation({},true)==Reclaim::retiring,
          "dense physical retirement did not complete in admitted steps");
      require(capture.read("payload",0,50000)==expected && capture.read("witness",0,1000)==witness_bytes,
          "held selected snapshot changed while moving physical units");
    }
    after=session.dense_file_space().value();
    std::cout<<"windows_dense_tail_observed eof_before="<<before.file_end<<" eof_after="<<after.file_end
        <<" allocated_before="<<before.allocated_file_bytes.value_or(0)
        <<" allocated_after="<<after.allocated_file_bytes.value_or(0)
        <<" owned_before="<<before.owned_payload_bytes<<" owned_after="<<after.owned_payload_bytes
        <<" selected_epoch="<<session.volume_info().selection.epoch<<" steps="<<steps<<'\n';
    require(after.file_end<before.file_end && after.allocated_file_bytes &&
        *after.allocated_file_bytes<*before.allocated_file_bytes &&
        after.owned_payload_bytes==before.owned_payload_bytes && session.volume_info().selection.epoch==2,
        "dense retirement failed to release host allocation or changed selected data");
  }
  const auto cpu=NativeVolumeFile::inspect(path,id(61));
  cpu.filesystem().verify_all();
  require(std::filesystem::file_size(path)==after.file_end &&
      cpu.filesystem().read("payload",0,50000)==expected &&
      cpu.filesystem().read("witness",0,1000)==witness_bytes,"ordinary file lost acknowledged content after EOF release");
  std::cout<<"windows_dense_tail eof_before="<<before.file_end<<" eof_after="<<after.file_end
      <<" allocated_before="<<*before.allocated_file_bytes<<" allocated_after="<<*after.allocated_file_bytes
      <<" steps="<<steps<<" held_readers_verified=1 independent_cpu_reopen=1\n";
}

void disposable_dense_cross_file(const std::filesystem::path& base) {
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root=std::filesystem::canonical(base)/("flitzfs-dense-cross-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"could not exclusively create the cross-group fixture directory");
  const auto path=root/"volume.flfs";
  constexpr StoreIoContract model{4096,true,false,true};
  NativeCrossFixture result;
  {
    auto session=NativeVolumeFile::create_dense(path,model,cross_fixture_limits.maximum_physical_bytes,
        id(61),id(62),cross_fixture_limits,raw);
    result=advance_native_cross_group(session);
  }
  {
    const auto header=ReadOnlyFile::open(path);
    std::byte a{},b{};
    header.read_exact_at(8,&a,1);
    header.read_exact_at(kStoreCommitUnit+8,&b,1);
    require(a==std::byte{2} && b==a,"ordinary backing did not settle both upgraded anchors");
  }
  verify_cross_fixture(NativeVolumeFile::inspect(path,id(61)),result.expected);
  {
    auto writer=NativeVolumeFile::resume(path,model,id(61),raw);
    append_cross_fixture_edit(writer,result.expected);
  }
  verify_cross_fixture(NativeVolumeFile::inspect(path,id(61)),result.expected);
  report_cross_fixture("windows_dense_cross",result);
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

void disposable_positioned_file(const std::filesystem::path& base) {
  require(store_lz4_available(),"positioned file qualification needs the CPU LZ4 codec");
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root=std::filesystem::canonical(base)/("flitzfs-positioned-volume-"+std::to_string(ticks));
  require(std::filesystem::create_directory(root),"could not create the positioned file fixture");
  const auto path=root/"positioned.flfs";
  constexpr StoreIoContract isolated{65536,true,true};
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  constexpr auto extent=kNativeExtentBytes;
  constexpr auto frame_bytes=extent+detail::kIndexedExtentHeaderBytes;
  auto old=native_lz4_fixture::source();
  const auto tail=native_lz4_fixture::source();
  old.insert(old.end(),tail.begin(),tail.end()-37);
  auto latest=old;
  latest.front()^=std::byte{7};
  using Frames=std::vector<detail::NativeLz4CapturedFrame>;
  const auto verify=[&](const Frames& frames) {
    require(frames.size()==2,"positioned capture did not collect both checked extents");
    detail::StoreCodecDecoder decoder;
    std::size_t matched=0;
    for(const auto& frame:frames) {
      const auto record=decoder.decode(detail::kStoreLz4,frame.encoded(),frame.key().decoded_bytes);
      const auto count=(std::min)(std::size_t(extent),old.size()-matched);
      require(record.size()==detail::kIndexedExtentHeaderBytes+count &&
          std::equal(record.begin()+detail::kIndexedExtentHeaderBytes,record.end(),old.begin()+matched),
          "pinned capture returned a different committed extent");
      matched+=count;
    }
    require(matched==old.size(),"positioned capture did not cover the file");
  };
  {
    auto session=NativeVolumeFile::create(path,isolated,id(90),id(91),limits,fast);
    put(session,"payload",old);
    for(unsigned revision=0;revision<12;++revision) {
      old[1]=static_cast<std::byte>(revision);
      update(session,old);
    }
    latest[1]=old[1];
    const auto file=session.open("payload",{true,true,false});
    const auto bytes=static_cast<std::uint32_t>(old.size());
    auto lease=session.lease_native_lz4_capture(file,0,frame_bytes);
    require(bool(lease),"positioned file could not pin its checked namespace");

    Frames frames;
    std::promise<void> entered,release;
    auto entered_future=entered.get_future();
    auto release_future=release.get_future();
    std::jthread worker([&] {
      unsigned observations=0;
      frames=lease->capture_range(bytes,4*frame_bytes,[&] {
        if(++observations!=2)return false;
        entered.set_value();
        return release_future.wait_for(std::chrono::seconds(5))!=std::future_status::ready;
      });
    });
    require(entered_future.wait_for(std::chrono::seconds(5))==std::future_status::ready,
        "positioned capture did not reach its first request");
    require(session.read(file,0,bytes)==old,"foreground read lost the original bytes during capture");
    (void)session.write(file,0,latest);
    require(session.read(file,0,bytes)==latest,"foreground write was not visible during capture");
    release.set_value();
    worker.join();
    verify(frames);
    session.cleanup(file,false);
    session.close(file);

    require(stage_compaction(session,92)==Reclaim::ready &&
        session.advance_persistent_reclamation({})==Reclaim::published,"positioned volume did not publish its replacement");
    require(session.advance_persistent_reclamation({})==Reclaim::pinned,"lease did not protect the inactive file range");
    std::shared_ptr<detail::PagedReadMetadata> retained_metadata;
    verify(lease->capture_range(bytes,4*frame_bytes,{},&retained_metadata));
    require(bool(retained_metadata),"captured file did not retain checked query metadata");
    lease.reset();
    require(session.advance_persistent_reclamation({})==Reclaim::retiring,
        "checked metadata improperly owned the retired backing");
    release_inactive(session);
  }
  const auto view=NativeVolumeFile::inspect(path,id(90));
  view.filesystem().verify_all();
  require(view.info().selection.epoch==2 &&
      view.filesystem().read("payload",0,static_cast<std::uint32_t>(latest.size()))==latest,
      "independent reopen lost the foreground update after positioned capture");
  std::cout<<"windows_positioned_volume foreground_exact_while_capture_held=1 retained_extent_frames=2"
      <<" inactive_range_pinned=1 retired_after_release=1 independent_cpu_reopen=1 metadata_did_not_pin=1\n";
}
#endif
}
int main(int argc,char** argv){
  try {
    if(argc==2 && std::string_view(argv[1])=="--dense-allocation") {
      dense_allocation();
      dense_replacement_admission();
      dense_group_boundary();
      dense_directory_cuts();
      std::cout<<"PASS: "<<checks<<" dense ownership checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--dense-session") {
      dense_session_reuse();
      dense_seal_workspace_alignment();
      std::cout<<"PASS: "<<checks<<" dense volume session checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--dense-session-cuts") {
      dense_session_cuts();
      std::cout<<"PASS: "<<checks<<" dense publication recovery checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--dense-tail") {
      dense_tail_retirement();
      std::cout<<"PASS: "<<checks<<" dense tail checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--dense-cross-group") {
      paired_image_admission();
      dense_cross_group_moves();
      std::cout<<"PASS: "<<checks<<" dense cross-group checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--dense-cross-native") {
      modeled_native_dense_cross_file();
      std::cout<<"PASS: "<<checks<<" modeled native cross-group checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--overwrite-rename-cuts") {
      overwrite_and_rename_publication_cuts();
      std::cout<<"PASS: "<<checks<<" overwrite and rename recovery checks\n";
      return 0;
    }
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
    if(argc==2 && std::string_view(argv[1])=="--positioned-request") {
      volume_file_lifetime();
      positioned_request_retirement();
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-positioned-file") {
      disposable_positioned_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows positioned volume checks\n";
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-file") {
      disposable_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows file-handle checks\n";
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-dense-file") {
      disposable_dense_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows dense backing checks\n";
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-dense-tail-file") {
      disposable_dense_tail_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows dense tail checks\n";
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--disposable-dense-cross-file") {
      disposable_dense_cross_file(std::filesystem::path(argv[2]));
      std::cout<<"PASS: "<<checks<<" isolated Windows cross-group checks\n";
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
    if(argc==2 && std::string_view(argv[1])=="--catalog-ranges") {
      managed_catalog_ranges();
      std::cout<<"PASS: "<<checks<<" managed catalog range checks\n";
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--sealed-ranges") {
      sealed_ranges(false);
      sealed_ranges(true);
      std::cout<<"PASS: "<<checks<<" paired sealed range checks\n";
      return 0;
    }
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
