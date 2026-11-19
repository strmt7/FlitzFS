#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "record_digest.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>
namespace flitzfs::detail {
inline constexpr std::uint64_t kNativeGroupBaseBit=1ULL<<61;
enum class NativeGroupMethod : std::uint32_t { aligned_patches=1,anchor_copies=2,packed_xor=3,modular_delta=4 };
struct NativeGroupReference {
  std::uint64_t base_id=0,base_generation=0;
  Digest base_digest{},decoded_digest{};
  std::uint32_t member_bytes=0;
  NativeGroupMethod method=NativeGroupMethod::aligned_patches;
  bool operator==(const NativeGroupReference&) const=default;
};
NativeGroupReference parse_native_group_member(std::span<const std::byte>,
    NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);
std::optional<std::vector<std::byte>> encode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation);
std::optional<std::vector<std::byte>> encode_native_group_xor(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
std::optional<std::vector<std::byte>> encode_native_group_delta(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
struct NativeGroupXorCandidate {
  std::optional<std::vector<std::byte>> frame;
  NativePackedXorCheck check=NativePackedXorCheck::pending;
};
NativeGroupXorCandidate check_native_group_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t base_generation,std::size_t exclusive_limit);
// Bind a private, already CPU-verified frame to its final protected object.
void bind_native_group_member(std::span<std::byte>,std::uint64_t base_id,std::uint64_t base_generation,
    NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);

// Content evidence for a sparse search. A zero entry contributes no posting.
inline constexpr std::size_t kNativeGroupShiftFeatures=4;
std::array<std::uint64_t,kNativeGroupShiftFeatures> native_group_shift_features(std::span<const std::byte>) noexcept;
using NativeGroupDeltaSignatures=std::array<std::uint64_t,4>;
NativeGroupDeltaSignatures native_group_delta_signatures(std::span<const std::byte> extent);

inline constexpr std::size_t kNativeGroupCopyMinimumBytes=136;
inline constexpr std::size_t kNativeGroupAnchorIndexBytes=65536;
std::optional<std::vector<std::byte>> encode_native_group_copies(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,
    std::span<const NativeSharedBaseCopyRange>,std::size_t exclusive_limit);
class NativeGroupAnchorIndex final {
 public:
  explicit NativeGroupAnchorIndex(std::span<const std::byte> base);
  // The exclusive size bound is the competing frame size or the logical extent.
  std::optional<std::vector<std::byte>> encode(std::span<const std::byte> member,
      std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,
      std::size_t exclusive_limit) const;
 private:
  using Entry=std::pair<std::uint64_t,std::uint32_t>;
  std::vector<Entry> entries_;
};
} // namespace flitzfs::detail
