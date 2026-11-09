#include <hip/hip_runtime_api.h>
#include <windows.h>
#include <cstddef>
#include <iostream>
#include <type_traits>
// Types come directly from the selected complete SDK, never a hand-written ABI.
using Init=decltype(&hipInit);
using Properties=decltype(&hipGetDeviceProperties);
using Launch=decltype(&hipModuleLaunchKernel);
using Stream=decltype(&hipStreamCreateWithPriority);
static_assert(sizeof(void*)==8 && sizeof(hipError_t)==4);
static_assert(std::is_pointer_v<Init> && std::is_pointer_v<Properties> &&
    std::is_pointer_v<Launch> && std::is_pointer_v<Stream>);
int main() {
  if(GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME))return 2;
  std::cout<<"ROCm header observation: distribution="<<FLITZFS_SELECTED_ROCM
      <<" HIP="<<HIP_VERSION_MAJOR<<'.'<<HIP_VERSION_MINOR<<'.'<<HIP_VERSION_PATCH
      <<'-'<<HIP_VERSION_GITHASH<<" properties_bytes="<<sizeof(hipDeviceProp_t)
      <<" required_runtime="<<FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME
      <<" hip_runtime_loaded=false; CPU header inspection only\n";
  return 0;
}
