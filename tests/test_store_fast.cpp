#include "store_codec.hpp"
#include "native_fs_fixture.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f,const char* why){
  ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
Bytes input(std::size_t count,bool compressible){
  Bytes prefix(8192);std::uint64_t x=0x679a210fed44c3abULL;
  for(auto& b:prefix){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>21);}
  Bytes out(count);
  for(std::size_t i=0;i<count;++i){
    if(compressible)out[i]=prefix[i%prefix.size()];
    else {x^=x<<13;x^=x>>7;x^=x<<17;out[i]=static_cast<std::byte>(x>>21);}
  }return out;
}
void policies(){
  require(store_lz4_available()==bool(
#ifdef FLITZFS_TEST_HAS_LZ4
      LZ4_versionNumber()>=11101
#else
      false
#endif
      ),"CPU LZ4 capability disagrees with actual linking");
  StoreCodecEncoder fast({StoreCompressionMode::fast,6,false});
  StoreCodecEncoder raw({StoreCompressionMode::raw,6,false});
  StoreCodecDecoder reader;
  for(const auto n:{0U,1U,2U,63U,64U,4095U,4096U,8193U,65535U,65536U,65537U,1048576U})
    for(const bool patterned:{false,true}){
      const auto plain=input(n,patterned);
      const auto coded=fast.encode(plain);
      require(coded.payload.size()<=plain.size() &&
          reader.decode(coded.codec,coded.payload,n)==plain,"fast CPU exact recovery");
      require(coded.codec==kStoreRaw?coded.decoded_digest==Digest{}:
          coded.decoded_digest==record_digest(plain),"fast CPU digest binding");
      require(fast.peak_bytes()<=65536,"fast library state exceeded 64 KiB");
      const auto historical=raw.encode(plain);
      require(historical.codec==kStoreRaw && historical.payload==plain &&
          historical.decoded_digest==Digest{},"historical raw encoding changed");
    }
  require(fast.encode(Bytes(65537)).codec==kStoreZero,"fast zero representation lost");
  const auto motif=fast.encode(Bytes(65537,std::byte{0x51}));
  require(motif.codec==kStorePattern && motif.payload.size()==1,"fast motif representation lost");
  const auto plain=input(65536,true);
  const auto coded=fast.encode(plain);
  require(store_lz4_available()?coded.codec==kStoreLz4 && coded.payload.size()<plain.size():
      coded.codec==kStoreRaw,"fast data did not use independent LZ4 or explicit raw fallback");
  StoreCodecEncoder starved({StoreCompressionMode::fast,6,false},1);
  const auto fallback=starved.encode(plain);
  require(fallback.codec==kStoreRaw && fallback.payload==plain && starved.peak_bytes()==0,
      "fast CPU state pressure did not keep raw bytes");
  rejects([&]{StoreCodecEncoder bad({StoreCompressionMode::fast,6,true});},
      "fast profile claimed mandatory Zstandard");
}
void advisory(){
  const auto plain=input(65536,true);
  StoreCodecEncoder fast({StoreCompressionMode::fast,6,false});
  StoreCodecEncoder automatic;
  StoreCodecDecoder reader;
#ifdef FLITZFS_TEST_HAS_LZ4
  if(store_lz4_available()){
  Bytes candidate(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(plain.size()))));
  const auto n=LZ4_compress_default(reinterpret_cast<const char*>(plain.data()),
      reinterpret_cast<char*>(candidate.data()),static_cast<int>(plain.size()),static_cast<int>(candidate.size()));
  require(n>0 && static_cast<std::size_t>(n)<plain.size(),"independent candidate encode");
  candidate.resize(static_cast<std::size_t>(n));
  const auto retained=plain,encoded_copy=candidate;
  const auto picked=fast.encode_with_lz4(plain,candidate);
  require(picked.codec==kStoreLz4 && picked.payload==candidate &&
      reader.decode(picked.codec,picked.payload,static_cast<std::uint32_t>(plain.size()))==plain,
      "verified advisory did not enter fast native representation");
  require(fast.peak_bytes()==0,"valid fast advisory redundantly ran CPU compressor");
  require(plain==retained && candidate==encoded_copy,"advisory modified source or candidate");
  const auto base=automatic.encode(plain),selected=automatic.encode_with_lz4(plain,candidate);
  require(selected.payload.size()<=base.payload.size() &&
      reader.decode(selected.codec,selected.payload,static_cast<std::uint32_t>(plain.size()))==plain,
      "automatic advisory exceeded ordinary CPU baseline");
  candidate[0]^=std::byte{0x10};
  const auto expected=fast.encode(plain),bad=fast.encode_with_lz4(plain,candidate);
  require(bad.codec==expected.codec && bad.payload==expected.payload,"invalid advisory changed CPU encoding");
  auto wrong=plain;wrong[0]^=std::byte{1};
  const auto stale=fast.encode_with_lz4(wrong,encoded_copy);
  require(reader.decode(stale.codec,stale.payload,static_cast<std::uint32_t>(wrong.size()))==wrong,
      "stale advisory replaced the current CPU source");
  const auto small=Bytes(100,std::byte{0x41});
  // Use the independent encoder for the complete inferior-but-valid block.
  Bytes uniform(128);const auto u=LZ4_compress_default(reinterpret_cast<const char*>(small.data()),
      reinterpret_cast<char*>(uniform.data()),100,128);require(u>0,"uniform oracle");uniform.resize(static_cast<std::size_t>(u));
  const auto smaller=fast.encode_with_lz4(small,uniform);
  require(smaller.codec==kStorePattern && smaller.payload.size()==1,"advisory displaced a smaller motif");
  }
#endif
  for(const auto& candidate:std::array<Bytes,3>{Bytes{},Bytes(plain.size()),Bytes{std::byte{0xff}}}){
    const auto expected=fast.encode(plain),bad=fast.encode_with_lz4(plain,candidate);
    require(bad.codec==expected.codec && bad.payload==expected.payload,"malformed/non-saving advisory changed fallback");
  }
  StoreCodecEncoder raw({StoreCompressionMode::raw,6,false});
  const auto no_advice=raw.encode_with_lz4(plain,Bytes{std::byte{0xff}});
  require(no_advice.codec==kStoreRaw && no_advice.payload==plain,"raw mode processed advisory bytes");
}
void native_file_readback(){
  using namespace native_fixture;
  auto ram=std::make_shared<Ram>();
  auto volume=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),
      {StoreCompressionMode::fast,6,false}));
  auto original=input(131073,true),expected=original;
  const std::array<NativeFileMutation,2> create{file_create("content.bin"),
      file_write("content.bin",0,original)};
  volume.apply(create);auto pinned=volume.snapshot();
  require(pinned.read("CONTENT.BIN",0,static_cast<std::uint32_t>(original.size()))==original,
      "native extent reader failed fast CPU representation");
  if(store_lz4_available())require(pinned.storage_info().stored_payload_bytes<original.size(),
      "native fast fixture did not store compressed extents");
  const auto patch=input(31,false);
  file_apply(volume,file_write("content.bin",65530,patch));
  std::copy(patch.begin(),patch.end(),expected.begin()+65530);
  require(volume.snapshot().read("content.bin",0,static_cast<std::uint32_t>(expected.size()))==expected &&
      pinned.read("content.bin",0,static_cast<std::uint32_t>(original.size()))==original,
      "fast cross-extent overwrite changed current or retained bytes");
  file_apply(volume,file_truncate("content.bin",70000));expected.resize(70000);
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
  require(reopened.read("content.bin",0,131073)==expected,
      "independent native reopen did not preserve fast truncate/read semantics");
  reopened.verify_all();
}

}
int main(){
  try{policies();advisory();native_file_readback();std::cout<<"Fast native ingestion: "<<checks
      <<" checks; cpu_lz4="<<store_lz4_available()<<" gpu_executed=false\n";return 0;}
  catch(const std::exception& e){std::cerr<<"fast ingestion: "<<e.what()<<'\n';return 1;}
}
