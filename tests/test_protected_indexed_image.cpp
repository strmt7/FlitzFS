#include "native_corpus_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "protected_stripe_image.hpp"
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
}
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string_view(argv[1])=="--profile"){profile(argv[2]);return 0;}
    require(argc==1,"invalid protected image fixture options");
    for(auto unit:{4096U,65536U})native_stripes(unit);
    interrupted_seal();
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
