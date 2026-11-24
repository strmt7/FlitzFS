#include "native_group_read_wire.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace flitzfs::detail {
namespace {
using Bytes=std::vector<std::byte>;
using View=std::span<const std::byte>;
using group_decode::Layout;
using group_decode::Member;
constexpr std::size_t request_header=32,reply_header=56;
constexpr auto request_magic=std::string_view("FLNGRQ01");
constexpr auto reply_magic=std::string_view("FLNGRP01");
static_assert(std::endian::native==std::endian::little);
static_assert(group_decode::extent_bytes==kNativeExtentBytes && group_decode::tile_bytes==kNativePackedXorTileBytes &&
    group_decode::maximum_members==kNativeMaxReadBytes/kNativeExtentBytes);

[[noreturn]]void invalid(const char* reason){throw std::invalid_argument(reason);}
bool magic(View bytes,std::string_view tag) {
  return std::ranges::equal(bytes.first(tag.size()),std::as_bytes(std::span(tag)));
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::byte>(value>>(i*8));
}
std::uint64_t get(View bytes,std::size_t at,unsigned width) noexcept {
  std::uint64_t result=0;
  for(unsigned i=0;i<width;++i)result|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(i*8);
  return result;
}
bool valid_execution(WorkerExecution execution) noexcept {
  return execution==WorkerExecution::hip || execution==WorkerExecution::cpu_semantics;
}
bool granted(Layout layout,std::uint32_t members,std::uint64_t bytes) noexcept {
  return group_decode::valid(layout) && layout.members<=members && group_decode::slab_bytes(layout)<=bytes;
}
}

std::size_t native_group_reply_bytes(Layout layout) noexcept {
  return reply_header+std::size_t(layout.members)*kNativeExtentBytes;
}

NativeGroupReadExchange NativeGroupReadExchange::prepare(const NativeGroupReadBatch& capture,std::uint64_t id,
    std::uint32_t maximum_members,std::uint64_t maximum_device_bytes) {
  const auto members=capture.members();
  const auto bases=capture.bases();
  if(!id || !capture.metadata_ || members.empty() || members.size()>group_decode::maximum_members ||
      members.size()>maximum_members)
    invalid("native group request count");
  Layout layout{static_cast<std::uint32_t>(members.size()),static_cast<std::uint32_t>(bases.size()),0,group_decode::version};
  for(const auto& member:members)layout.body_bytes+=member.offsets.back();
  if(!granted(layout,maximum_members,maximum_device_bytes))invalid("native group request grant");
  Bytes bytes(request_header+group_decode::output_at(layout));
  const auto header=std::span(bytes).first(request_header);
  std::ranges::copy(std::as_bytes(std::span(request_magic)),header.begin());
  put(header,8,id,8);put(header,16,layout.members,4);put(header,20,layout.bases,4);
  put(header,24,layout.body_bytes,4);put(header,28,layout.abi,4);
  const auto upload=std::span(bytes).subspan(request_header);
  for(std::size_t i=0;i<bases.size();++i)
    std::ranges::copy(bases[i].bytes.payload(),upload.subspan(group_decode::bases_at(layout)+i*kNativeExtentBytes).begin());
  std::uint32_t body=0;
  for(std::size_t i=0;i<members.size();++i) {
    const auto& captured=members[i];
    const auto packed=std::span<const std::byte>(captured.frame).subspan(kNativeGroupMemberHeaderBytes);
    Member descriptor{static_cast<std::uint32_t>(captured.base),body,static_cast<std::uint32_t>(packed.size()),{},0};
    std::ranges::copy(captured.offsets,descriptor.offsets);
    std::memcpy(upload.data()+i*sizeof(Member),&descriptor,sizeof(Member));
    std::ranges::copy(packed,upload.subspan(group_decode::bodies_at(layout)+body).begin());
    body+=descriptor.body_bytes;
  }
  NativeGroupReadExchange exchange;
  exchange.layout_=layout;
  exchange.id_=id;
  exchange.capture_=capture.metadata_;
  exchange.digest_=record_digest(bytes);
  exchange.request_=std::make_shared<const WorkerRequestSnapshot>(std::move(bytes));
  return exchange;
}

NativeGroupWireRequest decode_native_group_request(View bytes,std::uint32_t maximum_members,
    std::uint64_t maximum_device_bytes) {
  if(bytes.size()<request_header || !magic(bytes,request_magic) || !get(bytes,8,8))invalid("native group request header");
  const Layout layout{static_cast<std::uint32_t>(get(bytes,16,4)),static_cast<std::uint32_t>(get(bytes,20,4)),
      static_cast<std::uint32_t>(get(bytes,24,4)),static_cast<std::uint32_t>(get(bytes,28,4))};
  if(!granted(layout,maximum_members,maximum_device_bytes) || bytes.size()-request_header!=group_decode::output_at(layout))
    invalid("native group upload extent");
  const auto upload=bytes.subspan(request_header);
  const auto bodies=upload.subspan(group_decode::bodies_at(layout),layout.body_bytes);
  std::uint32_t body=0;
  std::uint64_t used_bases=0;
  for(std::uint32_t i=0;i<layout.members;++i) {
    Member member;
    std::memcpy(&member,upload.data()+i*sizeof(Member),sizeof(Member));
    if(member.reserved || member.base>=layout.bases || member.body_at!=body ||
        member.body_bytes>bodies.size()-body)invalid("native group member extent");
    const auto offsets=native_packed_xor_offsets(bodies.subspan(body,member.body_bytes));
    if(!std::ranges::equal(offsets,member.offsets))invalid("native group tile offsets");
    body+=member.body_bytes;
    used_bases|=std::uint64_t{1}<<member.base;
  }
  if(body!=bodies.size() || std::popcount(used_bases)!=static_cast<int>(layout.bases) ||
      std::ranges::any_of(upload.subspan(group_decode::bodies_at(layout)+body),[](auto value){return value!=std::byte{};}))
    invalid("native group upload coverage");
  return {get(bytes,8,8),record_digest(bytes),layout,Bytes(upload.begin(),upload.end())};
}

Bytes encode_native_group_reply(const NativeGroupWireRequest& request,View output,WorkerExecution execution) {
  const auto layout=request.layout;
  if(!group_decode::valid(layout) || !valid_execution(execution) ||
      output.size()!=group_decode::slab_bytes(layout)-group_decode::output_at(layout))invalid("native group readback extent");
  const auto plain_bytes=std::size_t(layout.members)*kNativeExtentBytes;
  if(!std::ranges::all_of(output.subspan(plain_bytes),[](auto status){return status==std::byte{group_decode::complete};}))
    invalid("native group incomplete tile");
  Bytes reply(reply_header+plain_bytes);
  std::ranges::copy(std::as_bytes(std::span(reply_magic)),reply.begin());
  put(reply,8,request.id,8);put(reply,16,layout.members,4);put(reply,20,static_cast<std::uint32_t>(execution),4);
  std::ranges::copy(request.digest,reply.begin()+24);
  std::ranges::copy(output.first(plain_bytes),reply.begin()+reply_header);
  return reply;
}

std::unique_ptr<NativeGroupReadyReads> NativeGroupReadExchange::accept(NativeGroupReadBatch& capture,
    View reply,WorkerExecution expected,const std::function<bool()>& interrupted) const {
  if(!request_ || !valid_execution(expected) || reply.size()!=reply_capacity() || !magic(reply,reply_magic) ||
      get(reply,8,8)!=id_ || get(reply,16,4)!=layout_.members || get(reply,20,4)!=static_cast<std::uint32_t>(expected) ||
      !std::ranges::equal(reply.subspan(24,digest_.size()),digest_) ||
      !capture.metadata_ || capture_.lock()!=capture.metadata_)return {};
  std::vector<Bytes> decoded;
  decoded.reserve(layout_.members);
  for(std::uint32_t i=0;i<layout_.members;++i) {
    if(interrupted && interrupted())return {};
    const auto member=reply.subspan(reply_header+i*kNativeExtentBytes,kNativeExtentBytes);
    decoded.emplace_back(member.begin(),member.end());
  }
  return NativeGroupReadyReads::verify(capture,std::move(decoded),interrupted);
}
}
