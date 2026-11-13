// Experimental compile-only SDK API probe. Never loaded, linked or launched.
#include <hip/hip_runtime.h>
#include <cstdint>

#ifdef HIP_DISABLE_WARP_SYNC_BUILTINS
#error "The explicit synchronization probe requires the selected SDK builtins"
#endif

namespace {
using Mask = unsigned long long;
static_assert(sizeof(Mask) == 8);
static_assert(sizeof(std::uint32_t) == 4);

template<class SourceMask>
__device__ inline void typed_wave_sync(SourceMask source) {
  static_assert(sizeof(SourceMask) == 4 || sizeof(SourceMask) == 8);
  // The selected SDK intentionally rejects an implicit 32-bit mask.
  __syncwarp(static_cast<Mask>(source));
}
}

// Illustrative one-wave geometry only. This source is not a runtime module.
// Future execution would need a separate admitted caller and independent oracle.
extern "C" __global__ void flitzfs_compile_warp_sync_probe(
    Mask* output, unsigned output_elements) {
  const unsigned width = static_cast<unsigned>(warpSize);
  if ((width != 32 && width != 64) || output == nullptr ||
      output_elements != width || blockDim.x != width ||
      blockDim.y != 1 || blockDim.z != 1 ||
      gridDim.x != 1 || gridDim.y != 1 || gridDim.z != 1) return;

  __shared__ std::uint32_t values[64];
  const unsigned lane = threadIdx.x;
  values[lane] = lane & 3U;
  output[lane] = 0;
  __syncwarp();

  const unsigned participants = width - 3;
  const Mask mask = (Mask{1} << participants) - 1;
  if (lane < participants) {
    if (width == 32) typed_wave_sync(static_cast<std::uint32_t>(mask));
    else typed_wave_sync(mask);
    output[lane] = __match_any_sync(mask, values[lane]);
    typed_wave_sync(mask);
  }
  __syncwarp();
}
