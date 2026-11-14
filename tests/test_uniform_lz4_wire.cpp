#include "flitzfs/uniform_lz4_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <utility>
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){try{f();}catch(const std::exception&){++checks;return;}throw std::runtime_error("invalid uniform operation accepted");}
void put(Bytes& b,std::size_t at,std::uint64_t n,unsigned width){for(unsigned i=0;i<width;++i)b[at+i]=static_cast<std::byte>((n>>(i*8))&255);}
Bytes literals(UniformLz4Bytes b){
  Bytes p{static_cast<std::byte>((std::min)(b.size(),std::size_t{15})<<4)};
  if(b.size()>=15){auto left=b.size()-15;while(left>=255){p.push_back(std::byte{255});left-=255;}p.push_back(static_cast<std::byte>(left));}
  p.insert(p.end(),b.begin(),b.end());return p;
}
UniformLz4Candidates candidates(const PreparedUniformLz4& batch){
  UniformLz4Candidates c;
  for(std::uint32_t i=0;i<batch.layout().chunks;++i){
    const auto p=literals(batch.input(i));c.sizes.push_back(static_cast<std::uint32_t>(p.size()));c.payload.insert(c.payload.end(),p.begin(),p.end());
  }
  return c;
}
PreparedUniformLz4 prepared(std::size_t count){
  Bytes source(count);for(std::size_t i=0;i<count;++i)source[i]=static_cast<std::byte>((i*17+i/101)%256);
  uniform_lz4::Layout p{};check(uniform_lz4::make_layout(count,65536,p),"uniform fixture layout");
  return PreparedUniformLz4::prepare(source,uniform_lz4_limits(p));
}
void shape_and_wire(){
  auto source=prepared(16U*65536+37);
  check(source.ready() && source.layout().chunks==17 && source.layout().total_bytes>262144,"legacy ceiling retained");
  const Bytes expected(source.plaintext().begin(),source.plaintext().end());
  auto frame=encode_uniform_lz4_request(source,73);
  check(inspect_uniform_lz4_request(frame,source.limits().request_bytes,source.limits().reply_bytes)==source.limits(),
      "independent uniform channel capacities lost");
  rejects([&]{(void)inspect_uniform_lz4_request(frame,source.limits().request_bytes-1,source.limits().reply_bytes);});
  rejects([&]{(void)inspect_uniform_lz4_request(frame,source.limits().request_bytes,source.limits().reply_bytes-1);});
  rejects([&]{(void)inspect_uniform_lz4_request(std::span(frame).first(79),79,source.limits().reply_bytes);});
  auto request=decode_uniform_lz4_request(frame,source.limits());
  check(request.request_id==73 && request.batch.digest()==source.digest() &&
      std::equal(expected.begin(),expected.end(),request.batch.plaintext().begin()),"version-2 source round trip");
  auto moved=std::move(request.batch);check(moved.ready() && !request.batch.ready(),"uniform move retained source ownership");
  rejects([&]{(void)encode_uniform_lz4_request(request.batch,73);});
  source=std::move(moved);check(source.ready() && !moved.ready(),"uniform move assignment retained old source");
  for(std::size_t i=0;i<kUniformLz4HeaderBytes;++i)if(i<8 || i>=16){
    auto changed=frame;changed[i]^=std::byte{1};rejects([&]{(void)decode_uniform_lz4_request(changed,source.limits());});
  }
  for(std::size_t n=0;n<kUniformLz4HeaderBytes;++n)
    rejects([&]{(void)decode_uniform_lz4_request(std::span(frame).first(n),source.limits());});
  auto changed=frame;changed.back()^=std::byte{1};
  rejects([&]{(void)decode_uniform_lz4_request(changed,source.limits());});
  changed=frame;changed.push_back(std::byte{0});rejects([&]{(void)decode_uniform_lz4_request(changed,source.limits());});
  const auto limits=source.limits();
  for(unsigned field=0;field<6;++field){
    auto bad=limits;
    if(field==0)bad.input_bytes++;
    if(field==1)bad.device_bytes++;
    if(field==2)bad.request_bytes++;
    if(field==3)bad.reply_bytes++;
    if(field==4)bad.chunk_bytes++;
    if(field==5)bad.chunks++;
    check(!valid_uniform_lz4_limits(bad),"changed admitted limit accepted");
    rejects([&]{(void)PreparedUniformLz4::prepare(expected,bad);});
  }
  rejects([&]{(void)PreparedUniformLz4::prepare(std::span(expected).first(expected.size()-1),limits);});
  rejects([&]{(void)encode_uniform_lz4_request(source,0);});
  if(!gpu_lz4_cpu_verification_available()){
    auto c=candidates(source);rejects([&]{verify_uniform_lz4_candidates(source,c);});
    rejects([&]{(void)encode_uniform_lz4_reply(source,73,c,WorkerExecution::hip);});return;
  }
  auto c=candidates(source);verify_uniform_lz4_candidates(source,c);
  const auto reply=encode_uniform_lz4_reply(source,73,c,WorkerExecution::cpu_semantics);
  const auto accepted=accept_uniform_lz4_reply(reply,source,73,WorkerExecution::cpu_semantics);
  check(accepted.payload==c.payload && accepted.sizes==c.sizes,"dense candidate round trip");
  rejects([&]{(void)accept_uniform_lz4_reply(reply,source,74,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_uniform_lz4_reply(reply,source,73,WorkerExecution::hip);});
  for(std::size_t i=0;i<kUniformLz4HeaderBytes+source.layout().chunks*8;++i){
    auto bad=reply;bad[i]^=std::byte{1};rejects([&]{(void)accept_uniform_lz4_reply(bad,source,73,WorkerExecution::cpu_semantics);});
  }
  for(const auto cut:std::array<std::size_t,6>{0,79,80,81,80+17*8-1,reply.size()-1})
    rejects([&]{(void)accept_uniform_lz4_reply(std::span(reply).first(cut),source,73,WorkerExecution::cpu_semantics);});
  auto bad=reply;bad.back()^=std::byte{1};rejects([&]{(void)accept_uniform_lz4_reply(bad,source,73,WorkerExecution::cpu_semantics);});
  bad=reply;bad.push_back(std::byte{0});rejects([&]{(void)accept_uniform_lz4_reply(bad,source,73,WorkerExecution::cpu_semantics);});
  c.payload.back()^=std::byte{1};rejects([&]{verify_uniform_lz4_candidates(source,c);});
  check(std::equal(expected.begin(),expected.end(),source.plaintext().begin()),"candidate fault changed source");
}
void slab(){
  auto batch=prepared(9U*65536+19);const auto& p=batch.layout();
  auto upload=uniform_lz4_upload(batch);
  rejects([&]{(void)verify_uniform_lz4_readback(batch,upload);}); // zero/unpublished output
  auto readback=upload;auto c=candidates(batch);std::size_t at=0;
  for(std::uint32_t i=0;i<p.chunks;++i){
    uniform_lz4::Chunk part{};check(uniform_lz4::chunk(p,i,part),"fixture chunk mapping");
    put(readback,i*8,c.sizes[i],8);
    std::copy_n(c.payload.begin()+static_cast<std::ptrdiff_t>(at),c.sizes[i],
        readback.begin()+static_cast<std::ptrdiff_t>(part.output));at+=c.sizes[i];
  }
  if(!gpu_lz4_cpu_verification_available()){
    rejects([&]{(void)verify_uniform_lz4_readback(batch,readback);});return;
  }
  const auto accepted=verify_uniform_lz4_readback(batch,readback);
  check(accepted.sizes==c.sizes && accepted.payload==c.payload,"guarded scalable slab verification");
  for(std::uint32_t i=0;i<p.chunks;++i){
    auto bad=readback;const auto guard=p.output_offset+std::uint64_t(i+1)*p.output_stride-uniform_lz4::guard_bytes;
    bad[guard]^=std::byte{1};rejects([&]{(void)verify_uniform_lz4_readback(batch,bad);});
    bad=readback;put(bad,i*8,~std::uint64_t{0},8);rejects([&]{(void)verify_uniform_lz4_readback(batch,bad);});
  }
  for(const auto at:std::array<std::uint64_t,3>{p.input_offset,p.input_offset+p.input_bytes-1,p.output_offset-1}){
    auto bad=readback;bad[at]^=std::byte{1};rejects([&]{(void)verify_uniform_lz4_readback(batch,bad);});
  }
  auto bad=readback;bad[p.output_offset+2]^=std::byte{1};rejects([&]{(void)verify_uniform_lz4_readback(batch,bad);});
  rejects([&]{(void)verify_uniform_lz4_readback(batch,std::span(readback).first(readback.size()-1));});
}
}
int main(){try{shape_and_wire();slab();std::cout<<"Uniform LZ4 frames/slab: checks="<<checks
    <<" CPU_decoder="<<gpu_lz4_cpu_verification_available()<<" gpu_executed=false\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
