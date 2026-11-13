#include "flitzfs/gpu_lz4_layout.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <utility>
namespace {
using namespace flitzfs;
unsigned checks=0;
void require(bool value,const char* why) {
  ++checks;if(!value)throw std::runtime_error(why);
}
template<class F> void rejects(F&& function) {
  ++checks;
  try{function();}catch(const std::invalid_argument&){return;}
  throw std::runtime_error("Unsafe LZ4 layout admitted");
}
void verify(const GpuLz4DeviceLayout& layout,std::span<const std::uint32_t> sizes) {
  using Region=std::pair<std::uint32_t,std::uint32_t>;
  std::array<Region,21> regions{};
  std::size_t count=0;std::uint32_t total_input=0;
  require(layout.count==sizes.size() && layout.count>0 && layout.count<=8,"Layout count");
  const auto table_bytes=layout.count*8U;
  for(const auto offset:{layout.input_pointers,layout.input_sizes,layout.output_pointers,layout.output_sizes})
    regions[count++]={offset,table_bytes};
  for(std::size_t i=0;i<sizes.size();++i) {
    const auto& chunk=layout.chunks[i];total_input+=sizes[i];
    require(chunk.input_bytes==sizes[i] && chunk.input_bytes>0 &&
        chunk.input_bytes<=65536,"Input range/identity");
    // A terminal literal-only LZ4 sequence must fit, including extended length
    // bytes. This is a capacity property, not codec interoperability evidence.
    const auto literal_bytes=sizes[i]+1U+(sizes[i]>=15U?(sizes[i]-15U)/255U+1U:0U);
    require(chunk.output_capacity>=literal_bytes && chunk.output_capacity%8U==0 &&
        chunk.output_capacity<=65800U,"Output capacity");
    regions[count++]={chunk.input_offset,chunk.input_bytes};
    regions[count++]={chunk.output_offset,chunk.output_capacity+32U};
  }
  regions[count++]={layout.scratch_offset,layout.scratch_bytes};
  require(count<=regions.size(),"Region inventory");
  std::sort(regions.begin(),regions.begin()+static_cast<std::ptrdiff_t>(count));
  std::uint64_t end=0;
  for(std::size_t i=0;i<count;++i) {
    const auto [offset,bytes]=regions[i];
    require(offset%8U==0 && offset>=end && offset-end<8U && bytes>0,
        "Region overlap, alignment or unexplained gap");
    end=static_cast<std::uint64_t>(offset)+bytes;
    require(end<=layout.total_bytes,"Region outside allocation");
  }
  require(end==layout.total_bytes && end<=262144U,"Total device allocation cap");
  require(total_input==layout.input_bytes && total_input<=262144U,"Aggregate input");
  require(layout.hash_table_entries>0 && layout.hash_table_entries<=16384U &&
      (layout.hash_table_entries&(layout.hash_table_entries-1U))==0 &&
      (layout.hash_table_entries>=*std::max_element(sizes.begin(),sizes.end()) ||
       layout.hash_table_entries==16384U),"Hash table shape");
  require(layout.scratch_bytes==layout.hash_table_entries*2U*layout.count,
      "Scratch partition size");
  for(std::size_t i=sizes.size();i<layout.chunks.size();++i) {
    const auto& unused=layout.chunks[i];
    require(!unused.input_offset && !unused.input_bytes && !unused.output_offset &&
        !unused.output_capacity,"Unused slot exposes offsets");
  }
}
}
int main(){try{
  using namespace flitzfs;
  // Every admitted single-block length, including all hash and token boundaries.
  for(std::uint32_t size=1;size<=65536;++size) {
    const std::array sizes{size};
    verify(gpu_lz4_device_layout(sizes),sizes);
  }
  const std::array small{1U};const auto first=gpu_lz4_device_layout(small);
  require(first.total_bytes==82 && first.chunks[0].output_capacity==8 &&
      first.hash_table_entries==1 && first.scratch_bytes==2,"Small upstream-bound checkpoint");
  const std::array maximum{65536U};const auto last=gpu_lz4_device_layout(maximum);
  require(last.total_bytes==164168 && last.chunks[0].output_capacity==65800 &&
      last.hash_table_entries==16384 && last.scratch_bytes==32768,"Large upstream-bound checkpoint");
  const std::array<std::uint32_t,24> boundaries{1,2,3,4,7,8,9,14,15,16,17,254,
      255,256,257,4095,4096,4097,8191,8192,16383,16384,32768,65536};
  unsigned admitted=0,capacity_refusals=0;
  for(std::size_t count=2;count<=8;++count)for(const auto size:boundaries) {
    std::array<std::uint32_t,8> values{};std::fill_n(values.begin(),count,size);
    const auto sizes=std::span<const std::uint32_t>(values).first(count);
    try {const auto layout=gpu_lz4_device_layout(sizes);verify(layout,sizes);++admitted;}
    catch(const std::invalid_argument&) {
      ++capacity_refusals;
      // Refusals must have a demonstrated aggregate or minimum device cost,
      // independently ignoring all padding and output guards.
      const auto input=static_cast<std::uint64_t>(size)*count;
      const auto hash_min=static_cast<std::uint64_t>(std::min(size,16384U))*2U*count;
      require(input>262144 || 2U*input+hash_min+32U*count>262144,
          "Capacity refusal without necessary lower-bound exhaustion");
    }
  }
  require(admitted>0 && capacity_refusals>0,"Missing batch admission/refusal cases");
  // Changing order must retain exact chunk identity and independent partitions.
  std::array<std::uint32_t,4> mixed{1,17,254,4096};
  do {verify(gpu_lz4_device_layout(mixed),mixed);}
  while(std::next_permutation(mixed.begin(),mixed.end()));
  rejects([]{(void)gpu_lz4_device_layout({});});
  const std::array<std::uint32_t,9> too_many{1,1,1,1,1,1,1,1,1};
  rejects([&]{(void)gpu_lz4_device_layout(too_many);});
  for(const auto invalid:{0U,65537U,(std::numeric_limits<std::uint32_t>::max)()}) {
    const std::array values{invalid};rejects([&]{(void)gpu_lz4_device_layout(values);});
  }
  std::array<std::uint32_t,8> zeros{1,1,1,1,1,1,1,1};
  for(auto& value:zeros){value=0;rejects([&]{(void)gpu_lz4_device_layout(zeros);});value=1;}
  const std::array exhausted{65536U,65536U};
  rejects([&]{(void)gpu_lz4_device_layout(exhausted);});
  for(const auto invalid:{262145U,(std::numeric_limits<std::uint32_t>::max)()}) {
    auto cursor=invalid;rejects([&]{(void)gpu_lz4_detail::append(cursor,1);});
    require(cursor==invalid,"Invalid cursor changed after refusal");
  }
  auto cursor=262144U;
  rejects([&]{(void)gpu_lz4_detail::append(cursor,1);});
  require(cursor==262144U,"Exhausted cursor changed after refusal");
  std::cout<<"GPU LZ4 layout checks: "<<checks<<" single_sizes_checked=65536 batches_admitted="
      <<admitted<<" capacity_refusals="<<capacity_refusals
      <<" device_ceiling=262144 gpu_executed=false\n";
  return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
