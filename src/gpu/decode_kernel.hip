// Offline-compilable HIP candidate. No HIP runtime/library headers are required:
// only compiler language attributes and AMDGPU grid builtins are used.
#include "decode_kernel.hpp"
extern "C" __attribute__((global))
__attribute__((amdgpu_flat_work_group_size(64,64)))
void flitzfs_decode_v1(const unsigned char* input, unsigned int input_size,
    const flitzfs::gpu_detail::Task* tasks, unsigned int task_count,
    unsigned char* output, unsigned int output_size, unsigned int* status,
    unsigned int version) {
  const auto index=__builtin_amdgcn_workgroup_id_x()*64U+__builtin_amdgcn_workitem_id_x();
  if(version!=flitzfs::gpu_detail::abi_version || task_count>flitzfs::gpu_detail::max_tasks ||
      index>=task_count) return;
  status[index]=flitzfs::gpu_detail::decode_task(tasks[index],input,input_size,output,output_size);
}
