#pragma once
#include "../src/native_group_lz4.hpp"
#include <algorithm>

namespace anchor_search_fixture {
using Bytes=std::vector<std::byte>;
using namespace flitzfs;

// Rotate distinct fifteen-byte regions and alter their separating byte.
// This isolates shifted matches shorter than the CPU anchor's sixteen-byte
// search window. The source can be synthetic or a read-only public sample.
inline Bytes fragmented(std::span<const std::byte> base,unsigned member) {
  constexpr unsigned region=16,count=kNativeExtentBytes/region;
  Bytes result(kNativeExtentBytes);
  for(unsigned i=0;i<count;++i) {
    const auto source=((i*1543+member*71)%count)*region;
    std::copy_n(base.begin()+source,region-1,result.begin()+i*region);
    result[i*region+region-1]=base[source+region-1]^std::byte{0x7b};
  }
  return result;
}
inline std::vector<NativeSharedBaseCopyRange> fragment_ranges(unsigned member) {
  constexpr unsigned region=16,count=kNativeExtentBytes/region;
  std::vector<NativeSharedBaseCopyRange> ranges;
  ranges.reserve(count);
  for(unsigned i=0;i<count;++i)
    ranges.push_back({i*region,((i*1543+member*71)%count)*region,region-1});
  return ranges;
}
}
