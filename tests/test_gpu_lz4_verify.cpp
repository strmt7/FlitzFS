#include "flitzfs/gpu_lz4_verify.hpp"
#include "flitzfs/gpu_lz4_layout.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
GpuLz4Verification expected(GpuLz4Verification code) {
  return gpu_lz4_cpu_verification_available()?code:GpuLz4Verification::unavailable;
}
Bytes literal(std::span<const std::byte> source) {
  Bytes result{static_cast<std::byte>(std::min<std::size_t>(source.size(),15U)<<4)};
  if(source.size()>=15) {
    auto left=source.size()-15;
    while(left>=255){result.push_back(std::byte{255});left-=255;}
    result.push_back(static_cast<std::byte>(left));
  }
  result.insert(result.end(),source.begin(),source.end());return result;
}
void check_candidate(const Bytes& input,const Bytes& payload,GpuLz4Verification code) {
  auto source=input,encoded=payload;
  Bytes guarded(input.size()+32,std::byte{0xa5});
  const auto result=verify_gpu_lz4_candidate(source,encoded,
      std::span<std::byte>(guarded).subspan(16,input.size()));
  require(result==expected(code),"Candidate verification classification");
  require(source==input && encoded==payload,"CPU verification modified reference/input");
  require(std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==std::byte{0xa5};}) &&
      std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==std::byte{0xa5};}),
      "CPU decoder wrote outside private scratch");
  if(result==GpuLz4Verification::verified)
    require(std::equal(source.begin(),source.end(),guarded.begin()+16),"Verified bytes differ");
  if(result==GpuLz4Verification::unavailable)
    require(std::all_of(guarded.begin(),guarded.end(),[](auto b){return b==std::byte{0xa5};}),
        "Unavailable decoder changed scratch");
}
}
int main(){try{
  using namespace flitzfs;
#ifdef FLITZFS_TEST_HAS_LZ4
  require(gpu_lz4_cpu_verification_available(),"Linked CPU verifier unavailable");
#else
  require(!gpu_lz4_cpu_verification_available(),"CPU verifier falsely available");
#endif
  for(const auto invalid:{0U,65537U,(std::numeric_limits<std::uint32_t>::max)()})
    require(gpu_lz4_detail::output_capacity(invalid)==0,"Unsafe size obtains output capacity");
  const std::array<std::size_t,17> sizes{1,2,3,4,14,15,16,254,255,256,257,4095,4096,4097,16384,65535,65536};
  for(const auto size:sizes)for(unsigned pattern=0;pattern<5;++pattern) {
    Bytes source(size);std::uint32_t random=0x812703adU;
    for(std::size_t i=0;i<size;++i) {
      random^=random<<13;random^=random>>17;random^=random<<5;
      source[i]=static_cast<std::byte>(pattern==0?0:pattern==1?i%7:
          pattern==2?(i/32)%251:pattern==3?(i*17U+i/11U)%256:random&255U);
    }
    check_candidate(source,literal(source),GpuLz4Verification::verified);
#ifdef FLITZFS_TEST_HAS_LZ4
    Bytes compressed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(size))));
    const auto encoded=LZ4_compress_default(reinterpret_cast<const char*>(source.data()),
        reinterpret_cast<char*>(compressed.data()),static_cast<int>(size),static_cast<int>(compressed.size()));
    require(encoded>0 && static_cast<std::size_t>(encoded)<=compressed.size(),"Independent CPU fixture compression");
    compressed.resize(static_cast<std::size_t>(encoded));
    check_candidate(source,compressed,GpuLz4Verification::verified);
#endif
  }
  Bytes source(4096);
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::byte>(i%251);
  auto payload=literal(source);const auto original=source,encoded=payload;
  Bytes scratch(source.size(),std::byte{0x5a});
  auto truncated=payload;truncated.pop_back();
  check_candidate(source,truncated,GpuLz4Verification::decode_failed);
  auto different=source;different[17]^=std::byte{1};
  check_candidate(source,literal(different),GpuLz4Verification::mismatch);
  auto short_source=source;short_source.pop_back();
  check_candidate(source,literal(short_source),GpuLz4Verification::decode_failed);
  const std::array<Bytes,5> malicious{
    Bytes{std::byte{0},std::byte{0},std::byte{0}},
    Bytes{std::byte{0xf0},std::byte{0xff}},
    Bytes{std::byte{0x10},std::byte{0x41},std::byte{1},std::byte{0}},
    Bytes{std::byte{0},std::byte{0xff},std::byte{0xff}},
    Bytes{std::byte{0xff},std::byte{0xff},std::byte{0xff},std::byte{0xff}}};
  for(const auto& bytes:malicious)check_candidate(source,bytes,GpuLz4Verification::decode_failed);
  // Invalid framing and aliasing must refuse before writing even without LZ4.
  const auto unchanged=scratch;
  require(verify_gpu_lz4_candidate(source,{},scratch)==GpuLz4Verification::invalid_extent,"Empty candidate admitted");
  Bytes oversized(gpu_lz4_detail::output_capacity(4096)+1,std::byte{0});
  require(verify_gpu_lz4_candidate(source,oversized,scratch)==GpuLz4Verification::invalid_extent,"Oversized candidate admitted");
  require(verify_gpu_lz4_candidate(source,payload,std::span<std::byte>(scratch).first(4095))==
      GpuLz4Verification::invalid_extent,"Short scratch admitted");
  Bytes long_scratch(4097,std::byte{0x5a});
  require(verify_gpu_lz4_candidate(source,payload,long_scratch)==GpuLz4Verification::invalid_extent,"Long scratch admitted");
  require(scratch==unchanged && std::all_of(long_scratch.begin(),long_scratch.end(),
      [](auto b){return b==std::byte{0x5a};}),"Framing refusal changed scratch");
  require(verify_gpu_lz4_candidate(source,payload,source)==GpuLz4Verification::overlapping_output,
      "Source overwrite admitted");
  require(verify_gpu_lz4_candidate(std::span<const std::byte>(source).first(4095),payload,
      std::span<std::byte>(source).subspan(1))==GpuLz4Verification::overlapping_output,"Partial source overlap");
  require(verify_gpu_lz4_candidate(source,payload,std::span<std::byte>(payload).first(4096))==
      GpuLz4Verification::overlapping_output,"Payload overwrite admitted");
  require(verify_gpu_lz4_candidate(source,payload,std::span<std::byte>(payload).subspan(1,4096))==
      GpuLz4Verification::overlapping_output,"Partial payload overlap");
  require(source==original && payload==encoded,"Overlap rejection corrupted fallback bytes");
  Bytes too_large(65537,std::byte{0}),too_large_scratch(65537,std::byte{0x5a});
  require(verify_gpu_lz4_candidate(too_large,payload,too_large_scratch)==GpuLz4Verification::invalid_extent,
      "Oversized source admitted");
  require(std::all_of(too_large_scratch.begin(),too_large_scratch.end(),[](auto b){return b==std::byte{0x5a};}),
      "Oversized source changed scratch");
  require(verify_gpu_lz4_candidate({},payload,{})==GpuLz4Verification::invalid_extent,"Empty source admitted");
  std::cout<<"GPU LZ4 CPU verification checks: "<<checks<<" lz4_available="
      <<gpu_lz4_cpu_verification_available()<<" gpu_executed=false stored_codec_added=false\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
