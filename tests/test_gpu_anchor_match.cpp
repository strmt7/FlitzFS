#include "native_group_anchor.hpp"
#include "anchor_search_fixture.hpp"
#include "record_digest.hpp"
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
  auto records=cpu_anchor_match_records(request.view());
  const auto response=encode_anchor_match_reply(request.view(),records);
  const auto advice=accept_anchor_match_reply(response,request.view());
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
    rejects([&]{(void)accept_anchor_match_reply(corrupt,request.view());});
  }
  rejects([&]{(void)accept_anchor_match_reply(std::span(response).first(response.size()-1),request.view());});
  auto corrupt=frame;
  corrupt.back()^=std::byte{1};
  rejects([&]{(void)decode_anchor_match_request(corrupt);});
  records.front()=match::empty;
  rejects([&]{(void)verify_anchor_match_records(request.view(),records);});
  rejects([&]{(void)encode_anchor_match_request(base,{},1);});
  rejects([&]{(void)encode_anchor_match_request(base,members,0);});
  rejects([&]{(void)encode_anchor_match_request(std::span(base).subspan(1),members,1);});
}

void unrelated() {
  const Bytes base(match::extent_bytes,std::byte{'a'}),member(match::extent_bytes,std::byte{'b'});
  const auto request=decode_anchor_match_request(encode_anchor_match_request(base,member,2));
  auto records=cpu_anchor_match_records(request.view());
  check(records.size()==match::tiles && std::ranges::none_of(records,[](auto word){return word!=0;}),
      "unrelated input produced a match");
  records.front()=match::seed_bytes<<20; // Valid coordinates with unequal bytes.
  rejects([&]{(void)verify_anchor_match_records(request.view(),records);});

  const auto identical=decode_anchor_match_request(encode_anchor_match_request(base,base,3));
  records=cpu_anchor_match_records(identical.view());
  const auto advice=verify_anchor_match_records(identical.view(),records);
  check(!advice.front().empty(),"repetitive anchor should remain searchable");
  const auto tail=advice.front().back();
  check(tail.member_offset+tail.length==match::extent_bytes,"last repeat range misses the extent tail");
  records.back()=(match::maximum_match<<20)|65535;
  rejects([&]{(void)verify_anchor_match_records(identical.view(),records);});
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
  auto records=cpu_anchor_match_records(request.view());
  for (std::uint32_t first=0;first<count;) {
    const auto n=(std::min)(plan.quantum_members,count-first);
    const auto part=std::span(members).subspan(first*match::extent_bytes,n*match::extent_bytes);
    const auto subrequest=decode_anchor_match_request(encode_anchor_match_request(base,part,22));
    const auto response=cpu_anchor_match_records(subrequest.view());
    check(std::ranges::equal(response,std::span(records).subspan(first*match::tiles,n*match::tiles)),
        "window changed canonical records");
    first+=n;
  }
  const auto reply=encode_anchor_match_reply(request.view(),records);
  check(reply.size()==plan.whole.reply_bytes && accept_anchor_match_reply(reply,request.view()).size()==count,
      "sized anchor reply did not cover the whole request");
  records.back()=match::empty;
  rejects([&]{(void)encode_anchor_match_reply(request.view(),records);});
}

void independent_batch() {
  constexpr std::array<std::uint32_t,3> counts{7,1,4};
  constexpr auto device=match::device_bytes(3);
  const auto plan=plan_anchor_match_batch(counts,3,device,9);
  check(plan.groups.size()==counts.size() && plan.launches==9 && plan.device_bytes==device &&
      plan.groups[1].quantum_members==1 && plan.groups[2].windows==2,
      "batch did not reuse one slab for mixed group shapes");
  rejects([&]{(void)plan_anchor_match_batch(counts,3,device,8);});
  rejects([&]{(void)plan_anchor_match_batch(counts,3,device-1,9);});
  const auto original=plan_anchor_match_transfers(plan,device);
  check(original==AnchorMatchTransferPlan{3,device,6},"one dispatch per transfer");
  check(plan_anchor_match_transfers(plan,match::device_bytes(5))==original,
      "transfer grant must round down to a dispatch boundary");
  check(plan_anchor_match_transfers(plan,match::device_bytes(6))==
      AnchorMatchTransferPlan{6,match::device_bytes(6),4},"funded transfers must preserve a final partial dispatch");
  check(plan_anchor_match_transfers(plan,match::device_bytes(7))==
      AnchorMatchTransferPlan{7,match::device_bytes(7),3},"entire groups fit separate transfers");
  check(plan_anchor_match_transfers(plan,(std::numeric_limits<std::uint64_t>::max)())==
      plan_anchor_match_transfers(plan,match::device_bytes(7)),"device allocation must clamp to data size");
  rejects([&]{(void)plan_anchor_match_transfers(plan,device-1);});
  auto inconsistent=plan;
  inconsistent.groups.front().quantum_members=2;
  rejects([&]{(void)plan_anchor_match_transfers(inconsistent,device);});
  constexpr auto largest=(std::numeric_limits<std::int32_t>::max)()/match::extent_bytes-1;
  const std::array<std::uint32_t,2> oversized{largest,1};
  rejects([&]{(void)plan_anchor_match_batch(oversized,3,device,20000);});

  std::vector<AnchorMatchRequest> groups;
  for(std::size_t i=0;i<counts.size();++i) {
    Bytes base(match::extent_bytes),members;
    std::uint64_t state=0x7febc654bd02c2c5ULL+i;
    for(auto& byte:base) {
      state^=state<<13;
      state^=state>>7;
      state^=state<<17;
      byte=static_cast<std::byte>(state>>21);
    }
    for(std::uint32_t j=0;j<counts[i];++j) {
      const auto member=anchor_search_fixture::fragmented(base,j+1);
      members.insert(members.end(),member.begin(),member.end());
    }
    const auto layout=plan.groups[i].whole;
    groups.push_back(decode_anchor_match_request(encode_anchor_match_request(base,members,17+i,layout),layout));
  }
  auto frame=encode_anchor_match_batch_request(groups,500,plan);
  const auto allocation=frame.data();
  auto snapshot=std::make_shared<const flitzfs::WorkerRequestSnapshot>(std::move(frame));
  const auto decode=[&](flitzfs::SharedWorkerRequest bytes) {
    return decode_anchor_match_batch_request(std::move(bytes),plan.request_bytes,plan.reply_bytes,3,device,plan.launches);
  };
  auto decoded=decode(snapshot);
  snapshot.reset();
  const auto batch=std::move(decoded);
  const auto wire=batch.snapshot()->bytes();
  check(batch.id()==500 && batch.plan()==plan && wire.size()==plan.request_bytes && wire.data()==allocation,
      "batch did not retain the original private frame");
  check(decoded.groups().empty() && batch.groups().back().source.data()==wire.last(groups.back().source.size()).data(),
      "batch group view did not stay within the shared frame");
  Bytes corrupt(wire.begin(),wire.end());
  corrupt.back()^=std::byte{1};
  rejects([&]{(void)decode(std::make_shared<const flitzfs::WorkerRequestSnapshot>(std::move(corrupt)));});
  corrupt.assign(wire.begin(),wire.end());
  constexpr std::size_t digest_offset=32,header_bytes=64;
  const auto last_group_header=wire.size()-plan.groups.back().whole.request_bytes;
  corrupt[last_group_header+digest_offset]^=std::byte{1};
  const auto outer_digest=record_digest(std::span<const std::byte>(corrupt).subspan(header_bytes));
  std::copy(outer_digest.begin(),outer_digest.end(),corrupt.begin()+digest_offset);
  rejects([&]{(void)decode(std::make_shared<const flitzfs::WorkerRequestSnapshot>(std::move(corrupt)));});
  rejects([&]{(void)decode(std::make_shared<const flitzfs::WorkerRequestSnapshot>(wire.first(wire.size()-1)));});
  rejects([&]{(void)decode({});});
  rejects([&]{(void)decode_anchor_match_batch_request(batch.snapshot(),plan.request_bytes,plan.reply_bytes-1,3,device,9);});
  rejects([&]{(void)decode_anchor_match_batch_request(batch.snapshot(),plan.request_bytes,plan.reply_bytes,3,device,8);});
  groups.back().id=groups.front().id;
  rejects([&]{(void)encode_anchor_match_batch_request(groups,500,plan);});

  std::vector<std::vector<match::u32>> records;
  for(std::size_t i=0;i<groups.size();++i) {
    check(std::ranges::equal(batch.groups()[i].source,groups[i].source) &&
        batch.groups()[i].source_digest==groups[i].source_digest,
        "batch did not preserve independent source bytes");
    records.push_back(cpu_anchor_match_records(batch.groups()[i]));
  }
  const auto reply=encode_anchor_match_batch_reply(batch,records);
  const auto last_reply=encode_anchor_match_reply(batch.groups().back(),records.back());
  check(std::ranges::equal(std::span(reply).last(last_reply.size()),last_reply),
      "batch serialization changed the nested reply bytes");
  const auto advice=accept_anchor_match_batch_reply(reply,batch);
  check(advice.size()==counts.size() && reply.size()==plan.reply_bytes,"batch did not return all results");
  for(std::size_t i=0;i<counts.size();++i) {
    check(advice[i].size()==counts[i] && std::ranges::all_of(advice[i],[](const auto& ranges){return !ranges.empty();}),
        "batch did not bind coverage to each independent group");
  }
  corrupt=reply;
  std::fill_n(corrupt.end()-sizeof(match::u32),sizeof(match::u32),std::byte{0xff});
  rejects([&]{(void)accept_anchor_match_batch_reply(corrupt,batch);});
  rejects([&]{(void)accept_anchor_match_batch_reply(std::span(reply).first(reply.size()-1),batch);});
  records.back().front()=match::empty;
  rejects([&]{(void)encode_anchor_match_batch_reply(batch,records);});
}
}

int main() {
  try {
    fragmented();
    unrelated();
    sized_sweep();
    independent_batch();
    std::cout<<"Direct shared base match CPU semantics and reply verification passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
