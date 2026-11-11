#pragma once
#include "flitzfs/gpu_batch.hpp"
#include "hip_worker_exit.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
namespace flitzfs {
// Explicit diagnostic metadata only: never a decoder candidate or stored bytes.
struct HipMemoryBinding {
  std::uint64_t request_id=0;
  std::uint32_t ordinal=0,node_mask=0;
  std::array<std::byte,8> luid{};
  std::uint64_t device_bytes=0,decoded_bytes=0;
};
enum HipMemoryFlags : std::uint32_t {
  hip_memory_resources_released=1,hip_memory_modules_retired=2,hip_memory_admitted=4,
  hip_memory_launch_submitted=8,hip_memory_stream_completed=16,hip_memory_statuses_verified=32
};
struct HipMemoryObservation {
  HipMemoryBinding binding;
  std::uint64_t cpu_verified_bytes=0;
  std::uint32_t failure_category=0,failure_stage=0,captured=0,flags=0;
  std::array<GpuMemorySnapshot,3> snapshots{};
};
namespace hip_memory_detail {
using Frame=std::array<std::byte,256>;
inline constexpr std::array<std::byte,8> magic{
    std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'M'},
    std::byte{'E'},std::byte{'M'},std::byte{'0'},std::byte{'1'}};
inline void require(bool ok){if(!ok)throw std::invalid_argument("invalid HIP memory observation");}
inline void put(Frame& frame,std::size_t at,std::uint64_t value,unsigned bytes=8){
  for(unsigned i=0;i<bytes;++i)frame[at+i]=static_cast<std::byte>(value>>(8*i));
}
inline std::uint64_t get(std::span<const std::byte> frame,std::size_t at,unsigned bytes=8){
  std::uint64_t value=0;
  for(unsigned i=0;i<bytes;++i)value|=std::uint64_t(std::to_integer<unsigned>(frame[at+i]))<<(8*i);
  return value;
}
inline void binding_valid(const HipMemoryBinding& b){
  require(b.request_id && b.ordinal<64 && b.node_mask && !(b.node_mask&(b.node_mask-1)) &&
      std::any_of(b.luid.begin(),b.luid.end(),[](auto v){return v!=std::byte{0};}) &&
      b.device_bytes && b.device_bytes<=gpu_detail::max_batch_bytes &&
      b.decoded_bytes && b.decoded_bytes<=gpu_detail::max_batch_bytes);
}
inline void valid(const HipMemoryObservation& v){
  binding_valid(v.binding);
  require(v.captured==0 || v.captured==1 || v.captured==3 || v.captured==7);
  require((v.flags&~63U)==0 && (v.flags&hip_memory_modules_retired)!=0);
  require(v.cpu_verified_bytes==0 || v.cpu_verified_bytes==v.binding.decoded_bytes);
  require(!(v.flags&hip_memory_stream_completed) || (v.flags&hip_memory_launch_submitted));
  require(!(v.flags&hip_memory_statuses_verified) || (v.flags&hip_memory_stream_completed));
  require(!v.cpu_verified_bytes || ((v.flags&56U)==56U && v.captured==7));
  if(v.flags&hip_memory_admitted) {
    require(v.failure_category==0 && v.failure_stage==0 && v.captured==7 &&
        v.cpu_verified_bytes==v.binding.decoded_bytes && (v.flags&hip_memory_resources_released));
  }else require(hip_worker_category_valid(v.failure_category) && v.failure_stage<=31);
  for(unsigned i=0;i<3;++i){
    const auto& s=v.snapshots[i];
    if(!(v.captured&(1U<<i))){
      require(s.luid==std::array<std::byte,8>{} && s.node_mask==0 && s.captured_ms==0 &&
          s.dedicated_bytes==0 && s.local_budget==0 && s.local_usage==0 &&
          s.nonlocal_budget==0 && s.nonlocal_usage==0 && !s.valid && !s.amd_hardware);
    }else if(s.valid)require(s.luid==v.binding.luid && s.node_mask==v.binding.node_mask && s.amd_hardware);
  }
}
}
inline hip_memory_detail::Frame encode_hip_memory_observation(const HipMemoryObservation& v){
  using namespace hip_memory_detail;valid(v);Frame f{};std::copy(magic.begin(),magic.end(),f.begin());
  put(f,8,v.binding.request_id);put(f,16,v.binding.ordinal,4);put(f,20,v.binding.node_mask,4);
  std::copy(v.binding.luid.begin(),v.binding.luid.end(),f.begin()+24);
  put(f,32,v.binding.device_bytes);put(f,40,v.cpu_verified_bytes);
  put(f,48,v.failure_category,4);put(f,52,v.failure_stage,4);put(f,56,v.captured,4);put(f,60,v.flags,4);
  for(unsigned i=0;i<3;++i){
    const auto at=64U+64U*i;const auto& s=v.snapshots[i];
    put(f,at,s.captured_ms);put(f,at+8,s.dedicated_bytes);put(f,at+16,s.local_budget);
    put(f,at+24,s.local_usage);put(f,at+32,s.nonlocal_budget);put(f,at+40,s.nonlocal_usage);
    std::copy(s.luid.begin(),s.luid.end(),f.begin()+at+48);put(f,at+56,s.node_mask,4);
    put(f,at+60,(s.valid?1U:0U)|(s.amd_hardware?2U:0U),4);
  }
  return f;
}
inline HipMemoryObservation decode_hip_memory_observation(std::span<const std::byte> f,
    const HipMemoryBinding& expected){
  using namespace hip_memory_detail;binding_valid(expected);
  require(f.size()==256 && std::equal(magic.begin(),magic.end(),f.begin()));
  require(get(f,8)==expected.request_id && get(f,16,4)==expected.ordinal &&
      get(f,20,4)==expected.node_mask && std::equal(expected.luid.begin(),expected.luid.end(),f.begin()+24) &&
      get(f,32)==expected.device_bytes);
  HipMemoryObservation v;v.binding=expected;v.cpu_verified_bytes=get(f,40);
  v.failure_category=static_cast<std::uint32_t>(get(f,48,4));v.failure_stage=static_cast<std::uint32_t>(get(f,52,4));
  v.captured=static_cast<std::uint32_t>(get(f,56,4));v.flags=static_cast<std::uint32_t>(get(f,60,4));
  for(unsigned i=0;i<3;++i){
    const auto at=64U+64U*i;auto& s=v.snapshots[i];
    s.captured_ms=get(f,at);s.dedicated_bytes=get(f,at+8);s.local_budget=get(f,at+16);
    s.local_usage=get(f,at+24);s.nonlocal_budget=get(f,at+32);s.nonlocal_usage=get(f,at+40);
    std::copy_n(f.begin()+at+48,8,s.luid.begin());s.node_mask=static_cast<std::uint32_t>(get(f,at+56,4));
    const auto flags=get(f,at+60,4);require((flags&~3ULL)==0);
    s.valid=(flags&1)!=0;s.amd_hardware=(flags&2)!=0;
  }
  valid(v);return v;
}
}
