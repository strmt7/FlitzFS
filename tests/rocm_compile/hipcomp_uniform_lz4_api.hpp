#pragma once
#include "gpu_lz4_uniform.hpp"
#include <type_traits>
namespace flitzfs::uniform_lz4 {
using CompressKernel=void (*)(unsigned char*,u64,Layout);
struct KernelApi {
  u32 size_bytes,layout_bytes,version,chunk_bytes;
  CompressKernel wave32,wave64;
};
static_assert(sizeof(KernelApi)==32 && std::is_standard_layout_v<KernelApi> &&
    std::is_trivially_copyable_v<KernelApi>);
inline KernelApi make_kernel_api(CompressKernel a,CompressKernel b) noexcept {
  return {sizeof(KernelApi),sizeof(Layout),2,max_chunk_bytes,a,b};
}
inline bool compatible_kernel_api(const KernelApi& p) noexcept {
  return p.size_bytes==sizeof(KernelApi) && p.layout_bytes==sizeof(Layout) &&
      p.version==2 && p.chunk_bytes==max_chunk_bytes && p.wave32 && p.wave64;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
