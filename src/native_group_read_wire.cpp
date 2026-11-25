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
using group_decode::Method;
constexpr std::size_t request_header=32,reply_header=56;
constexpr auto request_magic=std::string_view("FLNGRQ01");
constexpr auto mixed_magic=std::string_view("FLNGRQ02");
constexpr auto reply_magic=std::string_view("FLNGRP01");
static_assert(std::endian::native==std::endian::little);
static_assert(group_decode::extent_bytes==kNativeExtentBytes && group_decode::tile_bytes==kNativeGroupTileBytes &&
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
Method device_method(NativeGroupMethod method) {
  switch(method) {
    case NativeGroupMethod::packed_xor:return Method::packed_xor;
    case NativeGroupMethod::sparse_xor:return Method::sparse_xor;
    case NativeGroupMethod::modular_delta:return Method::modular_delta;
    default:invalid("native group method has no device decoder");
  }
}
NativeGroupMethod native_method(Method method) {
  switch(method) {
    case Method::packed_xor:return NativeGroupMethod::packed_xor;
    case Method::sparse_xor:return NativeGroupMethod::sparse_xor;
    case Method::modular_delta:return NativeGroupMethod::modular_delta;
    default:invalid("unsupported native group device method");
  }
}
}

std::size_t native_group_reply_bytes(Layout layout) noexcept {
  return reply_header+std::size_t(layout.members)*kNativeExtentBytes;
}

NativeGroupPreparedGrant plan_native_group_preparation(std::uint32_t maximum_members,
    std::uint64_t maximum_device_bytes,bool mixed) noexcept {
  const auto abi=mixed?group_decode::mixed_version:group_decode::version;
  const auto minimum_body=mixed?group_decode::delta_prefix_bytes:group_decode::tiles+group_decode::words_per_tile;
  auto count=(std::min)(maximum_members,group_decode::maximum_members);
  while(count && group_decode::slab_bytes({count,1,count*minimum_body,abi})>maximum_device_bytes)--count;
  if(!count)return {};
  // Each member has at most one private base and its stored body stays below
  // a full extent, including the optional delta prefix on this protocol.
  const Layout maximum{count,count,count*(kNativeExtentBytes-1),abi};
  const auto slab=(std::min)(maximum_device_bytes,group_decode::slab_bytes(maximum));
  const auto input=(std::min)(group_decode::output_at(maximum),
      slab-kNativeExtentBytes-group_decode::tiles);
  return {count,slab,request_header+static_cast<std::size_t>(input),native_group_reply_bytes(maximum)};
}

NativeGroupReadExchange NativeGroupReadExchange::prepare(const NativeGroupReadBatch& capture,std::uint64_t id,
    std::uint32_t maximum_members,std::uint64_t maximum_device_bytes) {
  const auto members=capture.members();
  const auto bases=capture.bases();
  if(!id || !capture.metadata_ || members.empty() || members.size()>group_decode::maximum_members ||
      members.size()>maximum_members)
    invalid("native group request count");
  const bool mixed=capture.mixed_methods();
  Layout layout{static_cast<std::uint32_t>(members.size()),static_cast<std::uint32_t>(bases.size()),0,
      mixed?group_decode::mixed_version:group_decode::version};
  for(const auto& member:members) {
    layout.body_bytes+=member.offsets.back();
    if(member.method==NativeGroupMethod::modular_delta)layout.body_bytes+=group_decode::delta_prefix_bytes;
  }
  if(!granted(layout,maximum_members,maximum_device_bytes))invalid("native group request grant");
  Bytes bytes(request_header+group_decode::output_at(layout));
  const auto header=std::span(bytes).first(request_header);
  const auto tag=mixed?mixed_magic:request_magic;
  std::ranges::copy(std::as_bytes(std::span(tag)),header.begin());
  put(header,8,id,8);put(header,16,layout.members,4);put(header,20,layout.bases,4);
  put(header,24,layout.body_bytes,4);put(header,28,layout.abi,4);
  const auto upload=std::span(bytes).subspan(request_header);
  for(std::size_t i=0;i<bases.size();++i)
    std::ranges::copy(bases[i].bytes.payload(),upload.subspan(group_decode::bases_at(layout)+i*kNativeExtentBytes).begin());
  std::uint32_t body=0;
  for(std::size_t i=0;i<members.size();++i) {
    const auto& captured=members[i];
    const auto tiles=std::span<const std::byte>(captured.frame).subspan(kNativeGroupMemberHeaderBytes);
    const auto prefix=captured.method==NativeGroupMethod::modular_delta?group_decode::delta_prefix_bytes:0U;
    Member descriptor{static_cast<std::uint32_t>(captured.base),body,prefix+static_cast<std::uint32_t>(tiles.size()),
        {},device_method(captured.method)};
    std::ranges::copy(captured.offsets,descriptor.offsets);
    std::memcpy(upload.data()+i*sizeof(Member),&descriptor,sizeof(Member));
    const auto destination=upload.subspan(group_decode::bodies_at(layout)+body,descriptor.body_bytes);
    if(prefix) {
      destination[0]=static_cast<std::byte>(captured.parameters.word_bytes);
      put(destination,8,captured.parameters.bias,8);
    }
    std::ranges::copy(tiles,destination.subspan(prefix).begin());
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
  if(bytes.size()<request_header || !get(bytes,8,8))invalid("native group request header");
  const Layout layout{static_cast<std::uint32_t>(get(bytes,16,4)),static_cast<std::uint32_t>(get(bytes,20,4)),
      static_cast<std::uint32_t>(get(bytes,24,4)),static_cast<std::uint32_t>(get(bytes,28,4))};
  const bool mixed=layout.abi==group_decode::mixed_version;
  if(!magic(bytes,mixed?mixed_magic:request_magic) || !granted(layout,maximum_members,maximum_device_bytes) ||
      bytes.size()-request_header!=group_decode::output_at(layout))
    invalid("native group upload extent");
  const auto upload=bytes.subspan(request_header);
  const auto bodies=upload.subspan(group_decode::bodies_at(layout),layout.body_bytes);
  std::uint32_t body=0;
  std::uint64_t used_bases=0;
  bool mixed_method=false;
  for(std::uint32_t i=0;i<layout.members;++i) {
    Member member;
    std::memcpy(&member,upload.data()+i*sizeof(Member),sizeof(Member));
    if(member.base>=layout.bases || member.body_at!=body || member.body_bytes>bodies.size()-body ||
        (!mixed && member.method!=Method::packed_xor))invalid("native group member extent");
    auto tiles=bodies.subspan(body,member.body_bytes);
    const auto method=native_method(member.method);
    NativeGroupTileParameters parameters;
    if(method==NativeGroupMethod::modular_delta) {
      if(tiles.size()<group_decode::delta_prefix_bytes ||
          std::ranges::any_of(tiles.subspan(1,7),[](auto value){return value!=std::byte{};}))
        invalid("native group delta parameters");
      parameters={get(tiles,8,8),std::to_integer<std::uint8_t>(tiles[0])};
      tiles=tiles.subspan(group_decode::delta_prefix_bytes);
    }
    NativeGroupTileOffsets offsets;
    try {offsets=native_group_tile_offsets(tiles,method,parameters);}
    catch(const std::runtime_error&) {invalid("native group tile body");}
    if(!std::ranges::equal(offsets,member.offsets))invalid("native group tile offsets");
    body+=member.body_bytes;
    used_bases|=std::uint64_t{1}<<member.base;
    mixed_method=mixed_method || member.method!=Method::packed_xor;
  }
  if(mixed!=mixed_method || body!=bodies.size() || std::popcount(used_bases)!=static_cast<int>(layout.bases) ||
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

NativeGroupReplyVerifier::NativeGroupReplyVerifier(NativeGroupReadBatch& capture,View payload)
    :capture_(&capture),payload_(payload),ready_(new NativeGroupReadyReads) {
  ready_->inode_=capture.inode_;
  ready_->members_.reserve(capture.members_.size());
}
NativeGroupReplyVerifier::State NativeGroupReplyVerifier::advance(const std::function<bool()>& interrupted) {
  if(!ready_ || !capture_->metadata_)return State::invalid;
  if(complete_)return State::verified;
  for(std::size_t i=ready_->members_.size();i<capture_->members_.size();++i) {
    if(interrupted && interrupted())return State::paused;
    const auto member=payload_.subspan(i*kNativeExtentBytes,kNativeExtentBytes);
    if(!ready_->append_checked(*capture_,Bytes(member.begin(),member.end()))) {
      ready_.reset();
      return State::invalid;
    }
  }
  if(interrupted && interrupted())return State::paused;
  complete_=true;
  return State::verified;
}
std::unique_ptr<NativeGroupReadyReads> NativeGroupReplyVerifier::take_verified() noexcept {
  if(!ready_ || !complete_ || !capture_->metadata_)return {};
  ready_->metadata_=std::move(capture_->metadata_);
  return std::move(ready_);
}

std::optional<NativeGroupReplyVerifier> NativeGroupReadExchange::begin_verification(
    NativeGroupReadBatch& capture,View reply,WorkerExecution expected) const {
  if(!request_ || !valid_execution(expected) || reply.size()!=reply_capacity() || !magic(reply,reply_magic) ||
      get(reply,8,8)!=id_ || get(reply,16,4)!=layout_.members || get(reply,20,4)!=static_cast<std::uint32_t>(expected) ||
      !std::ranges::equal(reply.subspan(24,digest_.size()),digest_) || capture.members_.size()!=layout_.members ||
      !capture.metadata_ || capture_.lock()!=capture.metadata_)return {};
  return NativeGroupReplyVerifier(capture,reply.subspan(reply_header));
}

std::unique_ptr<NativeGroupReadyReads> NativeGroupReadExchange::accept(NativeGroupReadBatch& capture,
    View reply,WorkerExecution expected,const std::function<bool()>& interrupted) const {
  auto verification=begin_verification(capture,reply,expected);
  if(!verification || verification->advance(interrupted)!=NativeGroupReplyVerifier::State::verified)return {};
  return verification->take_verified();
}
}
