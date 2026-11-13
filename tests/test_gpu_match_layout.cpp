#include "flitzfs/gpu_match_layout.hpp"
#include <array>
#include <iostream>
#include <limits>
using namespace flitzfs;
namespace {
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid match slab accepted");}
std::uint16_t get(std::span<const std::byte> bytes,std::size_t at) {
  return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at])|
      (std::to_integer<unsigned>(bytes[at+1])<<8));
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint16_t value) {
  bytes[at]=static_cast<std::byte>(value);bytes[at+1]=static_cast<std::byte>(value>>8);
}
}
int main(){try{
  // Exhaustive admitted sizes, with bounds/alignment rather than a second
  // implementation of the offset formula.
  for(std::uint32_t size=0;size<=match_detail::input_limit;++size) {
    const auto l=gpu_match_device_layout(size);
    if(l.previous_offset<size || l.previous_offset%alignof(match_detail::u16) ||
        l.output_offset%8 || l.output_offset<l.previous_offset+2ULL*size ||
        l.total_bytes!=l.output_offset+match_detail::lanes(size)*sizeof(match_detail::Match) ||
        l.total_bytes>229376)throw std::runtime_error("Slab overlap, alignment or device cap");
  }
  ++checks;
  rejects([]{(void)gpu_match_device_layout(match_detail::input_limit+1);});
  rejects([]{(void)gpu_match_device_layout((std::numeric_limits<std::uint32_t>::max)());});
  require(gpu_match_device_layout(65536).total_bytes==229376,"Maximum device allocation changed");
  require(gpu_match_device_layout(4096).total_bytes==14336,"Qualified small device allocation changed");
  for(const std::uint32_t size:{0U,1U,3U,4U,5U,15U,16U,17U,63U,64U,65U,513U,4095U,4096U,65535U,65536U}) {
    std::vector<std::byte> input(size);
    for(std::size_t i=0;i<input.size();++i)input[i]=static_cast<std::byte>((i%37)*13U);
    const auto source=input;
    const auto plan=PreparedGpuMatchSearch::prepare(input);
    const auto original_index=std::vector<match_detail::u16>(plan.predecessors().begin(),plan.predecessors().end());
    auto slab=gpu_match_device_upload(plan);const auto before=slab;
    const auto l=gpu_match_device_layout(size);
    require(slab.size()==l.total_bytes && plan.device_buffer_bytes()==slab.size(),"Slab/budget differs");
    require(std::equal(source.begin(),source.end(),slab.begin()),"Input bytes changed in upload");
    std::vector<match_detail::u16> unpacked(size);
    for(std::size_t i=0;i<size;++i)unpacked[i]=get(slab,l.previous_offset+2*i);
    require(unpacked==original_index,"Packed predecessor bytes differ");
    for(std::size_t i=size;i<l.previous_offset;++i)require(slab[i]==std::byte{},"Input padding not initialized");
    for(std::size_t i=l.previous_offset+2ULL*size;i<l.output_offset;++i)require(slab[i]==std::byte{},"Output alignment padding not initialized");
    if(l.output_offset<slab.size())require(std::all_of(slab.begin()+l.output_offset,slab.end(),
        [](auto b){return b==std::byte{0xff};}),"Result sentinel region not initialized");
    // CPU execution of the unchanged shared search body using independently
    // unpacked slab inputs must agree with the original separate inputs.
    for(unsigned lane=0;lane<plan.lanes();++lane) {
      const auto expected=match_detail::search(reinterpret_cast<const unsigned char*>(source.data()),
          size,original_index.data(),size,lane);
      const auto result=match_detail::search(reinterpret_cast<const unsigned char*>(slab.data()),
          size,unpacked.data(),size,lane);
      require(result==expected,"Slab changed shared-kernel CPU semantics");
      const auto at=l.output_offset+lane*sizeof(match_detail::Match);
      put(slab,at,result.distance);put(slab,at+2,result.length);
      put(slab,at+4,result.candidates);put(slab,at+6,result.compared);
    }
    require(std::equal(before.begin(),before.begin()+l.output_offset,slab.begin()),
        "Result writes touched input/index/padding");
    std::vector<match_detail::Match> results(plan.lanes());
    for(unsigned lane=0;lane<plan.lanes();++lane) {
      const auto at=l.output_offset+lane*sizeof(match_detail::Match);
      results[lane]={get(slab,at),get(slab,at+2),get(slab,at+4),get(slab,at+6)};
    }
    (void)plan.verified_proposals(results);
    require(input==source && std::ranges::equal(plan.predecessors(),original_index),"Upload modified its source");
    rejects([&]{(void)gpu_match_device_upload(PreparedGpuMatchSearch::prepare_request(source));});
  }
  std::cout<<"GPU match slab checks: "<<checks<<" sizes_checked=65537 max_device_bytes=229376 gpu_executed=false\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
