#pragma once
#include "flitzfs/gpu_protocol.hpp"

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
enum class BatchOutput : u32 {anchors,packed_xor,wide_packed_xor};

constexpr u32 reply_member_bytes(BatchOutput output) noexcept {
  return output==BatchOutput::wide_packed_xor?xor_wide_slot_bytes:xor_slot_bytes;
}

struct Bucket { u32 first,second; };
static_assert(sizeof(Bucket)==8);
inline constexpr u32 index_bytes=buckets*sizeof(Bucket);

constexpr u32 device_bytes(u32 members,BatchOutput output=BatchOutput::anchors) noexcept {
  // Preserve the narrow protocol's original grant. Wide XOR needs no index.
  const auto index=output==BatchOutput::wide_packed_xor?0U:index_bytes;
  return (members+1)*extent_bytes+index+members*reply_member_bytes(output);
}

FLITZFS_ANCHOR_INLINE u64 seed(const u8* bytes,u32 at) {
  u64 value=0;
  for (u32 i=0;i<seed_bytes;++i) value|=static_cast<u64>(bytes[at+i])<<(8*i);
  return value;
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
      auto length=seed_bytes;
      while (length<limit && base[source+length]==member[at+length]) ++length;
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
