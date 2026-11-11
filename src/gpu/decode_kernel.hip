// Built with the complete selected ROCm SDK. No runtime or kernel is launched
// during compile qualification; host/device definitions come from that SDK.
#include <hip/hip_runtime.h>
#include "decode_kernel.hpp"
#include "flitzfs/gpu_kernel_api.hpp"
extern "C" __global__
__attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_decode_v1(const unsigned char* input, unsigned int input_size,
    const flitzfs::gpu_detail::Task* tasks, unsigned int task_count,
    unsigned char* output, unsigned int output_size, unsigned int* status,
    unsigned int version) {
  const auto index=blockIdx.x*64U+threadIdx.x;
  if(version!=flitzfs::gpu_detail::abi_version || task_count>flitzfs::gpu_detail::max_tasks ||
      index>=task_count) return;
  status[index]=flitzfs::gpu_detail::decode_task(tasks[index],input,input_size,output,output_size);
}

// Taking the typed address inside the HIP translation unit preserves the SDK's
// registered host stub. No hand-built HIP symbol or registration ABI is used.
namespace flitzfs::gpu_detail {
void bind_decode_kernel(KernelApi& api) noexcept {
  api.decode=flitzfs_decode_v1;
}
}

#include "flitzfs/gpu_match_kernel_api.hpp"
extern "C" __global__
__attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_match_v1(const unsigned char* input,unsigned int size,
    const flitzfs::match_detail::u16* previous,unsigned int previous_count,
    flitzfs::match_detail::Match* output,unsigned int output_count,unsigned int version) {
  using namespace flitzfs::match_detail;
  const auto lane=blockIdx.x*64U+threadIdx.x;
  if(version!=abi_version || size>input_limit || previous_count!=size ||
      output_count!=lanes(size) || lane>=output_count || !output)return;
  output[lane]=search(input,size,previous,previous_count,lane);
}
namespace flitzfs::match_detail {
void bind_match_kernel(KernelApi& api) noexcept {api.search=flitzfs_match_v1;}
}
