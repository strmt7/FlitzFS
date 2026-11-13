// Experimental separate link fixture. Never loaded in hosted qualification.
#include "hipcomp_lz4_api.hpp"
namespace flitzfs::candidate_lz4 {
void bind_compression_kernels(KernelApi&) noexcept;
}
extern "C" __declspec(dllexport)
const flitzfs::candidate_lz4::KernelApi* flitzfs_candidate_lz4_api() noexcept {
  static const auto api=[]() noexcept {
    auto value=flitzfs::candidate_lz4::make_kernel_api(nullptr,nullptr);
    flitzfs::candidate_lz4::bind_compression_kernels(value);
    return value;
  }();
  return &api;
}
