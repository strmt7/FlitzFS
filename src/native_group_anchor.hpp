#pragma once
#include "flitzfs/gpu_anchor_match.hpp"
#include "flitzfs/native_filesystem.hpp"

namespace flitzfs::detail {

struct AnchorMatchRequest {
  std::uint64_t id;
  std::vector<std::byte> source;
};
using AnchorMatchAdvice=std::vector<std::vector<NativeSharedBaseCopyRange>>;

std::uint32_t anchor_match_members(std::span<const std::byte> source);
std::vector<std::byte> encode_anchor_match_request(std::span<const std::byte> base,
    std::span<const std::byte> members,std::uint64_t id);
AnchorMatchRequest decode_anchor_match_request(std::span<const std::byte> frame);
std::vector<anchor_match::u32> cpu_anchor_match_records(const AnchorMatchRequest&);
AnchorMatchAdvice verify_anchor_match_records(const AnchorMatchRequest&,std::span<const anchor_match::u32>);
std::vector<std::byte> encode_anchor_match_reply(const AnchorMatchRequest&,
    std::span<const anchor_match::u32>);
AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchRequest&);

} // namespace flitzfs::detail
