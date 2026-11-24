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

inline constexpr u32 window_abi_version=2,packed_xor_window_abi_version=3,wide_xor_window_abi_version=4;
inline constexpr u32 adaptive_xor_window_abi_version=5,modular_delta_window_abi_version=6;
inline constexpr u32 sparse_xor_window_abi_version=7;
inline constexpr u32 sparse_xor_arena_abi_version=8,collective_arena_abi_version=9,reference_price_abi_version=10;
inline constexpr u32 wide_anchor_window_abi_version=11,collective_dense_arena_abi_version=12;
constexpr u32 window_version(BatchOutput output) noexcept {
  switch(output) {
    case BatchOutput::anchors:return window_abi_version;
    case BatchOutput::wide_anchors:return wide_anchor_window_abi_version;
    case BatchOutput::packed_xor:return packed_xor_window_abi_version;
    case BatchOutput::wide_packed_xor:return adaptive_xor_window_abi_version;
    case BatchOutput::modular_delta:return modular_delta_window_abi_version;
    case BatchOutput::sparse_xor:return sparse_xor_window_abi_version;
    case BatchOutput::collective:return collective_arena_abi_version;
    case BatchOutput::reference_xor:return reference_price_abi_version;
  }
  return 0;
}
using WindowSearchKernel=void (*)(const u8* base,const u8* members,u32 member_count,const Bucket*,u32*,u32);
struct WindowSearchApi {
  u32 size_bytes,version;
  WindowSearchKernel search;
};
static_assert(std::is_standard_layout_v<WindowSearchApi> && std::is_trivially_copyable_v<WindowSearchApi>);
inline bool compatible_window_search_api(const WindowSearchApi& api,u32 version=window_abi_version) noexcept {
  return api.size_bytes==sizeof(WindowSearchApi) && api.version==version && api.search;
}
using WindowSearchApiEntry=const WindowSearchApi* (*)() noexcept;

using AnchorArenaKernel=void (*)(const u8* base,const u8* members,u32 member_count,
    u32* cursor,u32* directory,u32* arena,u32 arena_words,u32 version);
struct AnchorArenaApi {
  u32 size_bytes,version;
  AnchorArenaKernel search;
};
static_assert(std::is_standard_layout_v<AnchorArenaApi> && std::is_trivially_copyable_v<AnchorArenaApi>);
inline bool compatible_anchor_arena_api(const AnchorArenaApi& api,BatchOutput output) noexcept {
  if(!arena_output(output))return false;
  const bool version=output==BatchOutput::collective?
      api.version==collective_arena_abi_version || api.version==collective_dense_arena_abi_version:
      api.version==sparse_xor_arena_abi_version;
  return api.size_bytes==sizeof(AnchorArenaApi) && version && api.search;
}
using AnchorArenaApiEntry=const AnchorArenaApi* (*)() noexcept;
using AnchorPriceKernel=void (*)(const u8* base,const u8* peers,u32 peer_count,u32* prices,u32 version);
struct AnchorReferenceApi {
  u32 size_bytes,version;
  AnchorPriceKernel price;
};
static_assert(std::is_standard_layout_v<AnchorReferenceApi> && std::is_trivially_copyable_v<AnchorReferenceApi>);
inline bool compatible_anchor_reference_api(const AnchorReferenceApi& api) noexcept {
  return api.size_bytes==sizeof(AnchorReferenceApi) && api.version==reference_price_abi_version && api.price;
}
using AnchorReferenceApiEntry=const AnchorReferenceApi* (*)() noexcept;
}
