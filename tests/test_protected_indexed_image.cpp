#include "native_corpus_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/native_hybrid_backing.hpp"
#include "protected_stripe_image.hpp"
#include "folded_checkpoint.hpp"
#include "paused_capture_backing.hpp"
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
void concurrent_protected_reads() {
  constexpr auto unit=4096U;
  const auto expected=data(4*unit,327);
  auto source=image(expected,unit),sealed=empty(unit);
  (void)ProtectedStripeImage::seal(source,sealed,id(2),limits.maximum_physical_bytes);
  auto stopped=std::make_shared<test::PausedCaptureBacking>(sealed,prefix);
  const auto view=ProtectedStripeImage::open(stopped,limits.maximum_physical_bytes);
  auto capture=std::async(std::launch::async,[&] {
    const std::function<bool()> running;
    const detail::StoreCaptureReadScope scope(running);
    Bytes bytes(unit);
    view->read_exact(0,bytes);
    return bytes;
  });
  require(stopped->wait_until_held(),"protected capture did not reach its I/O hold");
  view->verify_all();
  require(capture.wait_for(std::chrono::seconds(0))==std::future_status::timeout,
      "protected cache held verification behind slow capture I/O");
  stopped->release();
  require(std::ranges::equal(capture.get(),std::span(expected).first(unit)),"held capture changed checked bytes");
  const auto before=stopped->reads();
  Bytes demand(unit);
  view->read_exact(0,demand);
  require(stopped->reads()>before && !view->degraded() && std::ranges::equal(demand,std::span(expected).first(unit)),
      "an older capture repopulated the audited cache or changed demand bytes");
  std::cout<<"concurrent_protected_read audit_while_held=1 old_cache_insert_refused=1 exact=1\n";
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
struct CooperativeProgress {
  std::uint64_t quanta=0,max_workspace_write=0,max_seal_write=0,max_binding_write=0;
  std::size_t seal_write_steps=0,binding_flush_steps=0;
};

void advance_hybrid(NativeHybridBacking::Build& work,NativeFileSession& source,const Ram& workspace,
    const Ram& base,const Ram& tail,std::uint64_t source_extent,CooperativeProgress& progress) {
  using Phase=NativeHybridBacking::BuildPhase;
  auto visited=work.phase();
  while(visited!=Phase::ready) {
    require(++progress.quanta<128+source_extent/32,"cooperative build made no bounded progress");
    const auto workspace_written=workspace.write_bytes,base_written=base.write_bytes,tail_written=tail.write_bytes;
    const auto workspace_flushes=workspace.flushes,base_flushes=base.flushes,tail_flushes=tail.flushes;
    const auto ready=work.advance(source);
    switch(visited) {
      case Phase::workspace:
        progress.max_workspace_write=(std::max)(progress.max_workspace_write,workspace.write_bytes-workspace_written);
        require(workspace.flushes-workspace_flushes<=1 && base.write_bytes==base_written &&
            tail.write_bytes==tail_written,"workspace stage did not yield between barriers or wrote into the candidate");
        break;
      case Phase::sealing: {
        const auto wrote=base.write_bytes-base_written;
        progress.max_seal_write=(std::max)(progress.max_seal_write,wrote);
        progress.seal_write_steps+=wrote!=0;
        require(base.flushes-base_flushes<=1 && tail.write_bytes==tail_written,
            "sealing issued multiple barriers or touched the mutable tail");
        break;
      }
      case Phase::binding:
        progress.max_binding_write=(std::max)(progress.max_binding_write,tail.write_bytes-tail_written);
        require(tail.flushes-tail_flushes==1 && base.write_bytes==base_written,
            "binding did not keep barriers in separate tail-only steps");
        ++progress.binding_flush_steps;
        break;
      default:
        fail("unexpected cooperative phase");
    }
    visited=work.phase();
    require(ready==(visited==Phase::ready),"cooperative readiness differed from the staged state");
  }
  require(progress.seal_write_steps>=2 && progress.binding_flush_steps==3 &&
      progress.max_seal_write<=(std::max<std::uint64_t>)(prefix,
          (detail::kProtectedStripeDataUnits+2)*static_cast<std::uint64_t>(base.failure_unit)) &&
      progress.max_binding_write<=tail_prologue,"cooperative phase exceeded its write unit");
}


std::shared_ptr<NativeHybridBacking> reopen_hybrid(std::shared_ptr<StoreByteIo> base,std::shared_ptr<StoreByteIo> tail,
    StoreIdentity identity=id(2)) {
  return NativeHybridBacking::open(std::move(base),std::move(tail),identity,
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

void deferred_hybrid_placement() {
  using Phase=NativeHybridBacking::BuildPhase;
  constexpr auto unit=4096U;
  const std::array files{data(847'389,94)};
  auto source=make_indexed(files,unit,raw),head=empty(unit);
  {const auto writer=NativeVolumeHeadWriter::create(head,id(9),NativeFilesystemSnapshot::open_image(source,ordinary_storage));}
  auto session=NativeFileSession::resume_closed_indexed_volume(head,id(9),1,
      [&](const auto& identity){return identity==id(1)?source:nullptr;},raw);
  const auto old=head->stable;
  const auto make_work=[&](const auto& workspace) {
    return NativeHybridBacking::prepare(session,workspace,StoreIoContract{unit,true},id(2),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
  };
  const auto advance_to=[&](auto& work,Phase phase) {
    for(unsigned i=0;work.phase()!=phase;++i)
      require(i<8192 && !work.advance(session),"deferred hybrid skipped a store request");
  };
  {
    auto scratch=empty(unit),wrong=empty(65536);
    auto work=make_work(scratch);
    advance_to(work,Phase::awaiting_base);
    rejects([&]{work.provide_store(session,wrong);},"deferred base accepted different geometry");
    require(work.phase()==Phase::stopped && !wrong->writes && head->stable==old,
        "base refusal wrote to the target or selector");
  }

  auto scratch=empty(unit),base=empty(unit),tail=empty(unit);
  auto work=make_work(scratch);
  advance_to(work,Phase::awaiting_base);
  const auto base_bytes=work.required_store_bytes();
  require(base_bytes && scratch.use_count()==2 && !work.advance(session) &&
      work.phase()==Phase::awaiting_base && !base->writes,
      "waiting for an exact base changed the candidate");
  work.provide_store(session,base);
  advance_to(work,Phase::awaiting_tail);
  const auto tail_bytes=work.required_store_bytes();
  require(base->extent()==base_bytes && scratch.use_count()==1 && tail_bytes>=tail_prologue &&
      base_bytes+tail_bytes<=limits.maximum_physical_bytes && !tail->writes,
      "completed seal did not release its workspace or price the writable tail");
  scratch.reset();
  auto file=session.open(path(0),{true,true,false});
  const auto edited=data(139,71);
  session.write(file,20,edited);
  rejects([&]{work.provide_store(session,tail);},"deferred tail accepted a stale source checkpoint");
  require(work.phase()==Phase::stopped && !tail->writes && head->stable==old &&
      session.read(file,20,edited.size())==edited,"stale placement changed selection or lost the edit");
  session.cleanup(file,false);
  session.close(file);
  std::cout<<"deferred_hybrid_placement base_bytes="<<base_bytes<<" tail_reservation="<<tail_bytes
      <<" workspace_released=1 invalid_contract_refused=1 stale_refused=1\n";
}

void cooperative_hybrid() {
  using Phase=NativeHybridBacking::BuildPhase;
  constexpr auto unit=4096U;
  const std::array files{data(1'913'821,810),data(1'063'482,183)};
  auto source=make_indexed(files,unit,raw),head=empty(unit);
  const auto original=NativeFilesystemSnapshot::open_image(source,ordinary_storage);
  {const auto authority=NativeVolumeHeadWriter::create(head,id(9),original);}
  const auto selected=head->stable;
  const auto source_writes=source->writes;
  auto workspace=empty(unit),base=empty(65536),tail=empty(unit);
  const auto resolve=[&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo> {
    return value==id(1)?source:nullptr;
  };
  std::shared_ptr<NativeHybridBacking> candidate;
  CooperativeProgress progress;
  {
    auto session=NativeFileSession::resume_closed_indexed_volume(head,id(9),1,resolve,raw);
    auto handle=session.open(path(0),{true,true,false});
    auto incompatible_tail=empty(65536);
    rejects([&]{(void)NativeHybridBacking::prepare(session,workspace,base,incompatible_tail,id(2),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);},
        "cooperative build admitted a tail with an incompatible failure unit");
    {
      auto over_budget=NativeHybridBacking::prepare(session,workspace,base,tail,id(2),limits.maximum_physical_bytes,
          tail_prologue,raw);
      rejects([&]{advance_hybrid(over_budget,session,*workspace,*base,*tail,source->extent(),progress);},
          "cooperative build ignored its combined staging budget");
      require(over_budget.phase()==Phase::stopped && !workspace->writes && !base->writes && !tail->writes,
          "staging budget was evaluated after an output write");
    }
    progress={};
    auto work=NativeHybridBacking::prepare(session,workspace,base,tail,id(2),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    require(work.phase()==Phase::workspace && !workspace->writes && !base->writes && !tail->writes,
        "prepare wrote into its private workspace");
    rejects([&]{(void)work.finish(session);},"unfinished preparation escaped");
    rejects([&]{(void)work.publish(session,candidate);},"unfinished preparation selected its candidate");
    require(!candidate && head->stable==selected,"unready publication changed ownership");
    require(!work.advance(session),"workspace finished before traversing even one object");
    const auto expected=std::span(files[0]).subspan(kNativeExtentBytes-31,257);
    require(std::ranges::equal(session.read(handle,kNativeExtentBytes-31,expected.size()),expected),
        "ordinary read failed while hybrid preparation was paused");
    advance_hybrid(work,session,*workspace,*base,*tail,source->extent(),progress);
    require(work.advance(session),"completed preparation lost readiness");
    require(source->writes==source_writes && head->stable==selected,"preparation changed the selected image");
    const auto head_calls=head->calls;
    require(work.publish(session,candidate),"the smaller prepared hybrid was declined");
    const auto selection=session.volume_info().selection;
    require(work.phase()==Phase::stopped && candidate && candidate->occupied()==base->extent()+tail->extent() &&
        source->writes==source_writes && head->calls==head_calls+4 &&
        selection.epoch==2 && selection.image_identity==id(2) && selection.parent_identity==id(1),
        "the live hybrid did not publish one ordered selector pair");
    require(std::ranges::equal(session.read(handle,kNativeExtentBytes-31,expected.size()),expected),
        "the existing handle lost its file on live selection");

    auto another_workspace=empty(unit),another_base=empty(65536),another_tail=empty(unit);
    auto duplicate=NativeHybridBacking::prepare(session,another_workspace,another_base,another_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    CooperativeProgress duplicate_progress;
    advance_hybrid(duplicate,session,*another_workspace,*another_base,*another_tail,candidate->extent(),duplicate_progress);
    std::shared_ptr<NativeHybridBacking> unselected;
    const auto unchanged=head->stable;
    require(!duplicate.publish(session,unselected) && !unselected && head->stable==unchanged,
        "an equally sized hybrid displaced the selected image");

    const auto patch=data(3988,209);
    const auto frozen_writes=base->writes;
    require(session.write(handle,kNativeExtentBytes-31,patch).bytes==patch.size() &&
        session.read(handle,kNativeExtentBytes-31,patch.size())==patch && base->writes==frozen_writes,
        "the still-open handle did not write the selected tail");

    auto mirror=empty(unit);
    require(session.begin_idle_compaction(mirror,id(4)),"could not evaluate the mirrored replacement");
    auto rewrite_progress=NativeFileSession::RecompressionProgress::copying;
    for(unsigned steps=0;rewrite_progress==NativeFileSession::RecompressionProgress::copying && steps<10000;++steps)
      rewrite_progress=session.advance_recompression();
    std::cout<<"live_hybrid_physical_choice status="<<static_cast<unsigned>(rewrite_progress)
        <<" selected_physical_bytes="<<candidate->occupied()<<" selected_logical_extent="<<candidate->extent()
        <<" mirror_bytes="<<mirror->extent()<<'\n';
    require(rewrite_progress==NativeFileSession::RecompressionProgress::ready && mirror->extent()<candidate->extent() &&
        mirror->extent()>candidate->occupied(),"physical comparison fixture lacks a larger mirrored candidate");
    require(!session.publish_idle_recompression() && head->stable==unchanged,
        "a smaller virtual extent displaced a smaller physical hybrid");
    verify(original,files);
    session.cleanup(handle,false);
    session.close(handle);

    auto patched=files;
    std::ranges::copy(patch,patched[0].begin()+kNativeExtentBytes-31);
    verify(NativeFilesystemSnapshot::open_image(candidate,ordinary_storage),patched);
  }

  auto stale_head=empty(unit);
  {const auto authority=NativeVolumeHeadWriter::create(stale_head,id(9),original);}
  const auto stale_selection=stale_head->stable;
  {
    auto session=NativeFileSession::resume_closed_indexed_volume(stale_head,id(9),1,resolve,raw);
    auto handle=session.open(path(0),{true,true,false});
    auto staged=empty(unit),cold=empty(unit),mutable_tail=empty(unit);
    auto work=NativeHybridBacking::prepare(session,staged,cold,mutable_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    while(work.phase()==Phase::workspace)require(!work.advance(session),"workspace published unexpectedly");
    require(work.phase()==Phase::sealing && !work.advance(session) && cold->writes,
        "stale-source fixture did not partially seal its candidate");
    const auto partial=cold->writes;
    const auto patch=data(257,193);
    require(session.write(handle,kNativeExtentBytes-31,patch).bytes==patch.size(),
        "foreground write did not finish during preparation");
    rejects([&]{(void)work.advance(session);},"a changed foreground checkpoint continued sealing");
    require(work.phase()==Phase::stopped && cold->writes==partial && !mutable_tail->writes &&
        stale_head->stable==stale_selection,"stale work published or issued another write");
    require(session.read(handle,kNativeExtentBytes-31,patch.size())==patch,
        "stale candidate changed the acknowledged foreground bytes");

    staged=empty(unit);cold=empty(unit);mutable_tail=empty(unit);
    work=NativeHybridBacking::prepare(session,staged,cold,mutable_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    while(work.phase()!=Phase::binding)require(!work.advance(session),"binding fixture published unexpectedly");
    require(!work.advance(session),"tail published before both binding roots");
    const auto prior_writes=mutable_tail->writes;
    mutable_tail->failure_unit=0;
    rejects([&]{(void)work.advance(session);},"a tail with an unavailable contract was written again");
    require(work.phase()==Phase::stopped && mutable_tail->writes==prior_writes && stale_head->stable==stale_selection,
        "loss of the staged medium changed selection or wrote another binding root");

    // A later preparation can claim new private resources after cancellation.
    staged=empty(unit);cold=empty(unit);mutable_tail=empty(unit);
    work=NativeHybridBacking::prepare(session,staged,cold,mutable_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    require(!work.advance(session),"cancel fixture finished immediately");
    work.cancel();
    work=NativeHybridBacking::prepare(session,staged,cold,mutable_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    CooperativeProgress last;
    advance_hybrid(work,session,*staged,*cold,*mutable_tail,source->extent(),last);
    const auto replacement=data(257,915);
    require(session.write(handle,kNativeExtentBytes-31,replacement).bytes==replacement.size(),
        "foreground mutation after the candidate was ready failed");
    std::shared_ptr<NativeHybridBacking> rejected;
    rejects([&]{(void)work.publish(session,rejected);},"a ready candidate escaped after its checkpoint changed");
    require(work.phase()==Phase::stopped && !rejected && stale_head->stable==stale_selection &&
        session.read(handle,kNativeExtentBytes-31,replacement.size())==replacement,
        "stale completion invalidated selected foreground data");

    staged=empty(unit);cold=empty(unit);mutable_tail=empty(unit);
    work=NativeHybridBacking::prepare(session,staged,cold,mutable_tail,id(3),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    CooperativeProgress binding;
    advance_hybrid(work,session,*staged,*cold,*mutable_tail,source->extent(),binding);
    damage(*mutable_tail,0,unit);
    rejects([&]{(void)work.publish(session,rejected);},"a changed binding reached the volume selector");
    require(work.phase()==Phase::stopped && !rejected && !session.poisoned() && stale_head->stable==stale_selection &&
        session.read(handle,kNativeExtentBytes-31,replacement.size())==replacement,
        "detached binding damage fenced or changed the selected image");
  }
  std::cout<<"cooperative_hybrid quanta="<<progress.quanta<<" seal_write_steps="<<progress.seal_write_steps
      <<" max_seal_write_bytes="<<progress.max_seal_write<<" binding_flush_steps="<<progress.binding_flush_steps
      <<" max_binding_write_bytes="<<progress.max_binding_write
      <<" mixed_failure_units=1 exact_cpu_readback=1 live_selection=1 retained_handle=1"
      <<" stale_refusals=2 staging_refusals=1 lost_contract_refusals=1 binding_refusals=1 growth_refusals=2\n";
}

void live_hybrid_selection_faults(std::uint32_t unit) {
  constexpr unsigned selector_steps=4;
  const std::array files{data(713'821,810),data(263'482,183)};
  const auto original=make_indexed(files,unit,raw);
  const auto at=kNativeExtentBytes-31;
  const auto expected=std::span(files[0]).subspan(at,257);
  unsigned previous=0,replacement=0;
  for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<selector_steps;++cut) {
    auto source=image(original->stable,unit),head=empty(unit);
    {const auto authority=NativeVolumeHeadWriter::create(head,id(9),
        NativeFilesystemSnapshot::open_image(source,ordinary_storage));}
    const auto source_image=[&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{
      return value==id(1)?source:nullptr;
    };
    auto session=NativeFileSession::resume_closed_indexed_volume(head,id(9),1,source_image,raw);
    auto handle=session.open(path(0),{true,true,false});
    auto workspace=empty(unit),base=empty(65536),tail=empty(unit);
    auto work=NativeHybridBacking::prepare(session,workspace,base,tail,id(2),
        limits.maximum_physical_bytes,limits.maximum_physical_bytes,raw);
    CooperativeProgress progress;
    advance_hybrid(work,session,*workspace,*base,*tail,source->extent(),progress);
    head->failure=mode;
    head->fail_at=head->calls+cut;
    std::shared_ptr<NativeHybridBacking> retained;
    rejects([&]{(void)work.publish(session,retained);},"interrupted selector returned success");
    require(session.poisoned() && retained,"uncertain publication released its owned candidate");
    rejects([&]{(void)session.read(handle,at,expected.size());},"an uncertain session exposed an open handle");

    auto stable_source=image(source->stable,unit);
    auto stable_candidate=reopen_hybrid(image(base->stable,65536),image(tail->stable,unit));
    auto restart=NativeVolumeHeadView::open(image(head->stable,unit),id(9),
        [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{
          if(value==id(1))return stable_source;
          return value==id(2)?stable_candidate:nullptr;
        });
    verify(restart.filesystem(),files);
    if(restart.info().selection.image_identity==id(1))++previous;
    else if(restart.info().selection.image_identity==id(2))++replacement;
    else fail("selector chose unrelated backing");

    head->fail_at=(std::numeric_limits<std::size_t>::max)();
    const auto restored=session.recover_volume().selection;
    require((restored.image_identity==id(1) || restored.image_identity==id(2)) &&
        std::ranges::equal(session.read(handle,at,expected.size()),expected),
        "selector recovery lost the existing file context");
    session.cleanup(handle,false);
    session.close(handle);
  }
  require(previous && replacement,"live selector cuts missed one durable outcome");
  std::cout<<"live_hybrid_selector unit="<<unit<<" cuts="<<previous+replacement<<" previous="<<previous
      <<" replacement="<<replacement<<" exact_reopen=1 retained_handle=1\n";
}

void profile_cooperative(const std::filesystem::path& corpus,bool publish_live=false) {
  require(store_zstd_available(),"cooperative corpus profile requires Zstandard");
  constexpr auto unit=4096U;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  const auto files=read_packed_corpus(corpus,true);
  auto source=make_indexed(files,unit,policy),head=empty(unit);
  const auto original=NativeFilesystemSnapshot::open_image(source,ordinary_storage);
  {const auto authority=NativeVolumeHeadWriter::create(head,id(9),original);}
  const auto selected=head->stable;
  auto workspace=empty(unit),base=empty(unit),tail=empty(unit);
  auto session=NativeFileSession::resume_closed_indexed_volume(head,id(9),1,
      [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{return value==id(1)?source:nullptr;},policy);
  auto work=NativeHybridBacking::prepare(session,workspace,base,tail,id(2),
      limits.maximum_physical_bytes,limits.maximum_physical_bytes,policy);
  const auto handle=publish_live?session.open(path(0),{true,true,false}):NativeFileHandle{};
  CooperativeProgress progress;
  advance_hybrid(work,session,*workspace,*base,*tail,source->extent(),progress);
  const auto read_bytes=[&]{return head->read_bytes+source->read_bytes+workspace->read_bytes+base->read_bytes+tail->read_bytes;};
  std::shared_ptr<NativeHybridBacking> candidate;
  const auto read_before=read_bytes(),writes_before=head->calls;
  if(publish_live)require(work.publish(session,candidate),"public hybrid was not selected");
  else candidate=work.finish(session);
  const auto selection_reads=read_bytes()-read_before;
  const auto selector_operations=head->calls-writes_before;
  if(publish_live) {
    const auto chosen=session.volume_info().selection;
    const auto expected=std::span(files[0]).subspan(kNativeExtentBytes-31,257);
    require(chosen.image_identity==id(2) && chosen.parent_identity==id(1) && chosen.epoch==2 &&
        selector_operations==4 && std::ranges::equal(session.read(handle,kNativeExtentBytes-31,expected.size()),expected),
        "public live selector did not preserve the open file context");
    session.cleanup(handle,false);
    session.close(handle);
    const auto restarted=reopen_hybrid(image(base->stable,unit),image(tail->stable,unit));
    const auto checked=NativeVolumeHeadView::open(image(head->stable,unit),id(9),
        [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{
          return value==id(2)?restarted:nullptr;
        });
    verify(checked.filesystem(),files);
  } else {
    require(head->stable==selected,"cooperative profile changed the selector");
    verify(NativeFilesystemSnapshot::open_image(candidate,ordinary_storage),files);
  }
  std::cout<<"cooperative_profile files="<<files.size()<<" quanta="<<progress.quanta
      <<" retained_source_bytes="<<source->extent()<<" selector_bytes="<<head->extent()
      <<" workspace_bytes="<<workspace->extent()<<" hybrid_bytes="<<candidate->occupied()
      <<" staged_total_bytes="<<source->extent()+head->extent()+workspace->extent()+candidate->occupied()
      <<" max_workspace_write_bytes="<<progress.max_workspace_write
      <<" max_seal_write_bytes="<<progress.max_seal_write<<" binding_flush_steps="<<progress.binding_flush_steps
      <<" max_binding_write_bytes="<<progress.max_binding_write
      <<" exact_cpu_readback=1 selection_unchanged="<<!publish_live;
  if(publish_live)std::cout<<" live_selection=1 retained_handle=1 selection_read_bytes="<<selection_reads
      <<" selector_operations="<<selector_operations<<" mirrored_address_bytes="<<candidate->extent();
  std::cout<<'\n';
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

struct ColdSealResult {
  std::uint64_t occupied=0,writes=0;
  bool selected=false,byte_planes=false;
};

ColdSealResult measure_cold_seal(NativeFileSession& session,Ram& head,std::span<const Bytes> files,
    StoreCompressionOptions policy,const char* arm,bool repair_sample) {
  const auto unit=head.contract().write_failure_unit_bytes;
  auto workspace=empty(unit),base=empty(unit),tail=empty(unit);
  const auto source=session.snapshot();
  const auto head_writes=head.write_bytes;
  auto work=NativeHybridBacking::prepare(session,workspace,base,tail,id(3),
      limits.maximum_physical_bytes,limits.maximum_physical_bytes,policy);
  CooperativeProgress progress;
  try {
    advance_hybrid(work,session,*workspace,*base,*tail,source.storage_info().physical_end,progress);
  }catch(const std::runtime_error& error) {
    if(std::string_view(error.what())!="hybrid base and tail do not reduce the protected footprint")throw;
    require(!base->write_bytes && !tail->write_bytes && head.write_bytes==head_writes,
        "unprofitable sealing changed authority or destination");
    verify(session.snapshot(),files);
    const auto occupied=head.extent()+source.storage_info().physical_end;
    std::cout<<"cold_placement arm="<<arm<<" occupied_bytes="<<occupied<<" selector_bytes="<<head.extent()
        <<" build_write_bytes="<<workspace->write_bytes<<" selected=0 declined_no_gain=1 cpu_exact=1\n";
    return {occupied,workspace->write_bytes};
  }
  std::shared_ptr<NativeHybridBacking> published;
  const bool selected=work.publish(session,published);
  verify(source,files);

  auto recovered_base=image(base->stable,unit),recovered_tail=image(tail->stable,unit);
  if(repair_sample)damage(*recovered_base,prefix,unit);
  const auto recovered=reopen_hybrid(recovered_base,recovered_tail,id(3));
  std::array<std::byte,4> feature{};
  recovered->read_exact(176,feature); // Paged root required features.
  const bool byte_planes=feature[0]==std::byte{1};
  const auto reopened=NativeFilesystemSnapshot::open_image(recovered,ordinary_storage);
  verify(reopened,files);
  if(selected) {
    const auto chosen=NativeVolumeHeadView::open(image(head.stable,unit),id(9),
        [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{return value==id(3)?recovered:nullptr;});
    require(chosen.info().selection.image_identity==id(3),"cold sealed selector did not recover the winner");
    verify(chosen.filesystem(),files);
  }
  const auto tail_reads=recovered_tail->read_bytes;
  const auto sampled=measure_reads(reopened,*recovered_base,files);
  const auto writes=workspace->write_bytes+base->write_bytes+tail->write_bytes+head.write_bytes-head_writes;
  const auto occupied=head.extent()+recovered->occupied();
  require(recovered->occupied()==base->extent()+tail->extent(),"cold sealed occupancy omitted a region");
  std::cout<<"cold_placement arm="<<arm<<" occupied_bytes="<<occupied<<" selector_bytes="<<head.extent()
      <<" base_bytes="<<base->extent()<<" tail_bytes="<<tail->extent()<<" workspace_bytes="<<workspace->extent()
      <<" workspace_write_bytes="<<workspace->write_bytes<<" backing_write_bytes="<<writes-workspace->write_bytes
      <<" build_write_bytes="<<writes<<" sampled_range_read_bytes="<<sampled.backing_bytes+recovered_tail->read_bytes-tail_reads
      <<" selected="<<selected<<" byte_planes="<<byte_planes<<" repaired_unit="<<repair_sample<<" cpu_exact=1\n";
  return {occupied,writes,selected,byte_planes};
}

void cold_placement(std::span<const Bytes> files,bool recovery_case) {
  constexpr auto unit=4096U;
  constexpr StoreCompressionOptions ordinary{StoreCompressionMode::automatic,3,true};
  constexpr StoreCompressionOptions planes{StoreCompressionMode::automatic,3,true,true};
  const auto initial=make_indexed(files,unit,ordinary),head=empty(unit);
  {
    const auto initial_view=NativeFilesystemSnapshot::open_image(initial,ordinary_storage);
    const auto selector=NativeVolumeHeadWriter::create(head,id(9),initial_view);
  }
  const auto& selected=head->stable;
  ColdSealResult staged;
  std::uint64_t staged_writes=0;
  for(const auto policy:{ordinary,planes}) {
    const bool transformed=policy.experimental_cold_byte_planes;
    auto input=image(initial->stable,unit),selector=image(selected,unit),mirror=empty(unit);
    {
      auto session=NativeFileSession::resume_closed_indexed_volume(selector,id(9),1,
          [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{return value==id(1)?input:nullptr;},policy);
      const auto selector_writes=selector->write_bytes;
      require(session.begin_idle_compaction(mirror,id(2),PagedRewriteMode::reconcile_foreground),
          "cold placement did not start the matched mirrored copy");
      using Progress=NativeFileSession::RecompressionProgress;
      auto progress=Progress::copying;
      for(std::uint64_t turn=0;progress==Progress::copying && turn<1024+initial->extent()/64;++turn)
        progress=session.advance_recompression();
      require(progress==Progress::ready && session.publish_idle_recompression().has_value(),
          "cold placement did not select the matched mirrored copy");
      const auto packed=session.snapshot();
      verify(packed,files);
      const auto mirror_writes=mirror->write_bytes+selector->write_bytes-selector_writes;
      const auto sampled=measure_reads(packed,*mirror,files);
      std::cout<<"cold_placement arm="<<(transformed?"planes_mirror":"ordinary_mirror")
          <<" occupied_bytes="<<packed.storage_info().physical_end+selector->extent()
          <<" selector_bytes="<<selector->extent()<<" build_write_bytes="<<mirror_writes
          <<" sampled_range_read_bytes="<<sampled.backing_bytes<<" selected=1 cpu_exact=1\n";
      auto sealed=measure_cold_seal(session,*selector,files,ordinary,
          transformed?"planes_seal":"ordinary_seal",false);
      if(transformed) {
        staged=sealed;
        staged_writes=mirror_writes+sealed.writes;
      }
    }
  }
  auto input=image(initial->stable,unit),selector=image(selected,unit);
  auto session=NativeFileSession::resume_closed_indexed_volume(selector,id(9),1,
      [&](const StoreIdentity& value)->std::shared_ptr<StoreByteIo>{return value==id(1)?input:nullptr;},planes);
  const auto fused=measure_cold_seal(session,*selector,files,planes,"planes_fused",recovery_case);
  if(recovery_case) {
    require(staged.selected && fused.selected && staged.byte_planes && fused.byte_planes,
        "cold sealing did not preserve the required byte-plane representation");
    require(fused.occupied<=staged.occupied && fused.writes<staged_writes,
        "one-pass cold sealing did not avoid the staged mirror");
  }
  std::cout<<"cold_placement_summary staged_write_bytes="<<staged_writes
      <<" fused_write_bytes="<<fused.writes<<" staged_seal_bytes="<<staged.occupied
      <<" fused_seal_bytes="<<fused.occupied<<'\n';
}

void cold_seal_recovery() {
  if(!store_zstd_available())return;
  std::array<Bytes,4> files;
  for(std::size_t file=0;file<files.size();++file) {
    auto& bytes=files[file];
    bytes=data(13*kNativeExtentBytes-1,52000+static_cast<unsigned>(file));
    for(std::size_t at=0;at+3<bytes.size();at+=4) {
      bytes[at+2]=std::byte{4};
      bytes[at+3]=std::byte{7};
    }
  }
  cold_placement(files,true);
}
}
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string_view(argv[1])=="--profile"){profile(argv[2]);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-hybrid"){profile_hybrid(argv[2],false);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-hybrid-mirror-first"){profile_hybrid(argv[2],true);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-cooperative"){profile_cooperative(argv[2]);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-live-hybrid"){profile_cooperative(argv[2],true);return 0;}
    if(argc==3 && std::string_view(argv[1])=="--profile-cold-placement"){
      require(store_zstd_available(),"cold placement profile requires Zstandard");
      cold_placement(read_packed_corpus(argv[2],true),false);
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--hybrid") {
      for(const auto unit:{4096U,65536U})hybrid_faults(unit);
      interrupted_hybrid_binding();
      deferred_hybrid_placement();
      cooperative_hybrid();
      for(const auto unit:{4096U,65536U})live_hybrid_selection_faults(unit);
      cold_seal_recovery();
      return 0;
    }
    require(argc==1,"invalid protected image fixture options");
    concurrent_protected_reads();
    for(auto unit:{4096U,65536U})native_stripes(unit);
    interrupted_seal();
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
