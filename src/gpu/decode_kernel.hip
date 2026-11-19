// Built with the complete selected ROCm SDK. No runtime or kernel is launched
// during compile qualification; host/device definitions come from that SDK.
#include <hip/hip_runtime.h>
#include "decode_kernel.hpp"
#include "flitzfs/gpu_kernel_api.hpp"
extern "C" __global__
__attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_decode_v1(const unsigned char* input, unsigned int input_size,
    const flitzfs::gpu_detail::Task* tasks, unsigned int task_count,
    unsigned char* output, unsigned int output_size, unsigned int* status,
    unsigned int version) {
  const auto index=blockIdx.x*64U+threadIdx.x;
  if(version!=flitzfs::gpu_detail::abi_version || task_count>flitzfs::gpu_detail::max_tasks ||
      index>=task_count) return;
  status[index]=flitzfs::gpu_detail::decode_task(tasks[index],input,input_size,output,output_size);
}

// Taking the typed address inside the HIP translation unit preserves the SDK's
// registered host stub. No hand-built HIP symbol or registration ABI is used.
namespace flitzfs::gpu_detail {
void bind_decode_kernel(KernelApi& api) noexcept {
  api.decode=flitzfs_decode_v1;
}
}

#include "flitzfs/gpu_match_kernel_api.hpp"
extern "C" __global__
__attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_match_v1(const unsigned char* input,unsigned int size,
    const flitzfs::match_detail::u16* previous,unsigned int previous_count,
    flitzfs::match_detail::Match* output,unsigned int output_count,unsigned int version) {
  using namespace flitzfs::match_detail;
  const auto lane=blockIdx.x*64U+threadIdx.x;
  if(version!=abi_version || size>input_limit || previous_count!=size ||
      output_count!=lanes(size) || lane>=output_count || !output)return;
  output[lane]=search(input,size,previous,previous_count,lane);
}
namespace flitzfs::match_detail {
void bind_match_kernel(KernelApi& api) noexcept {api.search=flitzfs_match_v1;}
}

#include "flitzfs/gpu_anchor_kernel_api.hpp"
extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_index_v1(const unsigned char* base,flitzfs::anchor_match::Bucket* table,
    unsigned int version) {
  using namespace flitzfs::anchor_match;
  const auto at=blockIdx.x*threads+threadIdx.x;
  u32 slot=0;
  if (version!=abi_version || !base || !table || !index_position(base,at,slot)) return;
  auto& entry=table[slot];
  const auto old=atomicMin(&entry.first,at);
  // Every position displaced from, or refused by, the first slot competes
  // for the second. The two lowest positions survive in any execution order.
  if (old!=empty) atomicMin(&entry.second,old>at?old:at);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_search_v1(const unsigned char* source,unsigned int members,
    const flitzfs::anchor_match::Bucket* table,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  const auto lane=blockIdx.x*threads+threadIdx.x;
  if (version!=abi_version || !members || members>max_members || lane>=members*tiles ||
      !source || !table || !output) return;
  const auto* member=source+(1+lane/tiles)*extent_bytes;
  output[lane]=search(source,member,table,lane%tiles);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_search_window_v2(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket* table,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  const auto lane=blockIdx.x*threads+threadIdx.x;
  if (version!=window_abi_version || !members || members>max_members || lane>=members*tiles ||
      !base || !window || !table || !output) return;
  const auto* member=window+(lane/tiles)*extent_bytes;
  output[lane]=search(base,member,table,lane%tiles);
}

namespace {
__device__ flitzfs::anchor_match::u64 xor_segment(const unsigned char* base,const unsigned char* member) {
  flitzfs::anchor_match::u64 left=0,right=0;
  __builtin_memcpy(&left,base,sizeof(left));
  __builtin_memcpy(&right,member,sizeof(right));
  return left^right;
}

template<unsigned abi,unsigned slot_bytes,unsigned member_limit=flitzfs::anchor_match::max_members>
__device__ __forceinline__
void pack_xor_members(const unsigned char* base,const unsigned char* window,unsigned int members,
    unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  if(version!=abi || !base || !window || !output ||
      !members || members>member_limit || blockIdx.x>=members || blockDim.x!=threads)return;
  constexpr unsigned lanes_per_tile=xor_plane_bytes;
  constexpr unsigned tiles_per_block=threads/lanes_per_tile;
  static_assert(lanes_per_tile==32 && xor_tiles%tiles_per_block==0);
  __shared__ unsigned masks[xor_tiles],offsets[xor_tiles],body_size;
  const auto lane=threadIdx.x;
  const auto segment=lane%lanes_per_tile;
  const auto first_tile=lane/lanes_per_tile;
  const auto* member=window+blockIdx.x*extent_bytes;
  auto* slot=output+blockIdx.x*(slot_bytes/sizeof(u32));
  for(unsigned tile=first_tile;tile<xor_tiles;tile+=tiles_per_block) {
    const auto at=tile*xor_tile_bytes+segment*8;
    auto difference=xor_segment(base+at,member+at);
    difference|=difference>>32;
    difference|=difference>>16;
    difference|=difference>>8;
    auto mask=static_cast<unsigned>(difference)&255U;
    for(unsigned distance=lanes_per_tile/2;distance;distance/=2)
      mask|=__shfl_down(mask,distance,lanes_per_tile);
    if(!segment)masks[tile]=mask;
  }
  __syncthreads();
  if(!lane) {
    unsigned size=0;
    for(unsigned tile=0;tile<xor_tiles;++tile) {
      offsets[tile]=size;
      size+=1+static_cast<unsigned>(__popc(masks[tile]))*xor_plane_bytes;
    }
    body_size=size>xor_tiles && size<=slot_bytes-sizeof(u32)?size:0;
    slot[0]=body_size;
  }
  __syncthreads();
  if(!body_size)return;
  auto* body=reinterpret_cast<unsigned char*>(slot+1);
  for(unsigned tile=first_tile;tile<xor_tiles;tile+=tiles_per_block) {
    const auto mask=masks[tile];
    auto* tile_body=body+offsets[tile];
    if(!segment)*tile_body=static_cast<unsigned char>(mask);
    if(!mask)continue;
    const auto at=tile*xor_tile_bytes+segment*8;
    const auto difference=xor_segment(base+at,member+at);
    unsigned plane_offset=1+segment;
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      unsigned packed=0;
      for(unsigned i=0;i<8;++i)
        packed|=static_cast<unsigned>(((difference>>(8*i+bit))&1ULL)<<i);
      tile_body[plane_offset]=static_cast<unsigned char>(packed);
      plane_offset+=xor_plane_bytes;
    }
  }
}

}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_packed_xor_v3(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket*,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  pack_xor_members<packed_xor_window_abi_version,xor_slot_bytes>(base,window,members,output,version);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_packed_xor_v4(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket*,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  pack_xor_members<wide_xor_window_abi_version,xor_wide_slot_bytes>(base,window,members,output,version);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_packed_xor_v5(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket*,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  pack_xor_members<adaptive_xor_window_abi_version,xor_wide_slot_bytes,
      dispatch_member_limit(BatchOutput::wide_packed_xor)>(base,window,members,output,version);
}

namespace flitzfs::anchor_match {
void bind_anchor_kernels(KernelApi& api) noexcept {
  api.index=flitzfs_anchor_index_v1;
  api.search=flitzfs_anchor_search_v1;
}
void bind_anchor_window_search(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_search_window_v2;
}
void bind_anchor_packed_xor_window(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_packed_xor_v3;
}
void bind_anchor_wide_xor_window(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_packed_xor_v4;
}
void bind_anchor_adaptive_xor_window(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_packed_xor_v5;
}
}
