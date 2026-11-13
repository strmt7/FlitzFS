#pragma once
#include "flitzfs/gpu_match.hpp"
#include <algorithm>
#include <stdexcept>

namespace flitzfs {
// Host-side offsets only. The unchanged kernel still receives three independent
// bounded pointers. This is not an IPC/native struct or a stored encoding.
struct GpuMatchDeviceLayout {
  std::uint32_t previous_offset=0,output_offset=0,total_bytes=0;
};
inline GpuMatchDeviceLayout gpu_match_device_layout(std::uint32_t input_bytes) {
  if(input_bytes>match_detail::input_limit)throw std::invalid_argument("GPU match layout input cap");
  static_assert(alignof(match_detail::u16)==2 && sizeof(match_detail::Match)==8);
  const auto previous=(input_bytes+1U)&~1U;
  const auto output=(previous+2U*input_bytes+7U)&~7U;
  const auto total=output+match_detail::lanes(input_bytes)*sizeof(match_detail::Match);
  if(total>229376U)throw std::invalid_argument("GPU match layout exceeds existing device ceiling");
  return {previous,output,static_cast<std::uint32_t>(total)};
}
inline std::vector<std::byte> gpu_match_device_upload(const PreparedGpuMatchSearch& search) {
  // Reject an input-only parent plan before allocating any upload scratch.
  const auto previous=search.predecessors();
  const auto input=search.input();
  if(previous.size()!=input.size() || input.size()>match_detail::input_limit)
    throw std::invalid_argument("GPU match upload index shape");
  const auto layout=gpu_match_device_layout(static_cast<std::uint32_t>(input.size()));
  std::vector<std::byte> upload(layout.total_bytes);
  if(upload.empty())return upload;
  std::copy(input.begin(),input.end(),upload.begin());
  for(std::size_t i=0;i<previous.size();++i) {
    upload[layout.previous_offset+2*i]=static_cast<std::byte>(previous[i]);
    upload[layout.previous_offset+2*i+1]=static_cast<std::byte>(previous[i]>>8);
  }
  // All four uint16 fields in every unwritten result are invalid sentinels.
  std::fill(upload.begin()+layout.output_offset,upload.end(),std::byte{0xff});
  return upload;
}
} // namespace flitzfs
