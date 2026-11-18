#pragma once
#include "../native_group_anchor.hpp"
#include "hip_phase_profile.hpp"
#include "flitzfs/process_budget.hpp"

namespace flitzfs {
inline constexpr std::size_t kAnchorSweepProfileBytes=sizeof(hip_phase_detail::Frame);
inline constexpr std::uint64_t kAnchorSweepProfileLimitNs=std::uint64_t{ProcessBudget{}.timeout_ms}*1'000'000;

struct AnchorSweepProfileBinding {
  HipProfileBinding identity;
  detail::AnchorMatchSweepPlan plan;
};

inline AnchorSweepProfileBinding anchor_sweep_profile_binding(const detail::AnchorMatchRequest& request,
    const detail::AnchorMatchSweepPlan& plan,std::uint32_t ordinal,
    std::array<std::byte,8> luid,std::uint32_t node_mask) {
  return {{request.id,ordinal,node_mask,static_cast<std::uint32_t>(request.source.size()),
      plan.whole.members*anchor_match::tiles,luid,request.source_digest},plan};
}

namespace anchor_sweep_profile_detail {
inline constexpr std::array<std::byte,8> magic{std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'P'},
    std::byte{'A'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
inline constexpr auto plan_at=hip_phase_detail::common_bytes;
static_assert(plan_at+12<=kAnchorSweepProfileBytes);

inline void validate(const AnchorSweepProfileBinding& b) {
  using hip_phase_detail::require;
  hip_phase_detail::identity_valid(b.identity);
  const auto& plan=b.plan;
  require(plan==detail::plan_anchor_match_sweep(plan.whole.members,plan.quantum_members,plan.device_bytes,plan.windows) &&
      b.identity.input_bytes==(plan.whole.members+1)*anchor_match::extent_bytes &&
      b.identity.lanes==plan.whole.members*anchor_match::tiles);
}
}

inline hip_phase_detail::Frame encode_anchor_sweep_profile(const AnchorSweepProfileBinding& binding,
    const HipPhaseTrace& trace) {
  using namespace anchor_sweep_profile_detail;
  validate(binding);
  auto frame=hip_phase_detail::encode_common({binding.identity,trace,WorkerExecution::hip},
      magic,kAnchorSweepProfileLimitNs);
  hip_phase_detail::put(frame,plan_at,binding.plan.quantum_members,4);
  hip_phase_detail::put(frame,plan_at+4,binding.plan.windows,4);
  hip_phase_detail::put(frame,plan_at+8,binding.plan.device_bytes,4);
  return frame;
}

inline HipPhaseTrace decode_anchor_sweep_profile(std::span<const std::byte> frame,
    const AnchorSweepProfileBinding& binding) {
  using namespace anchor_sweep_profile_detail;
  validate(binding);
  const auto profile=hip_phase_detail::decode_common(frame,binding.identity,WorkerExecution::hip,
      magic,kAnchorSweepProfileLimitNs);
  using hip_phase_detail::get;
  hip_phase_detail::require(get(frame,plan_at,4)==binding.plan.quantum_members &&
      get(frame,plan_at+4,4)==binding.plan.windows && get(frame,plan_at+8,4)==binding.plan.device_bytes &&
      std::all_of(frame.begin()+plan_at+12,frame.end(),[](auto value){return value==std::byte{};}));
  return profile.trace;
}
}
