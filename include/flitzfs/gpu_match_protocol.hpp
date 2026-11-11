#pragma once
#include "flitzfs/gpu_protocol.hpp"
#if defined(__HIP__)
#define FLITZFS_MATCH_INLINE __attribute__((host, device)) inline
#else
#define FLITZFS_MATCH_INLINE inline
#endif
namespace flitzfs::match_detail {
using gpu_detail::u8;
using gpu_detail::u32;
using u16=unsigned short;
static_assert(sizeof(u16)==2);
inline constexpr u32 abi_version=1,input_limit=65536,anchor_stride=16;
inline constexpr u32 lane_limit=input_limit/anchor_stride;
inline constexpr u32 candidate_limit=8,comparison_limit=1024,match_limit=8192;
inline constexpr u16 no_previous=65535;
// Adapted from the owner's bounded predecessor search at
// strmt7/FlitzZip 50bae7610275a8f68d1fb89f11f7e0e238187aba.
// The repositories share the GNU AGPL-3.0-or-later license.
struct Match {
  u16 distance=0,length=0,candidates=0,compared=0;
  bool operator==(const Match&) const = default;
};
static_assert(sizeof(Match)==8);
static_assert(input_limit*(1+sizeof(u16))+lane_limit*sizeof(Match)<=gpu_detail::max_batch_bytes);
FLITZFS_MATCH_INLINE u32 lanes(u32 size) {return size/anchor_stride+(size%anchor_stride!=0);}
FLITZFS_MATCH_INLINE u32 minimum(u32 a,u32 b) {return a<b?a:b;}

// All source accesses stay inside one admitted input. Every traversed link
// must be strictly backward, so malformed chains cannot cycle.
FLITZFS_MATCH_INLINE Match search(const u8* input,u32 size,const u16* previous,
    u32 previous_count,u32 lane) {
  Match best{};
  if(!input || !previous || size>input_limit || previous_count!=size || lane>=lanes(size))
    return best;
  const auto position=lane*anchor_stride;
  if(size-position<4)return best;
  const auto limit=minimum(match_limit,size-position);
  u32 candidate=previous[position];
  while(candidate<position && best.candidates<candidate_limit && best.compared<comparison_limit) {
    ++best.candidates;
    bool improve=true;
    if(best.length>=4) {
      ++best.compared;
      improve=input[candidate+best.length]==input[position+best.length];
    }
    if(improve) {
      u32 length=0;
      while(length<limit && best.compared<comparison_limit) {
        ++best.compared;
        if(input[candidate+length]!=input[position+length])break;
        ++length;
      }
      if(length>=4 && length>best.length) {
        best.distance=static_cast<u16>(position-candidate);
        best.length=static_cast<u16>(length);
        if(length==limit)break;
      }
    }
    const u32 next=previous[candidate];
    if(next>=candidate)break;
    candidate=next;
  }
  return best;
}
} // namespace flitzfs::match_detail
#undef FLITZFS_MATCH_INLINE
