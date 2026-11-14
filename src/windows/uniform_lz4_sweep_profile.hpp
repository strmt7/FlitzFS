#pragma once
#include "uniform_lz4_profile.hpp"
#include "uniform_lz4_sweep_trace.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
namespace flitzfs {
inline constexpr std::size_t kUniformLz4SweepProfileBytes=256;
struct UniformLz4SweepProfiledReply {
  UniformLz4SweepTrace trace;
  UniformLz4Candidates candidates;
};
namespace uniform_sweep_profile_detail {
using namespace uniform_profile_detail;
inline constexpr std::array<std::byte,8> sweep_magic{std::byte{'F'},std::byte{'L'},std::byte{'U'},std::byte{'S'},
    std::byte{'P'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
inline UniformLz4SweepPlan binding(const PreparedUniformLz4& batch,const UniformLz4ProfileBinding& b){
  require(batch.ready() && b.request_id && b.ordinal<64 && b.node_mask && !(b.node_mask&(b.node_mask-1)) &&
      std::any_of(b.luid.begin(),b.luid.end(),[](auto x){return x!=std::byte{0};}) &&
      b.maximum_chunks && b.profiled_chunk_bytes==batch.layout().chunk_bytes &&
      batch.limits().reply_bytes<=std::uint64_t((std::numeric_limits<std::int32_t>::max)())-kUniformLz4SweepProfileBytes);
  return plan_uniform_lz4_sweep(batch,b.maximum_chunks,b.maximum_device_bytes);
}
inline void trace(const UniformLz4SweepTrace& t,const UniformLz4SweepPlan& p){
  require(t.finished && t.completed_windows==p.windows && t.total_wall_ns<=hip_profile_limit_ns);
  std::uint64_t sum=0;for(const auto ns:t.wall_ns){
    require(ns<=t.total_wall_ns && sum<=t.total_wall_ns-ns);sum+=ns;
  }
  const auto kernel=t.wall_ns[static_cast<unsigned>(UniformSweepPhase::kernel_wait)];
  require(t.minimum_kernel_wall_ns<=t.maximum_kernel_wall_ns &&
      t.maximum_kernel_wall_ns<=kernel && t.maximum_kernel_wall_ns<=t.maximum_quantum_wall_ns &&
      t.maximum_quantum_wall_ns<=t.total_wall_ns);
  std::uint64_t rest=0,lower=0,upper=0;
  require(uniform_lz4::multiply(t.minimum_kernel_wall_ns,p.windows-1,rest) &&
      uniform_lz4::add(t.maximum_kernel_wall_ns,rest,lower) &&
      uniform_lz4::multiply(t.maximum_kernel_wall_ns,p.windows,upper) && kernel>=lower && kernel<=upper);
}
}
inline std::vector<std::byte> encode_uniform_lz4_sweep_profile_reply(const PreparedUniformLz4& batch,
    const UniformLz4ProfileBinding& b,const UniformLz4SweepTrace& trace,UniformLz4Bytes ordinary,WorkerExecution execution){
  using namespace uniform_sweep_profile_detail;
  const auto plan=uniform_sweep_profile_detail::binding(batch,b);
  uniform_sweep_profile_detail::trace(trace,plan);
  verify_uniform_lz4_reply(ordinary,batch,b.request_id,execution);
  std::vector<std::byte> out(kUniformLz4SweepProfileBytes);
  std::copy(sweep_magic.begin(),sweep_magic.end(),out.begin());
  put(out,8,b.request_id);put(out,16,b.ordinal,4);put(out,20,b.node_mask,4);
  std::copy(b.luid.begin(),b.luid.end(),out.begin()+24);
  put(out,32,batch.layout().input_bytes);put(out,40,batch.layout().chunks,4);put(out,44,batch.layout().chunk_bytes,4);
  std::copy(batch.digest().begin(),batch.digest().end(),out.begin()+48);
  put(out,80,trace.total_wall_ns);put(out,88,trace.completed_windows,4);put(out,92,static_cast<unsigned>(execution),4);
  for(unsigned i=0;i<uniform_sweep_phase_count;++i)put(out,96+8*i,trace.wall_ns[i]);
  put(out,168,trace.maximum_kernel_wall_ns);put(out,176,trace.minimum_kernel_wall_ns);
  put(out,184,trace.maximum_quantum_wall_ns);put(out,192,plan.quantum_chunks,4);put(out,196,plan.windows,4);
  put(out,200,plan.device_peak_bytes);put(out,208,b.maximum_chunks,4);put(out,212,b.profiled_chunk_bytes,4);
  put(out,216,b.maximum_device_bytes);put(out,224,ordinary.size());put(out,232,batch.layout().total_bytes);
  put(out,240,1);out.insert(out.end(),ordinary.begin(),ordinary.end());return out;
}
inline UniformLz4SweepProfiledReply accept_uniform_lz4_sweep_profile_reply(UniformLz4Bytes f,
    const PreparedUniformLz4& batch,const UniformLz4ProfileBinding& b,WorkerExecution execution){
  using namespace uniform_sweep_profile_detail;
  const auto plan=uniform_sweep_profile_detail::binding(batch,b);
  require(f.size()>=kUniformLz4SweepProfileBytes+kUniformLz4HeaderBytes &&
      f.size()<=batch.limits().reply_bytes+kUniformLz4SweepProfileBytes &&
      std::equal(sweep_magic.begin(),sweep_magic.end(),f.begin()) &&
      get(f,8)==b.request_id && get(f,16,4)==b.ordinal && get(f,20,4)==b.node_mask &&
      std::equal(b.luid.begin(),b.luid.end(),f.begin()+24) &&
      get(f,32)==batch.layout().input_bytes && get(f,40,4)==batch.layout().chunks &&
      get(f,44,4)==batch.layout().chunk_bytes &&
      std::equal(batch.digest().begin(),batch.digest().end(),f.begin()+48) &&
      get(f,92,4)==static_cast<unsigned>(execution) && get(f,192,4)==plan.quantum_chunks &&
      get(f,196,4)==plan.windows && get(f,200)==plan.device_peak_bytes &&
      get(f,208,4)==b.maximum_chunks && get(f,212,4)==b.profiled_chunk_bytes &&
      get(f,216)==b.maximum_device_bytes && get(f,224)==f.size()-kUniformLz4SweepProfileBytes &&
      get(f,232)==batch.layout().total_bytes && get(f,240)==1 && get(f,248)==0);
  UniformLz4SweepProfiledReply result;auto& t=result.trace;
  t.total_wall_ns=get(f,80);t.completed_windows=static_cast<std::uint32_t>(get(f,88,4));t.finished=true;
  for(unsigned i=0;i<uniform_sweep_phase_count;++i)t.wall_ns[i]=get(f,96+8*i);
  t.maximum_kernel_wall_ns=get(f,168);t.minimum_kernel_wall_ns=get(f,176);t.maximum_quantum_wall_ns=get(f,184);
  uniform_sweep_profile_detail::trace(t,plan);
  result.candidates=accept_uniform_lz4_reply(f.subspan(kUniformLz4SweepProfileBytes),batch,b.request_id,execution);
  return result;
}
}
