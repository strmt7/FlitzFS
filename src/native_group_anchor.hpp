#pragma once
#include "flitzfs/gpu_anchor_match.hpp"
#include "flitzfs/native_filesystem.hpp"
#include <array>
#include <optional>

namespace flitzfs::detail {

struct AnchorMatchLayout {
  std::uint32_t members,request_bytes,reply_bytes;
  bool operator==(const AnchorMatchLayout&) const = default;
};

struct AnchorMatchSweepPlan {
  AnchorMatchLayout whole;
  std::uint32_t quantum_members,windows,device_bytes;
  bool operator==(const AnchorMatchSweepPlan&) const = default;
};

struct AnchorMatchRequest {
  std::uint64_t id;
  std::vector<std::byte> source;
  std::array<std::byte,32> source_digest{};
};
using AnchorMatchAdvice=std::vector<std::vector<NativeSharedBaseCopyRange>>;

std::uint32_t anchor_match_members(std::span<const std::byte> source);
AnchorMatchLayout anchor_match_layout(std::uint32_t members);
// The caller independently admits the whole host request and its transport.
// This plan admits one device slab and bounds the number of search dispatches.
AnchorMatchSweepPlan plan_anchor_match_sweep(std::uint32_t members,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_windows);
std::vector<std::byte> encode_anchor_match_request(std::span<const std::byte> base,
    std::span<const std::byte> members,std::uint64_t id,std::optional<AnchorMatchLayout> admitted={});
AnchorMatchRequest decode_anchor_match_request(std::span<const std::byte> frame,
    std::optional<AnchorMatchLayout> admitted={});
std::vector<anchor_match::u32> cpu_anchor_match_records(const AnchorMatchRequest&);
AnchorMatchAdvice verify_anchor_match_records(const AnchorMatchRequest&,std::span<const anchor_match::u32>);
std::vector<std::byte> encode_anchor_match_reply(const AnchorMatchRequest&,
    std::span<const anchor_match::u32>,std::size_t reserve_tail_bytes=0);
AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchRequest&);

} // namespace flitzfs::detail
