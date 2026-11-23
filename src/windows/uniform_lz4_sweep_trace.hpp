#pragma once
#include "hip_phase_profile.hpp"
#include <optional>
namespace flitzfs {
// Host wall time around serialized, synchronized work. These are observations,
// not device-event times, resource grants, watchdog limits or qualified costs.
enum class UniformSweepPhase : unsigned {
  setup,admission,prepare,upload,kernel_wait,download,verification,completion,retirement,count
};
inline constexpr unsigned uniform_sweep_phase_count=static_cast<unsigned>(UniformSweepPhase::count);
inline constexpr std::array<std::string_view,uniform_sweep_phase_count> uniform_sweep_phase_names{
  "setup","admission","prepare","upload","kernel_wait","download","verification","completion","retirement"};
struct UniformLz4KernelInterval {
  std::uint64_t window=0,host_wall_ns=0,launch_call_ns=0,sync_call_ns=0,stream_interval_ns=0;
  bool operator==(const UniformLz4KernelInterval&) const = default;
};
struct UniformLz4KernelTimeline {
  UniformLz4KernelInterval first,slowest_later;
  bool operator==(const UniformLz4KernelTimeline&) const = default;
};
struct UniformLz4SweepTrace {
  std::array<std::uint64_t,uniform_sweep_phase_count> wall_ns{};
  std::uint64_t total_wall_ns=0,maximum_kernel_wall_ns=0,minimum_kernel_wall_ns=0,maximum_quantum_wall_ns=0;
  std::uint32_t completed_windows=0;
  bool finished=false;
  std::optional<UniformLz4KernelTimeline> timeline;
};
// Fixed state, no allocations. An enabled trace requires setup once, each full
// quantum in order, successful retirement once, then finish. Disabled recording
// makes ZERO clock calls. Any clock/ordering failure permanently poisons it.
class UniformLz4SweepRecorder final {
 public:
  using Clock=HipPhaseRecorder::Clock;
  UniformLz4SweepRecorder(UniformLz4SweepTrace* output,std::uint32_t windows,
      Clock clock=hip_phase_detail::clock_ns):output_(output),clock_(clock),windows_(windows) {
    if(!output_)return;
    check(clock_ && windows_ && !output_->finished && !output_->completed_windows &&
        !output_->total_wall_ns && !output_->maximum_kernel_wall_ns &&
        !output_->minimum_kernel_wall_ns && !output_->maximum_quantum_wall_ns && !output_->timeline &&
        std::all_of(output_->wall_ns.begin(),output_->wall_ns.end(),[](auto n){return n==0;}));
    origin_=last_=clock_();
  }
  UniformLz4SweepRecorder(const UniformLz4SweepRecorder&)=delete;
  UniformLz4SweepRecorder& operator=(const UniformLz4SweepRecorder&)=delete;
  void begin(UniformSweepPhase phase) {
    if(!output_)return;
    check(!failed_ && !output_->finished && active_==uniform_sweep_phase_count &&
        static_cast<unsigned>(phase)==next_ && next_<uniform_sweep_phase_count);
    active_=next_;started_=read();
    if(phase==UniformSweepPhase::admission)quantum_started_=started_;
  }
  template<class Call> std::uint64_t time_kernel_call(Call&& call) {
    check(output_ && !failed_ && active_==static_cast<unsigned>(UniformSweepPhase::kernel_wait));
    const auto start=read();
    call();
    return read()-start;
  }
  void end(UniformSweepPhase phase,std::optional<UniformLz4KernelInterval> interval=std::nullopt) {
    if(!output_)return;
    const auto n=static_cast<unsigned>(phase);
    check(!failed_ && !output_->finished && active_==n && n<uniform_sweep_phase_count &&
        (!interval || phase==UniformSweepPhase::kernel_wait));
    const auto now=read(),elapsed=now-started_;
    check(output_->wall_ns[n]<=hip_profile_limit_ns-elapsed);
    output_->wall_ns[n]+=elapsed;
    if(phase==UniformSweepPhase::kernel_wait){
      const auto window=output_->completed_windows;
      check(!window || interval.has_value()==output_->timeline.has_value());
      if(interval){
        check(interval->launch_call_ns<=elapsed && interval->sync_call_ns<=elapsed-interval->launch_call_ns &&
            interval->stream_interval_ns<=hip_profile_limit_ns);
        interval->window=window;
        interval->host_wall_ns=elapsed;
        if(!window)output_->timeline.emplace(UniformLz4KernelTimeline{*interval,{}});
        else if(window==1 || elapsed>output_->timeline->slowest_later.host_wall_ns)
          output_->timeline->slowest_later=*interval;
      }
      if(!window)output_->minimum_kernel_wall_ns=elapsed;
      else output_->minimum_kernel_wall_ns=(std::min)(output_->minimum_kernel_wall_ns,elapsed);
      output_->maximum_kernel_wall_ns=(std::max)(output_->maximum_kernel_wall_ns,elapsed);
    }
    active_=uniform_sweep_phase_count;
    if(phase==UniformSweepPhase::completion){
      output_->maximum_quantum_wall_ns=(std::max)(output_->maximum_quantum_wall_ns,now-quantum_started_);
      ++output_->completed_windows;
      next_=output_->completed_windows<windows_?static_cast<unsigned>(UniformSweepPhase::admission):
          static_cast<unsigned>(UniformSweepPhase::retirement);
    }else ++next_;
  }
  void finish() {
    if(!output_)return;
    check(!failed_ && !output_->finished && active_==uniform_sweep_phase_count &&
        next_==uniform_sweep_phase_count && output_->completed_windows==windows_);
    output_->total_wall_ns=read()-origin_;output_->finished=true;
  }
 private:
  void check(bool ok){if(!ok){failed_=true;throw std::invalid_argument("invalid uniform sweep observation");}}
  std::uint64_t read(){
    std::uint64_t now=0;try{now=clock_();}catch(...){failed_=true;throw;}
    check(now>=last_ && now>=origin_ && now-origin_<=hip_profile_limit_ns);last_=now;return now;
  }
  UniformLz4SweepTrace* output_;Clock clock_;std::uint32_t windows_;
  std::uint64_t origin_=0,last_=0,started_=0,quantum_started_=0;
  unsigned active_=uniform_sweep_phase_count,next_=0;bool failed_=false;
};
}
