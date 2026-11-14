#pragma once
#include "hip_phase_profile.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"
#include <limits>
namespace flitzfs {
inline constexpr std::size_t kUniformLz4ProfileBytes=256;
struct UniformLz4ProfileBinding {
  std::uint64_t request_id=0;
  std::uint32_t ordinal=0,node_mask=0,maximum_chunks=0,profiled_chunk_bytes=0;
  std::uint64_t maximum_device_bytes=0;
  std::array<std::byte,8> luid{};
  bool operator==(const UniformLz4ProfileBinding&) const=default;
};
struct UniformLz4ProfiledReply {
  HipPhaseTrace trace;
  UniformLz4Candidates candidates;
};
namespace uniform_profile_detail {
inline constexpr std::array<std::byte,8> magic{std::byte{'F'},std::byte{'L'},std::byte{'U'},std::byte{'L'},
    std::byte{'P'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
inline void require(bool ok){if(!ok)throw std::invalid_argument("invalid uniform LZ4 phase envelope");}
inline void put(std::span<std::byte> f,std::size_t at,std::uint64_t n,unsigned width=8){
  for(unsigned i=0;i<width;++i)f[at+i]=static_cast<std::byte>(n>>(8*i));
}
inline std::uint64_t get(UniformLz4Bytes f,std::size_t at,unsigned width=8){
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(std::to_integer<unsigned>(f[at+i]))<<(8*i);return n;
}
inline void binding(const PreparedUniformLz4& batch,const UniformLz4ProfileBinding& b){
  require(batch.ready() && b.request_id && b.ordinal<64 && b.node_mask && !(b.node_mask&(b.node_mask-1)) &&
      std::any_of(b.luid.begin(),b.luid.end(),[](auto x){return x!=std::byte{0};}) &&
      b.maximum_chunks>=batch.layout().chunks && b.profiled_chunk_bytes==batch.layout().chunk_bytes &&
      b.maximum_device_bytes>=batch.layout().total_bytes &&
      batch.limits().reply_bytes<=std::uint64_t((std::numeric_limits<std::int32_t>::max)())-kUniformLz4ProfileBytes);
}
inline void trace(const HipPhaseTrace& t){
  require(t.finished && t.completed==hip_phase_complete && t.total_wall_ns<=hip_profile_limit_ns);
  std::uint64_t sum=0;for(const auto n:t.wall_ns){
    require(n<=t.total_wall_ns && sum<=t.total_wall_ns-n);sum+=n;
  }
}
}
inline std::vector<std::byte> encode_uniform_lz4_profile_reply(const PreparedUniformLz4& batch,
    const UniformLz4ProfileBinding& b,const HipPhaseTrace& trace,UniformLz4Bytes ordinary,WorkerExecution execution){
  using namespace uniform_profile_detail;binding(batch,b);uniform_profile_detail::trace(trace);
  verify_uniform_lz4_reply(ordinary,batch,b.request_id,execution);
  std::vector<std::byte> out(kUniformLz4ProfileBytes);
  std::copy(magic.begin(),magic.end(),out.begin());
  put(out,8,b.request_id);put(out,16,b.ordinal,4);put(out,20,b.node_mask,4);
  std::copy(b.luid.begin(),b.luid.end(),out.begin()+24);
  put(out,32,batch.layout().input_bytes);put(out,40,batch.layout().chunks,4);put(out,44,batch.layout().chunk_bytes,4);
  std::copy(batch.digest().begin(),batch.digest().end(),out.begin()+48);
  put(out,80,trace.total_wall_ns);put(out,88,trace.completed|0x80000000U,4);
  put(out,92,static_cast<std::uint32_t>(execution),4);
  for(unsigned i=0;i<hip_phase_count;++i)put(out,96+8*i,trace.wall_ns[i]);
  put(out,216,batch.layout().total_bytes);put(out,224,b.maximum_chunks,4);put(out,228,b.profiled_chunk_bytes,4);
  put(out,232,b.maximum_device_bytes);put(out,240,ordinary.size());
  out.insert(out.end(),ordinary.begin(),ordinary.end());return out;
}
inline UniformLz4ProfiledReply accept_uniform_lz4_profile_reply(UniformLz4Bytes f,const PreparedUniformLz4& batch,
    const UniformLz4ProfileBinding& b,WorkerExecution execution){
  using namespace uniform_profile_detail;binding(batch,b);
  require(f.size()>=kUniformLz4ProfileBytes+kUniformLz4HeaderBytes &&
      f.size()<=batch.limits().reply_bytes+kUniformLz4ProfileBytes &&
      std::equal(magic.begin(),magic.end(),f.begin()) &&
      get(f,8)==b.request_id && get(f,16,4)==b.ordinal && get(f,20,4)==b.node_mask &&
      std::equal(b.luid.begin(),b.luid.end(),f.begin()+24) &&
      get(f,32)==batch.layout().input_bytes && get(f,40,4)==batch.layout().chunks &&
      get(f,44,4)==batch.layout().chunk_bytes &&
      std::equal(batch.digest().begin(),batch.digest().end(),f.begin()+48) &&
      get(f,88,4)==(hip_phase_complete|0x80000000U) && get(f,92,4)==static_cast<std::uint32_t>(execution) &&
      get(f,216)==batch.layout().total_bytes && get(f,224,4)==b.maximum_chunks &&
      get(f,228,4)==b.profiled_chunk_bytes && get(f,232)==b.maximum_device_bytes &&
      get(f,240)==f.size()-kUniformLz4ProfileBytes && get(f,248)==0);
  UniformLz4ProfiledReply result;result.trace.total_wall_ns=get(f,80);
  result.trace.completed=hip_phase_complete;result.trace.finished=true;
  for(unsigned i=0;i<hip_phase_count;++i)result.trace.wall_ns[i]=get(f,96+8*i);
  uniform_profile_detail::trace(result.trace);
  result.candidates=accept_uniform_lz4_reply(f.subspan(kUniformLz4ProfileBytes),batch,b.request_id,execution);
  return result;
}
}
