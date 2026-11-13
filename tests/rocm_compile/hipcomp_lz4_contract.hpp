#pragma once
#include <cstdint>
#include <type_traits>
// Private experimental compiler contract, not an IPC or stored-data format.
#if defined(__HIPCC__)
#define FLITZFS_LZ4_CHECK __host__ __device__
#else
#define FLITZFS_LZ4_CHECK
#endif
namespace flitzfs::candidate_lz4 {
using u32=std::uint32_t;
inline constexpr u32 allocation_limit=256U*1024U,chunk_limit=65536U,chunk_count=8U;
struct Chunk {u32 input_offset=0,input_bytes=0,output_offset=0,output_capacity=0;};
struct Layout {
  Chunk chunks[chunk_count]{};
  u32 count=0,input_bytes=0,input_pointers=0,input_sizes=0;
  u32 output_pointers=0,output_sizes=0,scratch_offset=0;
  u32 hash_table_entries=0,scratch_bytes=0,total_bytes=0;
};
static_assert(sizeof(Chunk)==16 && sizeof(Layout)==168);
static_assert(std::is_standard_layout_v<Layout> && std::is_trivially_copyable_v<Layout>);
// Every subtraction is guarded. No pointer, allocation, storage, runtime or I/O.
FLITZFS_LZ4_CHECK inline bool region(u32& cursor,u32 offset,u32 bytes) noexcept {
  if(cursor>allocation_limit || !bytes)return false;
  const u32 start=(cursor+7U)&~7U;
  if(start>allocation_limit || offset!=start || bytes>allocation_limit-start)return false;
  cursor=start+bytes;
  return true;
}
FLITZFS_LZ4_CHECK inline bool valid_layout(const Layout& p,u32 allocated) noexcept {
  if(!p.count || p.count>chunk_count || !allocated || allocated>allocation_limit ||
      p.total_bytes!=allocated)return false;
  u32 input=0,largest=0;
  for(u32 i=0;i<p.count;++i) {
    const auto size=p.chunks[i].input_bytes;
    if(!size || size>chunk_limit || size>allocation_limit-input)return false;
    input+=size;
    if(size>largest)largest=size;
  }
  if(input!=p.input_bytes)return false;
  u32 entries=1;
  while(entries<largest && entries<16384U)entries*=2U;
  if(p.hash_table_entries!=entries || p.scratch_bytes!=entries*2U*p.count)return false;
  u32 cursor=0;
  const auto table_bytes=p.count*8U;
  if(!region(cursor,p.input_pointers,table_bytes) ||
      !region(cursor,p.input_sizes,table_bytes) ||
      !region(cursor,p.output_pointers,table_bytes) ||
      !region(cursor,p.output_sizes,table_bytes))return false;
  for(u32 i=0;i<p.count;++i) {
    const auto& c=p.chunks[i];
    const auto capacity=(c.input_bytes+1U+(c.input_bytes+254U)/255U+7U)&~7U;
    if(c.output_capacity!=capacity || !region(cursor,c.input_offset,c.input_bytes) ||
        !region(cursor,c.output_offset,capacity+32U))return false;
  }
  for(u32 i=p.count;i<chunk_count;++i) {
    const auto& c=p.chunks[i];
    if(c.input_offset || c.input_bytes || c.output_offset || c.output_capacity)return false;
  }
  return region(cursor,p.scratch_offset,p.scratch_bytes) && cursor==allocated;
}
FLITZFS_LZ4_CHECK inline bool valid_geometry(u32 selected_wave,u32 hardware_wave,
    u32 block_x,u32 block_y,u32 block_z,u32 grid_x,u32 grid_y,u32 grid_z,u32 count) noexcept {
  return (selected_wave==32U || selected_wave==64U) && hardware_wave==selected_wave &&
      block_x==selected_wave && block_y==1U && block_z==1U &&
      count>0U && count<=chunk_count && grid_x==count && grid_y==1U && grid_z==1U;
}
} // namespace flitzfs::candidate_lz4
#undef FLITZFS_LZ4_CHECK
