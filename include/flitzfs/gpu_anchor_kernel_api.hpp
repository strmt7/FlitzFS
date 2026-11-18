#pragma once
#include "flitzfs/gpu_anchor_match.hpp"
#include <type_traits>

namespace flitzfs::anchor_match {
using IndexKernel=void (*)(const u8*,Bucket*,u32);
using SearchKernel=void (*)(const u8*,u32,const Bucket*,u32*,u32);
struct KernelApi {
  u32 size_bytes,version,extent,member_limit,index_size,threads_per_block;
  IndexKernel index;
  SearchKernel search;
};
static_assert(std::is_standard_layout_v<KernelApi> && std::is_trivially_copyable_v<KernelApi>);

inline bool compatible_kernel_api(const KernelApi& api) noexcept {
  return api.size_bytes==sizeof(KernelApi) && api.version==abi_version && api.extent==extent_bytes &&
      api.member_limit==max_members && api.index_size==index_bytes && api.threads_per_block==threads &&
      api.index && api.search;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
