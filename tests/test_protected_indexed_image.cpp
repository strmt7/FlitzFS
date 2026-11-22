#include "native_corpus_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/native_hybrid_backing.hpp"
#include "protected_stripe_image.hpp"
#include "folded_checkpoint.hpp"
#include <chrono>
#include <iostream>
#include <numeric>

namespace {
using namespace flitzfs;
using namespace native_fixture;
using detail::ProtectedStripeImage;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
constexpr NativeImageStorage sealed_storage{NativeImageKind::sealed_indexed,limits.maximum_physical_bytes};
constexpr NativeImageStorage ordinary_storage{NativeImageKind::indexed_paged,limits.maximum_physical_bytes};
constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
constexpr auto prefix=2*kStoreCommitUnit;

[[noreturn]] void fail(const char* why){throw std::runtime_error(why);}
void require(bool ok,const char* why){if(!ok)fail(why);}
template<class F>void rejects(F&& action,const char* why) {
  try {action();}catch(const std::exception&){return;}
  fail(why);
}
StoreIdentity id(unsigned number) {
  auto identity=native_fixture::identity();
  identity[0]=static_cast<std::byte>(number);
  return identity;
}
auto empty(std::uint32_t unit){return image({},unit);}
std::string path(std::size_t i){return "docs/f"+std::to_string(i)+".bin";}

std::shared_ptr<Ram> make_indexed(std::span<const Bytes> files,std::uint32_t unit,StoreCompressionOptions policy) {
  auto head=empty(unit),data=empty(unit);
  auto session=NativeFileSession::create_indexed_volume(head,id(9),data,id(1),limits,policy);
  session.create_entry("docs",true);
  for(std::size_t i=0;i<files.size();++i) {
    const auto name=path(i);
    const auto input=std::span(files[i]);
    session.create_entry(name);
    auto handle=session.open(name,{true,true,false});
    constexpr std::size_t quantum=1024*1024;
    for(std::size_t offset=0;offset<input.size();offset+=quantum)
      session.write(handle,offset,input.subspan(offset,(std::min)(quantum,input.size()-offset)));
    session.cleanup(handle,false);
    session.close(handle);
  }
  return data;
}
void verify(const NativeFilesystemSnapshot& snapshot,std::span<const Bytes> files) {
  snapshot.verify_all();
  require(snapshot.list("docs").size()==files.size(),"directory lost a sealed file");
  for(std::size_t i=0;i<files.size();++i) {
    const auto name=path(i);
    require(snapshot.stat(name).logical_bytes==files[i].size(),"sealed file length changed");
    require(snapshot.read(name,0,files[i].size())==files[i],"sealed file differed through CPU read");
  }
}
void damage(Ram& io,std::uint64_t offset,std::uint64_t unit) {
  const auto bytes=std::span(io.live).subspan(offset,unit);
  for(auto& b:bytes)b^=std::byte{0xa3};
}
void native_stripes(std::uint32_t unit) {
  const std::array files{data(91'677,752),data(76'205,918),Bytes(65'521)};
  auto backing=make_indexed(files,unit,raw);
  const auto original=NativeFilesystemSnapshot::open_image(backing,ordinary_storage);
  auto target=empty(unit),scratch=empty(unit);
  const auto sealed=original.create_sealed_indexed_copy(scratch,target,id(2),limits.maximum_physical_bytes,raw);
  require(sealed.image_storage()==sealed_storage && sealed.identity()==id(2) &&
      sealed.storage_info().physical_end==target->extent() && !sealed.storage_info().degraded_checkpoint,
      "wrong new sealed native descriptor");
  require(!PagedObjectStoreSnapshot::open(scratch,limits).info().mirrored_payloads,"sealed workspace duplicated payloads");
  rejects([&]{(void)NativeFilesystemSnapshot::open_image(scratch,ordinary_storage);},
      "unprotected workspace supplied an ordinary native snapshot");
  verify(sealed,files);
  const auto stable=target->stable;
  const auto recover=[&](std::uint64_t at) {
    auto damaged=image(stable,unit);
    damage(*damaged,at,unit);
    const auto reopened=NativeFilesystemSnapshot::open_image(std::move(damaged),sealed_storage);
    verify(reopened,files);
    require(reopened.storage_info().degraded_checkpoint,"protected stripe failed to report a lost unit");
  };
  recover(0);
  recover(kStoreCommitUnit);
  for(std::uint64_t at=prefix;at<stable.size();at+=unit)recover(at);
  auto broken=image(stable,unit);
  damage(*broken,prefix,unit);
  damage(*broken,prefix+unit,unit);
  rejects([&]{const auto read=ProtectedStripeImage::open(broken,limits.maximum_physical_bytes);read->verify_all();},
      "two lost units in one stripe were accepted");
  const auto cap=sealed.storage_info().physical_end-1;
  rejects([&]{(void)NativeFilesystemSnapshot::open_image(target,{NativeImageKind::sealed_indexed,cap});},
      "sealed image opened past its physical limit");
  rejects([&]{(void)NativeFilesystemSnapshot::open_image(target,ordinary_storage);},
      "ordinary native reader guessed the sealed format");
  auto hot_backing=image(stable,unit);
  const auto hot=ProtectedStripeImage::open(hot_backing,limits.maximum_physical_bytes);
  std::array<std::byte,128> first{};
  hot->read_exact(0,first);
  const auto read_before_cache=hot_backing->read_bytes;
  std::array<std::byte,128> repeated{};
  hot->read_exact(0,repeated);
  require(first==repeated && hot_backing->read_bytes==read_before_cache,
      "checked sealed unit was read from backing twice");
  auto hot_data_backing=image(stable,unit);
  const auto hot_data=ProtectedStripeImage::open(hot_data_backing,limits.maximum_physical_bytes);
  hot_data->read_exact(0,repeated);
  damage(*hot_data_backing,prefix,unit);
  hot_data->verify_all();
  require(hot_data->degraded(),"warm data cache hid physical damage from scrub");
  hot_data->read_exact(0,repeated);
  require(first==repeated,"checking a warm damaged unit changed recovered bytes");
  const auto first_count=(std::min<std::uint64_t>)(detail::kProtectedStripeDataUnits,(scratch->extent()+unit-1)/unit);
  damage(*hot_backing,prefix+first_count*unit,unit);
  hot->verify_all();
  require(hot->degraded(),"warm descriptor cache hid physical damage from scrub");
  auto wrapped=ProtectedStripeImage::open(target,limits.maximum_physical_bytes);
  rejects([&]{(void)PagedObjectStoreWriter::resume(wrapped,id(2),sealed.storage_info().generation,limits,raw);},
      "sealed backing granted a writer lease");
  auto damaged=image(stable,unit);
  damage(*damaged,prefix,unit);
  const auto recovered=NativeFilesystemSnapshot::open_image(damaged,sealed_storage);
  auto restored_io=empty(unit);
  auto restored=NativeFilesystemWriter::compact(recovered,restored_io,id(3),raw);
  require(PagedObjectStoreSnapshot::open(restored_io,limits).info().mirrored_payloads,
      "restored image lacks ordinary mirrors");
  verify(restored.snapshot(),files);
  auto extra=data(4821,932);
  auto write=file_write(path(0),files[0].size(),extra);
  (void)restored.apply({&write,1});
  auto larger=files;
  larger[0].insert(larger[0].end(),extra.begin(),extra.end());
  verify(restored.snapshot(),larger);
  require(target->live==stable,"recovery changed sealed source bytes");
  verify(original,files);
  std::cout<<"sealed_faults unit="<<unit<<" single_lost_units="<<2+(stable.size()-prefix)/unit
      <<" image_bytes="<<stable.size()<<" exact_native_recovery=1\n";
}

void interrupted_seal() {
  constexpr std::uint32_t unit=4096;
  const auto bytes=data(7*unit+113,1711);
  const auto source=image(bytes,unit);
  auto successful=empty(unit);
  const auto sealed=ProtectedStripeImage::seal(source,successful,id(2),limits.maximum_physical_bytes);
  const auto calls=successful->calls;
  const auto verify_stream=[&](const ProtectedStripeImage& read) {
    read.verify_all();
    Bytes actual(bytes.size());
    read.read_exact(0,actual);
    require(actual==bytes,"cold CPU recovery differed after a cut");
  };
  verify_stream(*sealed);
  unsigned unavailable=0,complete=0;
  for(unsigned failure=0;failure<3;++failure)for(std::size_t cut=0;cut<calls;++cut) {
    auto target=empty(unit);
    target->failure=failure;
    target->fail_at=cut;
    rejects([&]{(void)ProtectedStripeImage::seal(source,target,id(2),limits.maximum_physical_bytes);},
        "sealing returned success after a failing write or flush");
    require(source->live==bytes && source->stable==bytes,"interrupted sealing modified the source");
    auto replay=image(target->stable,unit);
    std::shared_ptr<ProtectedStripeImage> recovered;
    try {recovered=ProtectedStripeImage::open(std::move(replay),limits.maximum_physical_bytes);}
    catch(const std::exception&){++unavailable;}
    if(recovered){verify_stream(*recovered);++complete;}
  }
  auto full=empty(unit);
  rejects([&]{(void)ProtectedStripeImage::seal(source,full,id(3),prefix+unit);},
      "sealing exceeded admission");
  require(!full->extent() && !full->writes && unavailable && complete,"publication cut coverage");
  std::cout<<"sealed_cuts attempts="<<3*calls<<" unavailable="<<unavailable<<" complete="<<complete
      <<" prior_preserved=1\n";
}

struct Reads {std::uint64_t backing_bytes=0,elapsed_ns=0;};
Reads measure_reads(const NativeFilesystemSnapshot& snapshot,const Ram& io,std::span<const Bytes> files) {
  const auto initial=io.read_bytes;
  const auto started=std::chrono::steady_clock::now();
  for(std::size_t i=0;i<files.size();++i) {
    const auto& data=files[i];
    for(unsigned sample=1;sample<=16;++sample) {
      const auto length=(std::min)(std::size_t{128},data.size());
      const auto at=(data.size()-length)*sample/17;
      const auto out=snapshot.read(path(i),at,length);
      require(std::ranges::equal(out,std::span(data).subspan(at,length)),"sealed short-read bytes");
    }
  }
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started);
  return {io.read_bytes-initial,static_cast<std::uint64_t>(elapsed.count())};
}
void profile(const std::filesystem::path& corpus) {
  require(store_zstd_available(),"protected corpus profile requires Zstandard");
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr auto unit=4096U;
  const auto files=read_packed_corpus(corpus,true);
  auto source_io=make_indexed(files,unit,policy);
  const auto source=NativeFilesystemSnapshot::open_image(source_io,ordinary_storage);
  std::uint64_t mirrored=0;
  Reads mirror_reads;
  {
    auto target=empty(unit);
    {const auto packed=NativeFilesystemWriter::compact(source,target,id(2),policy);}
    mirrored=target->extent();
    const auto baseline=NativeFilesystemSnapshot::open_image(target,ordinary_storage);
    mirror_reads=measure_reads(baseline,*target,files);
    verify(baseline,files);
  }
  auto scratch=empty(unit),target=empty(unit);
  const auto started=std::chrono::steady_clock::now();
  {
    const auto sealed=source.create_sealed_indexed_copy(scratch,target,id(3),limits.maximum_physical_bytes,policy);
    require(!sealed.storage_info().degraded_checkpoint,"fresh sealed corpus reads as degraded");
  }
  const auto built=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started);
  const auto workspace_extent=scratch->extent();
  scratch.reset();
  const auto reopened=NativeFilesystemSnapshot::open_image(target,sealed_storage);
  const auto striped_reads=measure_reads(reopened,*target,files);
  verify(reopened,files);
  const auto logical=std::accumulate(files.begin(),files.end(),std::uint64_t{0},[](auto counted,const auto& f){
    return counted+f.size();
  });
  std::cout<<"sealed_profile files="<<files.size()<<" logical_bytes="<<logical<<" mirrored_image="<<mirrored
      <<" single_stream_bytes="<<workspace_extent<<" sealed_image="<<target->extent()
      <<" mirror_range_reads="<<mirror_reads.backing_bytes<<" sealed_range_reads="<<striped_reads.backing_bytes
      <<" mirror_range_ns="<<mirror_reads.elapsed_ns<<" sealed_range_ns="<<striped_reads.elapsed_ns
      <<" build_verify_ns="<<built.count()<<" independent_cpu_exact=1\n";
}
struct HybridFixture {
  std::shared_ptr<Ram> source,head,workspace,base,tail;
  std::shared_ptr<NativeHybridBacking> hybrid;
  NativeVolumeHeadInfo selected;
  std::size_t bootstrap_calls=0;

  HybridFixture(std::span<const Bytes> files,std::uint32_t unit,StoreCompressionOptions policy)
      :source(make_indexed(files,unit,policy)),head(empty(unit)),workspace(empty(unit)),
       base(empty(unit)),tail(empty(unit)) {
    const auto old=NativeFilesystemSnapshot::open_image(source,ordinary_storage);
    hybrid=NativeHybridBacking::create(old,workspace,base,tail,id(2),limits.maximum_physical_bytes,policy);
    bootstrap_calls=tail->calls;
    const auto copy=NativeFilesystemSnapshot::open_image(hybrid,ordinary_storage);
    auto writer=NativeVolumeHeadWriter::create(head,id(9),old);
    selected=writer.publish_compacted(old,copy);
    require(selected.selection.epoch==2 && selected.selection.image_identity==id(2),
        "verified hybrid image was not selected by the normal volume head");
  }
};

constexpr auto tail_prologue=4*kStoreCommitUnit;
std::shared_ptr<NativeHybridBacking> reopen_hybrid(std::shared_ptr<StoreByteIo> base,std::shared_ptr<StoreByteIo> tail) {
  return NativeHybridBacking::open(std::move(base),std::move(tail),id(2),
      limits.maximum_physical_bytes,limits.maximum_physical_bytes);
}

std::size_t apply_hybrid_patch(std::shared_ptr<StoreByteIo> head,std::shared_ptr<StoreByteIo> backing,
    const Ram& observed_io,std::span<const std::byte> patch,StoreCompressionOptions policy) {
  const auto resolve=[&](const StoreIdentity& id_value){return id_value==id(2)?backing:nullptr;};
  auto session=NativeFileSession::resume_closed_indexed_volume(std::move(head),id(9),2,resolve,policy);
  const auto handle=session.open(path(0),{true,true,false});
  const auto started=observed_io.calls;
  require(session.write(handle,kNativeExtentBytes-31,patch).bytes==patch.size(),"hybrid write returned a short count");
  const auto calls=observed_io.calls-started;
  require(std::ranges::equal(session.read(handle,kNativeExtentBytes-31,patch.size()),patch),
      "ordinary session failed hybrid read after write");
  session.cleanup(handle,false);
  session.close(handle);
  return calls;
}

void hybrid_faults(std::uint32_t unit) {
  const std::array files{data(713'821,810),data(263'482,183)};
  HybridFixture f(files,unit,raw);
  const auto frozen=f.base->stable;
  const auto initial_tail=f.tail->stable;
  const auto selected_head=f.head->stable;
  require(f.tail->extent()==tail_prologue && detail::folded_mirror_extent(f.hybrid->extent())==f.workspace->extent() &&
      f.hybrid->occupied()==f.base->extent()+f.tail->extent() && f.hybrid->occupied()<f.hybrid->extent(),
      "hybrid allocation does not account for both files and the folded workspace");
  const auto original=NativeFilesystemSnapshot::open_image(f.source,ordinary_storage);
  auto mirror=empty(unit);
  {const auto writer=NativeFilesystemWriter::compact(original,mirror,id(2),raw);}
  require(f.hybrid->extent()==mirror->extent(),"fold changed the mirrored address space");
  require(std::ranges::equal(std::span(mirror->stable).first(prefix),std::span(f.workspace->stable).first(prefix)),
      "folded workspace changed the root area");
  for(std::uint64_t at=prefix;at<mirror->extent();at+=2*unit) {
    const auto expected=std::span(mirror->stable).subspan(at,unit);
    const auto actual=std::span(f.workspace->stable).subspan(detail::folded_mirror_offset(at,unit),unit);
    require(std::ranges::equal(expected,actual) &&
        std::ranges::equal(expected,std::span(mirror->stable).subspan(at+unit,unit)),
        "folded workspace changed a payload or index unit");
  }
  rejects([&]{(void)NativeFilesystemSnapshot::open_image(f.workspace,ordinary_storage);},
      "folded workspace opened as a standalone native image");
  auto limited=empty(unit),limited_base=empty(unit),limited_tail=empty(unit);
  limited->capacity=f.workspace->extent();
  const auto fitted=NativeHybridBacking::create(original,limited,limited_base,limited_tail,id(2),
      limits.maximum_physical_bytes,raw);
  verify(NativeFilesystemSnapshot::open_image(fitted,ordinary_storage),files);
  require(limited->extent()==limited->capacity && limited->extent()<mirror->extent(),
      "builder required a mirrored-sized workspace");
  auto short_workspace=empty(unit),short_base=empty(unit),short_tail=empty(unit);
  short_workspace->capacity=limited->capacity-1;
  rejects([&]{(void)NativeHybridBacking::create(original,short_workspace,short_base,short_tail,id(2),
      limits.maximum_physical_bytes,raw);},"insufficient folded workspace was accepted");
  require(!short_workspace->writes && !short_base->writes && !short_tail->writes,
      "folded capacity refusal attempted writes");
  const auto prior=NativeFilesystemSnapshot::open_image(f.hybrid,ordinary_storage);
  verify(prior,files);
  rejects([&]{(void)reopen_hybrid(f.base,f.tail);},"used a second hybrid owner");
  const auto patch=data(3'988,193);
  auto expected=files;
  std::ranges::copy(patch,expected[0].begin()+kNativeExtentBytes-31);
  const auto calls=apply_hybrid_patch(f.head,f.hybrid,*f.tail,patch,raw);
  require(calls>=5 && f.base->stable==frozen && f.base->live==frozen &&
      std::ranges::equal(std::span(f.tail->stable).first(prefix),std::span(initial_tail).first(prefix)),
      "ordinary writes changed cold data or its binding");
  verify(prior,files);
  {
    const auto durable=reopen_hybrid(image(frozen,unit),image(f.tail->stable,unit));
    const auto selected=NativeVolumeHeadView::open(image(selected_head,unit),id(9),
        [&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(2)?durable:nullptr;});
    verify(selected.filesystem(),expected);
  }

  unsigned old_count=0,new_count=0;
  for(unsigned failure=0;failure<3;++failure)for(std::size_t cut=0;cut<calls;++cut) {
    auto log=image(initial_tail,unit),head=image(selected_head,unit),base=image(frozen,unit);
    {
      const auto ready=reopen_hybrid(base,log);
      const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(2)?ready:nullptr;};
      auto session=NativeFileSession::resume_closed_indexed_volume(head,id(9),2,resolve,raw);
      const auto handle=session.open(path(0),{true,true,false});
      log->failure=failure;
      log->fail_at=log->calls+cut;
      rejects([&]{(void)session.write(handle,kNativeExtentBytes-31,patch);},
          "interrupted hybrid write reported success");
    }
    require(base->live==frozen && base->stable==frozen,"interrupted mutation changed immutable cold bytes");
    const auto recovered=reopen_hybrid(std::move(base),image(log->stable,unit));
    const auto snapshot=NativeFilesystemSnapshot::open_image(recovered,ordinary_storage);
    if(snapshot.read(path(0),0,files[0].size())==files[0]) {
      verify(snapshot,files);
      ++old_count;
    }else {
      verify(snapshot,expected);
      ++new_count;
    }
  }
  require(old_count && new_count,"hybrid tail cuts did not reach both complete generations");

  for(const bool damage_binding:{false,true}) {
    auto base=image(frozen,unit),tail=image(initial_tail,unit);
    if(damage_binding)damage(*tail,0,unit);
    else damage(*base,prefix+unit,unit);
    const auto recovered=reopen_hybrid(std::move(base),std::move(tail));
    const auto snapshot=NativeFilesystemSnapshot::open_image(recovered,ordinary_storage);
    verify(snapshot,files);
    require(recovered->backing_degraded() && snapshot.storage_info().degraded_checkpoint,
        "recoverable hybrid damage was hidden from the native reader");
    rejects([&]{(void)PagedObjectStoreWriter::resume(recovered,id(2),snapshot.storage_info().generation,limits,raw);},
        "degraded hybrid backing admitted writes");
    auto repaired=empty(unit);
    {
      const auto writer=NativeFilesystemWriter::compact(snapshot,repaired,id(5),raw);
    }
    const auto reopened=NativeFilesystemSnapshot::open_image(repaired,ordinary_storage);
    require(!reopened.storage_info().degraded_checkpoint,"recovered bytes did not rebuild healthy mirrors");
    verify(reopened,files);
  }
  auto uncertain=image(frozen,unit);
  damage(*uncertain,prefix,unit);
  damage(*uncertain,prefix+unit,unit);
  rejects([&]{const auto invalid=reopen_hybrid(uncertain,image(initial_tail,unit));invalid->verify_backing();},
      "double loss in one protected stripe was accepted");
  auto read_only=image(initial_tail,unit);
  read_only->failure_unit=0;
  const auto rescue=reopen_hybrid(image(frozen,unit),read_only);
  verify(NativeFilesystemSnapshot::open_image(rescue,ordinary_storage),files);
  require(!valid_store_io_contract(rescue->contract()) && !read_only->writes,"read-only hybrid rescue granted a writer");

  auto other_files=files;
  other_files[0][41]^=std::byte{0x53};
  HybridFixture other(other_files,unit,raw);
  auto same_id_different_base=image(other.base->stable,unit),untouched_tail=image(initial_tail,unit);
  rejects([&]{(void)reopen_hybrid(same_id_different_base,untouched_tail);},
      "same image ID with different frozen roots supplied a base");
  require(!same_id_different_base->writes && !untouched_tail->writes,"failed hybrid identity check attempted writes");
  std::cout<<"hybrid_tail_cuts unit="<<unit<<" attempted="<<3*calls<<" old_exact="<<old_count
      <<" new_exact="<<new_count<<" base_unchanged=1 read_only_rescue=1 recovered_to_mirror=1"
      <<" exact_fold_verified=1 folded_capacity_admitted=1\n";
}

void interrupted_hybrid_binding() {
  constexpr auto unit=4096U;
  const std::array files{data(713'821,333),data(263'482,167)};
  HybridFixture fixture(files,unit,raw);
  const auto original=NativeFilesystemSnapshot::open_image(fixture.source,ordinary_storage);
  const auto original_bytes=fixture.source->stable;
  const auto scratch_calls=fixture.workspace->calls;
  require(scratch_calls>4,"folded workspace had no staged writes");
  unsigned scratch_cuts=0;
  for(unsigned failure=0;failure<3;++failure)
    for(const auto cut:{std::size_t{0},scratch_calls/2,scratch_calls-1}) {
      auto workspace=empty(unit),base=empty(unit),tail=empty(unit);
      workspace->failure=failure;
      workspace->fail_at=cut;
      rejects([&]{(void)NativeHybridBacking::create(original,workspace,base,tail,id(3),
          limits.maximum_physical_bytes,raw);},"failed workspace reached sealing");
      require(!base->writes && !tail->writes && fixture.source->live==original_bytes &&
          fixture.source->stable==original_bytes,"failed folded workspace affected selected or sealed data");
      ++scratch_cuts;
    }
  std::cout<<"folded_workspace_cuts attempted="<<scratch_cuts<<" prior_intact=1 no_sealed_writes=1\n";
  const auto calls=fixture.bootstrap_calls;
  require(calls==7,"hybrid mapping publication call count changed");
  unsigned incomplete=0,complete=0;
  for(unsigned failure=0;failure<3;++failure)for(std::size_t cut=0;cut<calls;++cut) {
    auto workspace=empty(unit),base=empty(unit),tail=empty(unit);
    tail->failure=failure;
    tail->fail_at=cut;
    rejects([&]{(void)NativeHybridBacking::create(original,workspace,base,tail,id(3),
        limits.maximum_physical_bytes,raw);},"failed hybrid binding returned success");
    require(fixture.source->stable==original_bytes && fixture.source->live==original_bytes,
        "failed hybrid bootstrap touched the old image");
    std::shared_ptr<NativeHybridBacking> recovered;
    try {
      recovered=NativeHybridBacking::open(image(base->stable,unit),image(tail->stable,unit),id(3),
          limits.maximum_physical_bytes,limits.maximum_physical_bytes);
    }catch(const std::exception&){++incomplete;}
    if(recovered) {
      verify(NativeFilesystemSnapshot::open_image(recovered,ordinary_storage),files);
      ++complete;
    }
  }
  require(incomplete && complete,"hybrid mapping cuts did not exercise open and refusal");
  auto workspace=empty(unit),base=empty(unit),tail=empty(unit);
  const std::array small{data(45,6)};
  const auto tiny=NativeFilesystemSnapshot::open_image(make_indexed(small,unit,raw),ordinary_storage);
  rejects([&]{(void)NativeHybridBacking::create(tiny,workspace,base,tail,id(3),limits.maximum_physical_bytes,raw);},
      "hybrid placement enlarged a small protected image");
  require(!base->writes && !tail->writes,"rejected hybrid capacity cost wrote a published backing");
  std::cout<<"hybrid_binding_cuts attempted="<<3*calls<<" incomplete="<<incomplete<<" complete="<<complete
      <<" intact_prior=1 rejected_growth_no_target_writes=1\n";
}

void profile_hybrid(const std::filesystem::path& corpus,bool mirror_first) {
  require(store_zstd_available(),"hybrid corpus profile requires Zstandard");
  constexpr auto unit=4096U;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  const auto files=read_packed_corpus(corpus,true);
  HybridFixture f(files,unit,policy);
  auto mirrored=empty(unit);
  {
    const auto original=NativeFilesystemSnapshot::open_image(f.source,ordinary_storage);
    const auto built=NativeFilesystemWriter::compact(original,mirrored,id(2),policy);
  }
  const auto reference=NativeFilesystemSnapshot::open_image(mirrored,ordinary_storage);
  const auto mapped=NativeFilesystemSnapshot::open_image(f.hybrid,ordinary_storage);
  const auto hybrid_reads=[&] {
    const auto before=f.tail->read_bytes;
    auto measured=measure_reads(mapped,*f.base,files);
    measured.backing_bytes+=f.tail->read_bytes-before;
    return measured;
  };
  Reads reference_reads,mapped_reads;
  if(mirror_first) {
    reference_reads=measure_reads(reference,*mirrored,files);
    mapped_reads=hybrid_reads();
  }else {
    mapped_reads=hybrid_reads();
    reference_reads=measure_reads(reference,*mirrored,files);
  }
  verify(reference,files);
  verify(mapped,files);
  const auto mirror=mirrored->extent(),combined=f.hybrid->occupied(),frozen=f.base->extent();
  const auto retained=f.source->extent(),selector=f.head->extent(),workspace=f.workspace->extent();
  const auto staged_total=retained+selector+workspace+combined;
  require(workspace==detail::folded_mirror_extent(mirror),"profile workspace did not fold the mirror");
  auto reference_head=empty(unit);
  {
    const auto source=NativeFilesystemSnapshot::open_image(f.source,ordinary_storage);
    auto controller=NativeVolumeHeadWriter::create(reference_head,id(9),source);
    (void)controller.publish_compacted(source,reference);
  }
  auto expected=files;
  require(expected[0].size()>kNativeExtentBytes+4096,"profile expects two native extents in file A or I");
  const auto patch=data(3'988,193);
  std::ranges::copy(patch,expected[0].begin()+kNativeExtentBytes-31);
  const auto frozen_writes=f.base->writes;
  (void)apply_hybrid_patch(reference_head,mirrored,*mirrored,patch,policy);
  (void)apply_hybrid_patch(f.head,f.hybrid,*f.tail,patch,policy);
  const auto reopened=reopen_hybrid(image(f.base->stable,unit),image(f.tail->stable,unit));
  verify(NativeFilesystemSnapshot::open_image(reopened,ordinary_storage),expected);
  verify(NativeFilesystemSnapshot::open_image(mirrored,ordinary_storage),expected);
  require(f.base->extent()==frozen && f.base->writes==frozen_writes,"public data patch wrote a cold region");
  std::cout<<"hybrid_profile files="<<files.size()<<" mirror_bytes="<<mirror
      <<" hybrid_bytes="<<combined<<" cold_bytes="<<frozen<<" mirror_after_write="<<mirrored->extent()
      <<" hybrid_after_write="<<reopened->occupied()<<" selector_bytes="<<selector
      <<" retained_source_bytes="<<retained<<" workspace_bytes="<<workspace<<" staged_total_bytes="<<staged_total
      <<" mirror_first="<<mirror_first<<" mirror_range_read_bytes="<<reference_reads.backing_bytes
      <<" hybrid_range_read_bytes="<<mapped_reads.backing_bytes
      <<" mirror_range_ns="<<reference_reads.elapsed_ns<<" hybrid_range_ns="<<mapped_reads.elapsed_ns
      <<" independent_cpu_exact=1 sealed_writes=0\n";
}
}
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string_view(argv[1])=="--profile"){profile(argv[2]);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-hybrid"){profile_hybrid(argv[2],false);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-hybrid-mirror-first"){profile_hybrid(argv[2],true);return 0;}
    if(argc==2 && std::string_view(argv[1])=="--hybrid") {
      for(const auto unit:{4096U,65536U})hybrid_faults(unit);
      interrupted_hybrid_binding();
      return 0;
    }
    require(argc==1,"invalid protected image fixture options");
    for(auto unit:{4096U,65536U})native_stripes(unit);
    interrupted_seal();
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
