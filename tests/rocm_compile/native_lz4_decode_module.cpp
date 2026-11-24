#include "native_lz4_decode_api.hpp"
#include "native_group_decode.hpp"

namespace flitzfs::native_lz4_decode {
void bind_decode_kernels(KernelApi& api) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::native_lz4_decode::KernelApi* flitzfs_native_lz4_decode_api() noexcept {
  static const auto api=[] {
    flitzfs::native_lz4_decode::KernelApi value{};
    flitzfs::native_lz4_decode::bind_decode_kernels(value);
    return value;
  }();
  return &api;
}
namespace flitzfs::group_decode {
void bind_group_decode_kernel(KernelApi& api,u32 abi) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::group_decode::KernelApi* flitzfs_native_group_decode_api() noexcept {
  static const auto api=[] {
    flitzfs::group_decode::KernelApi value{};
    flitzfs::group_decode::bind_group_decode_kernel(value,flitzfs::group_decode::version);
    return value;
  }();
  return &api;
}
extern "C" __declspec(dllexport)
const flitzfs::group_decode::KernelApi* flitzfs_native_group_decode_api_v2() noexcept {
  static const auto api=[] {
    flitzfs::group_decode::KernelApi value{};
    flitzfs::group_decode::bind_group_decode_kernel(value,flitzfs::group_decode::mixed_version);
    return value;
  }();
  return &api;
}
