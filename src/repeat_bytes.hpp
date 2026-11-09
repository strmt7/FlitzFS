#pragma once
#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
namespace flitzfs::detail {
// Input and output are disjoint. Copy only an initialized prefix; every
// doubling step copies <=filled bytes, so its ranges never overlap.
inline void repeat_bytes(std::span<const std::byte> motif,std::span<std::byte> output) {
  if(motif.empty())throw std::runtime_error("FlitzFS: empty repeat motif");
  if(motif.size()==1){std::fill(output.begin(),output.end(),motif[0]);return;}
  auto filled=(std::min)(motif.size(),output.size());
  std::copy_n(motif.begin(),filled,output.begin());
  while(filled<output.size()) {
    const auto count=(std::min)(filled,output.size()-filled);
    std::copy_n(output.begin(),count,output.begin()+static_cast<std::ptrdiff_t>(filled));
    filled+=count;
  }
}
}
