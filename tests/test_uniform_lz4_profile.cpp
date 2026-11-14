#include "windows/uniform_lz4_profile.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <iostream>
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;std::uint64_t ticks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){try{f();}catch(const std::exception&){++checks;return;}throw std::runtime_error("invalid profile accepted");}
std::uint64_t clock(){return ticks+=17;}
Bytes literal(UniformLz4Bytes source){
  Bytes p{static_cast<std::byte>((std::min)(source.size(),std::size_t{15})<<4)};
  if(source.size()>=15){auto n=source.size()-15;while(n>=255){p.push_back(std::byte{255});n-=255;}p.push_back(static_cast<std::byte>(n));}
  p.insert(p.end(),source.begin(),source.end());return p;
}
}
int main(){try{
  uniform_lz4::Layout layout{};check(uniform_lz4::make_layout(2U*65536,65536,layout),"profile layout");
  Bytes source(2U*65536,std::byte{0x51});
  const auto batch=PreparedUniformLz4::prepare(source,uniform_lz4_limits(layout));
  UniformLz4ProfileBinding binding{77,0,1,2,65536,layout.total_bytes,{std::byte{1}}};
  HipPhaseTrace trace;HipPhaseRecorder recorder(&trace,&clock);
  for(unsigned i=0;i<hip_phase_count;++i){recorder.begin(static_cast<HipPhase>(i));recorder.end(static_cast<HipPhase>(i));}
  recorder.finish();
  UniformLz4Candidates c;
  for(unsigned i=0;i<layout.chunks;++i){const auto p=literal(batch.input(i));c.sizes.push_back(static_cast<std::uint32_t>(p.size()));c.payload.insert(c.payload.end(),p.begin(),p.end());}
  if(!gpu_lz4_cpu_verification_available()){
    rejects([&]{(void)encode_uniform_lz4_reply(batch,77,c,WorkerExecution::cpu_semantics);});
    std::cout<<"Uniform profile: missing CPU decoder refused; gpu_executed=false\n";return 0;
  }
  const auto ordinary=encode_uniform_lz4_reply(batch,77,c,WorkerExecution::cpu_semantics);
  const auto frame=encode_uniform_lz4_profile_reply(batch,binding,trace,ordinary,WorkerExecution::cpu_semantics);
  const auto accepted=accept_uniform_lz4_profile_reply(frame,batch,binding,WorkerExecution::cpu_semantics);
  check(accepted.candidates.payload==c.payload && accepted.candidates.sizes==c.sizes &&
      accepted.trace.wall_ns==trace.wall_ns && accepted.trace.total_wall_ns==trace.total_wall_ns,"profile/data round trip");
  rejects([&]{(void)accept_uniform_lz4_reply(frame,batch,77,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_profile_reply(ordinary,batch,binding,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_profile_reply(frame,batch,binding,WorkerExecution::hip);});
  for(std::size_t at=0;at<256;++at)if(at<80 || (at>=88 && at<96) || at>=216){
    auto changed=frame;changed[at]^=std::byte{1};
    rejects([&]{(void)accept_uniform_lz4_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  }
  for(const auto at:{std::size_t{80},std::size_t{96}}){
    auto changed=frame;uniform_profile_detail::put(changed,at,hip_profile_limit_ns+1);
    rejects([&]{(void)accept_uniform_lz4_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  }
  auto changed=frame;changed.back()^=std::byte{1};
  rejects([&]{(void)accept_uniform_lz4_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  changed=frame;changed.push_back(std::byte{0});
  rejects([&]{(void)accept_uniform_lz4_profile_reply(changed,batch,binding,WorkerExecution::cpu_semantics);});
  for(const auto n:{std::size_t{0},std::size_t{255},std::size_t{256},frame.size()-1})
    rejects([&]{(void)accept_uniform_lz4_profile_reply(std::span(frame).first(n),batch,binding,WorkerExecution::cpu_semantics);});
  auto incomplete=trace;incomplete.finished=false;
  rejects([&]{(void)encode_uniform_lz4_profile_reply(batch,binding,incomplete,ordinary,WorkerExecution::cpu_semantics);});
  const auto before=ticks;HipPhaseRecorder disabled(nullptr,&clock);
  disabled.begin(HipPhase::kernel_wait);disabled.end(HipPhase::kernel_wait);disabled.finish();
  check(ticks==before,"disabled profiling queried clock");
  std::cout<<"Uniform profile: checks="<<checks<<" exact_data=true disabled_clock_calls=0 gpu_executed=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
