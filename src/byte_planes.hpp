#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace flitzfs::detail {
// The caller supplies equal, disjoint spans and a checked positive width.
// Keep an incomplete final word in its original order.
inline void transpose_byte_planes(std::span<const std::byte> input,std::span<std::byte> output,
    std::size_t width) noexcept {
  const auto rows=input.size()/width;
  for(std::size_t plane=0;plane<width;++plane)
    for(std::size_t row=0;row<rows;++row)
      output[plane*rows+row]=input[row*width+plane];
  std::ranges::copy(input.subspan(rows*width),output.subspan(rows*width).begin());
}
inline void restore_byte_planes(std::span<const std::byte> input,std::span<std::byte> output,
    std::size_t width) noexcept {
  const auto rows=input.size()/width;
  for(std::size_t row=0;row<rows;++row)
    for(std::size_t plane=0;plane<width;++plane)
      output[row*width+plane]=input[plane*rows+row];
  std::ranges::copy(input.subspan(rows*width),output.subspan(rows*width).begin());
}

// Each plane starts with a zero predictor. The incomplete word is outside
// all planes and is never part of the prediction.
inline void encode_plane_deltas(std::span<std::byte> planes,std::size_t width) noexcept {
  const auto rows=planes.size()/width;
  for(std::size_t plane=0;plane<width;++plane) {
    std::uint8_t previous=0;
    for(auto& byte:planes.subspan(plane*rows,rows)) {
      const auto current=std::to_integer<std::uint8_t>(byte);
      byte=static_cast<std::byte>(static_cast<std::uint8_t>(current-previous));
      previous=current;
    }
  }
}

inline void restore_plane_deltas(std::span<std::byte> planes,std::size_t width) noexcept {
  const auto rows=planes.size()/width;
  for(std::size_t plane=0;plane<width;++plane) {
    std::uint8_t previous=0;
    for(auto& byte:planes.subspan(plane*rows,rows)) {
      previous=static_cast<std::uint8_t>(previous+std::to_integer<std::uint8_t>(byte));
      byte=static_cast<std::byte>(previous);
    }
  }
}
}
