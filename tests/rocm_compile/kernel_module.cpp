// Separate module follows FlitzZip's deferred-registration build boundary.
// Qualification links it but NEVER loads it or executes a GPU kernel.
#include "flitzfs/gpu_protocol.hpp"
extern "C" __declspec(dllexport) unsigned flitzfs_rocm_kernel_abi() noexcept {
  return flitzfs::gpu_detail::abi_version;
}
