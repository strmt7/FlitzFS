#include "flitzfs/uniform_lz4_sweep.hpp"
#include <iostream>
#include <limits>
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){try{f();}catch(const std::exception&){++checks;return;}throw std::runtime_error("sweep accepted invalid work");}
Bytes literal(UniformLz4Bytes source){
  Bytes p{static_cast<std::byte>((std::min)(source.size(),std::size_t{15})<<4)};
  if(source.size()>=15){auto n=source.size()-15;while(n>=255){p.push_back(std::byte{255});n-=255;}p.push_back(static_cast<std::byte>(n));}
  p.insert(p.end(),source.begin(),source.end());return p;
}
UniformLz4Candidates cpu(const UniformLz4Request& r){
  UniformLz4Candidates c;
  for(std::uint32_t i=0;i<r.batch.layout().chunks;++i){
    const auto p=literal(r.batch.input(i));c.sizes.push_back(static_cast<std::uint32_t>(p.size()));
    c.payload.insert(c.payload.end(),p.begin(),p.end());
  }return c;
}
UniformLz4Request source(std::size_t bytes=16U*65536+37){
  Bytes data(bytes);for(std::size_t i=0;i<bytes;++i)data[i]=static_cast<std::byte>((i/65536+3*i)%251);
  uniform_lz4::Layout layout{};check(uniform_lz4::make_layout(bytes,65536,layout),"source layout");
  return {193,PreparedUniformLz4::prepare(data,uniform_lz4_limits(layout))};
}
}
int main(){try{
  auto request=source();uniform_lz4::Layout two{};check(uniform_lz4::make_layout(2U*65536,65536,two),"quantum layout");
  const auto plan=plan_uniform_lz4_sweep(request.batch,2,two.total_bytes);
  check(plan.quantum_chunks==2 && plan.windows==9 && plan.device_peak_bytes==two.total_bytes,"17-chunk serial plan");
  check(plan_uniform_lz4_sweep(request.batch,UINT32_MAX,two.total_bytes)==plan,"device grant selects same maximum");
  uniform_lz4::Layout one{};check(uniform_lz4::make_layout(65536,65536,one),"one layout");
  check(plan_uniform_lz4_sweep(request.batch,2,two.total_bytes-1).quantum_chunks==1,"device peak boundary");
  rejects([&]{(void)plan_uniform_lz4_sweep(request.batch,2,one.total_bytes-1);});
  rejects([&]{(void)plan_uniform_lz4_sweep(request.batch,0,two.total_bytes);});
  rejects([&]{(void)plan_uniform_lz4_sweep(request.batch,2,0);});
  unsigned calls=0;std::uint64_t offset=0;
  auto callback=[&](const UniformLz4Request& part){
    ++calls;check(part.request_id==request.request_id && part.batch.layout().chunks<=2 &&
        part.batch.layout().total_bytes<=plan.device_peak_bytes,"quantum identity/grant");
    const auto expected=request.batch.plaintext().subspan(static_cast<std::size_t>(offset),part.batch.plaintext().size());
    check(std::equal(expected.begin(),expected.end(),part.batch.plaintext().begin()),"quantum source order");
    offset+=part.batch.plaintext().size();return cpu(part);
  };
  if(!gpu_lz4_cpu_verification_available()){
    rejects([&]{(void)compute_uniform_lz4_sweep(request,plan,callback);});
    check(calls==0,"missing decoder invoked callback");
    std::cout<<"Uniform sweep: missing CPU decoder refused before compute; gpu_executed=false\n";return 0;
  }
  const auto result=compute_uniform_lz4_sweep(request,plan,callback);
  check(calls==9 && offset==request.batch.plaintext().size() && result.sizes.size()==17,"serial full coverage");
  const auto reply=encode_uniform_lz4_reply(request.batch,request.request_id,result,WorkerExecution::cpu_semantics);
  check(accept_uniform_lz4_reply(reply,request.batch,request.request_id,WorkerExecution::cpu_semantics).payload==result.payload,
      "combined ordinary reply CPU verification");
  for(unsigned fault=0;fault<3;++fault){
    calls=0;UniformLz4Candidates escaped;
    rejects([&]{escaped=compute_uniform_lz4_sweep(request,plan,[&](const UniformLz4Request& part){
      ++calls;auto c=cpu(part);if(calls==2){
        if(fault==0)throw std::runtime_error("simulated device failure");
        if(fault==1)c.payload.back()^=std::byte{1};
        if(fault==2)c.sizes.pop_back();
      }return c;
    });});
    check(calls==2 && escaped.payload.empty() && escaped.sizes.empty(),"failed sweep retried or exposed partial result");
  }
  for(unsigned fault=0;fault<4;++fault){
    auto invalid=plan;if(fault==0)++invalid.windows;if(fault==1)++invalid.device_peak_bytes;
    if(fault==2)++invalid.whole.input_bytes;if(fault==3)invalid.quantum_chunks=0;
    calls=0;rejects([&]{(void)compute_uniform_lz4_sweep(request,invalid,[&](const auto& part){++calls;return cpu(part);});});
    check(calls==0,"forged plan reached compute");
  }
  auto small=source(37);const auto small_plan=plan_uniform_lz4_sweep(small.batch,UINT32_MAX,two.total_bytes);
  check(small_plan.quantum_chunks==1 && small_plan.windows==1,"single partial quantum");
  check(compute_uniform_lz4_sweep(small,small_plan,cpu).sizes.size()==1,"single partial readback");
  auto moved=std::move(small.batch);rejects([&]{(void)plan_uniform_lz4_sweep(small.batch,1,two.total_bytes);});
  check(moved.ready(),"moved source ownership");
  std::cout<<"Uniform sweep: checks="<<checks<<" serial_complete=true partial_failure_terminal=true gpu_executed=false\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
