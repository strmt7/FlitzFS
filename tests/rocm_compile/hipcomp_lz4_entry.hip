// Experimental offline-only byte compressor. No caller, loader or product link.
#include <hip/hip_runtime.h>
#include "LZ4Kernels.cuh"
#include "hipcomp_lz4_api.hpp"
#include <cstddef>
#include <cstdint>
static_assert(sizeof(std::size_t)==8 && sizeof(void*)==8);
static_assert(sizeof(hipcomp::offset_type)==2 && sizeof(hipcomp::position_type)==4);
namespace {
template<unsigned int Wave>
__device__ void compress_checked(unsigned char* slab,unsigned int allocated,
    flitzfs::candidate_lz4::Layout p) {
  using namespace flitzfs::candidate_lz4;
  // All conditions are uniform within the one-wave block; no lane leaves a
  // cooperating compression call. Geometry/layout reject before any slab access.
  if(!valid_geometry(Wave,static_cast<u32>(warpSize),blockDim.x,blockDim.y,blockDim.z,
        gridDim.x,gridDim.y,gridDim.z,p.count) || !valid_layout(p,allocated) ||
      blockIdx.x>=p.count || blockIdx.y || blockIdx.z || !slab ||
      (reinterpret_cast<std::uintptr_t>(slab)&7U))return;
  const auto index=static_cast<u32>(blockIdx.x);
  const auto& chunk=p.chunks[index];
  auto* result=reinterpret_cast<std::size_t*>(slab+p.output_sizes)+index;
  if(threadIdx.x==0U)*result=0;
  __syncwarp();
  auto* hash=reinterpret_cast<hipcomp::offset_type*>(slab+p.scratch_offset)+
      index*p.hash_table_entries;
  hipcomp::compressStream<Wave,unsigned char>(slab+chunk.output_offset,
      slab+chunk.input_offset,hash,p.hash_table_entries,chunk.input_bytes,result);
}
}
extern "C" __global__
void flitzfs_candidate_lz4_wave32(unsigned char* slab,unsigned int allocated,
    flitzfs::candidate_lz4::Layout layout) {compress_checked<32>(slab,allocated,layout);}
extern "C" __global__
void flitzfs_candidate_lz4_wave64(unsigned char* slab,unsigned int allocated,
    flitzfs::candidate_lz4::Layout layout) {compress_checked<64>(slab,allocated,layout);}

namespace flitzfs::candidate_lz4 {
// Bind in the real HIP translation unit so the addresses remain SDK-registered
// host stubs. No manual registration ABI or raw device-symbol lookup.
void bind_compression_kernels(KernelApi& api) noexcept {
  api.wave32=flitzfs_candidate_lz4_wave32;
  api.wave64=flitzfs_candidate_lz4_wave64;
}
}

#include "hipcomp_uniform_lz4_api.hpp"
namespace {
template<unsigned int Wave>
__device__ void compress_uniform_checked(unsigned char* slab,flitzfs::uniform_lz4::u64 allocated,
    flitzfs::uniform_lz4::Layout p) {
  using namespace flitzfs::uniform_lz4;
  if(!valid_geometry(Wave,static_cast<u32>(warpSize),blockDim.x,blockDim.y,blockDim.z,
        gridDim.x,gridDim.y,gridDim.z,p.chunks) || !valid_layout(p,allocated) ||
      !slab || (reinterpret_cast<std::uintptr_t>(slab)&7U))return;
  Chunk part{};if(!chunk(p,static_cast<u32>(blockIdx.x),part))return;
  auto* result=reinterpret_cast<std::size_t*>(slab+part.size_slot);
  if(threadIdx.x==0U)*result=0;
  __syncwarp();
  auto* hash=reinterpret_cast<hipcomp::offset_type*>(slab+part.scratch);
  hipcomp::compressStream<Wave,unsigned char>(slab+part.output,slab+part.input,
      hash,p.hash_entries,part.input_bytes,result);
}
}
extern "C" __global__
void flitzfs_uniform_lz4_wave32(unsigned char* slab,flitzfs::uniform_lz4::u64 allocated,
    flitzfs::uniform_lz4::Layout p){compress_uniform_checked<32>(slab,allocated,p);}
extern "C" __global__
void flitzfs_uniform_lz4_wave64(unsigned char* slab,flitzfs::uniform_lz4::u64 allocated,
    flitzfs::uniform_lz4::Layout p){compress_uniform_checked<64>(slab,allocated,p);}
namespace flitzfs::uniform_lz4 {
void bind_compression_kernels(KernelApi& api) noexcept {
  api.wave32=flitzfs_uniform_lz4_wave32;api.wave64=flitzfs_uniform_lz4_wave64;
}
}
