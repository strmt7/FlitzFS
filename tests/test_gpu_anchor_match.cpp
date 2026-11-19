#include "native_group_anchor.hpp"
#include "native_group_codec.hpp"
#include "anchor_search_fixture.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace match=flitzfs::anchor_match;
using namespace flitzfs::detail;
using flitzfs::NativePackedXorCheck;
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
  constexpr std::size_t header_bytes=64;
  for(std::size_t index=0;index<records.size();++index) {
    for(unsigned byte=0;byte<4;++byte) {
      const auto expected=static_cast<std::byte>((records[index]>>(8*byte))&0xff);
      check(response[header_bytes+index*4+byte]==expected,"anchor record is not little-endian");
    }
  }
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

  inconsistent=plan;
  ++inconsistent.request_bytes;
  rejects([&]{(void)AnchorMatchBatchBuilder(500,inconsistent);});
  AnchorMatchBatchBuilder builder(500,plan);
  rejects([&]{(void)builder.finish();});
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
    if(i)rejects([&]{builder.append(16+i,base,members);});
    rejects([&]{builder.append(17+i,base,std::span(members).first(members.size()-1));});
    builder.append(17+i,base,members);
    groups.push_back(decode_anchor_match_request(encode_anchor_match_request(base,members,17+i,layout),layout));
  }
  rejects([&]{builder.append(500,{},{});});
  auto created=builder.finish();
  rejects([&]{(void)builder.finish();});
  rejects([&]{builder.append(500,{},{});});
  auto snapshot=created.snapshot();
  const auto allocation=snapshot->bytes().data();
  const auto decode=[&](flitzfs::SharedWorkerRequest bytes) {
    return decode_anchor_match_batch_request(std::move(bytes),plan.request_bytes,plan.reply_bytes,3,device,plan.launches);
  };
  auto decoded=decode(snapshot);
  snapshot.reset();
  const auto batch=std::move(created);
  const auto wire=batch.snapshot()->bytes();
  check(batch.id()==500 && batch.plan()==plan && wire.size()==plan.request_bytes && wire.data()==allocation,
      "batch did not retain the original private frame");
  check(created.groups().empty() && batch.groups().back().source.data()==wire.last(groups.back().source.size()).data(),
      "batch group view did not stay within the shared frame");
  check(batch.id()==decoded.id() && batch.plan()==decoded.plan() && batch.digest()==decoded.digest() &&
      batch.groups().size()==decoded.groups().size(),"built batch differs from independently parsed wire");
  for(std::size_t i=0;i<batch.groups().size();++i) {
    const auto& built=batch.groups()[i];
    const auto& parsed=decoded.groups()[i];
    check(built.id==parsed.id && built.source_digest==parsed.source_digest &&
        built.source.data()==parsed.source.data() && built.source.size()==parsed.source.size(),
        "built group differs from independently parsed wire");
  }
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
  const auto source=std::span(groups.back().source);
  const auto nested=encode_anchor_match_request(source.first(match::extent_bytes),
      source.subspan(match::extent_bytes),groups.back().id,plan.groups.back().whole);
  check(std::ranges::equal(wire.last(nested.size()),nested),"batch changed the nested request wire bytes");

  std::vector<std::vector<match::u32>> records;
  for(std::size_t i=0;i<groups.size();++i) {
    check(std::ranges::equal(batch.groups()[i].source,groups[i].source) &&
        batch.groups()[i].source_digest==groups[i].source_digest,
        "batch did not preserve independent source bytes");
    records.push_back(cpu_anchor_match_records(batch.groups()[i]));
  }
  const auto reply=encode_anchor_match_batch_reply(batch,records);
  constexpr std::size_t profile_capacity=256;
  const auto with_reserve=encode_anchor_match_batch_reply(batch,records,profile_capacity);
  check(with_reserve==reply && with_reserve.capacity()>=reply.size()+profile_capacity,
      "profile reserve changed the reply or requires a second allocation");
  rejects([&]{(void)encode_anchor_match_batch_reply(batch,records,(std::numeric_limits<std::size_t>::max)());});
  const auto last_reply=encode_anchor_match_reply(batch.groups().back(),records.back());
  check(std::ranges::equal(std::span(reply).last(last_reply.size()),last_reply),
      "batch serialization changed the nested reply bytes");
  Bytes unaligned_reply(1,std::byte{0});
  unaligned_reply.insert(unaligned_reply.end(),reply.begin(),reply.end());
  const auto advice=accept_anchor_match_batch_reply(std::span(unaligned_reply).subspan(1),batch);
  check(advice.size()==counts.size() && reply.size()==plan.reply_bytes,"batch did not return all results");
  for(std::size_t i=0;i<counts.size();++i) {
    check(advice[i].size()==counts[i] && std::ranges::all_of(advice[i],[](const auto& ranges){return !ranges.empty();}),
        "batch did not bind coverage to each independent group");
  }
  corrupt=reply;
  corrupt[reply.size()-last_reply.size()+digest_offset]^=std::byte{1};
  rejects([&]{(void)accept_anchor_match_batch_reply(corrupt,batch);});
  corrupt=reply;
  std::fill_n(corrupt.end()-sizeof(match::u32),sizeof(match::u32),std::byte{0xff});
  rejects([&]{(void)accept_anchor_match_batch_reply(corrupt,batch);});
  rejects([&]{(void)accept_anchor_match_batch_reply(std::span(reply).first(reply.size()-1),batch);});
  records.back().front()=match::empty;
  rejects([&]{(void)encode_anchor_match_batch_reply(batch,records);});
}
void put_xor_test_slot(std::span<match::u32> slot,std::span<const std::byte> body) {
  check(sizeof(match::u32)+body.size()<=slot.size_bytes(),"fixture body exceeds its slot");
  slot[0]=static_cast<match::u32>(body.size());
  for(std::size_t i=0;i<body.size();++i) {
    const auto shift=static_cast<unsigned>(i%4)*8;
    auto& word=slot[1+i/4];
    word=(word&~(255U<<shift))|(std::to_integer<unsigned>(body[i])<<shift);
  }
}
void packed_xor_reply() {
  const std::array<std::uint32_t,1> counts{2};
  const auto plan=plan_anchor_match_batch(counts,2,match::device_bytes(2),3,match::BatchOutput::packed_xor);
  AnchorMatchBatchBuilder builder(91,plan);
  Bytes base(match::extent_bytes,std::byte{0x73});
  Bytes members(2*match::extent_bytes,std::byte{0x73});
  members[0]^=std::byte{0x80};
  members[255]^=std::byte{0x04};
  builder.append(1,base,members);
  const auto request=builder.finish();
  constexpr auto id=kNativeGroupBaseBit+1;
  const auto member=std::span<const std::byte>(members).first(match::extent_bytes);
  const auto candidate=encode_native_group_xor(member,base,id,1,match::extent_bytes);
  check(candidate.has_value(),"missing independent XOR test body");
  const auto body=std::span<const std::byte>(*candidate).subspan(128);
  std::vector<std::vector<match::u32>> slots(1,std::vector<match::u32>(2*match::tiles,match::empty));
  put_xor_test_slot(std::span(slots.front()).first(match::tiles),body);
  slots.front()[match::tiles]=0;
  const auto encoded=encode_anchor_match_batch_reply(request,slots);
  const auto decoded=decode_anchor_xor_batch_reply(encoded,request);
  check(decoded.size()==1 && decoded.front().size()==2 && decoded.front().back().empty() &&
      std::ranges::equal(decoded.front().front(),body),"packed XOR reply lost its accepted or declined slot");
  const auto accepted=check_native_group_xor_body(decoded.front().front(),member,base,id,1,match::extent_bytes);
  check(accepted.frame==candidate && accepted.check==NativePackedXorCheck::verified,
      "independent CPU readback did not reconstruct the packed XOR reply");
  const auto too_large=check_native_group_xor_body(body,member,base,id,1,candidate->size());
  check(!too_large.frame && too_large.check==NativePackedXorCheck::verified,
      "a correct but unprofitable body was treated as an integrity failure");
  const auto wrong=check_native_group_xor_body(body,base,base,id,1,candidate->size());
  check(!wrong.frame && wrong.check==NativePackedXorCheck::invalid,
      "competing size concealed a mismatched XOR body");
  const auto truncated=check_native_group_xor_body(body.first(body.size()-1),member,base,id,1,match::extent_bytes);
  check(!truncated.frame && truncated.check==NativePackedXorCheck::invalid,
      "truncated advice passed the canonical body check");
  const auto empty=check_native_group_xor_body({},member,base,id,1,match::extent_bytes);
  check(!empty.frame && empty.check==NativePackedXorCheck::pending,
      "an empty transport slot supplied an integrity verdict");
  rejects([&]{(void)accept_anchor_match_batch_reply(encoded,request);});
  for(const auto at:{std::size_t{32},std::size_t{64+4+32},std::size_t{64+4+64},encoded.size()-1}) {
    auto bad=encoded;
    bad[at]^=std::byte{1};
    rejects([&]{(void)decode_anchor_xor_batch_reply(bad,request);});
  }
  slots.front().front()=match::empty;
  rejects([&]{(void)encode_anchor_match_batch_reply(request,slots);});
}
void wide_xor_reply() {
  constexpr auto wide=match::BatchOutput::wide_packed_xor;
  constexpr auto narrow=match::BatchOutput::packed_xor;
  const std::array<std::uint32_t,1> counts{2};
  const auto plan=plan_anchor_match_batch(counts,2,match::device_bytes(1,wide),2,wide);
  const auto original=plan_anchor_match_batch(counts,2,match::device_bytes(2,narrow),3,narrow);
  check(plan.quantum_members==1 && plan.launches==2 && plan.request_bytes==original.request_bytes &&
      plan.reply_bytes-original.reply_bytes==2*(match::xor_wide_slot_bytes-match::xor_slot_bytes) &&
      plan.device_bytes==3*match::extent_bytes,"wide request did not charge its exact geometry");
  rejects([&]{(void)plan_anchor_match_batch(counts,2,plan.device_bytes-1,2,wide);});
  check(plan_anchor_match_transfers(plan,match::device_bytes(2,wide))==
      AnchorMatchTransferPlan{2,5*match::extent_bytes,1},"wide transfer did not share one base and two full slots");

  Bytes base(match::extent_bytes,std::byte{0x71});
  Bytes members(2*match::extent_bytes,std::byte{0x71});
  std::fill_n(members.begin(),match::extent_bytes,std::byte{0x41});
  AnchorMatchBatchBuilder builder(92,plan);
  builder.append(1,base,members);
  const auto request=builder.finish();
  const auto received=decode_anchor_match_batch_request(request.snapshot(),plan.request_bytes,plan.reply_bytes,
      plan.quantum_members,plan.device_bytes,plan.launches,wide);
  check(received.plan()==plan && received.digest()==request.digest(),"wide request changed at the worker boundary");
  rejects([&]{(void)decode_anchor_match_batch_request(request.snapshot(),original.request_bytes,original.reply_bytes,
      original.quantum_members,original.device_bytes,original.launches,narrow);});
  const auto member=std::span<const std::byte>(members).first(match::extent_bytes);
  constexpr auto base_id=kNativeGroupBaseBit+1;
  const auto candidate=encode_native_group_xor(member,base,base_id,1,match::extent_bytes);
  check(candidate.has_value(),"two varying planes lacked a CPU candidate");
  const auto body=std::span<const std::byte>(*candidate).subspan(128);
  check(body.size()==16640 && body.size()>match::xor_body_capacity,"fixture did not exceed the narrow reply");
  constexpr auto words=match::xor_wide_slot_bytes/sizeof(match::u32);
  std::vector<std::vector<match::u32>> slots(1,std::vector<match::u32>(2*words,match::empty));
  put_xor_test_slot(std::span(slots.front()).first(words),body);
  slots.front()[words]=0;
  const auto frame=encode_anchor_match_batch_reply(received,slots);
  const auto decoded=decode_anchor_xor_batch_reply(frame,request);
  check(frame.size()==plan.reply_bytes && decoded.size()==1 && decoded.front().size()==2 &&
      decoded.front().back().empty() && check_native_group_xor_body(decoded.front().front(),member,base,
          base_id,1,match::extent_bytes).frame==candidate,"wide frame did not preserve the independent CPU result");
  auto malformed=frame;
  malformed[7]=std::byte{1};
  rejects([&]{(void)decode_anchor_xor_batch_reply(malformed,request);});
  malformed=frame;
  malformed.back()=std::byte{};
  rejects([&]{(void)decode_anchor_xor_batch_reply(malformed,request);});

  // A canonical reply can fit the transport and still lose to a raw extent.
  Bytes almost_raw;
  for(unsigned tile=0;tile<match::xor_tiles;++tile) {
    const auto mask=tile<247?std::byte{0xff}:std::byte{0x7f};
    almost_raw.push_back(mask);
    almost_raw.insert(almost_raw.end(),(tile<247?8:7)*match::xor_plane_bytes,std::byte{0xff});
    std::fill_n(members.begin()+tile*match::xor_tile_bytes,match::xor_tile_bytes,base.front()^mask);
  }
  const auto large=check_native_group_xor_body(almost_raw,member,base,base_id,1,match::extent_bytes);
  check(almost_raw.size()==65504 && !large.frame && large.check==NativePackedXorCheck::verified,
      "size selection treated valid wide advice as corruption");
  std::cout<<"wide_xor narrow_slot_bytes="<<match::xor_slot_bytes<<" wide_slot_bytes="<<match::xor_wide_slot_bytes
      <<" two_plane_bytes="<<body.size()<<" device_bytes="<<plan.device_bytes<<" launch_count="<<plan.launches<<'\n';
}
}

int main() {
  try {
    fragmented();
    unrelated();
    sized_sweep();
    independent_batch();
    packed_xor_reply();
    wide_xor_reply();
    std::cout<<"Direct shared base match CPU semantics and reply verification passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
