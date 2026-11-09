#pragma once
#include "flitzfs/gpu_protocol.hpp"
#include <type_traits>

namespace flitzfs::gpu_detail {
// In-process module interface, NEVER an IPC/on-disk struct or a HIP runtime ABI.
// Mirrors the current FlitzZip deferred-registration/typed-entrypoint mechanism.
// The caller must first authenticate both the runtime and module, keep them
// resident, then check this contract before using any returned kernel address.
using DecodeKernel = void (*)(const u8*,u32,const Task*,u32,u8*,u32,u32*,u32);
inline constexpr u32 kernel_api_version=1;
inline constexpr u32 decode_threads=64;
struct KernelApi {
  u32 size_bytes;
  u32 api_version;
  u32 task_abi;
  u32 task_bytes;
  u32 task_limit;
  u32 batch_limit;
  u32 threads;
  u32 reserved;
  DecodeKernel decode;
};
static_assert(std::is_standard_layout_v<KernelApi> &&
    std::is_trivially_copyable_v<KernelApi>);
inline KernelApi make_kernel_api(DecodeKernel decode) noexcept {
  return {static_cast<u32>(sizeof(KernelApi)),kernel_api_version,abi_version,
      static_cast<u32>(sizeof(Task)),max_tasks,max_batch_bytes,decode_threads,0,decode};
}
inline bool compatible_kernel_api(const KernelApi& api) noexcept {
  return api.size_bytes==sizeof(KernelApi) && api.api_version==kernel_api_version &&
      api.task_abi==abi_version && api.task_bytes==sizeof(Task) &&
      api.task_limit==max_tasks && api.batch_limit==max_batch_bytes &&
      api.threads==decode_threads && !api.reserved && api.decode;
}
using KernelApiEntry = const KernelApi* (*)() noexcept;
} // namespace flitzfs::gpu_detail
