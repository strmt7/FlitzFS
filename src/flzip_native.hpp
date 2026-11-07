#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace flitzfs::detail {
// Internal CPU implementation of GPU-native FLZIP representations. Never loads HIP.
bool native_flzip_codec_available(std::uint8_t kind) noexcept;
// Borrowed, disjoint source/destination, exact output size <= 16 MiB. Only private
// output may be supplied: failure can leave partial bytes, which must not escape.
// Compositions are closed and nonrecursive; scratch <= one output-sized buffer.
void decode_flzip_native(std::uint8_t kind, std::uint8_t fill,
    std::span<const std::byte> source, std::span<std::byte> output);
} // namespace flitzfs::detail
