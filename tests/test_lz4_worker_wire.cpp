#include "flitzfs/lz4_worker_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
namespace {
using namespace flitzfs;using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f) {
  ++checks;try{f();}catch(const std::invalid_argument&){return;}
  throw std::runtime_error("Malformed LZ4 wire admitted");
}
template<class F>void unavailable(F&& f) {
  ++checks;try{f();}catch(const std::runtime_error& e) {
    if(std::string_view(e.what()).find("verification unavailable")!=std::string_view::npos)return;
    throw;
  }
  throw std::runtime_error("Unavailable CPU decoder admitted candidate");
}
void set32(Bytes& out,std::size_t at,std::uint32_t value) {
  for(unsigned i=0;i<4;++i)out[at+i]=static_cast<std::byte>((value>>(8*i))&255U);
}
Bytes literal(Lz4WorkerBytes input) {
  Bytes result{static_cast<std::byte>(std::min<std::size_t>(input.size(),15U)<<4)};
  if(input.size()>=15) {
    auto left=input.size()-15;
    while(left>=255){result.push_back(std::byte{255});left-=255;}
    result.push_back(static_cast<std::byte>(left));
  }
  result.insert(result.end(),input.begin(),input.end());return result;
}
std::vector<Lz4WorkerBytes> views(const std::vector<Bytes>& chunks) {
  std::vector<Lz4WorkerBytes> result;for(const auto& chunk:chunks)result.emplace_back(chunk);
  return result;
}
Bytes manual_reply(const PreparedLz4WorkerBatch& p,std::uint64_t id,
    const std::vector<Bytes>& chunks,WorkerExecution execution) {
  Bytes result(64+8*chunks.size());
  const std::array<unsigned char,8> magic{'F','L','F','S','L','R','P',1};
  for(std::size_t i=0;i<magic.size();++i)result[i]=static_cast<std::byte>(magic[i]);
  for(unsigned i=0;i<8;++i)result[8+i]=static_cast<std::byte>((id>>(8*i))&255U);
  set32(result,16,p.count());set32(result,20,p.input_bytes());
  set32(result,28,static_cast<std::uint32_t>(execution));
  std::copy(p.source_digest().begin(),p.source_digest().end(),result.begin()+32);
  for(std::size_t i=0;i<chunks.size();++i) {
    set32(result,64+8*i,static_cast<std::uint32_t>(p.input(i).size()));
    set32(result,68+8*i,static_cast<std::uint32_t>(chunks[i].size()));
  }
  for(const auto& chunk:chunks)result.insert(result.end(),chunk.begin(),chunk.end());
  return result;
}
void roundtrip(std::vector<Bytes> chunks,bool mutate) {
  const auto original=chunks;const auto input_views=views(chunks);
  auto p=PreparedLz4WorkerBatch::prepare(input_views);
  const auto request=encode_lz4_worker_request(p,519);
  const auto parsed=decode_lz4_worker_request(request);
  require(parsed.request_id==519 && parsed.batch.count()==chunks.size(),"Request identity/count");
  require(parsed.batch.source_digest()==p.source_digest(),"Source binding");
  auto mutable_request=request;
  const auto isolated=decode_lz4_worker_request(mutable_request);
  std::fill(mutable_request.begin()+64,mutable_request.end(),std::byte{0});
  require(std::ranges::equal(isolated.batch.plaintext(),p.plaintext()),"Parsed request borrowed frame bytes");
  for(std::size_t i=0;i<chunks.size();++i)
    require(std::ranges::equal(parsed.batch.input(i),chunks[i]),"Request plaintext");
  const auto source_copy=Bytes(p.plaintext().begin(),p.plaintext().end());
  chunks[0][0]^=std::byte{1};
  require(std::ranges::equal(p.plaintext(),source_copy),"Preparation borrowed mutable source");
  std::vector<Bytes> payloads;for(const auto& chunk:original)payloads.push_back(literal(chunk));
  const auto payload_views=views(payloads);
  const auto frame=manual_reply(p,519,payloads,WorkerExecution::cpu_semantics);
  if(gpu_lz4_cpu_verification_available()) {
    const auto encoded=encode_lz4_worker_reply(parsed.batch,519,payload_views,WorkerExecution::cpu_semantics);
    require(encoded==frame,"Independent fixture/producer framing disagreement");
    require(accept_lz4_worker_reply(frame,p,519,WorkerExecution::cpu_semantics)==payloads,"Candidate roundtrip");
    auto mutable_frame=frame;
    const auto accepted=accept_lz4_worker_reply(mutable_frame,p,519,WorkerExecution::cpu_semantics);
    std::fill(mutable_frame.begin()+64,mutable_frame.end(),std::byte{0});
    require(accepted==payloads,"Accepted candidates borrowed frame bytes");
    const auto hip_tag=encode_lz4_worker_reply(p,520,payload_views,WorkerExecution::hip);
    require(accept_lz4_worker_reply(hip_tag,p,520,WorkerExecution::hip)==payloads,"Synthetic backend declaration");
  }else {
    unavailable([&]{(void)encode_lz4_worker_reply(p,519,payload_views,WorkerExecution::cpu_semantics);});
    unavailable([&]{(void)accept_lz4_worker_reply(frame,p,519,WorkerExecution::cpu_semantics);});
  }
  require(std::ranges::equal(p.plaintext(),source_copy),"Reply verification changed source");
  if(!mutate)return;
  const std::array<std::byte,12> sizes_golden{
      std::byte{2},std::byte{0},std::byte{0},std::byte{0},
      std::byte{17},std::byte{0},std::byte{0},std::byte{0},
      std::byte{33},std::byte{0},std::byte{0},std::byte{0}};
  require(std::ranges::equal(p.canonical_sizes(),sizes_golden),"Canonical size directory changed");
  require(request[8]==std::byte{7} && request[9]==std::byte{2} &&
      request[16]==std::byte{2} && request[20]==std::byte{50} &&
      std::all_of(request.begin()+24,request.begin()+32,[](auto x){return x==std::byte{0};}),
      "Fixed little-endian request header changed");
  for(std::size_t n=0;n<request.size();++n)
    rejects([&]{(void)decode_lz4_worker_request(std::span(request).first(n));});
  for(std::size_t at=0;at<request.size();++at) {
    if(at>=8&&at<16)continue; // A changed nonzero ID is a valid distinct request.
    auto bad=request;bad[at]^=std::byte{1};
    rejects([&]{(void)decode_lz4_worker_request(bad);});
  }
  for(std::size_t n=0;n<frame.size();++n)
    rejects([&]{(void)accept_lz4_worker_reply(std::span(frame).first(n),p,519,WorkerExecution::cpu_semantics);});
  for(std::size_t at=0;at<64+8*p.count();++at) {
    auto bad=frame;bad[at]^=std::byte{1};
    rejects([&]{(void)accept_lz4_worker_reply(bad,p,519,WorkerExecution::cpu_semantics);});
  }
  auto extended=request;extended.push_back(std::byte{0});
  rejects([&]{(void)decode_lz4_worker_request(extended);});
  extended=frame;extended.push_back(std::byte{0});
  rejects([&]{(void)accept_lz4_worker_reply(extended,p,519,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_lz4_worker_reply(frame,p,520,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_lz4_worker_reply(frame,p,519,WorkerExecution::hip);});
  rejects([&]{(void)decode_worker_request(request);});
  rejects([&]{(void)decode_match_worker_request(request);});
  rejects([&]{(void)decode_lz4_worker_request(frame);});
  rejects([&]{(void)encode_lz4_worker_request(p,0);});
  rejects([&]{(void)encode_lz4_worker_reply(p,0,payload_views,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)encode_lz4_worker_reply(p,1,payload_views,static_cast<WorkerExecution>(3));});
  auto bad=frame;std::fill(bad.begin()+8,bad.begin()+16,std::byte{0});
  rejects([&]{(void)accept_lz4_worker_reply(bad,p,519,WorkerExecution::cpu_semantics);});
  if(gpu_lz4_cpu_verification_available()) {
    bad=frame;bad.back()^=std::byte{1};
    rejects([&]{(void)accept_lz4_worker_reply(bad,p,519,WorkerExecution::cpu_semantics);});
    auto bad_payload=payloads;bad_payload.back().back()^=std::byte{1};
    rejects([&]{(void)encode_lz4_worker_reply(p,1,views(bad_payload),WorkerExecution::hip);});
  }
  auto moved=std::move(p);
  require(!p.ready() && !p.count() && p.canonical_sizes().empty(),"Moved preparation remained ready");
  rejects([&]{(void)encode_lz4_worker_request(p,1);});
  rejects([&]{(void)p.input(0);});
  require(moved.ready() && std::ranges::equal(moved.plaintext(),source_copy),"Move lost immutable input");
  auto assigned=parsed.batch;
  assigned=std::move(moved);
  require(assigned.ready() && !moved.ready() && std::ranges::equal(assigned.plaintext(),source_copy),
      "Move assignment left stale ownership");
  auto& same=assigned;
  assigned=std::move(same);
  require(assigned.ready() && std::ranges::equal(assigned.plaintext(),source_copy),"Self move changed source");
}
}
int main(){try{
  std::vector<Bytes> small{Bytes(17),Bytes(33)};
  for(std::size_t k=0;k<small.size();++k)for(std::size_t i=0;i<small[k].size();++i)
    small[k][i]=static_cast<std::byte>(i+31*k);
  roundtrip(small,true);
  for(const std::size_t size:{1U,14U,15U,16U,254U,255U,256U,4096U,65536U})
    roundtrip({Bytes(size,std::byte{71})},false);
  roundtrip(std::vector<Bytes>(8,Bytes(1024,std::byte{81})),false);
  roundtrip(std::vector<Bytes>(2,Bytes(32768,std::byte{91})),false);
  const std::array<Lz4WorkerBytes,0> empty{};
  rejects([&]{(void)PreparedLz4WorkerBatch::prepare(empty);});
  std::vector<Bytes> nine(9,Bytes(1)),zero(1),large(1,Bytes(65537)),allocation(4,Bytes(65536));
  for(const auto* bad:{&nine,&zero,&large,&allocation})
    rejects([&]{(void)PreparedLz4WorkerBatch::prepare(views(*bad));});
  const std::vector<Bytes> split_a{Bytes(4,std::byte{1}),Bytes(8,std::byte{1})};
  const std::vector<Bytes> split_b{Bytes(8,std::byte{1}),Bytes(4,std::byte{1})};
  const auto a=PreparedLz4WorkerBatch::prepare(views(split_a));
  const auto b=PreparedLz4WorkerBatch::prepare(views(split_b));
  require(a.source_digest()!=b.source_digest(),"Chunk boundaries absent from source digest");
  const auto old_match=PreparedGpuMatchSearch::prepare(Bytes(32,std::byte{4}));
  const auto old_request=encode_match_worker_request(old_match,7);
  rejects([&]{(void)decode_lz4_worker_request(old_request);});
  rejects([&]{(void)accept_lz4_worker_reply(old_request,a,7,WorkerExecution::cpu_semantics);});
  const auto frame=manual_reply(a,1,{literal(split_a[0]),literal(split_a[1])},WorkerExecution::cpu_semantics);
  rejects([&]{(void)accept_lz4_worker_reply(frame,b,1,WorkerExecution::cpu_semantics);});
  auto malformed=encode_lz4_worker_request(a,1);
  set32(malformed,16,0xffffffffU);
  rejects([&]{(void)decode_lz4_worker_request(malformed);});
  malformed=encode_lz4_worker_request(a,1);set32(malformed,68,0xffffffffU);
  rejects([&]{(void)decode_lz4_worker_request(malformed);});
#ifdef FLITZFS_TEST_HAS_LZ4
  Bytes source(65536);
  for(std::size_t i=0;i<source.size();++i)source[i]=static_cast<std::byte>((i/63)%251);
  const std::array<Lz4WorkerBytes,1> source_view{source};
  const auto p=PreparedLz4WorkerBatch::prepare(source_view);
  Bytes payload(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(source.size()))));
  const auto n=LZ4_compress_default(reinterpret_cast<const char*>(source.data()),
      reinterpret_cast<char*>(payload.data()),static_cast<int>(source.size()),static_cast<int>(payload.size()));
  require(n>0,"Pinned CPU fixture encoder failed");payload.resize(static_cast<std::size_t>(n));
  const std::array<Lz4WorkerBytes,1> candidate{payload};
  const auto encoded=encode_lz4_worker_reply(p,9,candidate,WorkerExecution::cpu_semantics);
  require(accept_lz4_worker_reply(encoded,p,9,WorkerExecution::cpu_semantics).front()==payload,
      "Actual pinned CPU LZ4 candidate changed");
#endif
  std::cout<<"LZ4 worker framing: "<<checks<<" checks; decoder_available="
      <<gpu_lz4_cpu_verification_available()<<" gpu_executed=false\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"LZ4 worker framing failed: "<<e.what()<<'\n';return 1;}}
