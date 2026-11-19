#pragma once
#include "flitzfs/gpu_anchor_match.hpp"
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
inline constexpr auto indexed_small_extents_per_family=3*indexed_batch_extents_per_file;
inline constexpr auto indexed_batch_members=indexed_batch_families*(kNativeMaxDirtyExtents-1);
inline constexpr auto indexed_sample_bank_bytes=(indexed_batch_families+1)*kNativeExtentBytes;
static_assert(kNativeMaxDirtyExtents%indexed_batch_extents_per_file==0 && indexed_batch_members>kNativeMaxExtents);
enum class IndexedBatchPattern {
  packed_xor,modular_delta,shifted_bias,xor_reference,xor_tile_reference,xor_sparse_reference,xor_full_reference,
  sparse_xor,sparse_xor_all_planes,collective
};

inline bool small_indexed_batch(IndexedBatchPattern pattern) noexcept {
  return pattern==IndexedBatchPattern::shifted_bias || pattern==IndexedBatchPattern::xor_reference ||
      pattern==IndexedBatchPattern::xor_tile_reference || pattern==IndexedBatchPattern::xor_sparse_reference ||
      pattern==IndexedBatchPattern::xor_full_reference ||
      pattern==IndexedBatchPattern::sparse_xor ||
      pattern==IndexedBatchPattern::sparse_xor_all_planes || pattern==IndexedBatchPattern::collective;
}

inline void apply_reference_variation(std::span<std::byte> extent,unsigned member) {
  const auto variation=native_fixture::data(extent.size(),9811+member);
  for(std::size_t i=0;i<extent.size();++i) {
    // Keep a common masked signature. The first extent differs in a higher
    // plane while the other extents vary in the lowest plane.
    const auto mask=member==0?std::byte{0x40}:i%4096>=64?std::byte{1}:std::byte{};
    extent[i]^=variation[i]&mask;
  }
}

inline void apply_tile_reference_variation(std::span<std::byte> extent,unsigned member,unsigned group_size) {
  if(member>=2*group_size/3) {
    // The high-plane minority shares the low planes across complete tiles.
    // One distinct byte prevents the members from being exact duplicates.
    for(auto& byte:extent)byte^=std::byte{0x80};
    extent[768+member]^=std::byte{0x20};
    return;
  }
  const auto variation=native_fixture::data(extent.size(),14741+member);
  constexpr std::byte mask{0x1f};
  for(std::size_t i=0;i<extent.size();++i) {
    if(i%4096>=64)extent[i]^=variation[i]&mask;
  }
  // Keep the former single-byte sample identical within the high-plane
  // majority. Its bit-width score then prefers the first member.
  constexpr std::size_t samples=64,spacing=kNativeExtentBytes/samples;
  for(std::size_t site=0;site<samples;++site) {
    const auto offset=site*spacing+(spacing/4+site*7)%spacing;
    extent[offset]^=variation[offset]&mask;
  }
}

inline void apply_sparse_xor_variation(std::span<std::byte> extent,unsigned member) {
  if(!member)return;
  constexpr auto tile_bytes=anchor_match::xor_tile_bytes;
  for(std::size_t at=0;at<extent.size();at+=tile_bytes) {
    auto tile=extent.subspan(at,tile_bytes);
    if(member%2)for(auto& value:tile)value^=std::byte{0x80};
    for(unsigned i=0;i<6;++i) {
      // Keep the masked discovery samples and scatter the low-plane flips.
      const auto position=64+(61*member+13*(at/tile_bytes)+31*i)%192;
      tile[position]^=std::byte{3};
    }
  }
}

inline void apply_sparse_reference_variation(std::span<std::byte> extent,unsigned member,unsigned group_size) {
  constexpr auto tile_bytes=anchor_match::xor_tile_bytes;
  constexpr unsigned unchanged=64,positions=tile_bytes-unchanged;
  const bool dense_minority=member<group_size/4;
  for(std::size_t at=0;at<extent.size();at+=tile_bytes) {
    const auto index=static_cast<unsigned>(at/tile_bytes);
    auto tile=extent.subspan(at,tile_bytes);
    // The minority shares nearly all plane patterns, but differs in one
    // dense plane from the other members. The majority has more planes
    // with distinct patterns and far fewer changed bits per plane.
    if(dense_minority)for(unsigned site=unchanged;site<tile_bytes;site+=2)tile[site]^=std::byte{0x80};
    for(unsigned bit=0;bit<(dense_minority?1U:6U);++bit) {
      const auto first=(37*member+19*index+29*bit)%positions;
      const auto mask=static_cast<std::byte>(1U<<bit);
      tile[unchanged+first]^=mask;
      tile[unchanged+(first+79)%positions]^=mask;
    }
  }
}

inline void apply_full_reference_variation(std::span<std::byte> extent,std::span<const std::byte> base,
    unsigned member,unsigned group_size) {
  apply_sparse_reference_variation(extent,member,group_size);
  constexpr std::size_t samples=8,tile=anchor_match::xor_tile_bytes,spacing=kNativeExtentBytes/(samples*tile);
  for(std::size_t site=0;site<samples;++site) {
    const auto at=(site*spacing+2*((site*7)%(spacing/2))+1)*tile;
    std::ranges::copy(base.subspan(at,tile),extent.begin()+at);
  }
}

inline void apply_all_xor_planes(std::span<std::byte> extent,unsigned member) {
  if(!member)return;
  constexpr auto tile_bytes=anchor_match::xor_tile_bytes;
  for(std::size_t at=0;at<extent.size();at+=tile_bytes) {
    auto tile=extent.subspan(at,tile_bytes);
    const auto index=at/tile_bytes;
    // All eight planes differ, with a mixture of dense, sparse and inverse
    // lists across tiles. Keep the masked discovery bytes intact.
    if(index%4==0)for(std::size_t i=64;i<tile.size();i+=2)tile[i]^=std::byte{2};
    if(index%4==1 && member%2)for(auto& byte:tile)byte^=std::byte{0x80};
    for(unsigned bit=0;bit<8;++bit)
      tile[64+(73*member+37*bit+11*index)%192]^=static_cast<std::byte>(1U<<bit);
  }
}
// Each scan window sees one disjoint family spread across several files.
// The optional holdout bank also contributes unmodified unrelated extents.
inline std::vector<Bytes> indexed_batch_files(std::span<const std::byte> bank={},
    IndexedBatchPattern pattern=IndexedBatchPattern::packed_xor) {
  if(!bank.empty() && bank.size()!=indexed_sample_bank_bytes)throw std::invalid_argument("Indexed sample bank size");
  const bool shifted_bias=pattern==IndexedBatchPattern::shifted_bias;
  const bool xor_reference=pattern==IndexedBatchPattern::xor_reference;
  const bool xor_tile_reference=pattern==IndexedBatchPattern::xor_tile_reference;
  const bool collective=pattern==IndexedBatchPattern::collective;
  const auto extent_count=small_indexed_batch(pattern)?indexed_small_extents_per_family:kNativeMaxDirtyExtents;
  std::vector<Bytes> files;
  for(std::size_t family=0;family<indexed_batch_families;++family) {
    const bool modular=shifted_bias || pattern==IndexedBatchPattern::modular_delta || (collective && family%2);
    const auto generated=bank.empty()?native_fixture::data(kNativeExtentBytes,static_cast<unsigned>(714+family*5731)):Bytes{};
    const auto base=bank.empty()?std::span<const std::byte>(generated):bank.subspan(family*kNativeExtentBytes,kNativeExtentBytes);
    for(std::size_t first=0;first<extent_count;first+=indexed_batch_extents_per_file) {
      Bytes file(indexed_batch_extents_per_file*kNativeExtentBytes);
      for(std::size_t block=0;block<indexed_batch_extents_per_file;++block) {
        const auto member=first+block;
        auto extent=std::span(file).subspan(block*kNativeExtentBytes,kNativeExtentBytes);
        if(pattern==IndexedBatchPattern::packed_xor && !bank.empty() && member%7==6) {
          std::ranges::copy(bank.last(kNativeExtentBytes),extent.begin());
          continue;
        }
        std::ranges::copy(base,extent.begin());
        if(xor_reference) {
          apply_reference_variation(extent,static_cast<unsigned>(member));
          continue;
        }
        if(xor_tile_reference) {
          apply_tile_reference_variation(extent,static_cast<unsigned>(member),static_cast<unsigned>(extent_count));
          continue;
        }
        if(pattern==IndexedBatchPattern::xor_sparse_reference) {
          apply_sparse_reference_variation(extent,static_cast<unsigned>(member),static_cast<unsigned>(extent_count));
          continue;
        }
        if(pattern==IndexedBatchPattern::xor_full_reference) {
          apply_full_reference_variation(extent,base,static_cast<unsigned>(member),static_cast<unsigned>(extent_count));
          continue;
        }
        if(pattern==IndexedBatchPattern::sparse_xor) {
          apply_sparse_xor_variation(extent,static_cast<unsigned>(member));
          continue;
        }
        if(pattern==IndexedBatchPattern::sparse_xor_all_planes || (collective && !modular)) {
          apply_all_xor_planes(extent,static_cast<unsigned>(member));
          continue;
        }
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
inline Bytes collective_advice_payload(anchor_match::CollectiveBody codec,std::span<const std::byte> payload) {
  Bytes result(anchor_match::collective_body_tag_bytes);
  result[0]=static_cast<std::byte>(codec);
  result.insert(result.end(),payload.begin(),payload.end());
  return result;
}
}
