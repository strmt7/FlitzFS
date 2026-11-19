// Same deferred-registration boundary as the current FlitzZip kernel module.
// Link qualification NEVER loads this module or executes a GPU kernel.
#include "flitzfs/gpu_kernel_api.hpp"
namespace flitzfs::gpu_detail {
void bind_decode_kernel(KernelApi&) noexcept;
}
extern "C" __declspec(dllexport) unsigned flitzfs_rocm_kernel_abi() noexcept {
  return flitzfs::gpu_detail::abi_version;
}
// The returned table is immutable and module-owned. No C++ heap ownership or
// exception crosses this boundary. Runtime/module trust is a caller prerequisite.
extern "C" __declspec(dllexport)
const flitzfs::gpu_detail::KernelApi* flitzfs_rocm_kernel_api_v1() noexcept {
  static const auto api=[]() noexcept {
    auto value=flitzfs::gpu_detail::make_kernel_api(nullptr);
    flitzfs::gpu_detail::bind_decode_kernel(value);
    return value;
  }();
  return &api;
}

#include "flitzfs/gpu_match_kernel_api.hpp"
namespace flitzfs::match_detail {
void bind_match_kernel(KernelApi&) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::match_detail::KernelApi* flitzfs_rocm_match_api_v1() noexcept {
  static const auto api=[]() noexcept {
    auto value=flitzfs::match_detail::make_kernel_api(nullptr);
    flitzfs::match_detail::bind_match_kernel(value);
    return value;
  }();
  return &api;
}

#include "flitzfs/gpu_anchor_kernel_api.hpp"
namespace flitzfs::anchor_match {
void bind_anchor_kernels(KernelApi&) noexcept;
void bind_anchor_window_search(WindowSearchApi&) noexcept;
void bind_anchor_packed_xor_window(WindowSearchApi&) noexcept;
void bind_anchor_wide_xor_window(WindowSearchApi&) noexcept;
void bind_anchor_adaptive_xor_window(WindowSearchApi&) noexcept;
void bind_anchor_modular_delta_window(WindowSearchApi&) noexcept;
void bind_anchor_sparse_xor_window(WindowSearchApi&) noexcept;
void bind_anchor_sparse_xor_arena(AnchorArenaApi&) noexcept;
void bind_anchor_collective_arena(AnchorArenaApi&) noexcept;
void bind_anchor_reference_price(AnchorReferenceApi&) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::anchor_match::KernelApi* flitzfs_rocm_anchor_match_api_v1() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    KernelApi value{sizeof(KernelApi),abi_version,extent_bytes,max_members,index_bytes,threads,nullptr,nullptr};
    bind_anchor_kernels(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_window_api_v2() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),window_abi_version,nullptr};
    bind_anchor_window_search(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_xor_api_v3() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),packed_xor_window_abi_version,nullptr};
    bind_anchor_packed_xor_window(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_xor_api_v4() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),wide_xor_window_abi_version,nullptr};
    bind_anchor_wide_xor_window(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_xor_api_v5() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),adaptive_xor_window_abi_version,nullptr};
    bind_anchor_adaptive_xor_window(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_modular_delta_api_v6() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),modular_delta_window_abi_version,nullptr};
    bind_anchor_modular_delta_window(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::WindowSearchApi* flitzfs_rocm_anchor_sparse_xor_api_v7() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    WindowSearchApi value{sizeof(WindowSearchApi),sparse_xor_window_abi_version,nullptr};
    bind_anchor_sparse_xor_window(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::AnchorArenaApi* flitzfs_rocm_anchor_sparse_xor_arena_api_v8() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    AnchorArenaApi value{sizeof(AnchorArenaApi),sparse_xor_arena_abi_version,nullptr};
    bind_anchor_sparse_xor_arena(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::AnchorArenaApi* flitzfs_rocm_anchor_collective_arena_api_v9() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    AnchorArenaApi value{sizeof(AnchorArenaApi),collective_arena_abi_version,nullptr};
    bind_anchor_collective_arena(value);
    return value;
  }();
  return &api;
}

extern "C" __declspec(dllexport)
const flitzfs::anchor_match::AnchorReferenceApi* flitzfs_rocm_anchor_reference_api_v10() noexcept {
  using namespace flitzfs::anchor_match;
  static const auto api=[]() noexcept {
    AnchorReferenceApi value{sizeof(AnchorReferenceApi),reference_price_abi_version,nullptr};
    bind_anchor_reference_price(value);
    return value;
  }();
  return &api;
}
