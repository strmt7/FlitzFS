#include "record_digest.hpp"
#include "blake3.h"
#include "blake3_vectors.hpp"
#include "hosted_digest_limits.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" {
void flitzfs_oracle_blake3_hasher_init(blake3_hasher*);
void flitzfs_oracle_blake3_hasher_update(blake3_hasher*,const void*,size_t);
void flitzfs_oracle_blake3_hasher_finalize(const blake3_hasher*,uint8_t*,size_t);
void flitzfs_masked_blake3_hasher_init(blake3_hasher*);
void flitzfs_masked_blake3_hasher_update(blake3_hasher*,const void*,size_t);
void flitzfs_masked_blake3_hasher_finalize(const blake3_hasher*,uint8_t*,size_t);
uint32_t flitzfs_masked_detect(void);
uint32_t flitzfs_masked_limit(uint32_t);
size_t flitzfs_masked_blake3_simd_degree(void);
size_t blake3_simd_degree(void);
}
namespace {
using namespace flitzfs::detail;
using Bytes=std::vector<std::byte>;
using View=std::span<const std::byte>;
using Clock=std::chrono::steady_clock;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
Digest scalar(View a,View b={}) {
  blake3_hasher state;flitzfs_oracle_blake3_hasher_init(&state);
  flitzfs_oracle_blake3_hasher_update(&state,a.data(),a.size());
  flitzfs_oracle_blake3_hasher_update(&state,b.data(),b.size());
  Digest out{};flitzfs_oracle_blake3_hasher_finalize(&state,reinterpret_cast<uint8_t*>(out.data()),out.size());return out;
}
Digest masked(View a,View b={}) {
  blake3_hasher state;flitzfs_masked_blake3_hasher_init(&state);
  flitzfs_masked_blake3_hasher_update(&state,a.data(),a.size());
  flitzfs_masked_blake3_hasher_update(&state,b.data(),b.size());
  Digest out{};flitzfs_masked_blake3_hasher_finalize(&state,reinterpret_cast<uint8_t*>(out.data()),out.size());return out;
}
Bytes input(std::size_t n) {
  Bytes bytes(n);for(std::size_t i=0;i<n;++i)bytes[i]=static_cast<std::byte>(i%251);return bytes;
}
std::string hex(const Digest& digest) {
  constexpr char chars[]="0123456789abcdef";std::string out;
  for(auto b:digest){const auto n=std::to_integer<unsigned>(b);out+=chars[n>>4];out+=chars[n&15];}return out;
}
void concurrent_first_use() {
  const auto bytes=input(65537);const auto expected=scalar(bytes);
  std::barrier start(2);std::atomic<bool> same{true};
  {
    std::array<std::jthread,2> threads;
    for(auto& thread:threads)thread=std::jthread([&] {
      start.arrive_and_wait();
      for(unsigned i=0;i<8;++i)if(record_digest(bytes)!=expected)same=false;
    });
  }
  require(same.load(),"concurrent production dispatcher first-use parity");
}
class Guarded {
  std::byte* base_=nullptr;std::size_t total_=0,page_=0,capacity_=0;
 public:
  explicit Guarded(std::size_t n) {
#ifdef _WIN32
    SYSTEM_INFO info{};GetSystemInfo(&info);page_=info.dwPageSize;
#else
    const auto value=sysconf(_SC_PAGESIZE);if(value<=0)throw std::runtime_error("page size");page_=static_cast<std::size_t>(value);
#endif
    capacity_=((std::max)(n,std::size_t{1})+page_-1)/page_*page_;total_=capacity_+2*page_;
#ifdef _WIN32
    base_=static_cast<std::byte*>(VirtualAlloc(nullptr,total_,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!base_)throw std::runtime_error("guard allocation");
    DWORD prior=0;
    if(!VirtualProtect(base_,page_,PAGE_NOACCESS,&prior) ||
        !VirtualProtect(base_+page_+capacity_,page_,PAGE_NOACCESS,&prior)) {
      VirtualFree(base_,0,MEM_RELEASE);base_=nullptr;throw std::runtime_error("guard protection");
    }
#else
    auto* allocation=mmap(nullptr,total_,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(allocation==MAP_FAILED)throw std::runtime_error("guard allocation");base_=static_cast<std::byte*>(allocation);
    if(mprotect(base_,page_,PROT_NONE) || mprotect(base_+page_+capacity_,page_,PROT_NONE)) {
      munmap(base_,total_);base_=nullptr;throw std::runtime_error("guard protection");
    }
#endif
  }
  ~Guarded(){
#ifdef _WIN32
    if(base_)VirtualFree(base_,0,MEM_RELEASE);
#else
    if(base_)munmap(base_,total_);
#endif
  }
  Guarded(const Guarded&)=delete;Guarded& operator=(const Guarded&)=delete;
  std::span<std::byte> range(std::size_t n,bool tail) {return {base_+page_+(tail?capacity_-n:0),n};}
};
void guarded() {
  for(const auto n:{0U,1U,31U,32U,63U,64U,65U,127U,128U,129U,1023U,1024U,1025U,4095U,4096U,4097U,65535U,65536U,65537U}) {
    auto bytes=input(n);const auto expected=scalar(bytes);
    Guarded storage(n),output(32);
    for(bool tail:{false,true}) {
      auto view=storage.range(n,tail);std::copy(bytes.begin(),bytes.end(),view.begin());
      require(record_digest(view)==expected && masked(view)==expected,"guard-boundary input digest mismatch");
      auto out=output.range(32,tail);
      blake3_hasher state;flitzfs_masked_blake3_hasher_init(&state);
      flitzfs_masked_blake3_hasher_update(&state,view.data(),view.size());
      flitzfs_masked_blake3_hasher_finalize(&state,reinterpret_cast<uint8_t*>(out.data()),out.size());
      require(std::equal(out.begin(),out.end(),expected.begin()),"guard-boundary output mismatch");
    }
  }
}
void vectors_and_ranges() {
  for(const auto& test:digest_vectors::cases) {
    auto bytes=input(test.bytes);
    require(hex(scalar(bytes))==test.hex,"published scalar known answer mismatch");
    require(hex(masked(bytes))==test.hex && hex(record_digest(bytes))==test.hex,"published accelerated known answer mismatch");
  }
  for(const auto n:{0U,1U,63U,64U,65U,255U,256U,257U,1023U,1024U,1025U,4095U,4096U,4097U,
      16383U,16384U,16385U,65535U,65536U,65537U,1048575U,1048576U,1048577U}) {
    const auto bytes=input(n);const auto expected=scalar(bytes);
    for(const auto offset:{0U,1U,3U,15U,63U}) {
      Bytes allocation(n+offset+64,std::byte{0xa7});std::copy(bytes.begin(),bytes.end(),allocation.begin()+offset);
      const auto view=View(allocation).subspan(offset,n);
      for(const auto split:{std::size_t{0},(std::min)(std::size_t{63},view.size()),view.size()/2,view.size()}) {
        require(masked(view.first(split),view.subspan(split))==expected &&
            record_digest(view.first(split),view.subspan(split))==expected,"unaligned/two-span digest mismatch");
      }
    }
  }
  auto a=input(8193),b=input(65537);auto expected_input=a;expected_input.insert(expected_input.end(),b.begin(),b.end());
  require(masked(a,b)==scalar(expected_input) && record_digest(a,b)==scalar(expected_input),"noncontiguous span concatenation mismatch");
  for(const auto chunk:{1U,63U,64U,65U,1023U,1024U,1025U,4097U}) {
    blake3_hasher state;flitzfs_masked_blake3_hasher_init(&state);
    for(std::size_t at=0;at<b.size();) {
      const auto size=(std::min)(std::size_t{chunk},b.size()-at);
      flitzfs_masked_blake3_hasher_update(&state,b.data()+at,size);at+=size;
    }
    Digest out{};flitzfs_masked_blake3_hasher_finalize(&state,reinterpret_cast<uint8_t*>(out.data()),out.size());
    require(out==scalar(b),"incremental chunk-boundary digest mismatch");
  }
}
void invariants() {
  concurrent_first_use();const auto actual=flitzfs_masked_detect();
  std::cout<<"Digest dispatch invariants: {\"schema\":1,\"backend\":\""<<FLITZFS_DIGEST_BACKEND
      <<"\",\"actual_features\":"<<actual<<",\"production_degree\":"<<blake3_simd_degree()<<",\"profiles\":[";
  bool comma=false;
  for(const auto mask:{0U,1U,7U,31U,127U}) {
    const auto selected=flitzfs_masked_limit(mask);
    require((selected&~actual)==0,"test override enabled an unsupported feature");
    vectors_and_ranges();guarded();
    if(comma)std::cout<<",";comma=true;
    std::cout<<"{\"requested\":"<<mask<<",\"selected\":"<<selected
        <<",\"degree\":"<<flitzfs_masked_blake3_simd_degree()<<"}";
  }
  std::cout<<"],\"checks\":"<<checks<<",\"complete\":true}\n";
}
// Hosted-only microbenchmark; neither filesystem throughput nor GPU evidence.
void stdout_probe(std::string_view mode) {
  hosted_digest_limits();
#ifdef _WIN32
  if(mode!="--stdio-default" &&
      _setmode(_fileno(stdout),mode=="--stdio-binary"?_O_BINARY:_O_TEXT)==-1)
    throw std::runtime_error("stdio probe mode");
  constexpr std::array<unsigned char,7> payload{{0,10,13,10,26,128,255}};
  std::cout.write(reinterpret_cast<const char*>(payload.data()),payload.size());
  if(!std::cout)throw std::runtime_error("stdio probe output");
#else
  (void)mode;throw std::runtime_error("stdio probe is hosted Windows only");
#endif
}
void benchmark() {
  hosted_digest_limits();const auto end=Clock::now()+std::chrono::seconds(15);
  std::cout<<"{\"schema\":1,\"backend\":\""<<FLITZFS_DIGEST_BACKEND<<"\",\"degree\":"
      <<blake3_simd_degree()<<",\"samples\":[";
  bool comma=false;std::uint64_t sink=0;
  for(const auto size:{4096U,65536U,1048576U}) {
    const auto bytes=input(size);const auto expected=scalar(bytes);const auto repetitions=8*1024*1024/size;
    for(unsigned sample=0;sample<5;++sample)for(unsigned order=0;order<2;++order) {
      const bool portable=(order^(sample&1U))==0;
      const auto begin=Clock::now();
      for(unsigned i=0;i<repetitions;++i) {
        const auto result=portable?scalar(bytes):record_digest(bytes);
        if(result!=expected)throw std::runtime_error("timed digest byte mismatch");
        sink+=std::to_integer<unsigned>(result[i%32]);std::atomic_signal_fence(std::memory_order_seq_cst);
      }
      const auto elapsed=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
      if(Clock::now()>end)throw std::runtime_error("digest benchmark deadline");
      if(comma)std::cout<<",";comma=true;
      std::cout<<"{\"bytes\":"<<size<<",\"sample\":"<<sample<<",\"mode\":\""
          <<(portable?"scalar-oracle":"production")<<"\",\"hashed_bytes\":"<<size*repetitions
          <<",\"ms\":"<<elapsed<<"}";
    }
  }
  std::cout<<"],\"sink\":"<<sink<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("digest test mode required");
  const std::string_view mode=argv[1];
  if(mode=="--invariants")invariants();else if(mode=="--benchmark")benchmark();
  else if(mode=="--stdio-default" || mode=="--stdio-text" || mode=="--stdio-binary")stdout_probe(mode);
  else throw std::runtime_error("unknown digest mode");
  return 0;
}catch(const std::exception& e){std::cerr<<"Digest dispatch failure: "<<e.what()<<"\n";return 1;}}
