// Built with the complete selected ROCm SDK. No runtime or kernel is launched
// during compile qualification; host/device definitions come from that SDK.
#include <hip/hip_runtime.h>
#include "decode_kernel.hpp"
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
