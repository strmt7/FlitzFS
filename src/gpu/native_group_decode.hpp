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
inline constexpr u32 version=1,mixed_version=2,extent_bytes=65536,tile_bytes=256,tiles=extent_bytes/tile_bytes;
inline constexpr u32 words_per_tile=tile_bytes/8,words=extent_bytes/8;
inline constexpr u32 maximum_members=64;
inline constexpr u8 complete=0x5a;
inline constexpr u32 delta_prefix_bytes=16,sparse_inverse=0x80,sparse_max=words_per_tile-2;
enum class Method:u16 {packed_xor=0,sparse_xor=1,modular_delta=2};

struct Layout {u32 members,bases,body_bytes,abi;};
struct Member {u32 base,body_at,body_bytes;u16 offsets[tiles+1];Method method;};
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
  return (layout.abi==version || layout.abi==mixed_version) && layout.members &&
      layout.members<=maximum_members && layout.bases && layout.bases<=layout.members &&
      layout.body_bytes>0 && layout.body_bytes<layout.members*extent_bytes &&
      (layout.abi!=version || layout.body_bytes>=layout.members*tiles);
}
FLITZFS_GROUP_INLINE u64 spread(u8 value) {
  auto expanded=u64(value);
  expanded=(expanded|(expanded<<28))&0x0000000f0000000fULL;
  expanded=(expanded|(expanded<<14))&0x0003000300030003ULL;
  return (expanded|(expanded<<7))&0x0101010101010101ULL;
}

FLITZFS_GROUP_INLINE unsigned sparse_lane(const u8* positions,u32 count,u32 lane,bool invert) {
  const auto start=lane*8;
  u32 low=0,high=count;
  while(low<high) {
    const auto middle=low+(high-low)/2;
    if(positions[middle]<start)low=middle+1;
    else high=middle;
  }
  unsigned bits=invert?0xffU:0;
  while(low<count && positions[low]<start+8) {
    const auto position=positions[low++];
    if(position>=start)bits^=1U<<(position-start);
  }
  return bits;
}

FLITZFS_GROUP_INLINE bool xor_word(const u8* data,u32 bytes,u32 lane,bool sparse,u64& difference) {
  const unsigned mask=data[0];
  u32 at=1;
  unsigned compact=0;
  if(sparse && mask) {
    if(bytes<2)return false;
    compact=data[at++];
    if(compact&~mask)return false;
  }
  for(unsigned bit=0;bit<8;++bit) {
    if(!(mask&(1U<<bit)))continue;
    unsigned packed=0;
    if(compact&(1U<<bit)) {
      if(at==bytes)return false;
      const auto descriptor=data[at++];
      const auto count=descriptor&~sparse_inverse;
      if(count>sparse_max || count>bytes-at || (!count && !(descriptor&sparse_inverse)))return false;
      packed=sparse_lane(data+at,count,lane,(descriptor&sparse_inverse)!=0);
      at+=count;
    }else {
      if(words_per_tile>bytes-at)return false;
      packed=data[at+lane];
      at+=words_per_tile;
    }
    difference|=spread(static_cast<u8>(packed))<<bit;
  }
  return at==bytes;
}

template<u32 Width> FLITZFS_GROUP_INLINE u64 load_little(const u8* bytes) {
  u64 word=0;
  for(u32 i=0;i<Width;++i)word|=u64(bytes[i])<<(8*i);
  return word;
}

template<u32 Width> FLITZFS_GROUP_INLINE bool delta_word(const u8* data,u32 bytes,u32 lane,u64 bias,
    const u8* base,u8* output) {
  constexpr auto words_here=8/Width,plane_bytes=words_per_tile/Width;
  const auto residuals=static_cast<u32>(bytes?data[0]:0);
  if(residuals>8*Width || (bytes && bytes!=1+residuals*plane_bytes))return false;
  const auto shift=(lane%Width)*words_here;
  const auto position=lane/Width;
  u64 encoded[words_here]{};
  for(u32 bit=0;bit<residuals;++bit) {
    const auto packed=static_cast<unsigned>(data[1+bit*plane_bytes+position])>>shift;
    for(u32 word=0;word<words_here;++word)encoded[word]|=u64((packed>>word)&1U)<<bit;
  }
  for(u32 word=0;word<words_here;++word) {
    const auto code=encoded[word];
    const auto delta=(code>>1)^(u64{0}-(code&1U));
    const auto value=load_little<Width>(base+word*Width)+bias+delta;
    for(u32 byte=0;byte<Width;++byte)output[word*Width+byte]=static_cast<u8>(value>>(8*byte));
  }
  return true;
}

// Each lane produces eight consecutive bytes from a self-contained tile.
FLITZFS_GROUP_INLINE bool reconstruct(const Member& member,const u8* bases,const u8* bodies,
    Layout layout,u32 word,u8* output) {
  if(word>=words || member.base>=layout.bases || member.body_at>layout.body_bytes ||
      member.body_bytes>layout.body_bytes-member.body_at ||
      (member.method!=Method::packed_xor && layout.abi!=mixed_version))return false;
  const auto tile=word/words_per_tile,lane=word%words_per_tile;
  const auto begin=member.offsets[tile],end=member.offsets[tile+1];
  const auto* encoded=bodies+member.body_at;
  const auto* base=bases+member.base*extent_bytes+word*8;
  auto* target=output+word*8;
  if(member.method==Method::modular_delta) {
    if(member.body_bytes<delta_prefix_bytes || end>member.body_bytes-delta_prefix_bytes ||
        (member.body_bytes==delta_prefix_bytes?begin!=0 || end!=0:begin>=end))return false;
    const auto* data=encoded+delta_prefix_bytes+begin;
    const auto bytes=end-begin;
    const auto bias=load_little<8>(encoded+8);
    switch(encoded[0]) {
      case 1:return delta_word<1>(data,bytes,lane,bias,base,target);
      case 2:return delta_word<2>(data,bytes,lane,bias,base,target);
      case 4:return delta_word<4>(data,bytes,lane,bias,base,target);
      case 8:return delta_word<8>(data,bytes,lane,bias,base,target);
      default:return false;
    }
  }
  if(begin>=end || end>member.body_bytes ||
      (member.method!=Method::packed_xor && member.method!=Method::sparse_xor))return false;
  u64 difference=0;
  if(!xor_word(encoded+begin,end-begin,lane,member.method==Method::sparse_xor,difference))return false;
  for(u32 i=0;i<8;++i)target[i]=base[i]^static_cast<u8>(difference>>(i*8));
  return true;
}

using Kernel=void (*)(u8*,u64,Layout);
struct KernelApi {u32 size_bytes,abi,layout_bytes,member_bytes;Kernel decode;};
inline bool compatible(const KernelApi& api,u32 abi) noexcept {
  return (abi==version || abi==mixed_version) && api.size_bytes==sizeof(KernelApi) &&
      api.abi==abi && api.layout_bytes==sizeof(Layout) && api.member_bytes==sizeof(Member) && api.decode;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
#undef FLITZFS_GROUP_INLINE
