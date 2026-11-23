#pragma once
#include "uniform_lz4_profile.hpp"
#include "uniform_lz4_sweep_trace.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
namespace flitzfs {
inline constexpr std::size_t kUniformLz4SweepProfileBytes=256;
inline constexpr std::size_t kUniformLz4SweepTimelineBytes=336;
enum class UniformLz4SweepProfileMode {aggregate,timeline};
inline constexpr std::size_t uniform_lz4_sweep_profile_bytes(UniformLz4SweepProfileMode mode) {
  return mode==UniformLz4SweepProfileMode::timeline?kUniformLz4SweepTimelineBytes:kUniformLz4SweepProfileBytes;
}
struct UniformLz4SweepProfiledReply {
  UniformLz4SweepTrace trace;
  UniformLz4Candidates candidates;
};
namespace uniform_sweep_profile_detail {
using namespace uniform_profile_detail;
inline constexpr std::array<std::byte,8> sweep_magic{std::byte{'F'},std::byte{'L'},std::byte{'U'},std::byte{'S'},
    std::byte{'P'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
inline UniformLz4SweepPlan binding(const PreparedUniformLz4& batch,const UniformLz4ProfileBinding& b,std::size_t header){
  require(batch.ready() && b.request_id && b.ordinal<64 && b.node_mask && !(b.node_mask&(b.node_mask-1)) &&
      std::any_of(b.luid.begin(),b.luid.end(),[](auto x){return x!=std::byte{0};}) &&
      b.maximum_chunks && b.profiled_chunk_bytes==batch.layout().chunk_bytes &&
      batch.limits().reply_bytes<=std::uint64_t((std::numeric_limits<std::int32_t>::max)())-header);
  return plan_uniform_lz4_sweep(batch,b.maximum_chunks,b.maximum_device_bytes);
}
inline void kernel_interval(const UniformLz4KernelInterval& v,const UniformLz4SweepTrace& t) {
  require(v.host_wall_ns>=t.minimum_kernel_wall_ns && v.host_wall_ns<=t.maximum_kernel_wall_ns &&
      v.launch_call_ns<=v.host_wall_ns && v.sync_call_ns<=v.host_wall_ns-v.launch_call_ns &&
      v.stream_interval_ns<=hip_profile_limit_ns);
}
inline void put_kernel_interval(std::vector<std::byte>& frame,std::size_t offset,const UniformLz4KernelInterval& v) {
  put(frame,offset,v.window);put(frame,offset+8,v.host_wall_ns);put(frame,offset+16,v.launch_call_ns);
  put(frame,offset+24,v.sync_call_ns);put(frame,offset+32,v.stream_interval_ns);
}
inline UniformLz4KernelInterval get_kernel_interval(UniformLz4Bytes frame,std::size_t offset) {
  return {get(frame,offset),get(frame,offset+8),get(frame,offset+16),get(frame,offset+24),get(frame,offset+32)};
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
  if(!t.timeline)return;
  const auto& first=t.timeline->first;
  const auto& later=t.timeline->slowest_later;
  kernel_interval(first,t);
  require(first.window==0);
  if(p.windows==1){
    require(later==UniformLz4KernelInterval{} && first.host_wall_ns==kernel &&
        first.host_wall_ns==t.minimum_kernel_wall_ns && first.host_wall_ns==t.maximum_kernel_wall_ns);
    return;
  }
  kernel_interval(later,t);
  require(later.window>0 && later.window<p.windows && (std::max)(first.host_wall_ns,later.host_wall_ns)==t.maximum_kernel_wall_ns);
  const auto tail=kernel-first.host_wall_ns;
  require(uniform_lz4::multiply(t.minimum_kernel_wall_ns,p.windows-2,rest) &&
      uniform_lz4::add(later.host_wall_ns,rest,lower) &&
      uniform_lz4::multiply(later.host_wall_ns,p.windows-1,upper) && tail>=lower && tail<=upper);
  if(p.windows==2)require(t.minimum_kernel_wall_ns==(std::min)(first.host_wall_ns,later.host_wall_ns));
}
}
inline std::vector<std::byte> encode_uniform_lz4_sweep_profile_reply(const PreparedUniformLz4& batch,
    const UniformLz4ProfileBinding& b,const UniformLz4SweepTrace& trace,UniformLz4Bytes ordinary,WorkerExecution execution,
    UniformLz4SweepProfileMode mode=UniformLz4SweepProfileMode::aggregate){
  using namespace uniform_sweep_profile_detail;
  const auto header=uniform_lz4_sweep_profile_bytes(mode);
  const auto plan=uniform_sweep_profile_detail::binding(batch,b,header);
  require(trace.timeline.has_value()==(mode==UniformLz4SweepProfileMode::timeline));
  uniform_sweep_profile_detail::trace(trace,plan);
  verify_uniform_lz4_reply(ordinary,batch,b.request_id,execution);
  std::vector<std::byte> out(header);
  std::copy(sweep_magic.begin(),sweep_magic.end(),out.begin());
  if(trace.timeline)out[7]=std::byte{'2'};
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
  put(out,240,1);
  if(trace.timeline){
    put_kernel_interval(out,256,trace.timeline->first);
    put_kernel_interval(out,296,trace.timeline->slowest_later);
  }
  out.insert(out.end(),ordinary.begin(),ordinary.end());return out;
}
inline UniformLz4SweepProfiledReply accept_uniform_lz4_sweep_profile_reply(UniformLz4Bytes f,
    const PreparedUniformLz4& batch,const UniformLz4ProfileBinding& b,WorkerExecution execution,
    UniformLz4SweepProfileMode mode=UniformLz4SweepProfileMode::aggregate){
  using namespace uniform_sweep_profile_detail;
  const auto header=uniform_lz4_sweep_profile_bytes(mode);
  const auto plan=uniform_sweep_profile_detail::binding(batch,b,header);
  auto magic=sweep_magic;
  if(mode==UniformLz4SweepProfileMode::timeline)magic.back()=std::byte{'2'};
  require(f.size()>=header+kUniformLz4HeaderBytes &&
      f.size()<=batch.limits().reply_bytes+header &&
      std::equal(magic.begin(),magic.end(),f.begin()) &&
      get(f,8)==b.request_id && get(f,16,4)==b.ordinal && get(f,20,4)==b.node_mask &&
      std::equal(b.luid.begin(),b.luid.end(),f.begin()+24) &&
      get(f,32)==batch.layout().input_bytes && get(f,40,4)==batch.layout().chunks &&
      get(f,44,4)==batch.layout().chunk_bytes &&
      std::equal(batch.digest().begin(),batch.digest().end(),f.begin()+48) &&
      get(f,92,4)==static_cast<unsigned>(execution) && get(f,192,4)==plan.quantum_chunks &&
      get(f,196,4)==plan.windows && get(f,200)==plan.device_peak_bytes &&
      get(f,208,4)==b.maximum_chunks && get(f,212,4)==b.profiled_chunk_bytes &&
      get(f,216)==b.maximum_device_bytes && get(f,224)==f.size()-header &&
      get(f,232)==batch.layout().total_bytes && get(f,240)==1 && get(f,248)==0);
  UniformLz4SweepProfiledReply result;auto& t=result.trace;
  t.total_wall_ns=get(f,80);t.completed_windows=static_cast<std::uint32_t>(get(f,88,4));t.finished=true;
  for(unsigned i=0;i<uniform_sweep_phase_count;++i)t.wall_ns[i]=get(f,96+8*i);
  t.maximum_kernel_wall_ns=get(f,168);t.minimum_kernel_wall_ns=get(f,176);t.maximum_quantum_wall_ns=get(f,184);
  if(mode==UniformLz4SweepProfileMode::timeline)
    t.timeline=UniformLz4KernelTimeline{get_kernel_interval(f,256),get_kernel_interval(f,296)};
  uniform_sweep_profile_detail::trace(t,plan);
  result.candidates=accept_uniform_lz4_reply(f.subspan(header),batch,b.request_id,execution);
  return result;
}
}
