#include <hip/hip_runtime.h>
#include "LZ4Kernels.cuh"
#include "native_lz4_decode_api.hpp"
#include "native_group_decode.hpp"

namespace {
using namespace flitzfs::native_lz4_decode;

template<u32 Wave> __device__ void decode_checked(u8* slab,u64 allocation,Layout layout) {
  if(!slab || (reinterpret_cast<std::uintptr_t>(slab)&(alignment-1)) ||
      warpSize!=Wave || blockDim.x!=Wave || blockDim.y!=1 || blockDim.z!=1 ||
      gridDim.x!=layout.chunks || gridDim.y!=1 || gridDim.z!=1 ||
      blockIdx.x>=layout.chunks || !valid_layout(layout,allocation))return;

  const auto index=static_cast<u32>(blockIdx.x);
  auto& state=reinterpret_cast<Record*>(slab)[index];
  const auto current=state;
  const auto* input=slab+input_at(layout,index);
  __shared__ bool valid;
  if(threadIdx.x==0)
    valid=valid_record(layout,current) && valid_block(input,current.input_bytes,current.output_bytes);
  __syncthreads();
  if(!valid)return;

  __shared__ __align__(16) u8 workspace[hipcomp::decomp_input_buffer_size(Wave)];
  __shared__ std::size_t actual;
  __shared__ hipcompStatus_t status;
  hipcomp::decompressStream<Wave>(workspace,slab+output_at(layout,index),input,
      current.input_bytes,current.output_bytes,&actual,&status,true);
  __syncthreads();
  if(threadIdx.x==0 && status==hipcompSuccess && actual==current.output_bytes) {
    state.actual_bytes=static_cast<u32>(actual);
    state.status=0;
  }
}
}

extern "C" __global__ void flitzfs_native_lz4_decode_wave32(
    flitzfs::native_lz4_decode::u8* slab,flitzfs::native_lz4_decode::u64 bytes,
    flitzfs::native_lz4_decode::Layout layout) {
  decode_checked<32>(slab,bytes,layout);
}
extern "C" __global__ void flitzfs_native_lz4_decode_wave64(
    flitzfs::native_lz4_decode::u8* slab,flitzfs::native_lz4_decode::u64 bytes,
    flitzfs::native_lz4_decode::Layout layout) {
  decode_checked<64>(slab,bytes,layout);
}

namespace flitzfs::native_lz4_decode {
void bind_decode_kernels(KernelApi& api) noexcept {
  api={sizeof(KernelApi),version,sizeof(Layout),sizeof(Record),
      flitzfs_native_lz4_decode_wave32,flitzfs_native_lz4_decode_wave64};
}
}

extern "C" __global__ void flitzfs_native_group_decode(flitzfs::group_decode::u8* slab,
    flitzfs::group_decode::u64 bytes,flitzfs::group_decode::Layout layout) {
  using namespace flitzfs::group_decode;
  if(!slab || (reinterpret_cast<std::uintptr_t>(slab)&7) || !valid(layout) || bytes!=slab_bytes(layout) ||
      blockDim.x!=warpSize || blockDim.y!=1 || blockDim.z!=1 ||
      gridDim.x!=layout.members*(words/blockDim.x) || gridDim.y!=1 || gridDim.z!=1)return;
  const auto index=blockIdx.x*blockDim.x+threadIdx.x;
  const auto word=index%words,member=index/words;
  const auto& descriptor=reinterpret_cast<const Member*>(slab)[member];
  if(reconstruct(descriptor,slab+bases_at(layout),slab+bodies_at(layout),layout,word,
      slab+output_at(layout)+member*extent_bytes) && word%words_per_tile==0)
    slab[status_at(layout)+member*tiles+word/words_per_tile]=complete;
}

namespace flitzfs::group_decode {
void bind_group_decode_kernel(KernelApi& api,u32 abi) noexcept {
  api={sizeof(KernelApi),abi,sizeof(Layout),sizeof(Member),flitzfs_native_group_decode};
}
}
