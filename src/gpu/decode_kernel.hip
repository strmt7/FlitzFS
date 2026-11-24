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

namespace {
template<unsigned abi,unsigned member_limit>
__device__ __forceinline__
void search_anchor_window(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket* table,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  const auto lane=blockIdx.x*threads+threadIdx.x;
  if(version!=abi || !members || members>member_limit || lane>=members*tiles ||
      !base || !window || !table || !output) return;
  const auto* member=window+(lane/tiles)*extent_bytes;
  output[lane]=search(base,member,table,lane%tiles);
}
}
extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_search_window_v2(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket* table,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  search_anchor_window<window_abi_version,max_members>(base,window,members,table,output,version);
}
extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_search_window_v11(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket* table,unsigned int* output,unsigned int version) {
  using namespace flitzfs::anchor_match;
  search_anchor_window<wide_anchor_window_abi_version,wide_anchor_members>(base,window,members,table,output,version);
}

namespace {
__device__ flitzfs::anchor_match::u64 xor_segment(const unsigned char* base,const unsigned char* member) {
  flitzfs::anchor_match::u64 left=0,right=0;
  __builtin_memcpy(&left,base,sizeof(left));
  __builtin_memcpy(&right,member,sizeof(right));
  return left^right;
}

template<class Mask>
__device__ __forceinline__ flitzfs::anchor_match::u32 packed_xor_offsets(const Mask* masks,
    flitzfs::anchor_match::u32* offsets) {
  using namespace flitzfs::anchor_match;
  u32 size=0;
  for(u32 tile=0;tile<xor_tiles;++tile) {
    if(offsets)offsets[tile]=size;
    size+=1+static_cast<u32>(__popc(masks[tile]))*xor_plane_bytes;
  }
  return size;
}

template<class Mask>
__device__ __forceinline__ void write_packed_xor(const Mask* masks,const flitzfs::anchor_match::u32* offsets,
    const unsigned char* base,const unsigned char* member,unsigned char* body) {
  using namespace flitzfs::anchor_match;
  constexpr auto lanes=xor_plane_bytes;
  constexpr auto tiles_per_block=threads/lanes;
  const auto segment=threadIdx.x%lanes;
  const auto first_tile=threadIdx.x/lanes;
  for(u32 tile=first_tile;tile<xor_tiles;tile+=tiles_per_block) {
    const auto mask=static_cast<u32>(masks[tile]);
    auto* tile_body=body+offsets[tile];
    if(!segment)*tile_body=static_cast<u8>(mask);
    if(!mask)continue;
    const auto at=tile*xor_tile_bytes+segment*8;
    const auto difference=xor_segment(base+at,member+at);
    auto plane_offset=1+segment;
    for(u32 bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      tile_body[plane_offset]=static_cast<u8>(xor_plane_bits(difference,bit));
      plane_offset+=xor_plane_bytes;
    }
  }
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
    const auto size=packed_xor_offsets(masks,offsets);
    body_size=size>xor_tiles && size<=slot_bytes-sizeof(u32)?size:0;
    slot[0]=body_size;
  }
  __syncthreads();
  if(body_size)write_packed_xor(masks,offsets,base,member,reinterpret_cast<u8*>(slot+1));
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

__device__ __forceinline__ void size_sparse_xor(SparseXorScratch& scratch,const u8* base,const u8* member) {
  constexpr auto lanes=xor_plane_bytes;
  constexpr auto tiles_per_block=threads/lanes;
  static_assert(lanes==32 && xor_tiles%tiles_per_block==0);
  const auto lane=threadIdx.x;
  const auto segment=lane%lanes;
  const auto first_tile=lane/lanes;
  for(u32 tile=first_tile;tile<xor_tiles;tile+=tiles_per_block) {
    const auto at=tile*xor_tile_bytes+segment*8;
    const auto difference=xor_segment(base+at,member+at);
    u32 mask=0,sparse=0,payload=0;
    for(u32 bit=0;bit<8;++bit) {
      auto ones=static_cast<u32>(__popc(xor_plane_bits(difference,bit)));
      for(u32 step=lanes/2;step;step/=2)ones+=__shfl_down(ones,step,lanes);
      if(segment)continue;
      const auto descriptor=sparse_xor_descriptor(ones);
      scratch.descriptors[tile][bit]=static_cast<u8>(descriptor);
      if(!descriptor)continue;
      mask|=1U<<bit;
      if(descriptor==sparse_plane_dense)payload+=xor_plane_bytes;
      else {
        sparse|=1U<<bit;
        payload+=1+(descriptor&~sparse_plane_inverse);
      }
    }
    if(!segment) {
      scratch.masks[tile]=static_cast<u8>(mask);
      scratch.sparse_masks[tile]=static_cast<u8>(sparse);
      scratch.offsets[tile]=1+(mask!=0)+payload;
    }
  }
  __syncthreads();
  if(!lane) {
    u32 size=0;
    for(u32 tile=0;tile<xor_tiles;++tile) {
      const auto bytes=scratch.offsets[tile];
      scratch.offsets[tile]=size;
      size+=bytes;
    }
    scratch.size=size>=sparse_xor_min_body_bytes && size<=sparse_xor_max_body_bytes?size:0;
  }
  __syncthreads();
}

__device__ __forceinline__ bool reserve_group_body(u32 size,u32* cursor,u32* directory,
    u32 arena_words,u32& offset) {
  const auto words=sparse_xor_arena_words(size);
  offset=words?atomicAdd(cursor,words):0U;
  // Leave a poisoned entry if a reservation escapes the admitted arena.
  if(offset>arena_words || words>arena_words-offset)return false;
  auto* entry=directory+blockIdx.x*sparse_xor_arena_entry_words;
  entry[0]=offset;
  entry[1]=size;
  return true;
}

__device__ __forceinline__ void write_sparse_xor(const SparseXorScratch& scratch,const u8* base,
    const u8* member,u8* body) {
  constexpr auto lanes=xor_plane_bytes;
  constexpr auto tiles_per_block=threads/lanes;
  const auto segment=threadIdx.x%lanes;
  const auto first_tile=threadIdx.x/lanes;
  for(u32 tile=first_tile;tile<xor_tiles;tile+=tiles_per_block) {
    auto* target=body+scratch.offsets[tile];
    const auto mask=scratch.masks[tile];
    if(!segment)*target=mask;
    if(!mask)continue;
    if(!segment)target[1]=scratch.sparse_masks[tile];
    target+=2;
    const auto at=tile*xor_tile_bytes+segment*8;
    const auto difference=xor_segment(base+at,member+at);
    for(u32 bit=0;bit<8;++bit) {
      const auto descriptor=static_cast<u32>(scratch.descriptors[tile][bit]);
      if(!descriptor)continue;
      auto packed=xor_plane_bits(difference,bit);
      if(descriptor==sparse_plane_dense) {
        target[segment]=static_cast<u8>(packed);
        target+=xor_plane_bytes;
        continue;
      }
      if(!segment)*target=static_cast<u8>(descriptor);
      const auto count=descriptor&~sparse_plane_inverse;
      if(count) {
        if(descriptor&sparse_plane_inverse)packed^=255U;
        const auto population=static_cast<u32>(__popc(packed));
        auto last=population;
        for(u32 step=1;step<lanes;step*=2) {
          const auto earlier=__shfl_up(last,step,lanes);
          if(segment>=step)last+=earlier;
        }
        // Prefix counts give each segment its disjoint part of the list.
        if(last<=count) {
          auto next=last-population+1;
          while(packed) {
            const auto bit_index=static_cast<u32>(__ffs(packed)-1);
            target[next++]=static_cast<u8>(segment*8+bit_index);
            packed&=packed-1;
          }
        }
      }
      target+=1+count;
    }
  }
}

template<bool Arena> __device__ __forceinline__
void pack_sparse_xor_members(const u8* base,const u8* window,u32 members,u32* output,u32 version,
    u32* cursor=nullptr,u32* directory=nullptr,u32 arena_words=0) {
  constexpr auto abi=Arena?sparse_xor_arena_abi_version:sparse_xor_window_abi_version;
  if(version!=abi || !base || !window || !output ||
      !members || members>dispatch_member_limit(BatchOutput::sparse_xor) ||
      blockIdx.x>=members || blockDim.x!=threads)return;
  if constexpr(Arena)if(!cursor || !directory || !arena_words)return;
  __shared__ SparseXorScratch scratch;
  const auto* member=window+blockIdx.x*extent_bytes;
  if constexpr(!Arena)output+=blockIdx.x*(xor_wide_slot_bytes/sizeof(u32));
  size_sparse_xor(scratch,base,member);
  if(!threadIdx.x) {
    if constexpr(Arena) {
      if(!reserve_group_body(scratch.size,cursor,directory,arena_words,scratch.body_offset))scratch.size=0;
    } else {
      output[0]=scratch.size;
      scratch.body_offset=1;
    }
  }
  __syncthreads();
  if(scratch.size)write_sparse_xor(scratch,base,member,reinterpret_cast<u8*>(output+scratch.body_offset));
}
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_sparse_xor_v7(const unsigned char* base,const unsigned char* window,unsigned int members,
    const flitzfs::anchor_match::Bucket*,unsigned int* output,unsigned int version) {
  pack_sparse_xor_members<false>(base,window,members,output,version);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_sparse_xor_arena_v8(const unsigned char* base,const unsigned char* window,unsigned int members,
    unsigned int* cursor,unsigned int* directory,unsigned int* arena,unsigned int arena_words,unsigned int version) {
  pack_sparse_xor_members<true>(base,window,members,arena,version,cursor,directory,arena_words);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_reference_price_v10(const unsigned char* base,const unsigned char* peers,
    unsigned int count,unsigned int* prices,unsigned int version) {
  using namespace flitzfs::anchor_match;
  if(version!=reference_price_abi_version || !base || !peers || !prices ||
      !count || count>dispatch_member_limit(BatchOutput::reference_xor) ||
      blockIdx.x>=count || blockDim.x!=threads)return;
  __shared__ SparseXorScratch scratch;
  size_sparse_xor(scratch,base,peers+blockIdx.x*extent_bytes);
  if(!threadIdx.x) {
    constexpr u32 header=extent_bytes-sparse_xor_max_body_bytes-1;
    u32 dense=xor_tiles;
    for(u32 tile=0;tile<xor_tiles;++tile)
      dense+=static_cast<u32>(__popc(scratch.masks[tile]))*xor_plane_bytes;
    const auto chosen=scratch.size && scratch.size<dense?scratch.size:dense;
    prices[blockIdx.x]=dense==xor_tiles?header:(chosen<extent_bytes-header?header+chosen:extent_bytes);
  }
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
void select_modular_biases(ModularDeltaScratch& scratch,u32 width_index,const u8* base,const u8* member) {
  Word sample[delta_bias_samples],biases[delta_bias_choices];
  for(u32 i=0;i<delta_bias_samples;++i) {
    const auto offset=modular_bias_sample_offset(i);
    sample[i]=modular_bias<Word>(base+offset,member+offset);
  }
  const auto count=modular_bias_candidates(modular_bias<Word>(base,member),sample,biases);
  scratch.bias_counts[width_index]=count;
  for(u32 i=0;i<count;++i)scratch.biases[width_index*delta_bias_choices+i]=biases[i];
}
template<class Word,u32 WidthIndex> __device__ __forceinline__
void score_modular_tile(ModularDeltaScratch& scratch,u32 tile,const u8* base,const u8* member) {
  constexpr auto first=WidthIndex*delta_bias_choices;
  const auto count=scratch.bias_counts[WidthIndex];
  Word biases[delta_bias_choices]{},combined[delta_bias_choices]{};
  #pragma unroll
  for(u32 i=0;i<delta_bias_choices;++i) {
    if(i<count)biases[i]=static_cast<Word>(scratch.biases[first+i]);
  }
  const auto start=tile*xor_tile_bytes;
  for(u32 at=start;at<start+xor_tile_bytes;at+=sizeof(Word)) {
    const auto difference=modular_bias<Word>(base+at,member+at);
    #pragma unroll
    for(u32 i=0;i<delta_bias_choices;++i) {
      if(i<count)combined[i]|=modular_residual<Word>(difference,biases[i]);
    }
  }
  #pragma unroll
  for(u32 i=0;i<delta_bias_choices;++i) {
    if(i<count)scratch.widths[first+i][tile]=modular_width(combined[i]);
  }
}
__device__ __forceinline__ u32 modular_word_bytes(u32 mode) {
  return 1U<<(mode/delta_bias_choices);
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
__device__ __forceinline__ void size_modular_delta(ModularDeltaScratch& scratch,const u8* base,const u8* member) {
  static_assert(sizeof(unsigned short)==2);
  const auto lane=threadIdx.x;
  switch(lane) {
    case 0:select_modular_biases<u8>(scratch,0,base,member);break;
    case 1:select_modular_biases<unsigned short>(scratch,1,base,member);break;
    case 2:select_modular_biases<u32>(scratch,2,base,member);break;
    case 3:select_modular_biases<u64>(scratch,3,base,member);break;
    default:break;
  }
  __syncthreads();
  for(u32 tile=lane;tile<xor_tiles;tile+=threads) {
    score_modular_tile<u8,0>(scratch,tile,base,member);
    score_modular_tile<unsigned short,1>(scratch,tile,base,member);
    score_modular_tile<u32,2>(scratch,tile,base,member);
    score_modular_tile<u64,3>(scratch,tile,base,member);
  }
  __syncthreads();
  if(lane<delta_modes) {
    scratch.sizes[lane]=0;
    if(lane%delta_bias_choices<scratch.bias_counts[lane/delta_bias_choices]) {
      u32 planes=0;
      for(u32 tile=0;tile<xor_tiles;++tile)planes+=scratch.widths[lane][tile];
      const auto size=planes?delta_parameters_bytes+xor_tiles+planes*(xor_plane_bytes/modular_word_bytes(lane)):
          scratch.biases[lane]?delta_parameters_bytes:0U;
      scratch.sizes[lane]=size<=delta_max_payload_bytes?size:0;
    }
  }
  __syncthreads();
  if(!lane) {
    scratch.mode=delta_modes;
    u32 best_size=0;
    for(u32 mode=0;mode<delta_modes;++mode) {
      const auto size=scratch.sizes[mode];
      if(size && (!best_size || size<best_size)) {
        best_size=size;
        scratch.mode=mode;
      }
    }
    if(best_size>delta_parameters_bytes) {
      const auto width=modular_word_bytes(scratch.mode);
      u32 at=0;
      for(u32 tile=0;tile<xor_tiles;++tile) {
        scratch.offsets[tile]=at;
        at+=1+scratch.widths[scratch.mode][tile]*(xor_plane_bytes/width);
      }
    }
  }
  __syncthreads();
}

__device__ __forceinline__ u32 modular_payload_size(const ModularDeltaScratch& scratch) {
  return scratch.mode<delta_modes?scratch.sizes[scratch.mode]:0U;
}

__device__ __forceinline__ void write_modular_delta(const ModularDeltaScratch& scratch,const u8* base,
    const u8* member,u32* output) {
  const auto mode=scratch.mode;
  const auto bias=scratch.biases[mode];
  const auto width=modular_word_bytes(mode);
  if(!threadIdx.x) {
    output[0]=width;
    output[1]=static_cast<u32>(bias);
    output[2]=static_cast<u32>(bias>>32);
  }
  if(scratch.sizes[mode]==delta_parameters_bytes)return;
  auto* body=reinterpret_cast<u8*>(output)+delta_parameters_bytes;
  for(u32 tile=threadIdx.x;tile<xor_tiles;tile+=threads) {
    const auto start=tile*xor_tile_bytes;
    auto* target=body+scratch.offsets[tile];
    const auto bits=scratch.widths[mode][tile];
    switch(width) {
      case 1:pack_modular_tile<u8>(target,base+start,member+start,bits,bias);break;
      case 2:pack_modular_tile<unsigned short>(target,base+start,member+start,bits,bias);break;
      case 4:pack_modular_tile<u32>(target,base+start,member+start,bits,bias);break;
      default:pack_modular_tile<u64>(target,base+start,member+start,bits,bias);break;
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
  __shared__ ModularDeltaScratch scratch;
  const auto* member=window+blockIdx.x*extent_bytes;
  auto* slot=output+blockIdx.x*(xor_wide_slot_bytes/sizeof(u32));
  size_modular_delta(scratch,base,member);
  const auto size=modular_payload_size(scratch);
  if(!threadIdx.x)slot[0]=size;
  if(size)write_modular_delta(scratch,base,member,slot+1);
}

namespace {
template<flitzfs::anchor_match::u32 Version>
__device__ __forceinline__ void pack_collective(const unsigned char* base,const unsigned char* window,
    unsigned int members,unsigned int* cursor,unsigned int* directory,unsigned int* arena,
    unsigned int arena_words,unsigned int version) {
  using namespace flitzfs::anchor_match;
  if(version!=Version || !base || !window || !arena || !cursor || !directory ||
      !arena_words || !members || members>dispatch_member_limit(BatchOutput::collective) ||
      blockIdx.x>=members || blockDim.x!=threads)return;
  __shared__ CollectivePackingScratch scratch;
  const auto* member=window+blockIdx.x*extent_bytes;
  size_modular_delta(scratch.delta,base,member);
  const auto delta_bytes=modular_payload_size(scratch.delta);
  // A bias-only result has no body in the stored frame.
  const bool examine_xor=delta_bytes!=delta_parameters_bytes;
  if(examine_xor)size_sparse_xor(scratch.sparse,base,member);
  if(!threadIdx.x) {
    const auto sparse_bytes=examine_xor?scratch.sparse.size:0;
    u32 packed_bytes=0;
    if constexpr(Version==collective_dense_arena_abi_version) {
      if(examine_xor) {
        const auto size=packed_xor_offsets(scratch.sparse.masks,nullptr);
        if(size>=packed_xor_min_body_bytes && size<=packed_xor_max_body_bytes)packed_bytes=size;
      }
    }
    const auto choice=choose_collective_body(sparse_bytes,delta_bytes,packed_bytes);
    if(choice.method==CollectiveBody::packed_xor)
      (void)packed_xor_offsets(scratch.sparse.masks,scratch.sparse.offsets);
    const auto size=choice.payload_bytes?choice.payload_bytes+collective_body_tag_bytes:0U;
    scratch.method=reserve_group_body(size,cursor,directory,arena_words,scratch.body_offset)?
        choice.method:CollectiveBody::none;
  }
  __syncthreads();
  if(scratch.method==CollectiveBody::none)return;
  auto* target=arena+scratch.body_offset;
  if(!threadIdx.x)*target=static_cast<u32>(scratch.method);
  auto* body=reinterpret_cast<u8*>(target+1);
  switch(scratch.method) {
    case CollectiveBody::packed_xor:write_packed_xor(scratch.sparse.masks,scratch.sparse.offsets,base,member,body);break;
    case CollectiveBody::sparse_xor:write_sparse_xor(scratch.sparse,base,member,body);break;
    case CollectiveBody::modular_delta:write_modular_delta(scratch.delta,base,member,target+1);break;
    case CollectiveBody::none:break;
  }
}
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_collective_arena_v9(const unsigned char* base,const unsigned char* window,unsigned int members,
    unsigned int* cursor,unsigned int* directory,unsigned int* arena,unsigned int arena_words,unsigned int version) {
  pack_collective<flitzfs::anchor_match::collective_arena_abi_version>(
      base,window,members,cursor,directory,arena,arena_words,version);
}

extern "C" __global__ __attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_anchor_collective_arena_v12(const unsigned char* base,const unsigned char* window,unsigned int members,
    unsigned int* cursor,unsigned int* directory,unsigned int* arena,unsigned int arena_words,unsigned int version) {
  pack_collective<flitzfs::anchor_match::collective_dense_arena_abi_version>(
      base,window,members,cursor,directory,arena,arena_words,version);
}

namespace flitzfs::anchor_match {
void bind_anchor_kernels(KernelApi& api) noexcept {
  api.index=flitzfs_anchor_index_v1;
  api.search=flitzfs_anchor_search_v1;
}
void bind_anchor_window_search(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_search_window_v2;
}
void bind_anchor_wide_search(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_search_window_v11;
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
void bind_anchor_sparse_xor_window(WindowSearchApi& api) noexcept {
  api.search=flitzfs_anchor_sparse_xor_v7;
}
void bind_anchor_sparse_xor_arena(AnchorArenaApi& api) noexcept {
  api.search=flitzfs_anchor_sparse_xor_arena_v8;
}
void bind_anchor_collective_arena(AnchorArenaApi& api) noexcept {
  api.search=api.version==collective_dense_arena_abi_version?
      flitzfs_anchor_collective_arena_v12:flitzfs_anchor_collective_arena_v9;
}
void bind_anchor_reference_price(AnchorReferenceApi& api) noexcept {
  api.price=flitzfs_anchor_reference_price_v10;
}
}
