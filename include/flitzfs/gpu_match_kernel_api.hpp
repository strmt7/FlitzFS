#pragma once
#include "flitzfs/gpu_match_protocol.hpp"
#include <type_traits>
namespace flitzfs::match_detail {
using SearchKernel=void (*)(const u8*,u32,const u16*,u32,Match*,u32,u32);
inline constexpr u32 threads=64;
struct KernelApi {
  u32 size_bytes,version,input_bytes,lane_count,match_bytes,threads_per_block,reserved;
  SearchKernel search;
};
static_assert(std::is_standard_layout_v<KernelApi> && std::is_trivially_copyable_v<KernelApi>);
inline KernelApi make_kernel_api(SearchKernel search) noexcept {
  return {static_cast<u32>(sizeof(KernelApi)),abi_version,input_limit,lane_limit,
      static_cast<u32>(sizeof(Match)),threads,0,search};
}
inline bool compatible_kernel_api(const KernelApi& api) noexcept {
  return api.size_bytes==sizeof(KernelApi) && api.version==abi_version &&
      api.input_bytes==input_limit && api.lane_count==lane_limit &&
      api.match_bytes==sizeof(Match) && api.threads_per_block==threads &&
      !api.reserved && api.search;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
