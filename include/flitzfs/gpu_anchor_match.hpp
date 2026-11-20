#pragma once
#include "flitzfs/gpu_protocol.hpp"
#if !defined(__HIP_DEVICE_COMPILE__)
#include <bit>
#include <cstring>
#endif

#if defined(__HIP__)
#define FLITZFS_ANCHOR_INLINE __attribute__((host, device)) inline
#else
#define FLITZFS_ANCHOR_INLINE inline
#endif

namespace flitzfs::anchor_match {
using gpu_detail::u8;
using gpu_detail::u32;
using u64=unsigned long long;
static_assert(sizeof(u64)==8);

inline constexpr u32 abi_version=1;
inline constexpr u32 extent_bytes=65536;
inline constexpr u32 max_members=gpu_detail::max_batch_bytes/extent_bytes-1;
inline constexpr u32 tile_bytes=16;
inline constexpr u32 tiles=extent_bytes/tile_bytes;
inline constexpr u32 seed_bytes=8;
inline constexpr u32 maximum_match=64;
inline constexpr u32 buckets=65536;
inline constexpr u32 empty=0xffffffffU;
inline constexpr u32 threads=64;
// A separate worker reply representation reuses the admitted per-member slab.
// Its body is the packed XOR format without the authoritative 128-byte header.
inline constexpr u32 xor_tile_bytes=256,xor_plane_bytes=xor_tile_bytes/8;
inline constexpr u32 xor_tiles=extent_bytes/xor_tile_bytes;
inline constexpr u32 xor_slot_bytes=tiles*sizeof(u32);
inline constexpr u32 xor_body_capacity=xor_slot_bytes-sizeof(u32);
inline constexpr u32 xor_wide_slot_bytes=extent_bytes;
inline constexpr u32 sparse_plane_max=xor_plane_bytes-2,sparse_plane_inverse=0x80,sparse_plane_dense=0x40;
inline constexpr u32 sparse_xor_min_body_bytes=xor_tiles+2;
inline constexpr u32 sparse_xor_max_body_bytes=extent_bytes-128-1;
inline constexpr u32 sparse_xor_arena_header_words=1,sparse_xor_arena_entry_words=2;
FLITZFS_ANCHOR_INLINE constexpr u32 sparse_xor_arena_words(u32 bytes) noexcept {
  return bytes/sizeof(u32)+(bytes%sizeof(u32)!=0);
}
constexpr u32 sparse_xor_arena_directory_words(u32 members) noexcept {
  return sparse_xor_arena_header_words+members*sparse_xor_arena_entry_words;
}
static_assert((sparse_xor_arena_directory_words(1)+sparse_xor_arena_words(sparse_xor_max_body_bytes))*
    sizeof(u32)<=xor_wide_slot_bytes);
struct SparseXorScratch {
  u8 descriptors[xor_tiles][8],masks[xor_tiles],sparse_masks[xor_tiles];
  u32 offsets[xor_tiles],size,body_offset;
};
static_assert(xor_tile_bytes==256 && sparse_plane_max<sparse_plane_dense && sparse_plane_dense<sparse_plane_inverse);
inline constexpr u32 delta_parameters_bytes=12;
inline constexpr u32 delta_max_payload_bytes=extent_bytes-128+delta_parameters_bytes-1;
inline constexpr u32 delta_word_widths=4;
inline constexpr u32 delta_bias_samples=16,delta_bias_choices=5;
inline constexpr u32 delta_modes=delta_word_widths*delta_bias_choices;
static_assert(delta_modes<=threads);
struct ModularDeltaScratch {
  u64 biases[delta_modes];
  u32 bias_counts[delta_word_widths];
  u8 widths[delta_modes][xor_tiles];
  u32 sizes[delta_modes],offsets[xor_tiles],mode;
};
enum class CollectiveBody : u32 {sparse_xor=1,modular_delta=2};
inline constexpr u32 collective_body_tag_bytes=sizeof(u32);
static_assert((sparse_xor_arena_directory_words(1)+
    sparse_xor_arena_words(collective_body_tag_bytes+delta_max_payload_bytes))*sizeof(u32)<=xor_wide_slot_bytes);
struct CollectivePackingScratch {
  ModularDeltaScratch delta;
  SparseXorScratch sparse;
  u32 body_offset,admitted;
};
enum class BatchOutput : u32 {anchors,packed_xor,wide_packed_xor,modular_delta,sparse_xor,collective,reference_xor};

constexpr bool arena_output(BatchOutput output) noexcept {
  return output==BatchOutput::sparse_xor || output==BatchOutput::collective || output==BatchOutput::reference_xor;
}
constexpr u32 reference_member_at(u32 candidate,u32 candidates,u32 members) noexcept {
  return candidates>1?static_cast<u32>(u64{candidate}*members/(candidates-1)):0;
}

constexpr bool compact_output(BatchOutput output) noexcept {
  return output==BatchOutput::modular_delta || arena_output(output);
}

constexpr bool wide_output(BatchOutput output) noexcept {
  return output==BatchOutput::wide_packed_xor || compact_output(output);
}

constexpr u32 reply_member_bytes(BatchOutput output) noexcept {
  return wide_output(output)?xor_wide_slot_bytes:xor_slot_bytes;
}

constexpr u32 dispatch_member_limit(BatchOutput output) noexcept {
  // One shared base and disjoint input/result slots must fit the 32-bit slab.
  // The work grant and live memory and grid limits further constrain each launch.
  return wide_output(output)?
      (0xffffffffU-extent_bytes)/(extent_bytes+xor_wide_slot_bytes):max_members;
}

struct Bucket { u32 first,second; };
static_assert(sizeof(Bucket)==8);
inline constexpr u32 index_bytes=buckets*sizeof(Bucket);

constexpr u32 device_bytes(u32 members,BatchOutput output=BatchOutput::anchors) noexcept {
  // Preserve the narrow protocol's original grant. Wide XOR needs no index.
  const auto index=wide_output(output)?0U:index_bytes;
  return (members+1)*extent_bytes+index+members*reply_member_bytes(output);
}

FLITZFS_ANCHOR_INLINE u32 xor_plane_bits(u64 differences,u32 bit) noexcept {
  u32 packed=0;
  for(u32 i=0;i<8;++i)packed|=static_cast<u32>((differences>>(8*i+bit))&1ULL)<<i;
  return packed;
}

FLITZFS_ANCHOR_INLINE constexpr u32 sparse_xor_descriptor(u32 ones) noexcept {
  if(!ones)return 0;
  const bool inverse=ones>xor_tile_bytes/2;
  const auto minority=inverse?xor_tile_bytes-ones:ones;
  if(minority>sparse_plane_max)return sparse_plane_dense;
  return minority|(inverse?sparse_plane_inverse:0U);
}

FLITZFS_ANCHOR_INLINE constexpr u32 modular_bias_sample_offset(u32 sample) noexcept {
  return sample*(extent_bytes/delta_bias_samples)+(sample+1)*128;
}
static_assert(modular_bias_sample_offset(delta_bias_samples-1)+sizeof(u64)<=extent_bytes);

// Order the sample in the signed modular interval around the first
// difference. Unsigned arithmetic keeps every word width reversible,
// including values straddling zero or the most significant bit.
template<class Word>
FLITZFS_ANCHOR_INLINE u32 modular_bias_candidates(Word first,Word (&samples)[delta_bias_samples],
    Word (&biases)[delta_bias_choices]) noexcept {
  constexpr auto sign=u64{1}<<(8*sizeof(Word)-1);
  for(u32 i=0;i<delta_bias_samples;++i) {
    const auto relative=static_cast<Word>(u64{samples[i]}-first);
    const auto key=static_cast<Word>(u64{relative}^sign);
    u32 at=i;
    while(at && samples[at-1]>key) {
      samples[at]=samples[at-1];
      --at;
    }
    samples[at]=key;
  }
  u32 count=1;
  biases[0]=first;
  if(first)biases[count++]=0;
  for(u32 rank=delta_bias_samples/4;rank<delta_bias_samples;rank+=delta_bias_samples/4) {
    const auto candidate=static_cast<Word>((u64{samples[rank]}^sign)+first);
    u32 at=0;
    while(at<count && biases[at]!=candidate)++at;
    if(at==count)biases[count++]=candidate;
  }
  return count;
}

FLITZFS_ANCHOR_INLINE u64 seed(const u8* bytes,u32 at) {
#if defined(__HIP_DEVICE_COMPILE__)
  u64 value;
  __builtin_memcpy(&value,bytes+at,sizeof(value));
  return value;
#else
  if constexpr(std::endian::native==std::endian::little) {
    u64 value;
    std::memcpy(&value,bytes+at,sizeof(value));
    return value;
  }
  u64 value=0;
  for(u32 i=0;i<seed_bytes;++i)value|=static_cast<u64>(bytes[at+i])<<(8*i);
  return value;
#endif
}

// Both spans have at least limit bytes and already match over the first seed.
FLITZFS_ANCHOR_INLINE u32 extend_match(const u8* base,const u8* member,u32 limit) {
  u32 length=seed_bytes;
  for(;limit-length>=seed_bytes;length+=seed_bytes) {
    const auto difference=seed(base,length)^seed(member,length);
    if(!difference)continue;
#if defined(__HIP_DEVICE_COMPILE__)
    return length+static_cast<u32>(__builtin_ctzll(difference)/8);
#else
    return length+static_cast<u32>(std::countr_zero(difference)/8);
#endif
  }
  while(length<limit && base[length]==member[length])++length;
  return length;
}
FLITZFS_ANCHOR_INLINE u32 bucket(u64 key) {
  key^=key>>33;
  key*=0xff51afd7ed558ccdULL;
  key^=key>>33;
  return static_cast<u32>(key)&(buckets-1);
}
FLITZFS_ANCHOR_INLINE bool index_position(const u8* base,u32 at,u32& slot) {
  if (at>extent_bytes-seed_bytes) return false;
  const auto key=seed(base,at);
  // Suppress redundant writes from long runs with a period dividing a tile.
  if (at>=tile_bytes && key==seed(base,at-tile_bytes)) return false;
  slot=bucket(key);
  return true;
}

// One 32-bit suggestion per tile. Zero is no match. Offsets and lengths
// are verified against the original bytes before becoming storage advice.
FLITZFS_ANCHOR_INLINE u32 search(const u8* base,const u8* member,const Bucket* table,u32 lane) {
  const auto begin=lane*tile_bytes;
  u32 best=0,best_length=0;
  for (u32 within=0;within<tile_bytes;++within) {
    const auto at=begin+within;
    if (extent_bytes-at<seed_bytes) break;
    const auto key=seed(member,at);
    const auto hit=table[bucket(key)];
    const u32 positions[]{hit.first,hit.second};
    for (const auto source:positions) {
      if (source>extent_bytes-seed_bytes || seed(base,source)!=key) continue;
      const auto last=source>at?source:at;
      const auto available=extent_bytes-last;
      const auto limit=available<maximum_match?available:maximum_match;
      if (limit<=best_length ||
          (best_length>=seed_bytes && base[source+best_length]!=member[at+best_length])) continue;
      const auto length=extend_match(base+source,member+at,limit);
      if (length>best_length) {
        best_length=length;
        best=source|(within<<16)|(length<<20);
        if (length==maximum_match) return best;
      }
    }
  }
  return best;
}
} // namespace flitzfs::anchor_match
#undef FLITZFS_ANCHOR_INLINE
