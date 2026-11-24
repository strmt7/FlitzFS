#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "record_digest.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>
namespace flitzfs::detail {
inline constexpr std::uint64_t kNativeGroupBaseBit=1ULL<<61;
enum class NativeGroupMethod : std::uint32_t {
  aligned_patches=1,anchor_copies=2,packed_xor=3,modular_delta=4,sparse_xor=5,prefixed_zstd=6
};
struct NativeGroupReference {
  std::uint64_t base_id=0,base_generation=0;
  Digest base_digest{},decoded_digest{};
  std::uint32_t member_bytes=0;
  NativeGroupMethod method=NativeGroupMethod::aligned_patches;
  bool operator==(const NativeGroupReference&) const=default;
};
// The decoded base and its lazy digest move together in the serialized read
// cache. A caller cannot substitute a digest for different bytes.
class NativeGroupBase final {
 public:
  NativeGroupBase(std::vector<std::byte> record,std::size_t payload_offset);
  std::span<const std::byte> payload() const noexcept;
  const Digest& payload_digest() const;
  std::vector<std::byte> take_payload() &&;
 private:
  std::vector<std::byte> record_;
  std::size_t payload_offset_;
  mutable std::optional<Digest> digest_;
};

// Read the bound identity and digests from the header. The checked decoder
// validates the payload as it reconstructs the member.
NativeGroupReference read_native_group_header(std::span<const std::byte>,NativeGroupMethod maximum_method);
NativeGroupReference parse_native_group_member(std::span<const std::byte>,
    NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> member,
    const NativeGroupBase& base,NativeGroupMethod maximum_method);
std::optional<std::vector<std::byte>> encode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation);
std::optional<std::vector<std::byte>> encode_native_group_xor(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
std::optional<std::vector<std::byte>> encode_native_group_sparse_xor(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
std::optional<std::vector<std::byte>> encode_native_group_prefix(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
std::size_t price_native_group_xor(std::span<const std::byte> member,std::span<const std::byte> base);
std::optional<std::vector<std::byte>> encode_native_group_delta(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t base_generation,std::size_t exclusive_limit);
struct NativeGroupPackedCandidate {
  std::optional<std::vector<std::byte>> frame;
  NativePackedGroupCheck check=NativePackedGroupCheck::pending;
  NativeGroupMethod method=NativeGroupMethod::packed_xor;
};
struct CollectiveGroupBody {
  NativePackedGroupMethod method;
  std::span<const std::byte> bytes;
};
std::optional<CollectiveGroupBody> parse_collective_group_body(std::span<const std::byte>) noexcept;
NativeGroupPackedCandidate check_native_group_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t base_generation,std::size_t exclusive_limit,bool include_sparse=false);
NativeGroupPackedCandidate check_native_group_sparse_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t base_generation,std::size_t exclusive_limit);
NativeGroupPackedCandidate check_native_group_delta_body(std::span<const std::byte> payload,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t base_generation,std::size_t exclusive_limit);
// Bind a private, already CPU-verified frame to its final protected object.
void bind_native_group_member(std::span<std::byte>,std::uint64_t base_id,std::uint64_t base_generation,
    NativeGroupMethod maximum_method=NativeGroupMethod::aligned_patches);

// Content evidence for a sparse search. A zero entry contributes no posting.
inline constexpr std::size_t kNativeGroupShiftFeatures=4;
using NativePackingSketch=std::array<std::uint64_t,16>;
std::array<std::uint64_t,kNativeGroupShiftFeatures> native_group_shift_features(
    std::span<const std::byte>,NativePackingSketch* packing=nullptr) noexcept;
using NativeGroupDeltaSignatures=std::array<std::uint64_t,4>;
NativeGroupDeltaSignatures native_group_delta_signatures(std::span<const std::byte> extent);

// The source extents remain immutable until finish releases the section views.
class NativeGroupSections final {
 public:
  void sample(std::span<const std::byte>);
  std::vector<std::byte> finish();
 private:
  struct Less {
    bool operator()(std::span<const std::byte>,std::span<const std::byte>) const noexcept;
  };
  struct Seen {
    std::size_t members=0,last=0,order=0;
  };
  std::map<std::span<const std::byte>,Seen,Less> sections_;
  std::size_t samples_=0;
};

inline constexpr std::size_t kNativeGroupMemberHeaderBytes=128;
inline constexpr std::size_t kNativeGroupTileBytes=256;
using NativeGroupTileOffsets=std::array<std::uint16_t,kNativeExtentBytes/kNativeGroupTileBytes+1>;
struct NativeGroupTileParameters {
  std::uint64_t bias=0;
  std::uint8_t word_bytes=0;
};
constexpr bool native_group_tiled(NativeGroupMethod method) noexcept {
  return method==NativeGroupMethod::packed_xor || method==NativeGroupMethod::sparse_xor ||
      method==NativeGroupMethod::modular_delta;
}
NativeGroupTileParameters native_group_tile_parameters(std::span<const std::byte> frame,NativeGroupMethod);
NativeGroupTileOffsets native_group_tile_offsets(std::span<const std::byte> body,NativeGroupMethod,
    NativeGroupTileParameters={});
inline constexpr std::size_t kNativeGroupCopyMinimumBytes=kNativeGroupMemberHeaderBytes+8;
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
