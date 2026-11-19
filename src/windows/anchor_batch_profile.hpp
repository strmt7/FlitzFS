#pragma once
#include "anchor_sweep_profile.hpp"

namespace flitzfs {
inline constexpr auto kAnchorBatchProfileBytes=kAnchorSweepProfileBytes;
inline constexpr std::uint32_t kAnchorProfileMaxQueuedDispatches=4;

struct AnchorBatchProfileBinding {
  HipProfileBinding identity;
  std::uint32_t groups,quantum_members,launches,dispatches_per_wait;
  detail::AnchorMatchTransferPlan transfer;
  anchor_match::BatchOutput output;
};

struct AnchorSetupTiming {
  std::uint64_t stream_create_ns=0,slab_allocate_ns=0;
  bool operator==(const AnchorSetupTiming&) const=default;
};

inline AnchorBatchProfileBinding anchor_batch_profile_binding(const detail::AnchorMatchBatchRequest& request,
    const detail::AnchorMatchTransferPlan& transfer,std::uint32_t ordinal,
    std::array<std::byte,8> luid,std::uint32_t node_mask,std::uint32_t dispatches_per_wait) {
  const auto& plan=request.plan();
  const auto groups=request.groups();
  const auto wide=plan.output==anchor_match::BatchOutput::wide_packed_xor;
  hip_phase_detail::require((wide || plan.output==anchor_match::BatchOutput::anchors) &&
      !groups.empty() && groups.size()==plan.groups.size() &&
      transfer==detail::plan_anchor_match_transfers(plan,transfer.device_bytes) &&
      dispatches_per_wait && dispatches_per_wait<=kAnchorProfileMaxQueuedDispatches);
  const auto words_per_member=anchor_match::reply_member_bytes(plan.output)/sizeof(anchor_match::u32);
  std::uint32_t lanes=0;
  for(const auto& group:plan.groups)lanes+=group.whole.members*static_cast<std::uint32_t>(words_per_member);
  return {{request.id(),ordinal,node_mask,plan.request_bytes,lanes,luid,request.digest()},
      static_cast<std::uint32_t>(groups.size()),plan.quantum_members,plan.launches,dispatches_per_wait,transfer,plan.output};
}

namespace anchor_batch_profile_detail {
inline std::array<std::byte,8> marker(anchor_match::BatchOutput output,bool setup) {
  const auto wide=output==anchor_match::BatchOutput::wide_packed_xor;
  hip_phase_detail::require(wide || (!setup && output==anchor_match::BatchOutput::anchors));
  return {std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'P'},
      std::byte{'B'},std::byte{'0'},std::byte{'0'},setup?std::byte{'4'}:wide?std::byte{'3'}:std::byte{'2'}};
}
inline constexpr auto work_at=hip_phase_detail::common_bytes;
using Work=std::array<std::uint32_t,7>;
inline constexpr auto work_end=work_at+4*Work{}.size();
inline constexpr auto setup_end=work_end+2*sizeof(std::uint64_t);
static_assert(setup_end<=kAnchorBatchProfileBytes);

inline Work work(const AnchorBatchProfileBinding& binding) noexcept {
  return {binding.groups,binding.quantum_members,binding.launches,binding.transfer.members_per_transfer,
      binding.transfer.device_bytes,binding.transfer.transfers,binding.dispatches_per_wait};
}

inline void check_setup(const HipPhaseTrace& trace,const AnchorSetupTiming& setup) {
  const auto total=trace.wall_ns[static_cast<unsigned>(HipPhase::stream_allocate)];
  hip_phase_detail::require(setup.stream_create_ns<=total && setup.slab_allocate_ns<=total-setup.stream_create_ns);
}
}

inline hip_phase_detail::Frame encode_anchor_batch_profile(const AnchorBatchProfileBinding& binding,
    const HipPhaseTrace& trace,const AnchorSetupTiming* setup=nullptr) {
  using namespace anchor_batch_profile_detail;
  hip_phase_detail::identity_valid(binding.identity);
  auto frame=hip_phase_detail::encode_common({binding.identity,trace,WorkerExecution::hip},
      marker(binding.output,setup!=nullptr),kAnchorSweepProfileLimitNs);
  auto at=work_at;
  for(const auto field:work(binding)) {
    hip_phase_detail::put(frame,at,field,4);
    at+=4;
  }
  if(setup) {
    check_setup(trace,*setup);
    hip_phase_detail::put(frame,work_end,setup->stream_create_ns);
    hip_phase_detail::put(frame,work_end+sizeof(std::uint64_t),setup->slab_allocate_ns);
  }
  return frame;
}

inline HipPhaseTrace decode_anchor_batch_profile(std::span<const std::byte> frame,
    const AnchorBatchProfileBinding& binding,AnchorSetupTiming* setup=nullptr) {
  using namespace anchor_batch_profile_detail;
  hip_phase_detail::identity_valid(binding.identity);
  const auto profile=hip_phase_detail::decode_common(frame,binding.identity,WorkerExecution::hip,
      marker(binding.output,setup!=nullptr),kAnchorSweepProfileLimitNs);
  auto at=work_at;
  for(const auto field:work(binding)) {
    hip_phase_detail::require(hip_phase_detail::get(frame,at,4)==field);
    at+=4;
  }
  const auto end=setup?setup_end:work_end;
  hip_phase_detail::require(std::all_of(frame.begin()+end,frame.end(),[](auto value){return value==std::byte{};}));
  if(setup) {
    const AnchorSetupTiming value{hip_phase_detail::get(frame,work_end),
        hip_phase_detail::get(frame,work_end+sizeof(std::uint64_t))};
    check_setup(profile.trace,value);
    *setup=value;
  }
  return profile.trace;
}
}
