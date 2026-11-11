#include "flitzfs/gpu_match.hpp"
#include <algorithm>
#include <stdexcept>
namespace flitzfs {
namespace {
[[noreturn]] void fail(const char* text){throw std::invalid_argument(text);}
}
PreparedGpuMatchSearch PreparedGpuMatchSearch::prepare(std::span<const std::byte> input) {
  if(input.size()>match_detail::input_limit)fail("GPU match input exceeds 64 KiB");
  PreparedGpuMatchSearch plan;
  plan.input_.assign(input.begin(),input.end());
  plan.previous_.assign(input.size(),match_detail::no_previous);
  // Exact 4-byte prefix and position keys reproduce the reference's ordered
  // predecessor index. Temporary keys occupy at most 512 KiB on the CPU.
  std::vector<std::uint64_t> keys;
  if(input.size()<4)return plan;
  keys.reserve(input.size()-3);
  for(std::uint32_t position=0;position+4<=input.size();++position) {
    std::uint32_t word=0;
    for(unsigned byte=0;byte<4;++byte)
      word|=std::uint32_t(std::to_integer<unsigned char>(input[position+byte]))<<(byte*8);
    keys.push_back((std::uint64_t(word)<<16)|position);
  }
  std::sort(keys.begin(),keys.end());
  for(std::size_t i=1;i<keys.size();++i)if((keys[i]>>16)==(keys[i-1]>>16)) {
    const auto position=static_cast<std::uint16_t>(keys[i]&65535U);
    plan.previous_[position]=static_cast<std::uint16_t>(keys[i-1]&65535U);
  }
  return plan;
}
std::uint32_t PreparedGpuMatchSearch::lanes() const noexcept {
  return match_detail::lanes(static_cast<std::uint32_t>(input_.size()));
}
std::uint64_t PreparedGpuMatchSearch::device_buffer_bytes() const noexcept {
  return input_.size()+previous_.size()*sizeof(match_detail::u16)+lanes()*sizeof(match_detail::Match);
}
std::vector<StoreMatchProposal> PreparedGpuMatchSearch::verified_proposals(
    std::span<const match_detail::Match> matches) const {
  if(matches.size()!=lanes())fail("GPU match lane count mismatch");
  std::vector<StoreMatchProposal> proposals;
  proposals.reserve(matches.size());
  std::uint32_t end=0;
  for(std::uint32_t lane=0;lane<matches.size();++lane) {
    const auto& match=matches[lane];
    const auto position=lane*match_detail::anchor_stride;
    if(match.candidates>match_detail::candidate_limit || match.compared>match_detail::comparison_limit)
      fail("GPU match effort exceeds its cap");
    if(!match.length) {
      if(match.distance)fail("empty GPU match has a distance");
      continue;
    }
    if(!match.distance || match.distance>position || match.length<4 ||
        match.length>match_detail::match_limit || match.length>input_.size()-position ||
        !match.candidates || match.compared<match.length ||
        !std::equal(input_.begin()+position,input_.begin()+position+match.length,
                    input_.begin()+position-match.distance))
      fail("GPU match differs from its owned input");
    if(position>=end) {
      proposals.push_back({position,match.distance,match.length});
      end=position+match.length;
    }
  }
  return proposals;
}
}
