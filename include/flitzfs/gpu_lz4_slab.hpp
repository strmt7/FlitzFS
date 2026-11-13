#pragma once
#include "flitzfs/lz4_worker_wire.hpp"
#include <cstdint>
#include <vector>
namespace flitzfs {
// Experimental CPU-only construction and readback admission for the bounded
// AMD64 candidate. Device addresses are integer data, never dereferenced here.
// This is neither resource/launch authority nor a stored or worker-wire format.
// The caller owns valid, immutable spans on one control thread for each call.
std::vector<std::byte> gpu_lz4_slab_upload(
    const PreparedLz4WorkerBatch&,std::uint64_t device_address);
// Requires the COMPLETE, exactly sized readback after confirmed completion.
// Every protected byte, size and independent CPU decode must pass before ANY
// candidate bytes are copied into the returned owned vectors. No partial result.
// Missing optional decoding refuses explicitly; preflight it before any launch.
// Callers still require current resource/identity and exact retirement gates.
std::vector<std::vector<std::byte>> verify_gpu_lz4_slab(
    const PreparedLz4WorkerBatch&,std::uint64_t device_address,Lz4WorkerBytes);
} // namespace flitzfs
