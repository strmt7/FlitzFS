#pragma once
#include "../src/native_group_lz4.hpp"
#include "native_fs_fixture.hpp"
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

inline constexpr std::size_t indexed_batch_families=5,indexed_batch_extents_per_file=7;
inline constexpr auto indexed_batch_members=indexed_batch_families*(kNativeMaxDirtyExtents-1);
static_assert(kNativeMaxDirtyExtents%indexed_batch_extents_per_file==0 && indexed_batch_members>kNativeMaxExtents);

// Each scan window sees one disjoint family spread across several files.
inline std::vector<Bytes> indexed_batch_files() {
  std::vector<Bytes> files;
  for(std::size_t family=0;family<indexed_batch_families;++family) {
    const auto base=native_fixture::data(kNativeExtentBytes,static_cast<unsigned>(714+family*5731));
    for(std::size_t first=0;first<kNativeMaxDirtyExtents;first+=indexed_batch_extents_per_file) {
      Bytes file(indexed_batch_extents_per_file*kNativeExtentBytes);
      for(std::size_t block=0;block<indexed_batch_extents_per_file;++block) {
        const auto member=first+block;
        auto extent=std::span(file).subspan(block*kNativeExtentBytes,kNativeExtentBytes);
        std::ranges::copy(base,extent.begin());
        if(member) {
          const auto delta=native_fixture::data(kNativeExtentBytes,
              static_cast<unsigned>(1511+family*kNativeMaxDirtyExtents+member));
          for(std::size_t at=0;at<extent.size();++at)extent[at]^=delta[at]&std::byte{0x30};
        }
      }
      files.push_back(std::move(file));
    }
  }
  return files;
}
}
