#pragma once
#include "flitzfs/uniform_lz4_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <stdexcept>
namespace flitzfs {
// One already-admitted immutable request; serial device quanta, one combined
// reply. This plan does not grant host memory or authorize runtime discovery.
struct UniformLz4SweepPlan {
  UniformLz4Limits whole{};
  std::uint32_t quantum_chunks=0,windows=0;
  std::uint64_t device_peak_bytes=0;
  bool operator==(const UniformLz4SweepPlan&) const=default;
};
inline UniformLz4SweepPlan plan_uniform_lz4_sweep(const PreparedUniformLz4& source,
    std::uint32_t maximum_chunks,std::uint64_t maximum_device_bytes){
  if(!source.ready() || !maximum_chunks || !maximum_device_bytes)
    throw std::invalid_argument("invalid uniform sweep grant");
  const auto& shape=source.layout();
  std::uint32_t low=1,high=(std::min)(shape.chunks,maximum_chunks),selected=0;
  std::uint64_t peak=0;
  while(low<=high){
    const auto count=low+(high-low)/2;
    const auto bytes=(std::min)(shape.input_bytes,std::uint64_t(count)*shape.chunk_bytes);
    uniform_lz4::Layout part{};
    if(uniform_lz4::make_layout(bytes,shape.chunk_bytes,part) && part.total_bytes<=maximum_device_bytes){
      selected=count;peak=part.total_bytes;
      if(count==high)break;
      low=count+1;
    }else high=count-1;
  }
  if(!selected)throw std::invalid_argument("uniform sweep cannot admit one quantum");
  return {source.limits(),selected,1+(shape.chunks-1)/selected,peak};
}
// Callback completes ONE quantum before another begins, returning independently
// verifiable candidates. It must synchronize device work before returning. A
// bounded caller may reuse its stream/slab, but must retire them before publishing
// the combined result. No automatic retry or partial publication. Successful
// child exit remains the parent acceptance boundary. Charge the whole source,
// result, one prepared quantum, compute scratch and transport BEFORE entry.
template<class Compute>
UniformLz4Candidates compute_uniform_lz4_sweep(const UniformLz4Request& request,
    const UniformLz4SweepPlan& plan,Compute&& compute){
  if(!request.request_id || !request.batch.ready() ||
      plan.whole!=request.batch.limits() || plan.quantum_chunks==0 ||
      plan!=plan_uniform_lz4_sweep(request.batch,plan.quantum_chunks,plan.device_peak_bytes))
    throw std::invalid_argument("uniform sweep plan/source mismatch");
  if(!gpu_lz4_cpu_verification_available())
    throw std::runtime_error("independent CPU uniform sweep verification unavailable");
  UniformLz4Candidates result;
  const auto capacity=plan.whole.reply_bytes-kUniformLz4HeaderBytes-std::uint64_t(plan.whole.chunks)*8;
  result.sizes.reserve(plan.whole.chunks);
  result.payload.reserve(static_cast<std::size_t>(capacity));
  std::uint64_t offset=0;
  for(std::uint32_t window=0;window<plan.windows;++window){
    const auto length=(std::min)(plan.whole.input_bytes-offset,
        std::uint64_t(plan.quantum_chunks)*plan.whole.chunk_bytes);
    uniform_lz4::Layout layout{};
    if(!length || !uniform_lz4::make_layout(length,plan.whole.chunk_bytes,layout) ||
        layout.total_bytes>plan.device_peak_bytes)
      throw std::invalid_argument("uniform sweep quantum geometry");
    UniformLz4Request part{request.request_id,PreparedUniformLz4::prepare(
        request.batch.plaintext().subspan(static_cast<std::size_t>(offset),static_cast<std::size_t>(length)),
        uniform_lz4_limits(layout))};
    auto candidate=compute(part);
    verify_uniform_lz4_candidates(part.batch,candidate);
    if(candidate.payload.size()>capacity-result.payload.size() ||
        candidate.sizes.size()>plan.whole.chunks-result.sizes.size())
      throw std::invalid_argument("uniform sweep cumulative result bounds");
    result.payload.insert(result.payload.end(),candidate.payload.begin(),candidate.payload.end());
    result.sizes.insert(result.sizes.end(),candidate.sizes.begin(),candidate.sizes.end());
    offset+=length;
  }
  if(offset!=plan.whole.input_bytes)
    throw std::invalid_argument("uniform sweep incomplete source");
  verify_uniform_lz4_candidates(request.batch,result);
  return result;
}
}
