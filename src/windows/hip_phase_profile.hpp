#pragma once
#include "flitzfs/match_worker_wire.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace flitzfs {
// Explicit diagnostic wall time, never device-event/kernel-only timing.
enum class HipPhase : unsigned {
  request_decode,bundle_verify,adapter_admit,modules_load,entry_bind,
  device_initialize,stream_allocate,host_to_device,kernel_wait,device_to_host,
  cpu_verification,post_admission,resources_release,reply_encode,modules_release,count
};
inline constexpr unsigned hip_phase_count=static_cast<unsigned>(HipPhase::count);
inline constexpr std::uint32_t hip_phase_complete=(1U<<hip_phase_count)-1;
inline constexpr std::uint64_t hip_profile_limit_ns=5'000'000'000ULL;
inline constexpr std::array<std::string_view,hip_phase_count> hip_phase_names{
  "request_decode","bundle_verify","adapter_admit","modules_load","entry_bind",
  "device_initialize","stream_allocate","host_to_device","kernel_wait","device_to_host",
  "cpu_verification","post_admission","resources_release","reply_encode","modules_release"};
struct HipPhaseTrace {
  std::array<std::uint64_t,hip_phase_count> wall_ns{};
  std::uint64_t total_wall_ns=0;
  std::uint32_t completed=0;
  bool finished=false;
};
namespace hip_phase_detail {
inline void require(bool value){if(!value)throw std::invalid_argument("invalid HIP phase profile");}
inline std::uint64_t clock_ns() {
  const auto value=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  require(value>=0);return static_cast<std::uint64_t>(value);
}
}
// One serial recorder. Disabled observation performs ZERO clock calls.
// A failed/open phase cannot be reused or emitted as a complete measurement.
class HipPhaseRecorder final {
 public:
  using Clock=std::uint64_t(*)();
  explicit HipPhaseRecorder(HipPhaseTrace* output=nullptr,Clock clock=hip_phase_detail::clock_ns)
      :output_(output),clock_(clock) {
    if(!output_)return;
    hip_phase_detail::require(clock_ && output_->completed==0 && !output_->finished && output_->total_wall_ns==0 &&
        std::all_of(output_->wall_ns.begin(),output_->wall_ns.end(),[](auto n){return n==0;}));
    origin_=last_=clock_();
  }
  HipPhaseRecorder(const HipPhaseRecorder&)=delete;
  HipPhaseRecorder& operator=(const HipPhaseRecorder&)=delete;
  HipPhaseRecorder(HipPhaseRecorder&&)=delete;
  HipPhaseRecorder& operator=(HipPhaseRecorder&&)=delete;
  void begin(HipPhase phase) {
    if(!output_)return;
    const auto n=static_cast<unsigned>(phase);
    check(!finished_ && !failed_ && n<hip_phase_count && active_==hip_phase_count &&
        output_->completed==((1U<<n)-1));
    // Poison before querying a fallible clock; no reuse after incomplete work.
    active_=n;started_=read();
  }
  void end(HipPhase phase) {
    if(!output_)return;
    const auto n=static_cast<unsigned>(phase);
    check(!finished_ && !failed_ && n<hip_phase_count && active_==n);
    const auto now=read();
    output_->wall_ns[n]=now-started_;output_->completed|=1U<<n;active_=hip_phase_count;
  }
  void finish() {
    if(!output_)return;
    check(!finished_ && !failed_ && active_==hip_phase_count &&
        output_->completed==hip_phase_complete);
    output_->total_wall_ns=read()-origin_;output_->finished=true;finished_=true;
  }
 private:
  void check(bool value) {
    if(!value){failed_=true;throw std::invalid_argument("invalid HIP phase sequence");}
  }
  std::uint64_t read() {
    std::uint64_t now=0;
    try{now=clock_();}catch(...){failed_=true;throw;}
    check(now>=last_ && now>=origin_ && now-origin_<=hip_profile_limit_ns);
    last_=now;return now;
  }
  HipPhaseTrace* output_;Clock clock_;std::uint64_t origin_=0,last_=0,started_=0;
  unsigned active_=hip_phase_count;bool finished_=false,failed_=false;
};
struct HipProfileBinding {
  std::uint64_t request_id=0;
  std::uint32_t ordinal=0,node_mask=0,input_bytes=0,lanes=0;
  std::array<std::byte,8> luid{};
  std::array<std::byte,32> source_digest{};
  bool operator==(const HipProfileBinding&) const=default;
};
struct HipPhaseProfile {
  HipProfileBinding binding;
  HipPhaseTrace trace;
  WorkerExecution execution=WorkerExecution::cpu_semantics;
};
namespace hip_phase_detail {
using Frame=std::array<std::byte,256>;
inline constexpr std::array<std::byte,8> magic{
  std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'P'},
  std::byte{'H'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
inline void put(Frame& f,std::size_t at,std::uint64_t n,unsigned width=8) {
  for(unsigned i=0;i<width;++i)f[at+i]=static_cast<std::byte>(n>>(8*i));
}
inline std::uint64_t get(std::span<const std::byte> f,std::size_t at,unsigned width=8) {
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(std::to_integer<unsigned>(f[at+i]))<<(8*i);
  return n;
}
inline void binding_valid(const HipProfileBinding& b) {
  require(b.request_id && b.ordinal<64 && b.node_mask && !(b.node_mask&(b.node_mask-1)) &&
      b.input_bytes>=4 && b.input_bytes<=match_detail::input_limit &&
      b.lanes==match_detail::lanes(b.input_bytes) &&
      std::any_of(b.luid.begin(),b.luid.end(),[](auto x){return x!=std::byte{};}));
}
inline void valid(const HipPhaseProfile& p) {
  binding_valid(p.binding);
  require(p.execution==WorkerExecution::cpu_semantics || p.execution==WorkerExecution::hip);
  require(p.trace.finished && p.trace.completed==hip_phase_complete && p.trace.total_wall_ns<=hip_profile_limit_ns);
  std::uint64_t sum=0;
  for(const auto ns:p.trace.wall_ns) {
    require(ns<=p.trace.total_wall_ns && sum<=p.trace.total_wall_ns-ns);sum+=ns;
  }
}
}
inline hip_phase_detail::Frame encode_hip_phase_profile(const HipPhaseProfile& p) {
  using namespace hip_phase_detail;valid(p);Frame f{};std::copy(magic.begin(),magic.end(),f.begin());
  put(f,8,p.binding.request_id);put(f,16,p.binding.ordinal,4);put(f,20,p.binding.node_mask,4);
  std::copy(p.binding.luid.begin(),p.binding.luid.end(),f.begin()+24);
  put(f,32,p.binding.input_bytes,4);put(f,36,p.binding.lanes,4);
  std::copy(p.binding.source_digest.begin(),p.binding.source_digest.end(),f.begin()+40);
  put(f,72,p.trace.total_wall_ns);put(f,80,p.trace.completed|0x80000000U,4);
  put(f,84,static_cast<unsigned>(p.execution),4);
  for(unsigned i=0;i<hip_phase_count;++i)put(f,88+8*i,p.trace.wall_ns[i]);
  return f;
}
inline HipPhaseProfile decode_hip_phase_profile(std::span<const std::byte> f,
    const HipProfileBinding& expected,WorkerExecution execution) {
  using namespace hip_phase_detail;binding_valid(expected);
  require(f.size()==256 && std::equal(magic.begin(),magic.end(),f.begin()) &&
      std::all_of(f.begin()+88+8*hip_phase_count,f.end(),[](auto x){return x==std::byte{};}));
  HipPhaseProfile p;p.binding=expected;
  require(get(f,8)==expected.request_id && get(f,16,4)==expected.ordinal &&
      get(f,20,4)==expected.node_mask &&
      std::equal(expected.luid.begin(),expected.luid.end(),f.begin()+24) &&
      get(f,32,4)==expected.input_bytes && get(f,36,4)==expected.lanes &&
      std::equal(expected.source_digest.begin(),expected.source_digest.end(),f.begin()+40) &&
      get(f,84,4)==static_cast<unsigned>(execution));
  p.execution=execution;p.trace.total_wall_ns=get(f,72);
  require(get(f,80,4)==(hip_phase_complete|0x80000000U));
  p.trace.completed=hip_phase_complete;p.trace.finished=true;
  for(unsigned i=0;i<hip_phase_count;++i)p.trace.wall_ns[i]=get(f,88+8*i);
  valid(p);return p;
}
static_assert(sizeof(hip_phase_detail::Frame)<=kWorkerReplyLimit);
} // namespace flitzfs
