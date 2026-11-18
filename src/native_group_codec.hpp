#pragma once
#include "record_digest.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace flitzfs::detail {
inline constexpr std::uint64_t kNativeGroupBaseBit=1ULL<<61;
struct NativeGroupReference {
  std::uint64_t base_id=0,base_generation=0;
  Digest base_digest{},decoded_digest{};
  std::uint32_t member_bytes=0;
  bool operator==(const NativeGroupReference&) const=default;
};
NativeGroupReference parse_native_group_member(std::span<const std::byte>);
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base);
std::optional<std::vector<std::byte>> encode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation);
// Bind a private, already CPU-verified frame to its final protected object.
void bind_native_group_member(std::span<std::byte>,std::uint64_t base_id,std::uint64_t base_generation);
} // namespace flitzfs::detail
