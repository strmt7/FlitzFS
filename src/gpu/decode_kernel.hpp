#pragma once
// Device-facing ABI. No allocation, exceptions, recursion, atomics or SDK types.
// Changes to layout require a new ABI version and module identity.
#if defined(__HIP__)
#define FLITZFS_DEVICE_INLINE __attribute__((host, device)) inline
#else
#define FLITZFS_DEVICE_INLINE inline
#endif
#include "flitzfs/gpu_protocol.hpp"
namespace flitzfs::gpu_detail {
// Subtraction-first checks protect both host semantic tests and device pointers.
FLITZFS_DEVICE_INLINE bool extent(u32 start, u32 length, u32 capacity) {
  return start <= capacity && length <= capacity-start;
}
FLITZFS_DEVICE_INLINE u32 little32(const u8* p) {
  return u32(p[0]) | (u32(p[1])<<8) | (u32(p[2])<<16) | (u32(p[3])<<24);
}
struct Bits {
  const u8* bytes;
  u32 size, bit=0;
  FLITZFS_DEVICE_INLINE bool take(u32 width, u32& result) {
    if (width > 12 || bit > size*8 || width > size*8-bit) return false;
    result=0;
    for (u32 i=0;i<width;++i) result |= u32((bytes[(bit+i)/8]>>((bit+i)%8))&1U)<<i;
    bit+=width; return true;
  }
  FLITZFS_DEVICE_INLINE u32 peek12() const {
    u32 result=0;
    for (u32 i=0;i<12 && bit+i<size*8;++i) result |= u32((bytes[(bit+i)/8]>>((bit+i)%8))&1U)<<i;
    return result;
  }
};
// Decode one independent segment. Caller owns separate source, destination and
// status arrays of their stated capacities. Host plan validates disjoint tasks.
// Failure can leave partial private output; the host must never publish it.
FLITZFS_DEVICE_INLINE u32 decode_task(const Task& t, const u8* input,
    u32 input_size, u8* output, u32 output_size) {
  if (!input || !output || input_size>max_batch_bytes || output_size>max_batch_bytes ||
      !t.decoded_bytes || t.decoded_bytes>max_block_bytes || !t.segment_length ||
      t.segment_length>segment_bytes || t.segment_offset%segment_bytes ||
      !extent(t.input_offset,t.input_bytes,input_size) ||
      !extent(t.output_offset,t.decoded_bytes,output_size) ||
      !extent(t.segment_offset,t.segment_length,t.decoded_bytes) || t.fill>255)
    return bad_extent;
  const auto remaining=t.decoded_bytes-t.segment_offset;
  const auto length=remaining<segment_bytes?remaining:segment_bytes;
  if (t.segment_length!=length) return bad_extent;
  const u8* source=input+t.input_offset;
  u8* target=output+t.output_offset+t.segment_offset;
  if (t.kind==0) {
    if (t.input_bytes!=t.decoded_bytes) return bad_extent;
    for(u32 i=0;i<length;++i) target[i]=source[t.segment_offset+i];
    return success;
  }
  if(t.kind==1) {
    if(t.input_bytes) return bad_extent;
    for(u32 i=0;i<length;++i) target[i]=static_cast<u8>(t.fill);
    return success;
  }
  if(t.kind==3) {
    if(t.input_bytes<2 || t.input_bytes>16384 || t.input_bytes>=t.decoded_bytes) return bad_extent;
    for(u32 i=0;i<length;++i) target[i]=source[(t.segment_offset+i)%t.input_bytes];
    return success;
  }
  if(t.kind!=4 && t.kind!=5 && t.kind!=10) return bad_kind;
  if(!t.input_bytes || t.input_bytes>=t.decoded_bytes) return bad_extent;
  const u32 header=t.kind==5?84:(t.kind==10?8192:0);
  const u32 segments=(t.decoded_bytes+segment_bytes-1)/segment_bytes;
  const u32 table=(segments+1)*4;
  if(header>t.input_bytes || table>=t.input_bytes-header) return bad_table;
  const u32 data_start=header+table, data_size=t.input_bytes-data_start;
  const u8* offsets=source+header;
  if(little32(offsets)!=0 || little32(offsets+segments*4)!=data_size) return bad_table;
  // All offsets are validated, even for a task in an earlier segment.
  u32 previous=0;
  for(u32 i=1;i<=segments;++i) {
    const auto next=little32(offsets+i*4);
    if(next<previous || next>data_size) return bad_table;
    previous=next;
  }
  const u32 index=t.segment_offset/segment_bytes;
  const u32 begin=little32(offsets+index*4), end=little32(offsets+(index+1)*4);
  Bits bits{source+data_start+begin,end-begin,0};
  for(u32 i=0;i<length;++i) {
    u32 value=0, one=0;
    if(t.kind==10) {
      const auto slot=bits.peek12();
      const auto width=u32(source[slot*2+1]);
      if(!width || width>12) return bad_code;
      if(!bits.take(width,value)) return truncated;
      target[i]=source[slot*2];
      continue;
    }
    if(!bits.take(1,one)) return truncated;
    u32 rank=0;
    if(!one) { if(!bits.take(2,rank)) return truncated; }
    else {
      if(!bits.take(1,one)) return truncated;
      if(!one) { if(!bits.take(4,rank)) return truncated; rank+=4; }
      else {
        if(!bits.take(1,one)) return truncated;
        if(!one) { if(!bits.take(6,rank)) return truncated; rank+=20; }
        else {
          if(!bits.take(8,value)) return truncated;
          if(t.kind==4 && value>171) return bad_code;
          target[i]=static_cast<u8>(t.kind==4?value+84:value);
          continue;
        }
      }
    }
    target[i]=t.kind==5?source[rank]:static_cast<u8>(rank);
  }
  return success;
}
} // namespace flitzfs::gpu_detail
#undef FLITZFS_DEVICE_INLINE
