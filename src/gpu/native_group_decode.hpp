#pragma once
#include <cstdint>

#if defined(__HIP__)
#define FLITZFS_GROUP_INLINE __host__ __device__ inline
#else
#define FLITZFS_GROUP_INLINE inline
#endif

namespace flitzfs::group_decode {
using u8=std::uint8_t;
using u16=std::uint16_t;
using u32=std::uint32_t;
using u64=std::uint64_t;
inline constexpr u32 version=1,extent_bytes=65536,tile_bytes=256,tiles=extent_bytes/tile_bytes;
inline constexpr u32 words_per_tile=tile_bytes/8,words=extent_bytes/8;
inline constexpr u32 maximum_members=64;
inline constexpr u8 complete=0x5a;

struct Layout {u32 members,bases,body_bytes,abi;};
struct Member {u32 base,body_at,body_bytes;u16 offsets[tiles+1],reserved;};
static_assert(sizeof(Member)==528 && sizeof(Layout)==16);

FLITZFS_GROUP_INLINE u64 bases_at(Layout layout) {
  return u64(layout.members)*sizeof(Member);
}
FLITZFS_GROUP_INLINE u64 bodies_at(Layout layout) {
  return bases_at(layout)+u64(layout.bases)*extent_bytes;
}
FLITZFS_GROUP_INLINE u64 output_at(Layout layout) {
  return (bodies_at(layout)+layout.body_bytes+7)&~u64{7};
}
FLITZFS_GROUP_INLINE u64 status_at(Layout layout) {
  return output_at(layout)+u64(layout.members)*extent_bytes;
}
FLITZFS_GROUP_INLINE u64 slab_bytes(Layout layout) {
  return status_at(layout)+u64(layout.members)*tiles;
}
FLITZFS_GROUP_INLINE bool valid(Layout layout) {
  return layout.abi==version && layout.members && layout.members<=maximum_members &&
      layout.bases && layout.bases<=layout.members && layout.body_bytes>=layout.members*tiles &&
      layout.body_bytes<layout.members*extent_bytes;
}
FLITZFS_GROUP_INLINE u64 spread(u8 value) {
  auto expanded=u64(value);
  expanded=(expanded|(expanded<<28))&0x0000000f0000000fULL;
  expanded=(expanded|(expanded<<14))&0x0003000300030003ULL;
  return (expanded|(expanded<<7))&0x0101010101010101ULL;
}

// Each lane produces eight consecutive bytes, and neighboring lanes read
// neighboring bytes in each active plane and in the shared decoded base.
FLITZFS_GROUP_INLINE bool reconstruct(const Member& member,const u8* bases,const u8* bodies,
    Layout layout,u32 word,u8* output) {
  if(word>=words || member.base>=layout.bases || member.reserved || member.body_at>layout.body_bytes ||
      member.body_bytes>layout.body_bytes-member.body_at)return false;
  const auto tile=word/words_per_tile,lane=word%words_per_tile;
  const auto begin=member.offsets[tile],end=member.offsets[tile+1];
  if(begin>=end || end>member.body_bytes)return false;
  const auto* tile_data=bodies+member.body_at+begin;
  const unsigned mask=*tile_data++;
  const auto size=end-begin-1;
  if(size%words_per_tile || size/words_per_tile>8)return false;
  u64 difference=0;
  unsigned at=0;
  for(unsigned bit=0;bit<8;++bit) {
    if(!(mask&(1U<<bit)))continue;
    if(at+words_per_tile>static_cast<unsigned>(size))return false;
    difference|=spread(tile_data[at+lane])<<bit;
    at+=words_per_tile;
  }
  if(at!=static_cast<unsigned>(size))return false;
  const auto* base=bases+member.base*extent_bytes+word*8;
  for(unsigned i=0;i<8;++i)output[word*8+i]=base[i]^static_cast<u8>(difference>>(i*8));
  return true;
}

using Kernel=void (*)(u8*,u64,Layout);
struct KernelApi {u32 size_bytes,abi,layout_bytes,member_bytes;Kernel packed_xor;};
inline bool compatible(const KernelApi& api) noexcept {
  return api.size_bytes==sizeof(KernelApi) && api.abi==version && api.layout_bytes==sizeof(Layout) &&
      api.member_bytes==sizeof(Member) && api.packed_xor;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
#undef FLITZFS_GROUP_INLINE
