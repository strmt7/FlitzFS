#include "flitzfs/native_volume_file.hpp"
#include "native_fs_fixture.hpp"
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
    require(session.advance_persistent_reclamation({})==Reclaim::retiring,"inactive range did not retire");
  }
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
        require(!recovery.volume_info().degraded && recovery.snapshot().read("payload",0,50000)==expected,
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
    range_publication_cuts();
    std::cout<<"PASS: "<<checks<<" memory-backed volume-file checks\n";
    return 0;
  }catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
