#include "store_codec.hpp"
#include "repeat_bytes.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
#ifdef FLITZFS_TEST_HAS_ZSTD
#include <zstd.h>
#endif
namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
template<class F>void rejects(F&& f,const char* reason){
  ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error(reason);
}
Bytes noise(std::size_t n) {
  Bytes b(n);std::uint64_t x=0x243f6a8885a308d3ULL;
  for(auto& value:b){x^=x<<13;x^=x>>7;x^=x<<17;value=static_cast<std::byte>(x>>19);}
  return b;
}
Bytes repeated(std::size_t n,std::size_t period) {
  auto motif=noise(period);Bytes b(n);
  for(std::size_t i=0;i<n;++i)b[i]=motif[i%period];return b;
}
void roundtrip(StoreCodecEncoder& encoder,StoreCodecDecoder& decoder,const Bytes& input) {
  const auto e=encoder.encode(input);
  require(e.payload.size()<=input.size(),"codec expanded stored payload");
  require(decoder.decode(e.codec,e.payload,static_cast<std::uint32_t>(input.size()))==input,"codec byte parity");
  require(e.codec==kStoreRaw?e.decoded_digest==Digest{}:e.decoded_digest==record_digest(input),"decoded digest binding");
  require(encoder.peak_bytes()<=kStoreEncoderMemoryCap && decoder.peak_bytes()<=kStoreDecoderMemoryCap,"codec exceeded memory cap");
}
void repeated_copy_guards() {
  for(auto period:{1U,2U,3U,17U,251U,4093U,4096U,16385U}) {
    const auto motif=noise(period);
    for(auto n:{0U,1U,31U,4095U,4096U,4097U,65537U}) {
      Bytes guarded(n+32,std::byte{0xa7});
      auto output=std::span<std::byte>(guarded).subspan(16,n);
      repeat_bytes(motif,output);
      for(std::size_t i=0;i<n;++i)require(output[i]==motif[i%period],"bulk repeat byte mismatch");
      require(std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==std::byte{0xa7};}) &&
              std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==std::byte{0xa7};}),"bulk repeat escaped output");
    }
  }
  Bytes untouched(17,std::byte{91});
  rejects([&]{repeat_bytes({},untouched);},"empty repeat motif accepted");
  require(untouched==Bytes(17,std::byte{91}),"invalid repeat changed output");
}
void policies_and_patterns() {
#ifdef FLITZFS_TEST_HAS_ZSTD
  require(store_zstd_available(),"required bounded Zstandard missing");
#else
  require(!store_zstd_available(),"raw-only build unexpectedly advertises Zstandard");
#endif
  for(int level:{1,6,9}) {
    StoreCodecEncoder encoder({StoreCompressionMode::automatic,level,false});
    StoreCodecDecoder decoder;
    for(auto n:{0U,1U,2U,63U,64U,4095U,4096U,4097U,65535U,65536U,65537U,1048576U}) {
      roundtrip(encoder,decoder,Bytes(n));
      roundtrip(encoder,decoder,Bytes(n,std::byte{73}));
      roundtrip(encoder,decoder,noise(n));
    }
    for(auto p:{2U,3U,17U,251U,4093U,4096U})roundtrip(encoder,decoder,repeated(131073,p));
    auto adversarial=repeated(131073,3);adversarial[8193]^=std::byte{1};
    auto e=encoder.encode(adversarial);
    require(e.codec!=kStorePattern,"sample-only period escaped full verification");
    roundtrip(encoder,decoder,adversarial);
    e=encoder.encode(Bytes(65537));
    require(e.codec==kStoreZero && e.payload.empty(),"zero extent not implicit");
    e=encoder.encode(Bytes(65537,std::byte{91}));
    require(e.codec==kStorePattern && e.payload.size()==1,"uniform extent not one-byte motif");
    e=encoder.encode(repeated(65537,3));
    require(e.codec==kStorePattern && e.payload.size()==3,"short motif not retained");
    e=encoder.encode(noise(65537));
    require(e.codec==kStoreRaw,"incompressible deterministic control expanded or misclassified");
  }
  StoreCodecEncoder raw({StoreCompressionMode::raw,6,false});StoreCodecDecoder decoder;
  for(const auto& b:{Bytes(4097),repeated(65537,3),noise(65537)}) {
    const auto e=raw.encode(b);require(e.codec==kStoreRaw && e.payload==b && e.decoded_digest==Digest{},"raw policy changed bytes");
    roundtrip(raw,decoder,b);
  }
  for(auto o:{StoreCompressionOptions{static_cast<StoreCompressionMode>(99),6,false},
              StoreCompressionOptions{StoreCompressionMode::automatic,0,false},
              StoreCompressionOptions{StoreCompressionMode::automatic,10,false},
              StoreCompressionOptions{StoreCompressionMode::raw,6,true}})
    rejects([&]{StoreCodecEncoder bad(o);},"invalid compression options accepted");
  if(!store_zstd_available())rejects([&]{StoreCodecEncoder bad({StoreCompressionMode::automatic,6,true});},"missing required codec accepted");
  rejects([&]{(void)raw.encode(Bytes(kStoreMaxObjectBytes+1));},"oversized encode admitted");
  rejects([&]{StoreCodecEncoder bad({},kStoreEncoderMemoryCap+1);},"encoder cap raised");
  rejects([&]{StoreCodecDecoder bad(kStoreDecoderMemoryCap+1);},"decoder cap raised");
}
void independent_lz4_record_oracle() {
  // Complete block: one literal, a 15-byte offset-one match, five final literals.
  // Derived from the pinned official block specification, not this encoder.
  const Bytes block{std::byte{0x1b},std::byte{'A'},std::byte{1},std::byte{0},std::byte{0x50},
      std::byte{'A'},std::byte{'A'},std::byte{'A'},std::byte{'A'},std::byte{'A'}};
  const Bytes source(21,std::byte{'A'});
  StoreCodecDecoder reader;
#ifdef FLITZFS_TEST_HAS_LZ4
  if(LZ4_versionNumber()<11101){
    rejects([&]{(void)reader.decode(4,block,21);},"unqualified old LZ4 returned stored bytes");return;
  }
  Bytes reference(source.size());
  require(LZ4_decompress_safe(reinterpret_cast<const char*>(block.data()),
      reinterpret_cast<char*>(reference.data()),static_cast<int>(block.size()),
      static_cast<int>(reference.size()))==21 && reference==source,"independent LZ4 block oracle");
  require(reader.decode(4,block,21)==source,"native LZ4 record CPU recovery");
  for(std::size_t n=1;n<block.size();++n)
    rejects([&]{(void)reader.decode(4,std::span(block).first(n),21);},"truncated LZ4 record admitted");
  auto trailing=block;trailing.push_back(std::byte{0});
  rejects([&]{(void)reader.decode(4,trailing,21);},"trailing LZ4 bytes admitted");
  rejects([&]{(void)reader.decode(4,block,20);},"undersized LZ4 output admitted");
  rejects([&]{(void)reader.decode(4,block,22);},"wrong LZ4 logical length admitted");
#else
  rejects([&]{(void)reader.decode(4,block,21);},"unavailable LZ4 recovery returned bytes");
#endif
}
void malformed_and_budget() {
  StoreCodecDecoder decoder;
  for(auto codec:{4U,99U,255U})rejects([&]{(void)decoder.decode(codec,{},32);},"unknown codec admitted");
  rejects([&]{(void)decoder.decode(kStoreZero,Bytes(1),32);},"zero payload admitted");
  rejects([&]{(void)decoder.decode(kStoreZero,{},0);},"zero empty logical admitted");
  rejects([&]{(void)decoder.decode(kStorePattern,{},32);},"empty motif admitted");
  rejects([&]{(void)decoder.decode(kStorePattern,Bytes(4097),8192);},"oversized motif admitted");
  rejects([&]{(void)decoder.decode(kStoreRaw,Bytes(31),32);},"raw mismatch admitted");
  rejects([&]{(void)decoder.decode(kStoreRaw,{},kStoreMaxObjectBytes+1);},"oversized decode admitted");
  rejects([&]{(void)decoder.decode(kStoreZstd,Bytes(32),32);},"non-saving compressed bytes admitted");
  for(auto magic:{0xFD2FB51EU,0xFD2FB527U,0x184D2A50U,0U}) {
    Bytes legacy(8);for(unsigned i=0;i<4;++i)legacy[i]=static_cast<std::byte>(magic>>(8*i));
    rejects([&]{(void)decoder.decode(kStoreZstd,legacy,32);},"legacy/unknown frame entered native decoder");
  }
#ifdef FLITZFS_TEST_HAS_ZSTD
  // Independent public-library encoding supplies an external frame oracle.
  auto source=noise(65537);
  for(std::size_t i=16384;i<source.size();++i)source[i]=source[i%16384];
  Bytes frame(ZSTD_compressBound(source.size()));
  const auto count=ZSTD_compress(frame.data(),frame.size(),source.data(),source.size(),3);
  require(!ZSTD_isError(count) && count<source.size(),"reference frame construction");frame.resize(count);
  require(decoder.decode(kStoreZstd,frame,static_cast<std::uint32_t>(source.size()))==source,"independent Zstandard frame recovery");
  auto trailing=frame;trailing.push_back(std::byte{0});
  rejects([&]{(void)decoder.decode(kStoreZstd,trailing,static_cast<std::uint32_t>(source.size()));},"trailing frame byte accepted");
  auto joined=frame;joined.insert(joined.end(),frame.begin(),frame.end());
  rejects([&]{(void)decoder.decode(kStoreZstd,joined,static_cast<std::uint32_t>(source.size()));},"concatenated frames accepted");
  rejects([&]{(void)decoder.decode(kStoreZstd,frame,static_cast<std::uint32_t>(source.size()-1));},"wrong decoded-size declaration accepted");
  for(std::size_t n:{std::size_t{1},frame.size()/2,frame.size()-1}) {
    auto short_frame=frame;short_frame.resize(n);
    rejects([&]{(void)decoder.decode(kStoreZstd,short_frame,static_cast<std::uint32_t>(source.size()));},"truncated frame accepted");
  }
  std::unique_ptr<ZSTD_CCtx,decltype(&ZSTD_freeCCtx)> context(ZSTD_createCCtx(),&ZSTD_freeCCtx);
  require(bool(context),"independent frame context");
  require(!ZSTD_isError(ZSTD_CCtx_setParameter(context.get(),ZSTD_c_contentSizeFlag,0)),"unknown-size oracle parameter");
  auto unknown=Bytes(ZSTD_compressBound(source.size()));
  const auto unknown_size=ZSTD_compress2(context.get(),unknown.data(),unknown.size(),source.data(),source.size());
  require(!ZSTD_isError(unknown_size) && unknown_size<source.size(),"unknown-size frame construction");
  unknown.resize(unknown_size);
  rejects([&]{(void)decoder.decode(kStoreZstd,unknown,static_cast<std::uint32_t>(source.size()));},"unknown-size frame accepted");
  const Bytes skip{std::byte{0x50},std::byte{0x2a},std::byte{0x4d},std::byte{0x18},
                   std::byte{0},std::byte{0},std::byte{0},std::byte{0}};
  rejects([&]{(void)decoder.decode(kStoreZstd,skip,32);},"skippable frame accepted as object content");
  StoreCodecDecoder starved(1);
  rejects([&]{(void)starved.decode(kStoreZstd,frame,static_cast<std::uint32_t>(source.size()));},"decoder ignored allocator cap");
  require(starved.peak_bytes()<=1 && starved.decode(kStoreRaw,Bytes(3),3)==Bytes(3),"decoder failed after resource rejection");
  StoreCodecEncoder hungry({},1);
  rejects([&]{(void)hungry.encode(source);},"encoder ignored allocator cap");
  require(hungry.peak_bytes()<=1 && hungry.encode(Bytes(17)).codec==kStoreZero,"encoder failed after resource rejection");
#else
  rejects([&]{(void)decoder.decode(kStoreZstd,Bytes(8),32);},"missing codec returned bytes");
#endif
}
}
int main(){
  try{independent_lz4_record_oracle();repeated_copy_guards();policies_and_patterns();malformed_and_budget();
    std::cout<<"PASS: "<<checks<<" native store compression checks; exact patterns, raw fallback, CPU frames and allocator caps.\n";return 0;
  }catch(const std::exception& e){std::cerr<<"store codec failure after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
