#pragma once
#include <cstddef>
#include <span>
namespace flitzfs {
enum class GpuLz4Verification { verified,unavailable,invalid_extent,overlapping_output,decode_failed,mismatch };
// Optional independent CPU verification only. No GPU, stored codec or worker.
bool gpu_lz4_cpu_verification_available() noexcept;
// Valid borrowed spans held on one control thread. Scratch must be private and
// exactly source.size(); source/payload must stay immutable throughout the call.
// Failure may change scratch, never source or payload. No result is published.
GpuLz4Verification verify_gpu_lz4_candidate(std::span<const std::byte> source,
    std::span<const std::byte> payload,std::span<std::byte> scratch) noexcept;
} // namespace flitzfs
