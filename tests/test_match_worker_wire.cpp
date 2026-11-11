#include "flitzfs/match_worker_wire.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("malformed match wire accepted");}
std::vector<match_detail::Match> cpu(const PreparedGpuMatchSearch& plan) {
  std::vector<match_detail::Match> result(plan.lanes());
  for(unsigned lane=0;lane<plan.lanes();++lane)
    result[lane]=match_detail::search(reinterpret_cast<const unsigned char*>(plan.input().data()),
        static_cast<unsigned>(plan.input().size()),plan.predecessors().data(),
        static_cast<unsigned>(plan.predecessors().size()),lane);
  return result;
}
}
int main(){try {
  Bytes input(128);
  for(unsigned i=0;i<input.size();++i)input[i]=static_cast<std::byte>(i%31);
  const auto plan=PreparedGpuMatchSearch::prepare(input);
  const auto request=encode_match_worker_request(plan,519);
  const auto parsed=decode_match_worker_request(request);
  require(parsed.request_id==519&&std::ranges::equal(plan.input(),parsed.search.input())&&
      std::ranges::equal(plan.predecessors(),parsed.search.predecessors()),"match request changed source/index");
  const auto matches=cpu(parsed.search);
  const auto frame=encode_match_worker_reply(parsed.search,519,matches,WorkerExecution::cpu_semantics);
  const auto proposals=accept_match_worker_reply(frame,plan,519,WorkerExecution::cpu_semantics);
  require(!proposals.empty(),"valid match wire produced no proposals");
  for(std::size_t size=0;size<request.size();++size)
    rejects([&]{(void)decode_match_worker_request(std::span(request).first(size));});
  for(unsigned at=0;at<64;++at) {
    auto bad=frame;bad[at]^=std::byte{1};
    rejects([&]{(void)accept_match_worker_reply(bad,plan,519,WorkerExecution::cpu_semantics);});
  }
  for(const unsigned at:{0U,16U,20U,24U,28U,32U,64U}) {
    auto bad=request;bad[at]^=std::byte{1};
    rejects([&]{(void)decode_match_worker_request(bad);});
  }
  auto wrong=request;std::fill(wrong.begin()+8,wrong.begin()+16,std::byte{0});
  rejects([&]{(void)decode_match_worker_request(wrong);});
  auto extended=request;extended.push_back(std::byte{0});
  rejects([&]{(void)decode_match_worker_request(extended);});
  auto malformed=frame;malformed[64]=std::byte{1};
  rejects([&]{(void)accept_match_worker_reply(malformed,plan,519,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_match_worker_reply(frame,plan,520,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)accept_match_worker_reply(frame,plan,519,WorkerExecution::hip);});
  auto other=input;other[0]^=std::byte{1};
  const auto foreign=PreparedGpuMatchSearch::prepare(other);
  rejects([&]{(void)accept_match_worker_reply(frame,foreign,519,WorkerExecution::cpu_semantics);});
  rejects([&]{(void)decode_worker_request(request);});
  rejects([&]{(void)encode_match_worker_request(plan,0);});
  rejects([&]{(void)encode_match_worker_request(PreparedGpuMatchSearch::prepare(Bytes(3)),1);});
  const auto full=PreparedGpuMatchSearch::prepare(Bytes(65536,std::byte{71}));
  const auto full_request=encode_match_worker_request(full,1);
  const auto full_reply=encode_match_worker_reply(full,1,cpu(full),WorkerExecution::hip);
  require(full_request.size()==65600&&full_reply.size()==32832&&
      !accept_match_worker_reply(full_reply,full,1,WorkerExecution::hip).empty(),"match wire boundary shape");
  std::cout<<"Match worker wire: checks="<<checks<<" maximum_request=65600 maximum_reply=32832 gpu_executed=false\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"Match worker wire failed: "<<error.what()<<'\n';return 1;}}
