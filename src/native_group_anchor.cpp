#include "native_group_anchor.hpp"
#include "flitzfs/worker_wire.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace flitzfs::detail {
namespace {
namespace match=anchor_match;
using Bytes=std::vector<std::byte>;
constexpr std::size_t header_size=64;
constexpr std::string_view request_magic="FLFSAMQ\1",reply_magic="FLFSAMP\1";
constexpr auto execution=static_cast<std::uint32_t>(WorkerExecution::hip);
static_assert(match::extent_bytes==kNativeExtentBytes);
static_assert(header_size+(match::max_members+1)*match::extent_bytes<=kWorkerRequestLimit);
static_assert(header_size+match::max_members*match::tiles*sizeof(match::u32)<=kWorkerReplyLimit);

void require(bool valid,const char* message) {
  if (!valid) throw std::invalid_argument(message);
}
void put(Bytes& frame,std::uint64_t value,unsigned width) {
  for (unsigned byte=0;byte<width;++byte) frame.push_back(static_cast<std::byte>(value>>(byte*8)));
}
std::uint64_t get(std::span<const std::byte> frame,std::size_t at,unsigned width) {
  std::uint64_t value=0;
  for (unsigned byte=0;byte<width;++byte)
    value|=std::uint64_t(std::to_integer<unsigned>(frame[at+byte]))<<(byte*8);
  return value;
}

Bytes header(std::string_view magic,std::uint64_t id,std::uint32_t count,
    std::uint32_t backend,const Digest& digest,std::size_t payload_size) {
  require(id!=0,"missing anchor match request identity");
  Bytes frame;
  frame.reserve(header_size+payload_size);
  for (const auto byte:magic) frame.push_back(static_cast<std::byte>(byte));
  put(frame,id,8);
  put(frame,count,4);
  put(frame,count*match::tiles,4);
  put(frame,backend,4);
  put(frame,0,4);
  frame.insert(frame.end(),digest.begin(),digest.end());
  return frame;
}

void check_header(std::span<const std::byte> frame,std::string_view magic,std::uint32_t backend) {
  require(frame.size()>=header_size && std::memcmp(frame.data(),magic.data(),magic.size())==0 &&
      get(frame,8,8)!=0 && get(frame,24,4)==backend && get(frame,28,4)==0,
      "invalid anchor match frame header");
}
} // namespace

std::uint32_t anchor_match_members(std::span<const std::byte> source) {
  require(source.size()%match::extent_bytes==0 && source.size()>=2*match::extent_bytes &&
      source.size()<=(match::max_members+1)*match::extent_bytes,"invalid anchor match source size");
  return static_cast<std::uint32_t>(source.size()/match::extent_bytes-1);
}

Bytes encode_anchor_match_request(std::span<const std::byte> base,std::span<const std::byte> members,
    std::uint64_t id) {
  require(base.size()==match::extent_bytes && !members.empty() &&
      members.size()%match::extent_bytes==0 && members.size()<=match::max_members*match::extent_bytes,
      "invalid anchor match extent inputs");
  const auto count=static_cast<std::uint32_t>(members.size()/match::extent_bytes);
  auto frame=header(request_magic,id,count,0,record_digest(base,members),base.size()+members.size());
  frame.insert(frame.end(),base.begin(),base.end());
  frame.insert(frame.end(),members.begin(),members.end());
  return frame;
}

AnchorMatchRequest decode_anchor_match_request(std::span<const std::byte> frame) {
  check_header(frame,request_magic,0);
  const auto source=frame.subspan(header_size);
  const auto count=anchor_match_members(source);
  require(get(frame,16,4)==count && get(frame,20,4)==count*match::tiles,"anchor match request extent count");
  AnchorMatchRequest request{get(frame,8,8),Bytes(source.begin(),source.end())};
  const auto digest=record_digest(request.source);
  require(std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor match request digest");
  return request;
}

std::vector<match::u32> cpu_anchor_match_records(const AnchorMatchRequest& request) {
  const auto members=anchor_match_members(request.source);
  const auto* base=reinterpret_cast<const match::u8*>(request.source.data());
  std::vector<match::Bucket> table(match::buckets,{match::empty,match::empty});
  // In increasing order, the first two positions are exactly the two
  // minimum positions retained by the device's atomic updates.
  for (match::u32 at=0;at<match::extent_bytes;++at) {
    match::u32 slot=0;
    if (!match::index_position(base,at,slot)) continue;
    auto& entry=table[slot];
    if (entry.first==match::empty) entry.first=at;
    else if (entry.second==match::empty) entry.second=at;
  }
  std::vector<match::u32> records(members*match::tiles);
  for (match::u32 member=0;member<members;++member) {
    const auto* bytes=base+(member+1)*match::extent_bytes;
    for (match::u32 tile=0;tile<match::tiles;++tile)
      records[member*match::tiles+tile]=match::search(base,bytes,table.data(),tile);
  }
  return records;
}

AnchorMatchAdvice verify_anchor_match_records(const AnchorMatchRequest& request,
    std::span<const match::u32> records) {
  const auto members=anchor_match_members(request.source);
  require(records.size()==members*match::tiles,"anchor match record count");
  AnchorMatchAdvice advice(members);
  for (match::u32 index=0;index<members;++index) {
    const auto* member=request.source.data()+(index+1)*match::extent_bytes;
    auto& ranges=advice[index];
    match::u32 end=0;
    for (match::u32 tile=0;tile<match::tiles;++tile) {
      const auto word=records[index*match::tiles+tile];
      if (!word) continue;
      auto length=word>>20;
      auto target=tile*match::tile_bytes+((word>>16)&15);
      auto source=word&65535;
      require(length>=match::seed_bytes && length<=match::maximum_match &&
          length<=match::extent_bytes-target && length<=match::extent_bytes-source,
          "anchor match range outside extent");
      require(std::memcmp(request.source.data()+source,member+target,length)==0,
          "anchor match suggestion differs from original bytes");
      if (target+length<=end) continue;
      if (target<end) {
        const auto overlap=end-target;
        target=end;
        source+=overlap;
        length-=overlap;
      }
      if (!ranges.empty() && target==end && ranges.back().base_offset+ranges.back().length==source)
        ranges.back().length+=length;
      else ranges.push_back({target,source,length});
      end=target+length;
    }
  }
  return advice;
}

Bytes encode_anchor_match_reply(const AnchorMatchRequest& request,std::span<const match::u32> records) {
  (void)verify_anchor_match_records(request,records);
  auto frame=header(reply_magic,request.id,anchor_match_members(request.source),execution,
      record_digest(request.source),records.size_bytes());
  for (const auto word:records) put(frame,word,sizeof(word));
  return frame;
}

AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchRequest& request) {
  check_header(frame,reply_magic,execution);
  const auto members=anchor_match_members(request.source);
  const auto count=members*match::tiles;
  require(get(frame,8,8)==request.id && get(frame,16,4)==members && get(frame,20,4)==count &&
      frame.size()==header_size+count*sizeof(match::u32),"anchor match reply binding");
  const auto digest=record_digest(request.source);
  require(std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor match reply source digest");
  std::vector<match::u32> records(count);
  for (std::size_t i=0;i<records.size();++i)
    records[i]=static_cast<match::u32>(get(frame,header_size+i*sizeof(match::u32),sizeof(match::u32)));
  return verify_anchor_match_records(request,records);
}

} // namespace flitzfs::detail
