#include "native_group_anchor.hpp"
#include "flitzfs/worker_wire.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace flitzfs::detail {
namespace {
namespace match=anchor_match;
using Bytes=std::vector<std::byte>;
constexpr std::uint32_t header_size=64;
constexpr auto maximum_channel_bytes=static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
constexpr auto maximum_source_bytes=maximum_channel_bytes-header_size;
constexpr auto maximum_members=maximum_source_bytes/match::extent_bytes-1;
constexpr auto record_bytes=static_cast<std::uint32_t>(sizeof(match::u32));
constexpr std::string_view request_magic="FLFSAMQ\1",reply_magic="FLFSAMP\1";
constexpr std::string_view batch_request_magic="FLFSABQ\1",batch_reply_magic="FLFSABP\1";
constexpr std::string_view xor_batch_magic="FLFSAXP\1",xor_group_magic="FLFSAXG\1";
constexpr std::uint32_t group_prefix_bytes=4;
constexpr auto minimum_group_request=group_prefix_bytes+header_size+2*match::extent_bytes;
constexpr auto execution=static_cast<std::uint32_t>(WorkerExecution::hip);
static_assert(match::extent_bytes==kNativeExtentBytes);
static_assert(header_size+(match::max_members+1)*match::extent_bytes<=kWorkerRequestLimit);
static_assert(header_size+match::max_members*match::tiles*sizeof(match::u32)<=kWorkerReplyLimit);
static_assert(maximum_members*match::tiles<=(maximum_channel_bytes-header_size)/record_bytes);
static_assert(std::uint64_t(maximum_members+1)*match::extent_bytes+match::index_bytes+
    std::uint64_t(maximum_members)*match::tiles*record_bytes<=(std::numeric_limits<std::uint32_t>::max)());

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

void append_records(Bytes& frame,std::span<const match::u32> records) {
  if constexpr (std::endian::native==std::endian::little) {
    const auto bytes=std::as_bytes(records);
    frame.insert(frame.end(),bytes.begin(),bytes.end());
  } else {
    for(const auto word:records)put(frame,word,record_bytes);
  }
}

void append_header(Bytes& frame,std::string_view magic,std::uint64_t id,std::uint32_t count,std::uint32_t work,
    std::uint32_t backend,const Digest& digest) {
  require(id!=0,"missing anchor match request identity");
  for (const auto byte:magic) frame.push_back(static_cast<std::byte>(byte));
  put(frame,id,8);
  put(frame,count,4);
  put(frame,work,4);
  put(frame,backend,4);
  put(frame,0,4);
  frame.insert(frame.end(),digest.begin(),digest.end());
}

Bytes header(std::string_view magic,std::uint64_t id,std::uint32_t count,std::uint32_t work,
    std::uint32_t backend,const Digest& digest,std::size_t payload_size) {
  Bytes frame;
  frame.reserve(header_size+payload_size);
  append_header(frame,magic,id,count,work,backend,digest);
  return frame;
}

void check_header(std::span<const std::byte> frame,std::string_view magic,std::uint32_t backend) {
  require(frame.size()>=header_size && std::memcmp(frame.data(),magic.data(),magic.size())==0 &&
      get(frame,8,8)!=0 && get(frame,24,4)==backend && get(frame,28,4)==0,
      "invalid anchor match frame header");
}

std::span<const std::byte> packed_xor_slot(std::span<const std::byte> slot) {
  require(slot.size()==match::xor_slot_bytes,"packed XOR reply slot extent");
  const auto size=static_cast<std::size_t>(get(slot,0,record_bytes));
  require(size<=match::xor_body_capacity &&
      (!size || (size>=match::xor_tiles+match::xor_plane_bytes &&
        (size-match::xor_tiles)%match::xor_plane_bytes==0)),
      "packed XOR reply body length");
  require(std::ranges::all_of(slot.subspan(record_bytes+size),[](auto byte){return byte==std::byte{255};}),
      "packed XOR reply slot padding");
  return slot.subspan(record_bytes,size);
}

void require_admitted_layout(AnchorMatchLayout layout,std::optional<AnchorMatchLayout> admitted) {
  require(admitted?layout==*admitted:layout.members<=match::max_members,
      "anchor match request exceeds its admitted transport");
}
} // namespace

std::uint32_t anchor_match_members(std::span<const std::byte> source) {
  require(source.size()%match::extent_bytes==0 && source.size()>=2*match::extent_bytes &&
      source.size()<=maximum_source_bytes,"invalid anchor match source size");
  return static_cast<std::uint32_t>(source.size()/match::extent_bytes-1);
}

AnchorMatchLayout anchor_match_layout(std::uint32_t members) {
  require(members && members<=maximum_members,"invalid anchor match member count");
  return {members,header_size+(members+1)*match::extent_bytes,
      header_size+members*match::tiles*record_bytes};
}

AnchorMatchSweepPlan plan_anchor_match_sweep(std::uint32_t members,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_windows) {
  const auto whole=anchor_match_layout(members);
  auto quantum=(std::min)({members,maximum_quantum,match::max_members});
  while (quantum && match::device_bytes(quantum)>maximum_device_bytes) --quantum;
  require(quantum!=0,"anchor match sweep cannot admit one search window");
  const auto windows=1+(members-1)/quantum;
  require(windows<=maximum_windows,"anchor match sweep exceeds its search window grant");
  return {whole,quantum,windows,match::device_bytes(quantum)};
}

Bytes encode_anchor_match_request(std::span<const std::byte> base,std::span<const std::byte> members,
    std::uint64_t id,std::optional<AnchorMatchLayout> admitted) {
  require(base.size()==match::extent_bytes && !members.empty() &&
      members.size()%match::extent_bytes==0 && members.size()<=maximum_members*match::extent_bytes,
      "invalid anchor match extent inputs");
  const auto count=static_cast<std::uint32_t>(members.size()/match::extent_bytes);
  require_admitted_layout(anchor_match_layout(count),admitted);
  auto frame=header(request_magic,id,count,count*match::tiles,0,record_digest(base,members),base.size()+members.size());
  frame.insert(frame.end(),base.begin(),base.end());
  frame.insert(frame.end(),members.begin(),members.end());
  return frame;
}

namespace {
AnchorMatchSource inspect_anchor_match_request(std::span<const std::byte> frame,
    std::optional<AnchorMatchLayout> admitted) {
  check_header(frame,request_magic,0);
  const auto source=frame.subspan(header_size);
  const auto count=anchor_match_members(source);
  require_admitted_layout(anchor_match_layout(count),admitted);
  require(get(frame,16,4)==count && get(frame,20,4)==count*match::tiles,"anchor match request extent count");
  AnchorMatchSource request{get(frame,8,8),source};
  std::copy_n(frame.begin()+32,request.source_digest.size(),request.source_digest.begin());
  return request;
}
}

AnchorMatchRequest decode_anchor_match_request(std::span<const std::byte> frame,
    std::optional<AnchorMatchLayout> admitted) {
  const auto inspected=inspect_anchor_match_request(frame,admitted);
  AnchorMatchRequest request{inspected.id,Bytes(inspected.source.begin(),inspected.source.end())};
  request.source_digest=record_digest(request.source);
  require(request.source_digest==inspected.source_digest,"anchor match request digest");
  return request;
}

std::vector<match::u32> cpu_anchor_match_records(const AnchorMatchSource& request) {
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

namespace {
template<typename ReadRecord>
void check_records(const AnchorMatchSource& request,std::uint32_t members,ReadRecord read_record,
    AnchorMatchAdvice* advice) {
  if (advice) advice->resize(members);
  for (match::u32 index=0;index<members;++index) {
    const auto* member=request.source.data()+(index+1)*match::extent_bytes;
    auto* const ranges=advice?&(*advice)[index]:nullptr;
    match::u32 end=0;
    for (match::u32 tile=0;tile<match::tiles;++tile) {
      const auto word=read_record(index*match::tiles+tile);
      if (!word) continue;
      auto length=word>>20;
      auto target=tile*match::tile_bytes+((word>>16)&15);
      auto source=word&65535;
      require(length>=match::seed_bytes && length<=match::maximum_match &&
          length<=match::extent_bytes-target && length<=match::extent_bytes-source,
          "anchor match range outside extent");
      require(std::memcmp(request.source.data()+source,member+target,length)==0,
          "anchor match suggestion differs from original bytes");
      if (!ranges || target+length<=end) continue;
      if (target<end) {
        const auto overlap=end-target;
        target=end;
        source+=overlap;
        length-=overlap;
      }
      if (!ranges->empty() && target==end && ranges->back().base_offset+ranges->back().length==source)
        ranges->back().length+=length;
      else ranges->push_back({target,source,length});
      end=target+length;
    }
  }
}

void check_records(const AnchorMatchSource& request,std::span<const match::u32> records,
    AnchorMatchAdvice* advice) {
  const auto members=anchor_match_members(request.source);
  require(records.size()==members*match::tiles,"anchor match record count");
  check_records(request,members,[records](std::uint32_t index){return records[index];},advice);
}

AnchorMatchAdvice read_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchSource& request,
    const Digest* bound_digest) {
  check_header(frame,reply_magic,execution);
  const auto members=anchor_match_members(request.source);
  const auto count=members*match::tiles;
  require(get(frame,8,8)==request.id && get(frame,16,4)==members && get(frame,20,4)==count &&
      frame.size()==header_size+count*record_bytes,"anchor match reply binding");
  const auto digest=bound_digest?*bound_digest:record_digest(request.source);
  require(std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor match reply source digest");

  const auto encoded=frame.subspan(header_size);
  const auto read_record=[encoded](std::uint32_t index) {
    const auto offset=index*record_bytes;
    if constexpr (std::endian::native==std::endian::little) {
      match::u32 word;
      std::memcpy(&word,encoded.data()+offset,record_bytes);
      return word;
    } else {
      return static_cast<match::u32>(get(encoded,offset,record_bytes));
    }
  };
  AnchorMatchAdvice advice;
  check_records(request,members,read_record,&advice);
  return advice;
}

} // namespace

AnchorMatchAdvice verify_anchor_match_records(const AnchorMatchSource& request,
    std::span<const match::u32> records) {
  AnchorMatchAdvice advice;
  check_records(request,records,&advice);
  return advice;
}

Bytes encode_anchor_match_reply(const AnchorMatchSource& request,std::span<const match::u32> records,
    std::size_t reserve_tail_bytes) {
  check_records(request,records,nullptr);
  require(reserve_tail_bytes<=maximum_channel_bytes-header_size-records.size_bytes(),"anchor reply tail capacity");
  const auto members=anchor_match_members(request.source);
  auto frame=header(reply_magic,request.id,members,members*match::tiles,execution,
      record_digest(request.source),records.size_bytes()+reserve_tail_bytes);
  append_records(frame,records);
  return frame;
}

AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchSource& request) {
  return read_anchor_match_reply(frame,request,nullptr);
}

AnchorMatchBatchPlan plan_anchor_match_batch(std::span<const std::uint32_t> members,
    std::uint32_t maximum_quantum,std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches) {
  require(!members.empty() && members.size()<=(maximum_channel_bytes-header_size)/minimum_group_request,
      "invalid anchor batch group count");
  AnchorMatchBatchPlan plan{};
  plan.request_bytes=plan.reply_bytes=header_size;
  plan.groups.reserve(members.size());
  for(const auto count:members) {
    const auto group=plan_anchor_match_sweep(count,maximum_quantum,maximum_device_bytes,maximum_launches);
    require(std::uint64_t(plan.request_bytes)+group_prefix_bytes+group.whole.request_bytes<=maximum_channel_bytes &&
        std::uint64_t(plan.reply_bytes)+group_prefix_bytes+group.whole.reply_bytes<=maximum_channel_bytes,
        "anchor batch channel capacity");
    require(std::uint64_t(plan.launches)+1+group.windows<=maximum_launches,"anchor batch exceeds its launch grant");
    plan.request_bytes+=group_prefix_bytes+group.whole.request_bytes;
    plan.reply_bytes+=group_prefix_bytes+group.whole.reply_bytes;
    plan.launches+=1+group.windows;
    plan.quantum_members=(std::max)(plan.quantum_members,group.quantum_members);
    plan.device_bytes=(std::max)(plan.device_bytes,group.device_bytes);
    plan.groups.push_back(group);
  }
  return plan;
}

AnchorMatchTransferPlan plan_anchor_match_transfers(const AnchorMatchBatchPlan& batch,
    std::uint64_t maximum_device_bytes) {
  const auto quantum=batch.quantum_members;
  require(!batch.groups.empty() && quantum && quantum<=match::max_members &&
      batch.device_bytes==match::device_bytes(quantum) && maximum_device_bytes>=batch.device_bytes,
      "invalid anchor transfer allocation grant");
  std::uint32_t largest=0;
  for(const auto& group:batch.groups) {
    const auto members=group.whole.members;
    require(members && members<=maximum_members && group.quantum_members==(std::min)(members,quantum),
        "invalid anchor transfer group geometry");
    largest=(std::max)(largest,members);
  }
  require(largest>=quantum,"anchor transfer does not contain its declared quantum");

  constexpr auto fixed_bytes=match::extent_bytes+match::index_bytes;
  constexpr auto per_member_bytes=match::extent_bytes+match::tiles*record_bytes;
  const auto fit=(maximum_device_bytes-fixed_bytes)/per_member_bytes;
  auto members=static_cast<std::uint32_t>((std::min)(fit,std::uint64_t(largest)));
  if(members<largest)members-=members%quantum;
  require(members>=quantum,"anchor transfer cannot fit one search dispatch");
  std::uint64_t transfers=0;
  for(const auto& group:batch.groups)transfers+=1+(group.whole.members-1)/members;
  require(batch.launches>=batch.groups.size() && transfers<=batch.launches-batch.groups.size(),
      "anchor transfer work exceeds batch launch grant");
  return {members,match::device_bytes(members),static_cast<std::uint32_t>(transfers)};
}

AnchorMatchBatchBuilder::AnchorMatchBatchBuilder(std::uint64_t id,const AnchorMatchBatchPlan& plan) {
  require(id!=0,"missing anchor batch identity");
  std::vector<std::uint32_t> counts;
  counts.reserve(plan.groups.size());
  for(const auto& group:plan.groups)counts.push_back(group.whole.members);
  require(plan==plan_anchor_match_batch(counts,plan.quantum_members,plan.device_bytes,plan.launches),
      "invalid anchor batch construction plan");
  request_.id_=id;
  request_.plan_=plan;
  request_.groups_.reserve(counts.size());
  frame_=header(batch_request_magic,id,static_cast<std::uint32_t>(counts.size()),plan.launches,0,
      Digest{},plan.request_bytes-header_size);
}

void AnchorMatchBatchBuilder::append(std::uint64_t id,std::span<const std::byte> base,
    std::span<const std::byte> members) {
  require(members.size()%match::extent_bytes==0,"unaligned anchor batch members");
  const auto source_offset=begin_group(id,base,members.size()/match::extent_bytes);
  frame_.insert(frame_.end(),members.begin(),members.end());
  complete_group(id,source_offset);
}

void AnchorMatchBatchBuilder::append(std::uint64_t id,const NativeSharedBaseSearchWindow& window) {
  const auto members=window.members();
  require(std::ranges::all_of(members,[](const auto& member){return member.size()==match::extent_bytes;}),
      "invalid staged anchor batch member");
  const auto source_offset=begin_group(id,window.base(),members.size());
  for(const auto& member:members)frame_.insert(frame_.end(),member.begin(),member.end());
  complete_group(id,source_offset);
}

std::size_t AnchorMatchBatchBuilder::begin_group(std::uint64_t id,std::span<const std::byte> base,
    std::size_t member_count) {
  const auto& groups=request_.groups_;
  require(!frame_.empty() && groups.size()<request_.plan_.groups.size(),"anchor batch has no remaining group");
  require(id && (groups.empty() || id>groups.back().id),"unordered anchor batch group identity");
  const auto count=request_.plan_.groups[groups.size()].whole.members;
  require(base.size()==match::extent_bytes && member_count==count,
      "anchor batch source differs from its plan");
  put(frame_,count,group_prefix_bytes);
  append_header(frame_,request_magic,id,count,count*match::tiles,0,Digest{});
  const auto source_offset=frame_.size();
  frame_.insert(frame_.end(),base.begin(),base.end());
  return source_offset;
}

void AnchorMatchBatchBuilder::complete_group(std::uint64_t id,std::size_t source_offset) {
  const auto digest=record_digest(std::span<const std::byte>(frame_).subspan(source_offset));
  std::copy(digest.begin(),digest.end(),frame_.begin()+source_offset-header_size+32);
  request_.groups_.push_back({id,{},digest});
}

AnchorMatchBatchRequest AnchorMatchBatchBuilder::finish() {
  require(!frame_.empty() && request_.groups_.size()==request_.plan_.groups.size() &&
      frame_.size()==request_.plan_.request_bytes,"anchor batch is incomplete or already transferred");
  request_.digest_=record_digest(std::span<const std::byte>(frame_).subspan(header_size));
  std::copy(request_.digest_.begin(),request_.digest_.end(),frame_.begin()+32);
  request_.snapshot_=std::make_shared<const WorkerRequestSnapshot>(std::move(frame_));
  frame_.clear();

  // Bind views after transfer. The builder owns these bytes and already
  // checked the plan, received lengths and digests as they were written.
  const auto wire=request_.snapshot_->bytes();
  std::size_t at=header_size;
  for(std::size_t i=0;i<request_.groups_.size();++i) {
    const auto length=request_.plan_.groups[i].whole.request_bytes;
    at+=group_prefix_bytes;
    request_.groups_[i].source=wire.subspan(at+header_size,length-header_size);
    at+=length;
  }
  return std::move(request_);
}

AnchorMatchBatchRequest decode_anchor_match_batch_request(SharedWorkerRequest snapshot,
    std::uint32_t request_capacity,std::uint32_t reply_capacity,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches) {
  require(snapshot!=nullptr,"missing anchor batch snapshot");
  const auto frame=snapshot->bytes();
  check_header(frame,batch_request_magic,0);
  const auto groups=get(frame,16,4);
  require(frame.size()==request_capacity && request_capacity<=maximum_channel_bytes &&
      groups && groups<=(frame.size()-header_size)/minimum_group_request,"anchor batch request capacity");
  // Inspect geometry and the total launch grant before binding group views.
  std::vector<std::uint32_t> counts;
  counts.reserve(static_cast<std::size_t>(groups));
  std::size_t at=header_size;
  for(std::uint64_t group=0;group<groups;++group) {
    require(frame.size()-at>=group_prefix_bytes,"truncated anchor batch group prefix");
    const auto members=static_cast<std::uint32_t>(get(frame,at,group_prefix_bytes));
    const auto layout=anchor_match_layout(members);
    at+=group_prefix_bytes;
    require(frame.size()-at>=layout.request_bytes,"truncated anchor batch group");
    counts.push_back(members);
    at+=layout.request_bytes;
  }
  auto plan=plan_anchor_match_batch(counts,maximum_quantum,maximum_device_bytes,maximum_launches);
  require(at==frame.size() && plan.request_bytes==request_capacity && plan.reply_bytes==reply_capacity &&
      get(frame,20,4)==plan.launches,"anchor batch plan or trailing data differs");
  const auto digest=record_digest(frame.subspan(header_size));
  require(std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor batch request digest");
  AnchorMatchBatchRequest request;
  request.snapshot_=std::move(snapshot);
  request.id_=get(frame,8,8);
  request.digest_=digest;
  request.plan_=std::move(plan);
  request.groups_.reserve(counts.size());
  at=header_size;
  std::uint64_t previous_id=0;
  for(const auto& group:request.plan_.groups) {
    at+=group_prefix_bytes;
    const auto decoded=inspect_anchor_match_request(frame.subspan(at,group.whole.request_bytes),group.whole);
    require(decoded.id>previous_id,"unordered anchor batch group identity");
    require(record_digest(decoded.source)==decoded.source_digest,"anchor match request digest");
    previous_id=decoded.id;
    request.groups_.push_back(decoded);
    at+=group.whole.request_bytes;
  }
  return request;
}

Bytes encode_anchor_match_batch_reply(const AnchorMatchBatchRequest& request,
    std::span<const std::vector<match::u32>> records,std::size_t reserve_tail_bytes,bool packed_xor) {
  const auto groups=request.groups();
  const auto& plan=request.plan();
  require(!records.empty() && records.size()==groups.size(),"anchor batch result count");
  require(reserve_tail_bytes<=maximum_channel_bytes-plan.reply_bytes,"anchor batch reply tail capacity");
  auto frame=header(packed_xor?xor_batch_magic:batch_reply_magic,request.id(),static_cast<std::uint32_t>(records.size()),
      plan.launches,execution,request.digest(),plan.reply_bytes-header_size+reserve_tail_bytes);
  for(std::size_t i=0;i<records.size();++i) {
    const auto& group=groups[i];
    const auto members=plan.groups[i].whole.members;
    if(packed_xor)require(records[i].size()==members*match::tiles,"packed XOR reply record count");
    else check_records(group,records[i],nullptr);
    put(frame,members,group_prefix_bytes);
    append_header(frame,packed_xor?xor_group_magic:reply_magic,
        group.id,members,members*match::tiles,execution,group.source_digest);
    const auto start=frame.size();
    append_records(frame,records[i]);
    if(packed_xor)for(auto at=start;at<frame.size();at+=match::xor_slot_bytes)
      (void)packed_xor_slot(std::span<const std::byte>(frame).subspan(at,match::xor_slot_bytes));
  }
  require(frame.size()==plan.reply_bytes,"anchor batch reply length");
  return frame;
}

namespace {
template<class ReadGroup>auto read_anchor_batch_reply(std::span<const std::byte> frame,
    const AnchorMatchBatchRequest& request,std::string_view magic,ReadGroup read_group) {
  check_header(frame,magic,execution);
  const auto groups=request.groups();
  const auto& plan=request.plan();
  const auto& digest=request.digest();
  require(!groups.empty() && frame.size()==plan.reply_bytes && get(frame,8,8)==request.id() &&
      get(frame,16,4)==groups.size() && get(frame,20,4)==plan.launches &&
      std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor batch reply binding");
  using Advice=decltype(read_group(frame,groups.front(),plan.groups.front().whole));
  std::vector<Advice> result;
  result.reserve(groups.size());
  std::size_t at=header_size;
  for(std::size_t i=0;i<groups.size();++i) {
    const auto layout=plan.groups[i].whole;
    require(frame.size()-at>=group_prefix_bytes && get(frame,at,group_prefix_bytes)==layout.members,
        "anchor batch reply group prefix");
    at+=group_prefix_bytes;
    require(frame.size()-at>=layout.reply_bytes,"truncated anchor batch reply group");
    // The batch owns these source bytes and binds their digest during construction or parsing.
    const auto& group=groups[i];
    result.push_back(read_group(frame.subspan(at,layout.reply_bytes),group,layout));
    at+=layout.reply_bytes;
  }
  require(at==frame.size(),"anchor batch reply trailing bytes");
  return result;
}
}

std::vector<AnchorMatchAdvice> accept_anchor_match_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest& request) {
  return read_anchor_batch_reply(frame,request,batch_reply_magic,
      [](auto bytes,const auto& group,auto) {
        return read_anchor_match_reply(bytes,group,&group.source_digest);
      });
}
std::vector<AnchorXorAdvice> decode_anchor_xor_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest& request) {
  return read_anchor_batch_reply(frame,request,xor_batch_magic,
      [](auto bytes,const auto& group,auto layout) {
        check_header(bytes,xor_group_magic,execution);
        require(get(bytes,8,8)==group.id && get(bytes,16,4)==layout.members &&
            get(bytes,20,4)==layout.members*match::tiles &&
            std::ranges::equal(group.source_digest,bytes.subspan(32,32)),"packed XOR reply group binding");
        AnchorXorAdvice result;
        result.reserve(layout.members);
        for(std::size_t at=header_size;at<bytes.size();at+=match::xor_slot_bytes) {
          const auto body=packed_xor_slot(bytes.subspan(at,match::xor_slot_bytes));
          result.emplace_back(body.begin(),body.end());
        }
        return result;
      });
}

} // namespace flitzfs::detail
