#pragma once
#include "hipcomp_lz4_contract.hpp"
#include <type_traits>
namespace flitzfs::candidate_lz4 {
// Private module interface only. No pointer is serialized or independently
// trusted; callers must authenticate and retain the module before querying it.
using CompressKernel=void (*)(unsigned char*,u32,Layout);
struct KernelApi {
  u32 size_bytes,layout_bytes,allocation_bytes,chunk_bytes,chunks;
  u32 table_entry_bytes,output_guard_bytes,reserved;
  CompressKernel wave32,wave64;
};
static_assert(std::is_standard_layout_v<KernelApi> && std::is_trivially_copyable_v<KernelApi>);
static_assert(sizeof(void*)==8 && sizeof(KernelApi)==48);
inline KernelApi make_kernel_api(CompressKernel a,CompressKernel b) noexcept {
  return {static_cast<u32>(sizeof(KernelApi)),static_cast<u32>(sizeof(Layout)),
      allocation_limit,chunk_limit,chunk_count,8,32,0,a,b};
}
inline bool compatible_kernel_api(const KernelApi& p) noexcept {
  return p.size_bytes==sizeof(KernelApi) && p.layout_bytes==sizeof(Layout) &&
      p.allocation_bytes==allocation_limit && p.chunk_bytes==chunk_limit &&
      p.chunks==chunk_count && p.table_entry_bytes==8 && p.output_guard_bytes==32 &&
      !p.reserved && p.wave32 && p.wave64;
}
// This is selection from authenticated metadata, not runtime/device discovery.
// Validation here grants no launch or resource authority.
inline CompressKernel select_kernel(const KernelApi& p,u32 reported_wave) noexcept {
  if(!compatible_kernel_api(p))return nullptr;
  if(reported_wave==32)return p.wave32;
  if(reported_wave==64)return p.wave64;
  return nullptr;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
} // namespace flitzfs::candidate_lz4
