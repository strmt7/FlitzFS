// Experimental adaptive-layout link fixture; not an automatic runtime route.
#include "hipcomp_uniform_lz4_api.hpp"
namespace flitzfs::uniform_lz4 {
void bind_compression_kernels(KernelApi&) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::uniform_lz4::KernelApi* flitzfs_uniform_lz4_kernel_api() noexcept {
  static const auto api=[]() noexcept {
    auto value=flitzfs::uniform_lz4::make_kernel_api(nullptr,nullptr);
    flitzfs::uniform_lz4::bind_compression_kernels(value);return value;
  }();
  return &api;
}
