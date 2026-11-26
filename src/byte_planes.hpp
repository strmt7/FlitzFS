#pragma once
#include <algorithm>
#include <cstddef>
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
}
