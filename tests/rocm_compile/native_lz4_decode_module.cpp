#include "native_lz4_decode_api.hpp"

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
