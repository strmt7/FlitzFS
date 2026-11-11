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
