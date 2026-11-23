#include "flitzfs/uniform_lz4_sweep.hpp"
#include "flitzfs/uniform_lz4_batch.hpp"
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
UniformLz4Candidates cpu_candidates(const PreparedUniformLz4& source){
  UniformLz4Candidates c;
  for(std::uint32_t i=0;i<source.layout().chunks;++i){
    const auto p=literal(source.input(i));c.sizes.push_back(static_cast<std::uint32_t>(p.size()));
    c.payload.insert(c.payload.end(),p.begin(),p.end());
  }return c;
}
UniformLz4Candidates cpu(const UniformLz4Request& r){return cpu_candidates(r.batch);}
UniformLz4Request source(std::size_t bytes=16U*65536+37){
  Bytes data(bytes);for(std::size_t i=0;i<bytes;++i)data[i]=static_cast<std::byte>((i/65536+3*i)%251);
  uniform_lz4::Layout layout{};check(uniform_lz4::make_layout(bytes,65536,layout),"source layout");
  return {193,PreparedUniformLz4::prepare(data,uniform_lz4_limits(layout))};
}
void independent_requests() {
  constexpr auto width=65536U;
  Bytes first(2*width,std::byte{11}),second(3*width,std::byte{27}),last(37,std::byte{91});
  const std::array inputs{UniformLz4BatchInput{7,first},UniformLz4BatchInput{19,second},UniformLz4BatchInput{35,last}};
  GpuHealthEvidence evidence;
  evidence.compatible=evidence.supervisor_quiescent=true;
  evidence.sequence=1;evidence.configuration[0]=std::byte{31};
  auto& memory=evidence.memory;
  memory.valid=memory.amd_hardware=true;memory.luid[0]=std::byte{7};memory.node_mask=1;memory.captured_ms=100;
  memory.local_budget=memory.dedicated_bytes=1ULL<<30;memory.nonlocal_budget=8ULL<<30;
  memory.physical_total=memory.commit_available=16ULL<<30;memory.physical_available=12ULL<<30;
  memory.commit_limit=32ULL<<30;
  GpuLz4ResidentCost cost{.fixed_bytes=1<<20,.input_copies=4,.slab_copies=2,.output_copies=2,
      .configuration=evidence.configuration,.profiled_chunk_bytes=width,.maximum_grid_x=2,
      .kernel_ns_per_chunk=600000,.qualified_kernel_quantum_ns=1200000,.qualified=true};
  uniform_lz4::Layout two{};
  check(uniform_lz4::make_layout(2ULL*width,width,two),"batch quantum");
  const GpuLz4SweepGrant grant{2,two.total_bytes};
  const GpuLz4WindowPolicy policy{16,1200000};
  const auto plan=[&](auto work,std::uint32_t deadline) {
    return plan_uniform_lz4_batch(work,width,evidence,100,cost,policy,grant,deadline);
  };
  const auto full=plan(std::span(inputs),5);
  check(full.requests==3 && full.window.code==GpuGateCode::admitted && full.window.layout.chunks==6 &&
      full.window.quantum.chunks==two.chunks && full.window.quantum.total_bytes==two.total_bytes &&
      !full.window.remaining_input_bytes,"batch planner lost a qualified request");
  const auto limited=plan(std::span(inputs),2);
  check(limited.requests==1 && limited.window.layout.input_bytes==first.size() &&
      limited.window.remaining_input_bytes==second.size()+last.size(),"batch planner split a request");
  check(!plan(std::span(inputs),1).requests,"batch planner split its first request");
  evidence.supervisor_quiescent=false;
  check(!plan(std::span(inputs),5).requests,"batch planner admitted work during another exchange");
  evidence.supervisor_quiescent=true;
  auto invalid=inputs;
  invalid[1].request_id=7;
  check(!plan(std::span(invalid),5).requests,"batch planner accepted a duplicate identity");
  invalid=inputs;invalid[1].source=last;
  check(!plan(std::span(invalid),5).requests,"batch planner accepted a non-final partial chunk");
  invalid=inputs;invalid[2].source=first;
  rejects([&]{(void)PreparedUniformLz4Batch::prepare(invalid,full);});

  const auto partial=PreparedUniformLz4Batch::prepare(inputs,limited);
  check(partial.members().size()==1 && std::ranges::equal(partial.input(0),first),"wrong complete prefix captured");
  const auto batch=PreparedUniformLz4Batch::prepare(inputs,full);
  first[0]^=std::byte{123};
  check(batch.input(0)[0]==std::byte{11} && std::ranges::equal(batch.input(1),second) &&
      std::ranges::equal(batch.input(2),last),"batch source ownership or boundary changed");
  if(!gpu_lz4_cpu_verification_available())return;

  constexpr auto exchange=178ULL;
  const auto wire=encode_uniform_lz4_reply(batch.source(),exchange,cpu_candidates(batch.source()),WorkerExecution::cpu_semantics);
  const auto verified=VerifiedUniformLz4Batch::accept(wire,batch,exchange,WorkerExecution::cpu_semantics);
  check(verified.size()==inputs.size() && verified.matches(batch),"verified batch lost its exact binding");
  const std::array replacement{UniformLz4BatchInput{7,batch.input(0)},UniformLz4BatchInput{80,batch.input(1)},
      UniformLz4BatchInput{110,batch.input(2)}};
  const auto rebound=PreparedUniformLz4Batch::prepare(replacement,batch.source().layout());
  check(rebound.source().digest()==batch.source().digest() && !verified.matches(rebound),
      "Identical combined bytes authorized different request identities");
  for(std::size_t index=0;index<verified.size();++index) {
    const auto reply=verified.reply(index);
    const auto& member=batch.members()[index];
    check(reply.request_id==inputs[index].request_id && reply.sizes.size()==member.chunks,
        "verified result bound to a different request");
    auto payload=reply.payload;
    for(std::uint32_t chunk=0;chunk<member.chunks;++chunk) {
      const auto expected=literal(batch.source().input(member.first_chunk+chunk));
      check(std::ranges::equal(payload.first(reply.sizes[chunk]),expected),"wrong group candidate bytes");
      payload=payload.subspan(reply.sizes[chunk]);
    }
    check(payload.empty(),"group payload includes another request");
  }
  std::vector<std::vector<std::byte>> parts;
  for(std::size_t index=0;index<batch.members().size();++index) {
    const auto member=batch.prepare_member(index);
    parts.push_back(encode_uniform_lz4_reply(member,batch.members()[index].request_id,
        cpu_candidates(member),WorkerExecution::cpu_semantics));
  }
  std::vector<UniformLz4Bytes> split;
  for(const auto& part:parts)split.push_back(part);
  const auto joined=VerifiedUniformLz4Batch::accept_members(split,batch,WorkerExecution::cpu_semantics);
  check(joined.matches(batch) && joined.size()==verified.size(),"individual responses lost their prepared group");
  for(std::size_t i=0;i<joined.size();++i) {
    const auto separate=joined.reply(i),combined=verified.reply(i);
    check(separate.request_id==combined.request_id && std::ranges::equal(separate.sizes,combined.sizes) &&
        std::ranges::equal(separate.payload,combined.payload),"different verified member after joining separate replies");
  }
  std::swap(split[0],split[1]);
  rejects([&]{(void)VerifiedUniformLz4Batch::accept_members(split,batch,WorkerExecution::cpu_semantics);});
  std::swap(split[0],split[1]);
  rejects([&]{(void)VerifiedUniformLz4Batch::accept_members(std::span(split).first(2),batch,WorkerExecution::cpu_semantics);});
  parts.back().back()^=std::byte{1};
  rejects([&]{(void)VerifiedUniformLz4Batch::accept_members(split,batch,WorkerExecution::cpu_semantics);});
  auto corrupt=wire;corrupt.back()^=std::byte{1};
  rejects([&]{(void)VerifiedUniformLz4Batch::accept(corrupt,batch,exchange,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)VerifiedUniformLz4Batch::accept(wire,batch,exchange+1,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)VerifiedUniformLz4Batch::accept(wire,batch,exchange,WorkerExecution::hip);});
}
}
int main(){try{
  independent_requests();
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
