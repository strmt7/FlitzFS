#include "flitzfs/gpu_batch.hpp"
#include "extent_codec.hpp"
#include "gpu_request_id.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>

namespace flitzfs {
namespace {
bool supported(unsigned kind) noexcept {
  return kind==0 || kind==1 || kind==3 || kind==4 || kind==5 || kind==10;
}
bool node(unsigned mask) noexcept { return mask && !(mask&(mask-1)); }
bool identity(const std::array<std::byte,8>& id) noexcept {
  return std::any_of(id.begin(),id.end(),[](auto b){return b!=std::byte{0};});
}
}
namespace detail {
// Globally unique per process; saturation refuses reuse rather than wrapping.
std::uint64_t next_gpu_request_id() noexcept {
  static std::atomic<std::uint64_t> next{1};
  auto current=next.load(std::memory_order_relaxed);
  while(current!=(std::numeric_limits<std::uint64_t>::max)()) {
    if(next.compare_exchange_weak(current,current+1,std::memory_order_relaxed)) return current;
  }
  return 0;
}
}
PreparedGpuBatch PreparedGpuBatch::prepare(std::span<const GpuBlockView> blocks) {
  if(blocks.empty() || blocks.size()>kGpuMaxBlocks)
    throw std::invalid_argument("GPU preview requires 1..8 blocks");
  std::size_t input_total=0, output_total=0;
  // Admit every extent before proportional copying or decoding. All payloads
  // are copied before any CPU decoding, isolating lifetime from caller buffers.
  for(const auto& b:blocks) {
    if(!supported(b.kind) || !b.decoded_bytes || b.decoded_bytes>gpu_detail::max_block_bytes ||
        b.decoded_bytes>gpu_detail::max_batch_bytes-output_total ||
        b.payload.size()>gpu_detail::max_batch_bytes-input_total)
      throw std::invalid_argument("unsupported GPU kind or oversized batch");
    if((b.kind==0 && b.payload.size()!=b.decoded_bytes) || (b.kind==1 && !b.payload.empty()) ||
        (b.kind!=0 && b.kind!=1 && (b.payload.empty() || b.payload.size()>=b.decoded_bytes)))
      throw std::invalid_argument("invalid GPU block framing");
    input_total+=b.payload.size();output_total+=b.decoded_bytes;
  }
  PreparedGpuBatch result;
  result.payload_.reserve(input_total);result.reference_.resize(output_total);
  result.tasks_.reserve(gpu_detail::max_tasks);
  for(const auto& b:blocks) result.payload_.insert(result.payload_.end(),b.payload.begin(),b.payload.end());
  std::uint32_t source=0,target=0;
  for(const auto& b:blocks) {
    const auto bytes=std::span<const std::byte>(result.payload_).subspan(source,b.payload.size());
    auto decoded=std::span<std::byte>(result.reference_).subspan(target,b.decoded_bytes);
    if(b.kind==0) std::copy(bytes.begin(),bytes.end(),decoded.begin());
    else if(b.kind==1) std::fill(decoded.begin(),decoded.end(),static_cast<std::byte>(b.fill));
    else detail::decode_extent(b.kind,b.fill,bytes,decoded);
    for(std::uint32_t start=0;start<b.decoded_bytes;start+=gpu_detail::segment_bytes) {
      result.tasks_.push_back({source,static_cast<std::uint32_t>(b.payload.size()),target,b.decoded_bytes,
          start,(std::min)(gpu_detail::segment_bytes,b.decoded_bytes-start),b.kind,b.fill});
    }
    source+=static_cast<std::uint32_t>(b.payload.size());target+=b.decoded_bytes;
  }
  if(result.tasks_.size()>gpu_detail::max_tasks) throw std::logic_error("GPU task budget invariant");
  return result;
}
std::uint64_t PreparedGpuBatch::device_buffer_bytes() const noexcept {
  return (std::max)(std::size_t{1},payload_.size())+reference_.size()+
      tasks_.size()*(sizeof(gpu_detail::Task)+sizeof(std::uint32_t));
}
GpuResultGate::GpuResultGate(PreparedGpuBatch batch,std::array<std::byte,8> luid,
    std::uint32_t mask,std::uint32_t ms) : batch_(std::move(batch)),luid_(luid),node_mask_(mask),deadline_ms_(ms) {
  if(!identity(luid) || !node(mask) || !ms || ms>100 || batch_.tasks().empty())
    throw std::invalid_argument("invalid GPU result gate identity/deadline");
}
GpuMemoryEnvelope gpu_memory_envelope(const GpuMemorySnapshot& s,
    std::uint64_t additional) noexcept {
  GpuMemoryEnvelope e;
  if(!s.valid || !s.amd_hardware || !identity(s.luid) || !node(s.node_mask) ||
      !s.dedicated_bytes || !s.local_budget || s.local_budget>s.dedicated_bytes ||
      !s.nonlocal_budget ||
      !s.physical_total || s.physical_available>s.physical_total ||
      !s.commit_limit || s.commit_available>s.commit_limit) return e;
  e.local_reserve=(std::max)(kGpuBudgetReserveBytes,s.local_budget/16);
  e.physical_reserve=(std::max)(kGpuBudgetReserveBytes,s.physical_total/16);
  e.commit_reserve=(std::max)(kGpuBudgetReserveBytes,s.commit_limit/16);
  e.code=GpuGateCode::insufficient_headroom;
  // Budgets may shrink below current usage: that is pressure, not corrupt
  // telemetry or proof of a failed device. Check before unsigned subtraction.
  if(s.local_usage>s.local_budget || s.nonlocal_usage>s.nonlocal_budget)return e;
  const auto local=s.local_budget-s.local_usage;
  const auto shared=s.nonlocal_budget-s.nonlocal_usage;
  const auto shared_reserve=s.nonlocal_budget/16;
  if(local<e.local_reserve || additional>local-e.local_reserve ||
      s.physical_available<e.physical_reserve || additional>s.physical_available-e.physical_reserve ||
      s.commit_available<e.commit_reserve || additional>s.commit_available-e.commit_reserve ||
      shared<shared_reserve || additional>shared-shared_reserve) return e;
  // At most 1/256 of remaining physical/commit capacity and 1/64 of remaining
  // nonlocal process budget, after reserves and explicit device allocations.
  // Subtract only after bounds checks; no multiplication/addition can overflow.
  e.nonlocal_growth=(std::min)({(shared-shared_reserve-additional)/64,
      (s.physical_available-e.physical_reserve-additional)/256,
      (s.commit_available-e.commit_reserve-additional)/256});
  e.code=GpuGateCode::admitted;return e;
}
GpuGateCode check_gpu_memory_budget(const GpuMemorySnapshot& s,
    std::array<std::byte,8> luid, std::uint32_t node_mask,
    std::uint64_t now, std::uint64_t additional) noexcept {
  const auto envelope=gpu_memory_envelope(s,additional);
  if(envelope.code==GpuGateCode::invalid_budget)return envelope.code;
  if(s.luid!=luid || s.node_mask!=node_mask) return GpuGateCode::wrong_adapter;
  if(now<s.captured_ms || now-s.captured_ms>kGpuMaxBudgetAgeMs) return GpuGateCode::stale_budget;
  return envelope.code;
}
GpuGateCode check_gpu_memory_transition(const GpuMemorySnapshot& before,
    const GpuMemorySnapshot& after,std::array<std::byte,8> luid,
    std::uint32_t node_mask,std::uint64_t now,std::uint64_t device_bytes) noexcept {
  // The caller retained before from its admitted attempt. Its age at completion
  // is expected to exceed a collector snapshot age; do not renew its timestamp.
  auto code=check_gpu_memory_budget(before,luid,node_mask,before.captured_ms,device_bytes);
  if(code!=GpuGateCode::admitted)return code;
  code=check_gpu_memory_budget(after,luid,node_mask,now,0);
  if(code!=GpuGateCode::admitted)return code;
  if(after.captured_ms<before.captured_ms)return GpuGateCode::stale_budget;
  if(after.dedicated_bytes!=before.dedicated_bytes)return GpuGateCode::wrong_adapter;
  const auto allowance=(std::min)(gpu_memory_envelope(before,device_bytes).nonlocal_growth,
      gpu_memory_envelope(after).nonlocal_growth);
  if(after.nonlocal_usage>before.nonlocal_usage &&
      after.nonlocal_usage-before.nonlocal_usage>allowance)return GpuGateCode::nonlocal_growth;
  return GpuGateCode::admitted;
}
GpuGateCode GpuResultGate::budget(const GpuMemorySnapshot& s,std::uint64_t now,
    std::uint64_t additional) const noexcept {
  return check_gpu_memory_budget(s,luid_,node_mask_,now,additional);
}
GpuAttempt GpuResultGate::begin(bool consent,const GpuMemorySnapshot& before,std::uint64_t now) noexcept {
  if(state_!=GpuGateState::idle) return {GpuGateCode::wrong_state};
  if(!consent) return {GpuGateCode::no_consent};
  const auto decision=budget(before,now,batch_.device_buffer_bytes());
  if(decision!=GpuGateCode::admitted) return {decision};
  if(now>(std::numeric_limits<std::uint64_t>::max)()-deadline_ms_) return {GpuGateCode::invalid_budget};
  generation_=detail::next_gpu_request_id();
  if(!generation_) return {GpuGateCode::invalid_budget};
  started_=now;deadline_=now+deadline_ms_;baseline_=before;
  state_=GpuGateState::waiting;return {GpuGateCode::admitted,generation_};
}
GpuGateCode GpuResultGate::poison(GpuGateCode why) noexcept {
  state_=GpuGateState::poisoned;accepted_.clear();return why;
}
GpuGateCode GpuResultGate::complete(GpuBatchReply reply,const GpuMemorySnapshot& after,
    std::uint64_t now) noexcept {
  if(state_!=GpuGateState::waiting) return GpuGateCode::wrong_state;
  if(now<started_ || now>=deadline_) return poison(GpuGateCode::expired);
  if(reply.generation!=generation_) return poison(GpuGateCode::stale_result);
  if(after.captured_ms<started_) return poison(GpuGateCode::stale_budget);
  const auto decision=check_gpu_memory_transition(baseline_,after,luid_,node_mask_,now,
      batch_.device_buffer_bytes());
  if(decision!=GpuGateCode::admitted) return poison(decision);
  if(reply.bytes.size()!=batch_.cpu_reference().size() || reply.status.size()!=batch_.tasks().size())
    return poison(GpuGateCode::invalid_output);
  if(std::any_of(reply.status.begin(),reply.status.end(),[](auto s){return s!=gpu_detail::success;}))
    return poison(GpuGateCode::task_failed);
  if(!std::equal(reply.bytes.begin(),reply.bytes.end(),batch_.cpu_reference().begin()))
    return poison(GpuGateCode::mismatch);
  accepted_=std::move(reply.bytes);state_=GpuGateState::accepted;return GpuGateCode::accepted;
}
GpuGateCode GpuResultGate::expire(std::uint64_t now) noexcept {
  if(state_!=GpuGateState::waiting) return GpuGateCode::wrong_state;
  return now<started_ || now>=deadline_?poison(GpuGateCode::expired):GpuGateCode::admitted;
}
GpuGateCode GpuResultGate::worker_failed() noexcept {
  return state_==GpuGateState::waiting?poison(GpuGateCode::worker_failed):GpuGateCode::wrong_state;
}
std::span<const std::byte> GpuResultGate::use_cpu_fallback() noexcept {
  (void)poison(GpuGateCode::worker_failed);
  return batch_.cpu_reference();
}
std::span<const std::byte> GpuResultGate::accepted_bytes() const noexcept {
  return state_==GpuGateState::accepted?std::span<const std::byte>(accepted_):std::span<const std::byte>{};
}
} // namespace flitzfs
