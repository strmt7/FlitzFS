#include "hip_phase_profile.hpp"
#include <iostream>
#include <limits>
using namespace flitzfs;
namespace {
unsigned checks=0,calls=0;std::uint64_t time_value=0;bool throwing=false,constant=false;
void require(bool v,const char* why){++checks;if(!v)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("invalid phase profile accepted");}
std::uint64_t clock_value(){++calls;if(throwing)throw std::runtime_error("clock unavailable");return constant?time_value:time_value++;}
HipProfileBinding binding() {
  HipProfileBinding b{71,2,1,4096,match_detail::lanes(4096),{},{}};
  b.luid[0]=std::byte{9};b.source_digest[31]=std::byte{17};return b;
}
HipPhaseTrace complete(bool zero=false) {
  time_value=0;calls=0;throwing=false;constant=zero;
  HipPhaseTrace trace;HipPhaseRecorder recorder(&trace,clock_value);
  for(unsigned n=0;n<hip_phase_count;++n){recorder.begin(static_cast<HipPhase>(n));recorder.end(static_cast<HipPhase>(n));}
  recorder.finish();return trace;
}
void recorder_cases() {
  calls=0;HipPhaseRecorder disabled(nullptr,clock_value);
  for(unsigned n=0;n<hip_phase_count;++n){disabled.begin(static_cast<HipPhase>(n));disabled.end(static_cast<HipPhase>(n));}
  disabled.finish();require(calls==0,"Unrequested observation queried a clock");
  auto trace=complete();
  require(trace.finished && trace.completed==hip_phase_complete && trace.total_wall_ns==31 &&
      std::all_of(trace.wall_ns.begin(),trace.wall_ns.end(),[](auto n){return n==1;}),"Serial phase accounting");
  require(calls==32,"Unexpected phase clock calls");
  trace=complete(true);
  require(trace.finished && trace.completed==hip_phase_complete && trace.total_wall_ns==0,
      "Measured zero confused with missing measurement");
  time_value=0;constant=false;
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    rejects([&]{r.finish();});rejects([&]{r.begin(HipPhase::request_decode);});
    require(!t.finished,"Incomplete recorder sealed");
  }
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);r.begin(HipPhase::request_decode);
    rejects([&]{r.begin(HipPhase::bundle_verify);});
    rejects([&]{r.end(HipPhase::request_decode);});require(!t.finished,"Nested recorder recovered");
  }
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    rejects([&]{r.begin(HipPhase::bundle_verify);});rejects([&]{r.begin(HipPhase::request_decode);});
  }
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    rejects([&]{r.begin(HipPhase::count);});rejects([&]{r.begin(HipPhase::request_decode);});
  }
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);r.begin(HipPhase::request_decode);
    rejects([&]{r.end(HipPhase::bundle_verify);});rejects([&]{r.end(HipPhase::request_decode);});
  }
  {
    HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);r.begin(HipPhase::request_decode);
    r.end(HipPhase::request_decode);
    rejects([&]{r.begin(HipPhase::request_decode);});rejects([&]{r.begin(HipPhase::bundle_verify);});
  }
  {
    time_value=10;HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    r.begin(HipPhase::request_decode);time_value=0;
    rejects([&]{r.end(HipPhase::request_decode);});
    time_value=100;const auto before=calls;rejects([&]{r.end(HipPhase::request_decode);});
    require(calls==before && !t.finished,"Backward clock retried or sealed");
  }
  {
    time_value=0;HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    r.begin(HipPhase::request_decode);throwing=true;
    rejects([&]{r.end(HipPhase::request_decode);});throwing=false;
    const auto before=calls;rejects([&]{r.end(HipPhase::request_decode);});
    require(calls==before && !t.finished,"Unavailable clock retried or sealed");
  }
  {
    time_value=0;HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    time_value=hip_profile_limit_ns+1;rejects([&]{r.begin(HipPhase::request_decode);});
    require(!t.finished,"Excessive elapsed time sealed");
  }
  {
    HipPhaseTrace t;t.finished=true;const auto before=calls;
    rejects([&]{HipPhaseRecorder r(&t,clock_value);});require(calls==before,"Reused trace queried a clock");
  }
  {
    time_value=0;HipPhaseTrace t;HipPhaseRecorder r(&t,clock_value);
    for(unsigned n=0;n<hip_phase_count;++n){r.begin(static_cast<HipPhase>(n));r.end(static_cast<HipPhase>(n));}
    r.finish();const auto before=calls;rejects([&]{r.finish();});
    require(calls==before && t.finished,"Completed trace queried the clock again");
  }
}
void wire_cases() {
  HipPhaseProfile p{binding(),complete(),WorkerExecution::cpu_semantics};
  const auto frame=encode_hip_phase_profile(p);
  const auto decoded=decode_hip_phase_profile(frame,p.binding,p.execution);
  require(decoded.trace.wall_ns==p.trace.wall_ns && decoded.trace.total_wall_ns==p.trace.total_wall_ns &&
      decoded.trace.finished && decoded.binding==p.binding,"Phase frame roundtrip");
  // Every truncated size, trailing bytes, reserved byte and magic position rejects.
  for(std::size_t n=0;n<frame.size();++n)rejects([&]{decode_hip_phase_profile(std::span(frame).first(n),p.binding,p.execution);});
  auto extra=std::vector<std::byte>(frame.begin(),frame.end());extra.push_back(std::byte{});
  rejects([&]{decode_hip_phase_profile(extra,p.binding,p.execution);});
  for(std::size_t n=0;n<8;++n){auto f=frame;f[n]^=std::byte{1};rejects([&]{decode_hip_phase_profile(f,p.binding,p.execution);});}
  for(std::size_t n=208;n<frame.size();++n){auto f=frame;f[n]=std::byte{1};rejects([&]{decode_hip_phase_profile(f,p.binding,p.execution);});}
  // Binding never derives authority from the supplied profile.
  for(std::size_t n=8;n<72;++n){auto f=frame;f[n]^=std::byte{1};rejects([&]{decode_hip_phase_profile(f,p.binding,p.execution);});}
  for(std::size_t n=80;n<88;++n){auto f=frame;f[n]^=std::byte{1};rejects([&]{decode_hip_phase_profile(f,p.binding,p.execution);});}
  auto wrong=p.binding;wrong.request_id=0;rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  wrong=p.binding;wrong.ordinal=64;rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  wrong=p.binding;wrong.node_mask=3;rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  wrong=p.binding;wrong.luid={};rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  wrong=p.binding;wrong.input_bytes=65537;rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  wrong=p.binding;wrong.lanes++;rejects([&]{decode_hip_phase_profile(frame,wrong,p.execution);});
  rejects([&]{decode_hip_phase_profile(frame,p.binding,WorkerExecution::hip);});
  rejects([&]{decode_hip_phase_profile(frame,p.binding,static_cast<WorkerExecution>(3));});
  p.trace.finished=false;rejects([&]{encode_hip_phase_profile(p);});p.trace.finished=true;
  p.trace.completed=0;rejects([&]{encode_hip_phase_profile(p);});p.trace.completed=hip_phase_complete;
  p.trace.total_wall_ns=hip_profile_limit_ns+1;rejects([&]{encode_hip_phase_profile(p);});p.trace=complete();
  p.trace.wall_ns[1]=p.trace.total_wall_ns;rejects([&]{encode_hip_phase_profile(p);});
  p.trace.wall_ns[1]=(std::numeric_limits<std::uint64_t>::max)();rejects([&]{encode_hip_phase_profile(p);});
  p.trace=complete(true);require(decode_hip_phase_profile(encode_hip_phase_profile(p),p.binding,p.execution).trace.finished,
      "Explicit measured-zero profile rejected");
  // Diagnostic metadata is not usable as a compressed-data/match reply.
  const auto search=PreparedGpuMatchSearch::prepare(std::vector<std::byte>(4096,std::byte{41}));
  rejects([&]{accept_match_worker_reply(frame,search,p.binding.request_id,WorkerExecution::cpu_semantics);});
}
}
int main(){try{
  recorder_cases();wire_cases();
  std::cout<<"HIP phase profile checks: "<<checks<<"; CPU clock/wire semantics only, gpu_executed=false\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
