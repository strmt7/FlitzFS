#include "flitzfs/gpu_lz4_slab.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f) {
  ++checks;try{f();}catch(const std::invalid_argument&){return;}
  throw std::runtime_error("Malformed LZ4 slab admitted");
}
template<class F>void unavailable(F&& f) {
  ++checks;try{f();}catch(const std::runtime_error& e) {
    if(std::string_view(e.what()).find("verification unavailable")!=std::string_view::npos)return;
    throw;
  }
  throw std::runtime_error("Unavailable CPU decoder admitted slab");
}
void put64(Bytes& out,std::size_t at,std::uint64_t value) {
  for(unsigned i=0;i<8;++i)out[at+i]=static_cast<std::byte>((value>>(8U*i))&255U);
}
std::uint64_t get64(Lz4WorkerBytes bytes,std::size_t at) {
  std::uint64_t value=0;
  for(unsigned i=0;i<8;++i)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(8U*i);
  return value;
}
Bytes literal(Lz4WorkerBytes source) {
  Bytes result{static_cast<std::byte>((std::min)(source.size(),std::size_t{15})<<4)};
  if(source.size()>=15) {
    auto left=source.size()-15;
    while(left>=255){result.push_back(std::byte{255});left-=255;}
    result.push_back(static_cast<std::byte>(left));
  }
  result.insert(result.end(),source.begin(),source.end());return result;
}
std::vector<Lz4WorkerBytes> views(const std::vector<Bytes>& chunks) {
  std::vector<Lz4WorkerBytes> result;
  for(const auto& chunk:chunks)result.emplace_back(chunk);
  return result;
}
Bytes completion(const PreparedLz4WorkerBatch& batch,std::uint64_t address,
    const std::vector<Bytes>& payloads) {
  auto bytes=gpu_lz4_slab_upload(batch,address);
  const auto& layout=batch.device_layout();
  require(payloads.size()==layout.count,"Fixture chunk count");
  for(std::uint32_t i=0;i<layout.count;++i) {
    const auto& c=layout.chunks[i];
    require(payloads[i].size()<=c.output_capacity,"Fixture payload cap");
    std::copy(payloads[i].begin(),payloads[i].end(),bytes.begin()+c.output_offset);
    put64(bytes,layout.output_sizes+8U*i,payloads[i].size());
  }
  // Scratch and unused output are unconstrained device working bytes.
  std::fill(bytes.begin()+layout.scratch_offset,bytes.end(),std::byte{0x39});
  return bytes;
}
void valid(std::vector<Bytes> chunks,bool mutate) {
  constexpr std::uint64_t address=0x0102030405060708ULL;
  const auto batch=PreparedLz4WorkerBatch::prepare(views(chunks));
  const auto& p=batch.device_layout();
  const auto upload=gpu_lz4_slab_upload(batch,address);
  require(upload.size()==p.total_bytes && upload.size()<=262144,"Upload complete cap");
  std::vector<Bytes> payloads;
  for(std::uint32_t i=0;i<p.count;++i) {
    const auto& c=p.chunks[i];
    require(get64(upload,p.input_pointers+8U*i)==address+c.input_offset,"Input device address");
    require(get64(upload,p.input_sizes+8U*i)==c.input_bytes,"Input size table");
    require(get64(upload,p.output_pointers+8U*i)==address+c.output_offset,"Output device address");
    require(get64(upload,p.output_sizes+8U*i)==0,"Output size must begin zero");
    require(std::ranges::equal(std::span(upload).subspan(c.input_offset,c.input_bytes),chunks[i]),
        "Upload input bytes");
    require(std::all_of(upload.begin()+c.output_offset,upload.begin()+c.output_offset+c.output_capacity,
        [](auto x){return x==std::byte{0};}),"Output initialization");
    require(std::all_of(upload.begin()+c.output_offset+c.output_capacity,
        upload.begin()+c.output_offset+c.output_capacity+32,[](auto x){return x==std::byte{0xa5};}),
        "Output guard initialization");
    payloads.push_back(literal(chunks[i]));
  }
  require(std::all_of(upload.begin()+p.scratch_offset,upload.end(),
      [](auto x){return x==std::byte{0};}),"Scratch initialization");
  auto slab=completion(batch,address,payloads);
  const auto accept=[&](const Bytes& bytes) {
    if(gpu_lz4_cpu_verification_available())
      require(verify_gpu_lz4_slab(batch,address,bytes)==payloads,"Verified owned candidates");
    else unavailable([&]{(void)verify_gpu_lz4_slab(batch,address,bytes);});
  };
  accept(slab);
  if(gpu_lz4_cpu_verification_available()) {
    auto changed=slab;
    const auto owned=verify_gpu_lz4_slab(batch,address,changed);
    std::fill(changed.begin(),changed.end(),std::byte{0});
    require(owned==payloads,"Returned candidates borrowed readback");
    require(std::ranges::equal(batch.input(0),chunks[0]),"Verification mutated input");
  }
  rejects([&]{(void)verify_gpu_lz4_slab(batch,address,upload);}); // No completed sizes.
  if(!mutate)return;
  // Independently fixed bytes for the first two input addresses: two chunks,
  // four 16-byte tables, then 17-byte input, output/guard and alignment.
  const std::array<std::byte,16> golden{
      std::byte{0x48},std::byte{7},std::byte{6},std::byte{5},std::byte{4},std::byte{3},std::byte{2},std::byte{1},
      std::byte{0x98},std::byte{7},std::byte{6},std::byte{5},std::byte{4},std::byte{3},std::byte{2},std::byte{1}};
  require(p.input_pointers==0 && std::ranges::equal(std::span(upload).first(16),golden),
      "AMD64 little-endian pointer golden");
  Bytes mutable_bytes(p.total_bytes,std::byte{0});
  const auto mark=[&](std::uint32_t at,std::uint32_t size) {
    std::fill_n(mutable_bytes.begin()+at,size,std::byte{1});
  };
  mark(p.output_sizes,p.count*8U);mark(p.scratch_offset,p.scratch_bytes);
  for(std::uint32_t i=0;i<p.count;++i)mark(p.chunks[i].output_offset,p.chunks[i].output_capacity);
  for(std::size_t at=0;at<slab.size();++at) {
    if(mutable_bytes[at]!=std::byte{0})continue;
    auto bad=slab;bad[at]^=std::byte{1};
    rejects([&]{(void)verify_gpu_lz4_slab(batch,address,bad);});
  }
  for(std::size_t n=0;n<slab.size();++n)
    rejects([&]{(void)verify_gpu_lz4_slab(batch,address,std::span(slab).first(n));});
  auto bad=slab;bad.push_back(std::byte{0});
  rejects([&]{(void)verify_gpu_lz4_slab(batch,address,bad);});
  rejects([&]{(void)verify_gpu_lz4_slab(batch,address+8,slab);});
  for(std::uint32_t i=0;i<p.count;++i) {
    for(const auto size:{std::uint64_t{0},std::uint64_t{p.chunks[i].output_capacity}+1,
        (std::numeric_limits<std::uint64_t>::max)()}) {
      bad=slab;put64(bad,p.output_sizes+8U*i,size);
      rejects([&]{(void)verify_gpu_lz4_slab(batch,address,bad);});
    }
    bad=slab;bad[p.chunks[i].output_offset+payloads[i].size()-1]^=std::byte{1};
    if(gpu_lz4_cpu_verification_available())
      rejects([&]{(void)verify_gpu_lz4_slab(batch,address,bad);});
    else unavailable([&]{(void)verify_gpu_lz4_slab(batch,address,bad);});
    // Unused capacity belongs to the kernel, not to the independent payload.
    if(payloads[i].size()<p.chunks[i].output_capacity) {
      bad=slab;bad[p.chunks[i].output_offset+payloads[i].size()]^=std::byte{1};accept(bad);
    }
  }
  const auto maximum=(std::numeric_limits<std::uint64_t>::max)();
  for(const auto base:{std::uint64_t{0},std::uint64_t{1},std::uint64_t{7},maximum&~std::uint64_t{7}})
    rejects([&]{(void)gpu_lz4_slab_upload(batch,base);});
  const auto largest_base=(maximum-p.total_bytes)&~std::uint64_t{7};
  require(gpu_lz4_slab_upload(batch,largest_base).size()==p.total_bytes,"Nonwrapping boundary refused");
  rejects([&]{(void)gpu_lz4_slab_upload(batch,largest_base+8);});
  auto moved=batch;const auto retained=std::move(moved);
  rejects([&]{(void)gpu_lz4_slab_upload(moved,address);});
  rejects([&]{(void)verify_gpu_lz4_slab(moved,address,slab);});
  require(retained.ready(),"Move lost source ownership");
  auto other_chunks=chunks;other_chunks[1][0]^=std::byte{1};
  const auto other=PreparedLz4WorkerBatch::prepare(views(other_chunks));
  rejects([&]{(void)verify_gpu_lz4_slab(other,address,slab);});
}
}
int main(){try {
  std::vector<Bytes> chunks{Bytes(17,std::byte{0x51}),Bytes(33,std::byte{0x62})};
  valid(chunks,true);
  for(const auto n:{1U,14U,15U,16U,254U,255U,256U,4096U,65536U})
    valid({Bytes(n,std::byte{0x73})},false);
  valid(std::vector<Bytes>(8,Bytes(1024,std::byte{0x34})),false);
  valid({Bytes(32768,std::byte{0x45}),Bytes(32768,std::byte{0x56})},false);
#ifdef FLITZFS_TEST_HAS_LZ4
  Bytes source(65536);
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::byte>((i/127)%251);
  const std::array<Lz4WorkerBytes,1> inputs{source};
  const auto batch=PreparedLz4WorkerBatch::prepare(inputs);
  Bytes compressed(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(source.size()))));
  const auto size=LZ4_compress_default(reinterpret_cast<const char*>(source.data()),
      reinterpret_cast<char*>(compressed.data()),static_cast<int>(source.size()),static_cast<int>(compressed.size()));
  require(size>0,"Pinned CPU compressor failed");compressed.resize(static_cast<std::size_t>(size));
  const auto slab=completion(batch,4096,{compressed});
  const auto verified=verify_gpu_lz4_slab(batch,4096,slab);
  require(verified.front()==compressed,"Actual CPU compressed slab");
  const auto reply=encode_lz4_worker_reply(batch,991,views(verified),
      WorkerExecution::cpu_semantics);
  require(accept_lz4_worker_reply(reply,batch,991,WorkerExecution::cpu_semantics).front()==compressed,
      "Slab candidate/frame composition");
#endif
  std::cout<<"LZ4 slab admission: "<<checks<<" checks; decoder_available="
      <<gpu_lz4_cpu_verification_available()<<" gpu_executed=false\n";return 0;
}catch(const std::exception& e){std::cerr<<"LZ4 slab admission failed: "<<e.what()<<'\n';return 1;}}
