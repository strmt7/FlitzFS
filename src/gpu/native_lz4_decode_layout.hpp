#pragma once
#include <cstdint>

#if defined(__HIP__)
#define FLITZFS_LZ4_INLINE __host__ __device__ inline
#else
#define FLITZFS_LZ4_INLINE inline
#endif

namespace flitzfs::native_lz4_decode {
using u8=std::uint8_t;
using u32=std::uint32_t;
using u64=std::uint64_t;
inline constexpr u32 version=1,alignment=16,pending=0xffffffffU;
struct Layout {
  u32 chunks,input_stride,output_stride,abi;
};
struct Record {
  u32 input_bytes,output_bytes,actual_bytes,status;
};
static_assert(sizeof(Layout)==16 && sizeof(Record)==alignment);

FLITZFS_LZ4_INLINE u64 input_at(Layout plan,u32 index) {
  return u64(plan.chunks)*sizeof(Record)+u64(index)*plan.input_stride;
}
FLITZFS_LZ4_INLINE u64 output_at(Layout plan,u32 index) {
  return input_at(plan,plan.chunks)+u64(index)*plan.output_stride;
}
FLITZFS_LZ4_INLINE bool valid_layout(Layout plan,u64 allocation) {
  const auto stride=u64(sizeof(Record))+plan.input_stride+plan.output_stride;
  return plan.abi==version && plan.chunks && plan.input_stride && plan.output_stride>alignment &&
      plan.input_stride%alignment==0 && plan.output_stride%alignment==0 &&
      plan.chunks<=allocation/stride && u64(plan.chunks)*stride==allocation;
}
FLITZFS_LZ4_INLINE bool valid_record(Layout plan,Record record) {
  return record.input_bytes && record.input_bytes<record.output_bytes &&
      record.input_bytes<=plan.input_stride && record.output_bytes<=plan.output_stride-alignment &&
      !record.actual_bytes && record.status==pending;
}

FLITZFS_LZ4_INLINE bool length(const u8* input,u32 bytes,u32& at,u32 limit,u32& value,bool extended) {
  if(value>limit)return false;
  if(!extended)return true;
  while(at<bytes) {
    const u32 add=input[at++];
    if(add>limit-value)return false;
    value+=add;
    if(add!=255)return true;
  }
  return false;
}

// Standalone LZ4 blocks use only data already produced by this block. Admit
// every input and output extent, including the final literal-only sequence.
FLITZFS_LZ4_INLINE bool valid_block(const u8* input,u32 bytes,u32 output) {
  if(!input || !bytes || bytes>=output)return false;
  u32 at=0,written=0;
  bool matched=false;
  while(at<bytes) {
    const auto token=input[at++];
    u32 literals=token>>4;
    if(!length(input,bytes,at,output-written,literals,literals==15) || literals>bytes-at)
      return false;
    written+=literals;
    at+=literals;
    if(at==bytes)return written==output && (!matched || literals>=5);
    if(bytes-at<2 || output-written<12)return false;
    const u32 distance=u32(input[at])|(u32(input[at+1])<<8);
    at+=2;
    if(!distance || distance>written)return false;
    const u32 nibble=token&15U;
    u32 match=4+nibble;
    if(!length(input,bytes,at,output-written,match,nibble==15))return false;
    written+=match;
    matched=true;
  }
  return false;
}
}
#undef FLITZFS_LZ4_INLINE
