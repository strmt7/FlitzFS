#pragma once
#include "flitzfs/gpu_kernel_api.hpp"
#include "flitzfs/worker_wire.hpp"
#include <windows.h>

namespace flitzfs {
void require_hip_worker_job();
// The caller owns the verified runtime and kernel module for the whole call.
// This is the compute stage of a supervised worker, with no module discovery.
GpuBatchReply run_hip_decode_in_worker(HMODULE runtime,
    const gpu_detail::KernelApi& kernel, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask, const WorkerBatchRequest& request);
}
