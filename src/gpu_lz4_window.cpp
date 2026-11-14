#include "flitzfs/gpu_lz4_window.hpp"
#include <algorithm>
#include <limits>
namespace flitzfs {
GpuLz4WindowPlan plan_gpu_lz4_window(std::uint64_t requested,std::uint32_t chunk_bytes,
    const GpuHealthEvidence& evidence,std::uint64_t now,const GpuLz4ResidentCost& cost,GpuLz4WindowPolicy policy) noexcept {
  GpuLz4WindowPlan result;result.remaining_input_bytes=requested;
  const auto divisor=policy.memory_share_divisor;
  const auto& memory=evidence.memory;const auto luid=memory.luid;const auto mask=memory.node_mask;
  result.code=check_gpu_memory_budget(memory,luid,mask,now,0);
  if(result.code!=GpuGateCode::admitted)return result;
  result.code=GpuGateCode::invalid_budget;
  if(!requested || !chunk_bytes || chunk_bytes>uniform_lz4::max_chunk_bytes ||
      divisor<16 || !cost.qualified || !cost.fixed_bytes || !cost.input_copies ||
      !evidence.compatible || !evidence.sequence ||
      cost.configuration!=evidence.configuration ||
      std::none_of(evidence.configuration.begin(),evidence.configuration.end(),[](auto b){return b!=std::byte{0};}) ||
      cost.recovery_generation!=evidence.recovery_generation || cost.profiled_chunk_bytes!=chunk_bytes ||
      !cost.maximum_grid_x || !cost.kernel_ns_per_chunk || !policy.kernel_quantum_ns ||
      policy.kernel_quantum_ns>cost.qualified_kernel_quantum_ns)return result;
  const auto envelope=gpu_memory_envelope(memory);
  if(envelope.code!=GpuGateCode::admitted){result.code=envelope.code;return result;}
  const auto local=memory.local_budget-memory.local_usage-envelope.local_reserve;
  const auto physical=memory.physical_available-envelope.physical_reserve;
  const auto commit=memory.commit_available-envelope.commit_reserve;
  const auto shared=memory.nonlocal_budget-memory.nonlocal_usage-memory.nonlocal_budget/16;
  const auto device_allowance=(std::min)({local,physical,commit,shared})/divisor;
  const auto host_allowance=(std::min)(physical,commit)/divisor;
  result.code=GpuGateCode::insufficient_headroom;
  auto low=std::uint64_t{1};
  auto high=(std::min)({requested/chunk_bytes+(requested%chunk_bytes!=0),
      std::uint64_t(cost.maximum_grid_x),
      policy.kernel_quantum_ns/cost.kernel_ns_per_chunk});
  // At most 32 trials; no allocation, polling, timers, scans or runtime calls.
  while(low<=high){
    const auto count=low+(high-low)/2;
    const auto bytes=count>requested/chunk_bytes?requested:count*chunk_bytes;
    uniform_lz4::Layout layout{};std::uint64_t peak=cost.fixed_bytes,part=0;
    bool valid=uniform_lz4::make_layout(bytes,chunk_bytes,layout);
    const auto charge=[&](std::uint64_t amount,std::uint64_t copies) noexcept {
      if(!uniform_lz4::multiply(amount,copies,part) || !uniform_lz4::add(peak,part,peak))return false;
      return true;
    };
    valid=valid && charge(bytes,cost.input_copies) &&
        charge(layout.total_bytes,cost.slab_copies) &&
        charge(layout.output_stride*layout.chunks,cost.output_copies) &&
        charge(layout.chunks,cost.per_chunk_bytes);
    if(valid && layout.total_bytes<=device_allowance && peak<=host_allowance &&
        check_gpu_memory_budget(memory,luid,mask,now,layout.total_bytes)==GpuGateCode::admitted){
      result={GpuGateCode::admitted,layout,peak,requested-bytes,count*cost.kernel_ns_per_chunk};low=count+1;
    }else high=count-1;
  }
  return result;
}
}
