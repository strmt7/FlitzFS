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

struct AnchorMatchBatchPlan {
  std::vector<AnchorMatchSweepPlan> groups;
  std::uint32_t request_bytes,reply_bytes,quantum_members,device_bytes,launches;
  bool operator==(const AnchorMatchBatchPlan&) const = default;
};

struct AnchorMatchBatchRequest {
  std::uint64_t id;
  std::array<std::byte,32> digest;
  AnchorMatchBatchPlan plan;
  std::vector<AnchorMatchRequest> groups;
};

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

// The grant counts the index build and all search launches for each group.
AnchorMatchBatchPlan plan_anchor_match_batch(std::span<const std::uint32_t> members,
    std::uint32_t maximum_quantum,std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches);
std::vector<std::byte> encode_anchor_match_batch_request(std::span<const AnchorMatchRequest> groups,
    std::uint64_t id,const AnchorMatchBatchPlan&);
AnchorMatchBatchRequest decode_anchor_match_batch_request(std::span<const std::byte> frame,
    std::uint32_t request_capacity,std::uint32_t reply_capacity,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches);
std::vector<std::byte> encode_anchor_match_batch_reply(const AnchorMatchBatchRequest&,
    std::span<const std::vector<anchor_match::u32>> records);
std::vector<AnchorMatchAdvice> accept_anchor_match_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest&);

} // namespace flitzfs::detail
