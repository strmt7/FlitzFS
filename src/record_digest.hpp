#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace flitzfs::detail {
using Digest = std::array<std::byte,32>;
// Unkeyed record integrity, NOT authentication or encryption.
Digest record_digest(std::span<const std::byte> first,
                     std::span<const std::byte> second = {}) noexcept;
}
