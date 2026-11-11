#pragma once
#include "flitzfs/gpu_match.hpp"
#include "flitzfs/worker_wire.hpp"
namespace flitzfs {
inline constexpr std::size_t kMatchWorkerHeaderBytes=64;
inline constexpr std::size_t kMatchWorkerRequestLimit=kMatchWorkerHeaderBytes+match_detail::input_limit;
inline constexpr std::size_t kMatchWorkerReplyLimit=kMatchWorkerHeaderBytes+
    match_detail::lane_limit*sizeof(match_detail::Match);
static_assert(kMatchWorkerRequestLimit<=kWorkerRequestLimit && kMatchWorkerReplyLimit<=kWorkerReplyLimit);
struct MatchWorkerRequest {
  std::uint64_t request_id;
  PreparedGpuMatchSearch search;
};
std::vector<std::byte> encode_match_worker_request(const PreparedGpuMatchSearch&,std::uint64_t request_id);
MatchWorkerRequest decode_match_worker_request(std::span<const std::byte>);
std::vector<std::byte> encode_match_worker_reply(const PreparedGpuMatchSearch&,std::uint64_t request_id,
    std::span<const match_detail::Match>,WorkerExecution);
std::vector<StoreMatchProposal> accept_match_worker_reply(std::span<const std::byte>,
    const PreparedGpuMatchSearch&,std::uint64_t request_id,WorkerExecution);
}
