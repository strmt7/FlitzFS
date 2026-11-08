#include "flitzfs/worker_wire.hpp"
#include "flzip_native_fixture.hpp"
#include "gpu/decode_kernel.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace flitzfs;
using namespace flzip_fixture;
unsigned checks=0;
void check(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F&& f) {
  try {f();} catch(const std::exception&) {++checks;return;}
  throw std::runtime_error("invalid worker frame accepted");
}
PreparedGpuBatch prepare(const std::vector<Block>& blocks) {
  std::vector<GpuBlockView> views;
  for(const auto& b:blocks)views.push_back({b.kind,b.fill,static_cast<std::uint32_t>(b.plain.size()),b.encoded});
  return PreparedGpuBatch::prepare(views);
}
GpuBatchReply host(const WorkerBatchRequest& request) {
  const auto& b=request.batch;
  GpuBatchReply reply{request.request_id,Bytes(b.cpu_reference().size()),{}};
  reply.status.resize(b.tasks().size(),gpu_detail::pending);
  const unsigned char dummy=0;
  const auto* input=b.payload().empty()?&dummy:reinterpret_cast<const unsigned char*>(b.payload().data());
  for(std::size_t i=reply.status.size();i>0;--i)
    reply.status[i-1]=gpu_detail::decode_task(b.tasks()[i-1],input,static_cast<unsigned>(b.payload().size()),
        reinterpret_cast<unsigned char*>(reply.bytes.data()),static_cast<unsigned>(reply.bytes.size()));
  return reply;
}
void roundtrip(const std::vector<Block>& blocks) {
  const auto batch=prepare(blocks);
  auto wire=encode_worker_request(batch,817);
  auto request=decode_worker_request(wire);
  check(request.request_id==817,"request identity changed");
  check(encode_worker_request(request.batch,817)==wire,"request canonical encoding mismatch");
  check(request.batch.cpu_reference().size()==batch.cpu_reference().size() &&
      std::equal(request.batch.cpu_reference().begin(),request.batch.cpu_reference().end(),batch.cpu_reference().begin()),
      "independent worker CPU parity");
  // Worker owns its bytes; subsequent changes in transport storage cannot alter them.
  std::fill(wire.begin(),wire.end(),std::byte{0});
  auto frame=encode_worker_reply(host(request),WorkerExecution::cpu_semantics);
  auto accepted=accept_worker_reply(frame,batch,817,WorkerExecution::cpu_semantics);
  check(accepted.bytes==Bytes(batch.cpu_reference().begin(),batch.cpu_reference().end()),"accepted output parity");
  rejects([&]{accept_worker_reply(frame,batch,818,WorkerExecution::cpu_semantics);});
  rejects([&]{accept_worker_reply(frame,batch,817,WorkerExecution::hip);});
  // Successful semantic execution cannot be relabeled hardware by the parent.
  frame.back()^=std::byte{1};
  rejects([&]{accept_worker_reply(frame,batch,817,WorkerExecution::cpu_semantics);});
}
}
int main() {try {
  roundtrip({raw(text("0123456789"))});roundtrip({fill(1,0)});
  roundtrip(std::vector<Block>(8,fill(1,255)));
  roundtrip(std::vector<Block>(4,raw(Bytes(65536,std::byte{37}))));
  roundtrip({raw(text("prefix")),fill(4097,31),pattern(8193),prefix_sample(),prefix_sample(true),huffman()});
  const auto batch=prepare({raw(text("abc"))});
  const auto good=encode_worker_request(batch,17);
  check(good.size()==47,"fixed wire extent changed");
  check(good[8]==std::byte{17} && good[16]==std::byte{1} && good[20]==std::byte{3} &&
      good[24]==std::byte{3} && good[36]==std::byte{3} && good[40]==std::byte{3},"wire is not little-endian");
  for(std::size_t n=0;n<good.size();++n)
    rejects([&]{decode_worker_request(std::span<const std::byte>(good).first(n));});
  auto mutate=[&](std::size_t at,std::uint64_t value,unsigned width) {
    auto bad=good;replace(bad,at,value,width);rejects([&]{decode_worker_request(bad);});
  };
  mutate(0,0,1);mutate(8,0,8);mutate(16,0,4);mutate(16,9,4);mutate(16,0xffffffffU,4);
  mutate(20,0xffffffffU,4);mutate(24,0,4);mutate(24,262145,4);mutate(28,1,4);
  mutate(32,255,1);mutate(34,1,2);mutate(36,0,4);mutate(36,65537,4);mutate(40,4,4);
  auto extra=good;extra.push_back(std::byte{0});rejects([&]{decode_worker_request(extra);});
  rejects([&]{encode_worker_request(batch,0);});
  auto huge=good;huge.resize(kWorkerRequestLimit+1);rejects([&]{decode_worker_request(huge);});
  const auto mixed=encode_worker_request(prepare({fill(65536,7),fill(65536,9)}),83);
  auto bad_totals=mixed;replace(bad_totals,24,65536,4);
  rejects([&]{decode_worker_request(bad_totals);});
  auto invalid_entropy=encode_worker_request(prepare({prefix_sample()}),29);
  replace(invalid_entropy,kWorkerHeaderBytes+kWorkerRecordBytes,1,4);
  rejects([&]{decode_worker_request(invalid_entropy);});
  const auto request=decode_worker_request(good);
  const auto reply=encode_worker_reply(host(request),WorkerExecution::cpu_semantics);
  for(std::size_t n=0;n<reply.size();++n)
    rejects([&]{accept_worker_reply(std::span<const std::byte>(reply).first(n),batch,17,WorkerExecution::cpu_semantics);});
  for(std::size_t i=0;i<reply.size();++i) {
    auto corrupt=reply;corrupt[i]^=std::byte{0x80};
    rejects([&]{accept_worker_reply(corrupt,batch,17,WorkerExecution::cpu_semantics);});
  }
  auto extra_reply=reply;extra_reply.push_back(std::byte{0});
  rejects([&]{accept_worker_reply(extra_reply,batch,17,WorkerExecution::cpu_semantics);});
  extra_reply.resize(kWorkerReplyLimit+1);
  rejects([&]{accept_worker_reply(extra_reply,batch,17,WorkerExecution::cpu_semantics);});
  for(auto status:{gpu_detail::pending,1U,0xffffffffU}) {
    auto incomplete=host(request);incomplete.status[0]=status;
    rejects([&]{accept_worker_reply(encode_worker_reply(incomplete,WorkerExecution::cpu_semantics),
        batch,17,WorkerExecution::cpu_semantics);});
  }
  auto incomplete=host(request);incomplete.generation=0;
  rejects([&]{encode_worker_reply(incomplete,WorkerExecution::cpu_semantics);});
  incomplete=host(request);incomplete.status.clear();rejects([&]{encode_worker_reply(incomplete,WorkerExecution::cpu_semantics);});
  incomplete=host(request);incomplete.bytes.clear();rejects([&]{encode_worker_reply(incomplete,WorkerExecution::cpu_semantics);});
  incomplete=host(request);incomplete.bytes.resize(gpu_detail::max_batch_bytes+1);
  rejects([&]{encode_worker_reply(incomplete,WorkerExecution::cpu_semantics);});
  incomplete=host(request);incomplete.status.resize(gpu_detail::max_tasks+1);
  rejects([&]{encode_worker_reply(incomplete,WorkerExecution::cpu_semantics);});
  rejects([&]{encode_worker_reply(host(request),static_cast<WorkerExecution>(999));});
  rejects([&]{accept_worker_reply(reply,batch,17,static_cast<WorkerExecution>(999));});
  // Deterministic parser mutations are bounded; accepted requests must re-encode
  // exactly and agree with independently checked child execution.
  std::uint32_t seed=0x48ab19U;
  for(unsigned n=0;n<512;++n) {
    auto candidate=good;seed=1664525U*seed+1013904223U;
    candidate[seed%candidate.size()]^=static_cast<std::byte>(1U<<((seed>>16)%8));
    try {
      auto parsed=decode_worker_request(candidate);
      check(encode_worker_request(parsed.batch,parsed.request_id)==candidate,"mutation normalized unexpectedly");
      auto wire=encode_worker_reply(host(parsed),WorkerExecution::cpu_semantics);
      (void)accept_worker_reply(wire,parsed.batch,parsed.request_id,WorkerExecution::cpu_semantics);
    } catch(const std::invalid_argument&) {++checks;}
  }
  std::cout<<"PASS: "<<checks<<" bounded worker wire assertions; host semantics only.\n";
  return 0;
} catch(const std::exception& e) {std::cerr<<"worker wire failure after "<<checks<<": "<<e.what()<<'\n';return 1;}}
