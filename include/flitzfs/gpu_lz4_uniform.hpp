#ifndef FLITZFS_GPU_LZ4_UNIFORM_HPP
#define FLITZFS_GPU_LZ4_UNIFORM_HPP
#include <cstdint>
#include <type_traits>
#if defined(__HIPCC__)
#define FLITZFS_UNIFORM_LZ4_HD __host__ __device__
#else
#define FLITZFS_UNIFORM_LZ4_HD
#endif
namespace flitzfs::uniform_lz4 {
using u32=std::uint32_t;using u64=std::uint64_t;
// Algorithmic block/offset constraint of the selected compressor, NOT a device,
// batch, operation, filesystem or host-memory capacity assumption.
inline constexpr u32 max_chunk_bytes=65536;
inline constexpr u32 guard_bytes=32;
struct Layout {
  u64 input_bytes=0,input_offset=0,output_sizes=0,output_offset=0;
  u64 output_stride=0,scratch_offset=0,scratch_stride=0,total_bytes=0;
  u32 chunks=0,chunk_bytes=0,hash_entries=0,reserved=0;
};
static_assert(sizeof(Layout)==80 && std::is_standard_layout_v<Layout> &&
    std::is_trivially_copyable_v<Layout>);
FLITZFS_UNIFORM_LZ4_HD inline bool add(u64 a,u64 b,u64& out) noexcept {
  if(b>~u64{0}-a)return false;out=a+b;return true;
}
FLITZFS_UNIFORM_LZ4_HD inline bool multiply(u64 a,u64 b,u64& out) noexcept {
  if(b && a>~u64{0}/b)return false;out=a*b;return true;
}
FLITZFS_UNIFORM_LZ4_HD inline bool region(u64& cursor,u64 bytes,u64& offset) noexcept {
  u64 aligned=0;if(!bytes || !add(cursor,7,aligned))return false;
  aligned&=~u64{7};u64 end=0;if(!add(aligned,bytes,end))return false;
  offset=aligned;cursor=end;return true;
}
// Constant-work canonical dense layout. No allocation, pointers, host queries or
// arbitrary byte ceiling. Checked integer/grid representation is independent of
// the caller's live resource admission. Failure always clears output.
FLITZFS_UNIFORM_LZ4_HD inline bool make_layout(u64 input,u32 chunk,Layout& out) noexcept {
  out={};if(!input || !chunk || chunk>max_chunk_bytes)return false;
  const auto count=input/chunk+(input%chunk!=0);
  if(count>~u32{0})return false;
  Layout p{};p.input_bytes=input;p.chunks=static_cast<u32>(count);p.chunk_bytes=chunk;
  p.hash_entries=1;while(p.hash_entries<chunk && p.hash_entries<16384U)p.hash_entries*=2U;
  // Pinned maxSizeOfStream, alignment and per-block output guard.
  p.output_stride=(u64(chunk)+1+(u64(chunk)+254)/255+7)&~u64{7};
  if(!add(p.output_stride,guard_bytes,p.output_stride))return false;
  p.scratch_stride=u64(p.hash_entries)*2;
  u64 cursor=0,bytes=0;
  if(!multiply(count,8,bytes) || !region(cursor,bytes,p.output_sizes) ||
      !region(cursor,input,p.input_offset) ||
      !multiply(count,p.output_stride,bytes) || !region(cursor,bytes,p.output_offset) ||
      !multiply(count,p.scratch_stride,bytes) || !region(cursor,bytes,p.scratch_offset))return false;
  p.total_bytes=cursor;out=p;return true;
}
FLITZFS_UNIFORM_LZ4_HD inline bool valid_layout(const Layout& p,u64 allocated) noexcept {
  Layout expected{};
  if(!make_layout(p.input_bytes,p.chunk_bytes,expected) || !allocated ||
      p.total_bytes!=allocated || p.reserved)return false;
  return p.input_offset==expected.input_offset && p.output_sizes==expected.output_sizes &&
      p.output_offset==expected.output_offset && p.output_stride==expected.output_stride &&
      p.scratch_offset==expected.scratch_offset && p.scratch_stride==expected.scratch_stride &&
      p.total_bytes==expected.total_bytes && p.chunks==expected.chunks &&
      p.hash_entries==expected.hash_entries;
}
FLITZFS_UNIFORM_LZ4_HD inline bool valid_geometry(u32 selected,u32 hardware,
    u32 block_x,u32 block_y,u32 block_z,u32 grid_x,u32 grid_y,u32 grid_z,u32 count) noexcept {
  return (selected==32 || selected==64) && hardware==selected &&
      block_x==selected && block_y==1 && block_z==1 && count>0 &&
      grid_x==count && grid_y==1 && grid_z==1;
}
struct Chunk {
  u64 input=0,output=0,scratch=0,size_slot=0;
  u32 input_bytes=0,output_capacity=0;
};
// Caller validates the common immutable layout once, then each independent
// block uses constant work. No per-block scan over all other descriptors.
FLITZFS_UNIFORM_LZ4_HD inline bool chunk(const Layout& p,u32 index,Chunk& out) noexcept {
  out={};if(index>=p.chunks || !valid_layout(p,p.total_bytes))return false;
  const u64 at=u64(index)*p.chunk_bytes;
  if(at>=p.input_bytes)return false;
  const auto left=p.input_bytes-at;Chunk c{};
  c.input=p.input_offset+at;c.output=p.output_offset+u64(index)*p.output_stride;
  c.scratch=p.scratch_offset+u64(index)*p.scratch_stride;c.size_slot=p.output_sizes+u64(index)*8;
  c.input_bytes=static_cast<u32>(left<p.chunk_bytes?left:p.chunk_bytes);
  c.output_capacity=static_cast<u32>(p.output_stride-guard_bytes);
  out=c;return true;
}
}
#undef FLITZFS_UNIFORM_LZ4_HD

#endif // FLITZFS_GPU_LZ4_UNIFORM_HPP
