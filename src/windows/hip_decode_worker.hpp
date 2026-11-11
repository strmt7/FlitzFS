#pragma once
#include "flitzfs/gpu_kernel_api.hpp"
#include "flitzfs/worker_wire.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include "flitzfs/gpu_match_kernel_api.hpp"
#include <windows.h>
#include "hip_memory_wire.hpp"

namespace flitzfs {
void require_hip_worker_job();
std::vector<match_detail::Match> run_hip_match_in_worker(HMODULE runtime,
    const match_detail::KernelApi&,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const MatchWorkerRequest&);
// The caller owns the verified runtime and kernel module for the whole call.
// This is the compute stage of a supervised worker, with no module discovery.
GpuBatchReply run_hip_decode_in_worker(HMODULE runtime,
    const gpu_detail::KernelApi& kernel, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask, const WorkerBatchRequest& request,
    HipMemoryObservation* observation=nullptr);
}
