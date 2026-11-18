#include "native_group_anchor.hpp"
#include "anchor_search_fixture.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace match=flitzfs::anchor_match;
using namespace flitzfs::detail;
using Bytes=std::vector<std::byte>;

void check(bool condition,const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void rejects(auto&& action) {
  try { action(); }
  catch (const std::invalid_argument&) { return; }
  throw std::runtime_error("invalid anchor match input accepted");
}

void fragmented() {
  Bytes base(match::extent_bytes),members;
  std::uint64_t state=0xb10378af291edc65ULL;
  for (auto& byte:base) {
    state^=state<<13;
    state^=state>>7;
    state^=state<<17;
    byte=static_cast<std::byte>(state>>21);
  }
  for (unsigned i=1;i<=match::max_members;++i) {
    const auto member=anchor_search_fixture::fragmented(base,i);
    members.insert(members.end(),member.begin(),member.end());
  }
  const auto frame=encode_anchor_match_request(base,members,17);
  const auto request=decode_anchor_match_request(frame);
  check(request.id==17 && anchor_match_members(request.source)==match::max_members,"request binding");
  check(std::ranges::equal(std::span(request.source).first(base.size()),base) &&
      std::ranges::equal(std::span(request.source).subspan(base.size()),members),"owned source differs");
  auto records=cpu_anchor_match_records(request);
  const auto response=encode_anchor_match_reply(request,records);
  const auto advice=accept_anchor_match_reply(response,request);
  check(advice.size()==match::max_members,"reply member binding");
  for (const auto& ranges:advice) {
    std::uint32_t end=0,covered=0;
    for (const auto range:ranges) {
      check(range.member_offset>=end && range.length>0,"overlapping or empty verified range");
      end=range.member_offset+range.length;
      covered+=range.length;
    }
    check(covered>match::extent_bytes/2,"fragmented corpus lost match coverage");
  }
  for (const auto offset:{0,8,16,20,24,28,32}) {
    auto corrupt=response;
    corrupt[offset]^=std::byte{1};
    rejects([&]{(void)accept_anchor_match_reply(corrupt,request);});
  }
  rejects([&]{(void)accept_anchor_match_reply(std::span(response).first(response.size()-1),request);});
  auto corrupt=frame;
  corrupt.back()^=std::byte{1};
  rejects([&]{(void)decode_anchor_match_request(corrupt);});
  records.front()=match::empty;
  rejects([&]{(void)verify_anchor_match_records(request,records);});
  rejects([&]{(void)encode_anchor_match_request(base,{},1);});
  rejects([&]{(void)encode_anchor_match_request(base,members,0);});
  rejects([&]{(void)encode_anchor_match_request(std::span(base).subspan(1),members,1);});
}

void unrelated() {
  const Bytes base(match::extent_bytes,std::byte{'a'}),member(match::extent_bytes,std::byte{'b'});
  const auto request=decode_anchor_match_request(encode_anchor_match_request(base,member,2));
  auto records=cpu_anchor_match_records(request);
  check(records.size()==match::tiles && std::ranges::none_of(records,[](auto word){return word!=0;}),
      "unrelated input produced a match");
  records.front()=match::seed_bytes<<20; // Valid coordinates with unequal bytes.
  rejects([&]{(void)verify_anchor_match_records(request,records);});

  const auto identical=decode_anchor_match_request(encode_anchor_match_request(base,base,3));
  records=cpu_anchor_match_records(identical);
  const auto advice=verify_anchor_match_records(identical,records);
  check(!advice.front().empty(),"repetitive anchor should remain searchable");
  const auto tail=advice.front().back();
  check(tail.member_offset+tail.length==match::extent_bytes,"last repeat range misses the extent tail");
  records.back()=(match::maximum_match<<20)|65535;
  rejects([&]{(void)verify_anchor_match_records(identical,records);});
}

void sized_sweep() {
  constexpr std::uint32_t count=7;
  const auto plan=plan_anchor_match_sweep(count,match::max_members,match::device_bytes(3),3);
  check(plan.quantum_members==3 && plan.windows==3 && plan.device_bytes==match::device_bytes(3),
      "whole request did not retain its final partial device window");
  const auto smaller=plan_anchor_match_sweep(count,3,match::device_bytes(2),4);
  check(smaller.quantum_members==2 && smaller.windows==4,"live device budget did not reduce the quantum");
  rejects([&]{(void)plan_anchor_match_sweep(count,3,match::device_bytes(2),3);});
  rejects([&]{(void)plan_anchor_match_sweep(count,3,match::device_bytes(1)-1,7);});
  rejects([&]{(void)plan_anchor_match_sweep(count,0,match::device_bytes(3),7);});
  rejects([&]{(void)anchor_match_layout((std::numeric_limits<std::uint32_t>::max)());});

  Bytes base(match::extent_bytes),members;
  std::uint64_t state=0xc71199403c8bfb63ULL;
  for (auto& byte:base) {
    state^=state<<13;
    state^=state>>7;
    state^=state<<17;
    byte=static_cast<std::byte>(state>>21);
  }
  for (unsigned index=1;index<=count;++index) {
    const auto member=anchor_search_fixture::fragmented(base,index);
    members.insert(members.end(),member.begin(),member.end());
  }
  rejects([&]{(void)encode_anchor_match_request(base,members,21);});
  const auto frame=encode_anchor_match_request(base,members,21,plan.whole);
  check(frame.size()==plan.whole.request_bytes,"sized anchor frame bounds");
  rejects([&]{(void)decode_anchor_match_request(frame);});
  auto wrong=plan.whole;
  --wrong.reply_bytes;
  rejects([&]{(void)decode_anchor_match_request(frame,wrong);});

  const auto request=decode_anchor_match_request(frame,plan.whole);
  auto records=cpu_anchor_match_records(request);
  for (std::uint32_t first=0;first<count;) {
    const auto n=(std::min)(plan.quantum_members,count-first);
    const auto part=std::span(members).subspan(first*match::extent_bytes,n*match::extent_bytes);
    const auto subrequest=decode_anchor_match_request(encode_anchor_match_request(base,part,22));
    const auto response=cpu_anchor_match_records(subrequest);
    check(std::ranges::equal(response,std::span(records).subspan(first*match::tiles,n*match::tiles)),
        "window changed canonical records");
    first+=n;
  }
  const auto reply=encode_anchor_match_reply(request,records);
  check(reply.size()==plan.whole.reply_bytes && accept_anchor_match_reply(reply,request).size()==count,
      "sized anchor reply did not cover the whole request");
  records.back()=match::empty;
  rejects([&]{(void)encode_anchor_match_reply(request,records);});
}
}

int main() {
  try {
    fragmented();
    unrelated();
    sized_sweep();
    std::cout<<"Direct shared base match CPU semantics and reply verification passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
