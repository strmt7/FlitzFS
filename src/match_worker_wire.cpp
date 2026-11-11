#include "flitzfs/match_worker_wire.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
constexpr std::array<std::byte,8> request_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'F'},std::byte{'S'},
    std::byte{'M'},std::byte{'R'},std::byte{'Q'},std::byte{1}};
constexpr std::array<std::byte,8> reply_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'F'},std::byte{'S'},
    std::byte{'M'},std::byte{'R'},std::byte{'P'},std::byte{1}};
[[noreturn]] void invalid(const char* why){throw std::invalid_argument(why);}
void put(Bytes& frame,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)frame.push_back(static_cast<std::byte>((value>>(i*8))&255U));
}
std::uint64_t get(std::span<const std::byte> frame,std::size_t at,unsigned width) {
  std::uint64_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned>(frame[at+i]))<<(i*8);
  return value;
}
void validate_input(const PreparedGpuMatchSearch& search,std::uint64_t id) {
  if(!id || search.input().size()<4 || search.input().size()>match_detail::input_limit)
    invalid("invalid match worker input");
}
void validate_execution(WorkerExecution execution) {
  if(execution!=WorkerExecution::cpu_semantics && execution!=WorkerExecution::hip)
    invalid("unknown match execution backend");
}
Bytes make_header(const PreparedGpuMatchSearch& search,std::uint64_t id,WorkerExecution execution,bool reply) {
  validate_input(search,id);
  Bytes frame;frame.reserve(reply?kMatchWorkerReplyLimit:kMatchWorkerRequestLimit);
  const auto& magic=reply?reply_magic:request_magic;
  frame.insert(frame.end(),magic.begin(),magic.end());
  put(frame,id,8);put(frame,search.input().size(),4);put(frame,search.lanes(),4);
  put(frame,reply?static_cast<std::uint32_t>(execution):0,4);put(frame,0,4);
  const auto digest=detail::record_digest(search.input());
  frame.insert(frame.end(),digest.begin(),digest.end());
  return frame;
}
void header(std::span<const std::byte> frame,std::size_t maximum,const std::array<std::byte,8>& magic) {
  if(frame.size()<kMatchWorkerHeaderBytes || frame.size()>maximum ||
      !std::equal(magic.begin(),magic.end(),frame.begin()) || get(frame,28,4)!=0)
    invalid("invalid match worker frame header");
}
}
std::vector<std::byte> encode_match_worker_request(const PreparedGpuMatchSearch& search,std::uint64_t id) {
  auto frame=make_header(search,id,WorkerExecution::cpu_semantics,false);
  frame.insert(frame.end(),search.input().begin(),search.input().end());
  return frame;
}
MatchWorkerRequest decode_match_worker_request(std::span<const std::byte> frame) {
  header(frame,kMatchWorkerRequestLimit,request_magic);
  const auto id=get(frame,8,8),size=get(frame,16,4),lanes=get(frame,20,4);
  if(!id || size<4 || size>match_detail::input_limit || lanes!=match_detail::lanes(static_cast<unsigned>(size)) ||
      get(frame,24,4)!=0 || frame.size()!=kMatchWorkerHeaderBytes+size)
    invalid("invalid match worker request shape");
  const auto input=frame.subspan(kMatchWorkerHeaderBytes);
  const auto digest=detail::record_digest(input);
  if(!std::equal(digest.begin(),digest.end(),frame.begin()+32))
    invalid("match worker input digest mismatch");
  return {id,PreparedGpuMatchSearch::prepare(input)};
}
std::vector<std::byte> encode_match_worker_reply(const PreparedGpuMatchSearch& search,std::uint64_t id,
    std::span<const match_detail::Match> matches,WorkerExecution execution) {
  validate_execution(execution);
  (void)search.verified_proposals(matches);
  auto frame=make_header(search,id,execution,true);
  for(const auto& match:matches) {
    put(frame,match.distance,2);put(frame,match.length,2);
    put(frame,match.candidates,2);put(frame,match.compared,2);
  }
  return frame;
}
std::vector<StoreMatchProposal> accept_match_worker_reply(std::span<const std::byte> frame,
    const PreparedGpuMatchSearch& search,std::uint64_t id,WorkerExecution execution) {
  validate_input(search,id);validate_execution(execution);
  header(frame,kMatchWorkerReplyLimit,reply_magic);
  const auto count=get(frame,20,4);
  const auto digest=detail::record_digest(search.input());
  if(get(frame,8,8)!=id || get(frame,16,4)!=search.input().size() ||
      count!=search.lanes() || get(frame,24,4)!=static_cast<std::uint32_t>(execution) ||
      frame.size()!=kMatchWorkerHeaderBytes+count*sizeof(match_detail::Match) ||
      !std::equal(digest.begin(),digest.end(),frame.begin()+32))
    invalid("match worker reply binding mismatch");
  std::vector<match_detail::Match> matches(static_cast<std::size_t>(count));
  std::size_t at=kMatchWorkerHeaderBytes;
  for(auto& match:matches) {
    match.distance=static_cast<match_detail::u16>(get(frame,at,2));at+=2;
    match.length=static_cast<match_detail::u16>(get(frame,at,2));at+=2;
    match.candidates=static_cast<match_detail::u16>(get(frame,at,2));at+=2;
    match.compared=static_cast<match_detail::u16>(get(frame,at,2));at+=2;
  }
  return search.verified_proposals(matches);
}
}
