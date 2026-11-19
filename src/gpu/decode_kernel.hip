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

namespace {
using namespace flitzfs::anchor_match;

template<class Word> __device__ __forceinline__
Word modular_word(const u8* bytes) {
  Word value{};
  __builtin_memcpy(&value,bytes,sizeof(value));
  return value;
}
template<class Word> __device__ __forceinline__
Word modular_bias(const u8* base,const u8* member) {
  return static_cast<Word>(u64{modular_word<Word>(member)}-modular_word<Word>(base));
}
template<class Word> __device__ __forceinline__
Word modular_residual(Word difference,Word bias) {
  const auto value=u64{static_cast<Word>(u64{difference}-bias)};
  return static_cast<Word>((value<<1)^(0ULL-(value>>(8*sizeof(Word)-1))));
}
__device__ __forceinline__ u8 modular_width(u64 value) {
  return value?static_cast<u8>(64-__builtin_clzll(value)):0;
}
template<class Word> __device__ __forceinline__
void score_modular_tile(ModularDeltaScratch& scratch,u32 mode,u32 tile,const u8* base,const u8* member) {
  const auto bias=modular_bias<Word>(base,member);
  Word centered=0,unbiased=0;
  const auto start=tile*xor_tile_bytes;
  for(u32 at=start;at<start+xor_tile_bytes;at+=sizeof(Word)) {
    const auto difference=modular_bias<Word>(base+at,member+at);
    centered|=modular_residual<Word>(difference,bias);
    unbiased|=modular_residual<Word>(difference,0);
  }
  scratch.widths[mode][tile]=modular_width(centered);
  scratch.widths[mode+1][tile]=modular_width(unbiased);
}
__device__ __forceinline__ u32 modular_word_bytes(u32 mode) {
  return 1U<<(mode/2);
}
__device__ __forceinline__
u64 selected_modular_bias(u32 mode,const u8* base,const u8* member) {
  if(mode%2)return 0;
  switch(modular_word_bytes(mode)) {
    case 1:return modular_bias<u8>(base,member);
    case 2:return modular_bias<unsigned short>(base,member);
    case 4:return modular_bias<u32>(base,member);
    default:return modular_bias<u64>(base,member);
  }
}
template<class Word> __device__ __forceinline__
void pack_modular_tile(u8* output,const u8* base,const u8* member,u32 bits,u64 bias) {
  constexpr u32 plane_bytes=xor_tile_bytes/(8*sizeof(Word));
  output[0]=static_cast<u8>(bits);
  if(!bits)return;
  for(u32 i=0;i<plane_bytes;++i) {
    Word values[8];
    for(u32 j=0;j<8;++j) {
      const auto at=(i*8+j)*sizeof(Word);
      values[j]=modular_residual<Word>(modular_bias<Word>(base+at,member+at),static_cast<Word>(bias));
    }
    for(u32 bit=0;bit<bits;++bit) {
      u32 packed=0;
      for(u32 j=0;j<8;++j)packed|=static_cast<u32>((values[j]>>bit)&1U)<<j;
      output[1+bit*plane_bytes+i]=static_cast<u8>(packed);
    }
  }
}
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_modular_delta_v6(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket*,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  if(version!=modular_delta_window_abi_version || !base || !window || !output ||
      !members || members>dispatch_member_limit(BatchOutput::modular_delta) ||
      blockIdx.x>=members || blockDim.x!=threads)return;
  static_assert(sizeof(unsigned short)==2);
  __shared__ ModularDeltaScratch scratch;
  const auto lane=threadIdx.x;
  const auto* member=window+blockIdx.x*extent_bytes;
  auto* slot=output+blockIdx.x*(xor_wide_slot_bytes/sizeof(u32));
  for(u32 tile=lane;tile<xor_tiles;tile+=threads) {
    score_modular_tile<u8>(scratch,0,tile,base,member);
    score_modular_tile<unsigned short>(scratch,2,tile,base,member);
    score_modular_tile<u32>(scratch,4,tile,base,member);
    score_modular_tile<u64>(scratch,6,tile,base,member);
  }
  __syncthreads();
  if(lane<delta_modes) {
    u32 planes=0;
    for(u32 tile=0;tile<xor_tiles;++tile)planes+=scratch.widths[lane][tile];
    const auto size=planes?delta_parameters_bytes+xor_tiles+planes*(xor_plane_bytes/modular_word_bytes(lane)):
        selected_modular_bias(lane,base,member)?delta_parameters_bytes:0U;
    scratch.sizes[lane]=size<=delta_max_payload_bytes?size:0;
  }
  __syncthreads();
  if(!lane) {
    scratch.mode=delta_modes;
    slot[0]=0;
    for(u32 mode=0;mode<delta_modes;++mode) {
      const auto size=scratch.sizes[mode];
      if(size && (!slot[0] || size<slot[0])) {
        slot[0]=size;
        scratch.mode=mode;
      }
    }
    if(scratch.mode<delta_modes) {
      const auto width=modular_word_bytes(scratch.mode);
      const auto bias=selected_modular_bias(scratch.mode,base,member);
      slot[1]=width;
      slot[2]=static_cast<u32>(bias);
      slot[3]=static_cast<u32>(bias>>32);
      if(slot[0]>delta_parameters_bytes) {
        u32 at=0;
        for(u32 tile=0;tile<xor_tiles;++tile) {
          scratch.offsets[tile]=at;
          at+=1+scratch.widths[scratch.mode][tile]*(xor_plane_bytes/width);
        }
      }
    }
  }
  __syncthreads();
  const auto mode=scratch.mode;
  if(mode==delta_modes || scratch.sizes[mode]==delta_parameters_bytes)return;
  const auto bias=selected_modular_bias(mode,base,member);
  auto* body=reinterpret_cast<u8*>(slot+1)+delta_parameters_bytes;
  for(u32 tile=lane;tile<xor_tiles;tile+=threads) {
    const auto start=tile*xor_tile_bytes;
    auto* target=body+scratch.offsets[tile];
    const auto bits=scratch.widths[mode][tile];
    switch(modular_word_bytes(mode)) {
      case 1:pack_modular_tile<u8>(target,base+start,member+start,bits,bias);break;
      case 2:pack_modular_tile<unsigned short>(target,base+start,member+start,bits,bias);break;
      case 4:pack_modular_tile<u32>(target,base+start,member+start,bits,bias);break;
      default:pack_modular_tile<u64>(target,base+start,member+start,bits,bias);break;
    }
  }
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
void bind_anchor_modular_delta_window(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_modular_delta_v6;
}
}
