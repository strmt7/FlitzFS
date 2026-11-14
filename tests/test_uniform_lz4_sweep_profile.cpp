#include "windows/uniform_lz4_sweep_profile.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <iostream>
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;std::uint64_t ticks=0,step=17;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){try{f();}catch(const std::invalid_argument&){++checks;return;}
  throw std::runtime_error("invalid sweep observation accepted");}
std::uint64_t clock(){return ticks+=step;}
std::uint64_t broken(){throw std::invalid_argument("clock unavailable");}
void quantum(UniformLz4SweepRecorder& r){
  for(unsigned i=static_cast<unsigned>(UniformSweepPhase::admission);i<=static_cast<unsigned>(UniformSweepPhase::completion);++i){
    const auto p=static_cast<UniformSweepPhase>(i);r.begin(p);r.end(p);
  }
}
UniformLz4SweepTrace observed(std::uint32_t windows){
  UniformLz4SweepTrace t;UniformLz4SweepRecorder r(&t,windows,&clock);
  r.begin(UniformSweepPhase::setup);r.end(UniformSweepPhase::setup);
  for(unsigned i=0;i<windows;++i){step=17+i;quantum(r);}step=17;
  r.begin(UniformSweepPhase::retirement);r.end(UniformSweepPhase::retirement);r.finish();return t;
}
Bytes literal(UniformLz4Bytes source){
  Bytes p{static_cast<std::byte>((std::min)(source.size(),std::size_t{15})<<4)};
  if(source.size()>=15){auto n=source.size()-15;while(n>=255){p.push_back(std::byte{255});n-=255;}p.push_back(static_cast<std::byte>(n));}
  p.insert(p.end(),source.begin(),source.end());return p;
}
void recorder_contract(){
  auto t=observed(2);check(t.finished && t.completed_windows==2 &&
      t.minimum_kernel_wall_ns==17 && t.maximum_kernel_wall_ns==18 &&
      t.wall_ns[static_cast<unsigned>(UniformSweepPhase::kernel_wait)]==35 &&
      t.maximum_quantum_wall_ns>=18,"sweep maxima, minimum, sum or window count");
  const auto before=ticks;UniformLz4SweepRecorder disabled(nullptr,0,&broken);
  disabled.begin(UniformSweepPhase::count);disabled.end(UniformSweepPhase::count);disabled.finish();
  check(ticks==before,"disabled sweep profile queried clock");
  UniformLz4SweepTrace invalid;
  rejects([&]{UniformLz4SweepRecorder r(&invalid,0,&clock);});
  rejects([&]{UniformLz4SweepRecorder r(&invalid,1,nullptr);});
  rejects([&]{UniformLz4SweepRecorder r(&t,2,&clock);});
  {
    UniformLz4SweepTrace partial;UniformLz4SweepRecorder r(&partial,1,&clock);
    rejects([&]{r.begin(UniformSweepPhase::kernel_wait);});
    rejects([&]{r.begin(UniformSweepPhase::setup);});check(!partial.finished,"failed ordering reusable");
  }
  {
    UniformLz4SweepTrace partial;UniformLz4SweepRecorder r(&partial,2,&clock);
    r.begin(UniformSweepPhase::setup);r.end(UniformSweepPhase::setup);quantum(r);
    rejects([&]{r.begin(UniformSweepPhase::retirement);});rejects([&]{r.finish();});
    check(!partial.finished && partial.completed_windows==1,"partial sweep completed");
  }
  {
    UniformLz4SweepTrace partial;UniformLz4SweepRecorder r(&partial,1,&clock);
    r.begin(UniformSweepPhase::setup);r.end(UniformSweepPhase::setup);quantum(r);
    r.begin(UniformSweepPhase::retirement);r.end(UniformSweepPhase::retirement);r.finish();
    rejects([&]{r.finish();});rejects([&]{r.begin(UniformSweepPhase::setup);});
  }
  {
    UniformLz4SweepTrace partial;UniformLz4SweepRecorder r(&partial,1,&clock);
    r.begin(UniformSweepPhase::setup);ticks-=step+1;
    rejects([&]{r.end(UniformSweepPhase::setup);});check(!partial.finished,"backwards clock accepted");
  }
  {
    UniformLz4SweepTrace partial;UniformLz4SweepRecorder r(&partial,1,&clock);
    ticks+=hip_profile_limit_ns+1;rejects([&]{r.begin(UniformSweepPhase::setup);});
    check(!partial.finished,"unbounded observation accepted");
  }
}
}
int main(){try{
  recorder_contract();
  uniform_lz4::Layout layout{},q{};
  check(uniform_lz4::make_layout(3U*65536+1,65536,layout) &&
      uniform_lz4::make_layout(2U*65536,65536,q),"sweep profile layout");
  Bytes source(static_cast<std::size_t>(layout.input_bytes),std::byte{0x51});
  const auto batch=PreparedUniformLz4::prepare(source,uniform_lz4_limits(layout));
  const UniformLz4ProfileBinding binding{77,0,1,2,65536,q.total_bytes,{std::byte{1}}};
  const auto plan=plan_uniform_lz4_sweep(batch,2,q.total_bytes);const auto trace=observed(plan.windows);
  UniformLz4Candidates c;
  for(unsigned i=0;i<layout.chunks;++i){
    const auto p=literal(batch.input(i));c.sizes.push_back(static_cast<std::uint32_t>(p.size()));
    c.payload.insert(c.payload.end(),p.begin(),p.end());
  }
  if(!gpu_lz4_cpu_verification_available()){
    bool refused=false;
    try{(void)encode_uniform_lz4_reply(batch,77,c,WorkerExecution::cpu_semantics);}
    catch(const std::runtime_error& e){
      check(std::string_view(e.what())=="independent CPU LZ4 verification unavailable","wrong raw decoder refusal");
      refused=true;
    }
    check(refused,"raw sweep profile encoded without CPU verification");
    std::cout<<"Uniform sweep profile: checks="<<checks<<" missing_decoder_refused=true gpu_executed=false\n";return 0;
  }
  const auto ordinary=encode_uniform_lz4_reply(batch,77,c,WorkerExecution::cpu_semantics);
  const auto frame=encode_uniform_lz4_sweep_profile_reply(batch,binding,trace,ordinary,WorkerExecution::cpu_semantics);
  const auto accepted=accept_uniform_lz4_sweep_profile_reply(frame,batch,binding,WorkerExecution::cpu_semantics);
  check(accepted.candidates.payload==c.payload && accepted.candidates.sizes==c.sizes &&
      accepted.trace.wall_ns==trace.wall_ns && accepted.trace.maximum_kernel_wall_ns==trace.maximum_kernel_wall_ns &&
      accepted.trace.minimum_kernel_wall_ns==trace.minimum_kernel_wall_ns &&
      accepted.trace.maximum_quantum_wall_ns==trace.maximum_quantum_wall_ns &&
      accepted.trace.total_wall_ns==trace.total_wall_ns,"sweep envelope did not preserve exact observations/data");
  rejects([&]{(void)accept_uniform_lz4_reply(frame,batch,77,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_profile_reply(frame,batch,binding,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(ordinary,batch,binding,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(frame,batch,binding,WorkerExecution::hip);});
  for(std::size_t at=0;at<256;++at)if(at<80 || (at>=88 && at<96) || at>=192){
    auto changed=frame;changed[at]^=std::byte{1};
    rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  }
  for(const auto at:{std::size_t{80},std::size_t{96},std::size_t{168},std::size_t{176},std::size_t{184}}){
    auto changed=frame;uniform_profile_detail::put(changed,at,hip_profile_limit_ns+1);
    rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  }
  auto changed=frame;uniform_profile_detail::put(changed,176,trace.maximum_kernel_wall_ns+1);
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  changed=frame;uniform_profile_detail::put(changed,128,0);
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  changed=frame;changed.back()^=std::byte{1};
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  changed=frame;changed.push_back(std::byte{0});
  rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  for(const auto n:{std::size_t{0},std::size_t{255},std::size_t{256},frame.size()-1})
    rejects([&]{(void)accept_uniform_lz4_sweep_profile_reply(std::span(frame).first(n),batch,binding,WorkerExecution::cpu_semantics);});
  auto partial=trace;partial.finished=false;
  rejects([&]{(void)encode_uniform_lz4_sweep_profile_reply(batch,binding,partial,ordinary,WorkerExecution::cpu_semantics);});
  partial=trace;partial.completed_windows--;
  rejects([&]{(void)encode_uniform_lz4_sweep_profile_reply(batch,binding,partial,ordinary,WorkerExecution::cpu_semantics);});
  std::cout<<"Uniform sweep profile: checks="<<checks<<" exact_data=true disabled_clock_calls=0 gpu_executed=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
