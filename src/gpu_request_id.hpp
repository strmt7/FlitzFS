#pragma once
#include <cstdint>
namespace flitzfs::detail {
// Shared request-ID namespace for decode and match controllers. Zero signals
// permanent exhaustion. No ID is reused after the counter reaches its ceiling.
std::uint64_t next_gpu_request_id() noexcept;
}
