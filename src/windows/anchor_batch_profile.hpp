#pragma once
#include "anchor_sweep_profile.hpp"

namespace flitzfs {
inline constexpr auto kAnchorBatchProfileBytes=kAnchorSweepProfileBytes;
inline constexpr std::uint32_t kAnchorProfileMaxQueuedDispatches=4;

struct AnchorBatchProfileBinding {
  HipProfileBinding identity;
  std::uint32_t groups,quantum_members,launches,dispatches_per_wait;
  detail::AnchorMatchTransferPlan transfer;
};

inline AnchorBatchProfileBinding anchor_batch_profile_binding(const detail::AnchorMatchBatchRequest& request,
    const detail::AnchorMatchTransferPlan& transfer,std::uint32_t ordinal,
    std::array<std::byte,8> luid,std::uint32_t node_mask,std::uint32_t dispatches_per_wait) {
  const auto& plan=request.plan();
  const auto groups=request.groups();
  hip_phase_detail::require(!groups.empty() && groups.size()==plan.groups.size() &&
      transfer==detail::plan_anchor_match_transfers(plan,transfer.device_bytes) &&
      dispatches_per_wait && dispatches_per_wait<=kAnchorProfileMaxQueuedDispatches);
  std::uint32_t lanes=0;
  for(const auto& group:plan.groups)lanes+=group.whole.members*anchor_match::tiles;
  return {{request.id(),ordinal,node_mask,plan.request_bytes,lanes,luid,request.digest()},
      static_cast<std::uint32_t>(groups.size()),plan.quantum_members,plan.launches,dispatches_per_wait,transfer};
}

namespace anchor_batch_profile_detail {
inline constexpr std::array<std::byte,8> magic{std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'P'},
    std::byte{'B'},std::byte{'0'},std::byte{'0'},std::byte{'2'}};
inline constexpr auto work_at=hip_phase_detail::common_bytes;
using Work=std::array<std::uint32_t,7>;
inline constexpr auto work_end=work_at+4*Work{}.size();
static_assert(work_end<=kAnchorBatchProfileBytes);

inline Work work(const AnchorBatchProfileBinding& binding) noexcept {
  return {binding.groups,binding.quantum_members,binding.launches,binding.transfer.members_per_transfer,
      binding.transfer.device_bytes,binding.transfer.transfers,binding.dispatches_per_wait};
}
}

inline hip_phase_detail::Frame encode_anchor_batch_profile(const AnchorBatchProfileBinding& binding,
    const HipPhaseTrace& trace) {
  using namespace anchor_batch_profile_detail;
  hip_phase_detail::identity_valid(binding.identity);
  auto frame=hip_phase_detail::encode_common({binding.identity,trace,WorkerExecution::hip},
      magic,kAnchorSweepProfileLimitNs);
  auto at=work_at;
  for(const auto field:work(binding)) {
    hip_phase_detail::put(frame,at,field,4);
    at+=4;
  }
  return frame;
}

inline HipPhaseTrace decode_anchor_batch_profile(std::span<const std::byte> frame,
    const AnchorBatchProfileBinding& binding) {
  using namespace anchor_batch_profile_detail;
  hip_phase_detail::identity_valid(binding.identity);
  const auto profile=hip_phase_detail::decode_common(frame,binding.identity,WorkerExecution::hip,
      magic,kAnchorSweepProfileLimitNs);
  auto at=work_at;
  for(const auto field:work(binding)) {
    hip_phase_detail::require(hip_phase_detail::get(frame,at,4)==field);
    at+=4;
  }
  hip_phase_detail::require(std::all_of(frame.begin()+work_end,frame.end(),[](auto value){return value==std::byte{};}));
  return profile.trace;
}
}
