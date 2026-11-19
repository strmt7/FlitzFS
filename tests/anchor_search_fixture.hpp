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
inline constexpr auto indexed_bias_extents_per_family=3*indexed_batch_extents_per_file;
inline constexpr auto indexed_batch_members=indexed_batch_families*(kNativeMaxDirtyExtents-1);
inline constexpr auto indexed_sample_bank_bytes=(indexed_batch_families+1)*kNativeExtentBytes;
static_assert(kNativeMaxDirtyExtents%indexed_batch_extents_per_file==0 && indexed_batch_members>kNativeMaxExtents);

// Each scan window sees one disjoint family spread across several files.
// The optional holdout bank also contributes unmodified unrelated extents.
inline std::vector<Bytes> indexed_batch_files(std::span<const std::byte> bank={},bool modular=false,
    bool shifted_bias=false) {
  if(!bank.empty() && bank.size()!=indexed_sample_bank_bytes)throw std::invalid_argument("Indexed sample bank size");
  const auto extent_count=shifted_bias?indexed_bias_extents_per_family:kNativeMaxDirtyExtents;
  std::vector<Bytes> files;
  for(std::size_t family=0;family<indexed_batch_families;++family) {
    const auto generated=bank.empty()?native_fixture::data(kNativeExtentBytes,static_cast<unsigned>(714+family*5731)):Bytes{};
    const auto base=bank.empty()?std::span<const std::byte>(generated):bank.subspan(family*kNativeExtentBytes,kNativeExtentBytes);
    for(std::size_t first=0;first<extent_count;first+=indexed_batch_extents_per_file) {
      Bytes file(indexed_batch_extents_per_file*kNativeExtentBytes);
      for(std::size_t block=0;block<indexed_batch_extents_per_file;++block) {
        const auto member=first+block;
        auto extent=std::span(file).subspan(block*kNativeExtentBytes,kNativeExtentBytes);
        if(!modular && !bank.empty() && member%7==6) {
          std::ranges::copy(bank.last(kNativeExtentBytes),extent.begin());
          continue;
        }
        std::ranges::copy(base,extent.begin());
        if(member) {
          if(modular) {
            const auto width=std::size_t{1}<<(family%4);
            for(std::size_t at=0;at<extent.size();at+=width) {
              auto bias=std::uint64_t{0xc74482fa26e3bd97ULL}*member;
              if(shifted_bias && at%4096>=64)bias+=std::uint64_t{0x39cd1329af427eedULL}*member;
              if(member%2 && at%4096>=64)bias+=(at/width+member)%3-std::uint64_t{1};
              native_fixture::add_little_endian(extent.subspan(at,width),bias);
            }
            continue;
          }
          const auto delta=native_fixture::data(kNativeExtentBytes,
              static_cast<unsigned>(1511+family*kNativeMaxDirtyExtents+member));
          const auto mask=!bank.empty() && member%3==0?std::byte{1}:std::byte{0x30};
          for(std::size_t at=0;at<extent.size();++at)extent[at]^=delta[at]&mask;
        }
      }
      files.push_back(std::move(file));
    }
  }
  return files;
}

inline Bytes modular_advice_payload(std::span<const std::byte> frame) {
  if(frame.size()<128 || frame[8]!=std::byte{4})throw std::invalid_argument("Expected a modular member frame");
  Bytes payload(12);
  payload[0]=frame[104];
  std::copy_n(frame.begin()+112,8,payload.begin()+4);
  payload.insert(payload.end(),frame.begin()+128,frame.end());
  return payload;
}
}
