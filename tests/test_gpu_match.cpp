#include "flitzfs/gpu_match.hpp"
#include "flitzfs/gpu_match_kernel_api.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
using namespace flitzfs;
using namespace flitzfs::match_detail;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("invalid match result accepted");}
std::vector<Match> cpu(const PreparedGpuMatchSearch& plan) {
  std::vector<Match> result(plan.lanes());
  for(u32 lane=0;lane<plan.lanes();++lane)
    result[lane]=search(reinterpret_cast<const u8*>(plan.input().data()),static_cast<u32>(plan.input().size()),
        plan.predecessors().data(),static_cast<u32>(plan.predecessors().size()),lane);
  return result;
}
void independent_index(const PreparedGpuMatchSearch& plan) {
  const auto input=plan.input();
  for(std::size_t position=0;position+4<=input.size();++position) {
    u16 expected=no_previous;
    for(std::size_t candidate=0;candidate<position;++candidate)
      if(std::equal(input.begin()+candidate,input.begin()+candidate+4,input.begin()+position))
        expected=static_cast<u16>(candidate);
    require(plan.predecessors()[position]==expected,"sorted index differs from independent history scan");
  }
}
void small_oracle(const PreparedGpuMatchSearch& plan,const std::vector<Match>& found) {
  const auto input=plan.input();
  for(u32 lane=0;lane<plan.lanes();++lane) {
    const auto position=lane*anchor_stride;
    std::size_t longest=0;
    for(std::size_t candidate=0;candidate<position;++candidate) {
      std::size_t length=0;
      while(length<input.size()-position&&input[candidate+length]==input[position+length])++length;
      if(length>=4)longest=(std::max)(longest,length);
    }
    require(found[lane].length==longest,"bounded small search differs from exhaustive match oracle");
  }
}
void fake(const u8*,u32,const u16*,u32,Match*,u32,u32) {}
}
int main(){try {
  for(const std::size_t size:{0U,1U,3U,4U,16U,17U,63U,64U,513U,65536U}) {
    Bytes input(size);
    for(std::size_t i=0;i<size;++i)input[i]=static_cast<std::byte>((i%31)*7);
    const auto original=input;
    auto plan=PreparedGpuMatchSearch::prepare(input);
    std::fill(input.begin(),input.end(),std::byte{0xff});
    require(std::ranges::equal(plan.input(),original),"prepared match input is borrowed");
    require(plan.device_buffer_bytes()<=gpu_detail::max_batch_bytes,"match device buffers exceed the existing cap");
    const auto result=cpu(plan);
    const auto proposals=plan.verified_proposals(result);
    std::uint32_t end=0;
    for(const auto& match:proposals) {
      require(match.position>=end&&match.length>=4&&match.distance<=match.position,
          "proposal ranges overlap or lack backward history");
      end=match.position+match.length;
    }
    if(size<=513)independent_index(plan);
    if(size<=64)small_oracle(plan,result);
    if(size==65536)require(plan.device_buffer_bytes()==229376&&!proposals.empty(),"full bounded search shape");
    if(size>3) {
      auto wrong=result;wrong.push_back({});
      rejects([&]{plan.verified_proposals(wrong);});
      wrong=result;wrong[0].distance=1;wrong[0].length=4;
      rejects([&]{plan.verified_proposals(wrong);});
      wrong=result;wrong.back().compared=comparison_limit+1;
      rejects([&]{plan.verified_proposals(wrong);});
    }
  }
  rejects([&]{PreparedGpuMatchSearch::prepare(Bytes(input_limit+1));});
  // Malformed index metadata remains bounded even when it has cycles or
  // plausible positions whose first bytes do not actually match.
  const std::array<u8,64> raw{1,2,3,4};
  std::array<u16,64> previous{};
  for(u32 i=0;i<64;++i)previous[i]=static_cast<u16>(i);
  require(search(raw.data(),64,previous.data(),64,1).length==0,"forward index accepted");
  previous[16]=8;previous[8]=8;
  auto bounded=search(raw.data(),64,previous.data(),64,1);
  require(bounded.candidates<=candidate_limit&&bounded.compared<=comparison_limit,"cyclic search exceeded work caps");
  require(search(nullptr,64,previous.data(),64,1)==Match{}&&
      search(raw.data(),64,nullptr,64,1)==Match{}&&
      search(raw.data(),65537,previous.data(),64,1)==Match{}&&
      search(raw.data(),64,previous.data(),63,1)==Match{}&&
      search(raw.data(),64,previous.data(),64,4096)==Match{},"malformed geometry reached source");
  // Validate an overlapping lane even if greedy parsing would skip it.
  const auto plan=PreparedGpuMatchSearch::prepare(Bytes(128,std::byte{81}));
  auto output=cpu(plan);
  output[2].distance=0;
  rejects([&]{plan.verified_proposals(output);});
  auto api=make_kernel_api(fake);
  require(compatible_kernel_api(api),"typed match entry rejected");
  api.version++;require(!compatible_kernel_api(api),"foreign match entry accepted");
  api=make_kernel_api(nullptr);require(!compatible_kernel_api(api),"missing match entry accepted");
  std::cout<<"GPU match CPU semantics: checks="<<checks<<" device_bytes=229376 gpu_executed=false\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"GPU match semantics failed: "<<error.what()<<'\n';return 1;}}
