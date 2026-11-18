#include "native_group_anchor.hpp"
#include "flitzfs/worker_wire.hpp"
#include "record_digest.hpp"
#include <algorithm>
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
void check_records(const AnchorMatchSource& request,std::span<const match::u32> records,
    AnchorMatchAdvice* advice) {
  const auto members=anchor_match_members(request.source);
  require(records.size()==members*match::tiles,"anchor match record count");
  if (advice) advice->resize(members);
  for (match::u32 index=0;index<members;++index) {
    const auto* member=request.source.data()+(index+1)*match::extent_bytes;
    auto* const ranges=advice?&(*advice)[index]:nullptr;
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
  for (const auto word:records) put(frame,word,sizeof(word));
  return frame;
}

AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchSource& request) {
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
  layouts_.reserve(counts.size());
  for(const auto& group:plan.groups)layouts_.push_back(group.whole);
  frame_=header(batch_request_magic,id,static_cast<std::uint32_t>(counts.size()),plan.launches,0,
      Digest{},plan.request_bytes-header_size);
}

void AnchorMatchBatchBuilder::append(std::uint64_t id,std::span<const std::byte> base,
    std::span<const std::byte> members) {
  require(next_group_<layouts_.size(),"anchor batch has no remaining group");
  require(id>previous_id_,"unordered anchor batch group identity");
  const auto count=layouts_[next_group_].members;
  require(base.size()==match::extent_bytes && members.size()==std::size_t(count)*match::extent_bytes,
      "anchor batch source differs from its plan");
  const auto digest=record_digest(base,members);
  put(frame_,count,group_prefix_bytes);
  append_header(frame_,request_magic,id,count,count*match::tiles,0,digest);
  frame_.insert(frame_.end(),base.begin(),base.end());
  frame_.insert(frame_.end(),members.begin(),members.end());
  previous_id_=id;
  ++next_group_;
}

SharedWorkerRequest AnchorMatchBatchBuilder::finish() {
  require(!frame_.empty() && next_group_==layouts_.size(),"anchor batch is incomplete or already transferred");
  const auto digest=record_digest(std::span<const std::byte>(frame_).subspan(header_size));
  std::copy(digest.begin(),digest.end(),frame_.begin()+32);
  auto snapshot=std::make_shared<const WorkerRequestSnapshot>(std::move(frame_));
  frame_.clear();
  return snapshot;
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
    std::span<const std::vector<match::u32>> records) {
  const auto groups=request.groups();
  const auto& plan=request.plan();
  require(!records.empty() && records.size()==groups.size(),"anchor batch result count");
  auto frame=header(batch_reply_magic,request.id(),static_cast<std::uint32_t>(records.size()),
      plan.launches,execution,request.digest(),plan.reply_bytes-header_size);
  for(std::size_t i=0;i<records.size();++i) {
    const auto& group=groups[i];
    const auto members=plan.groups[i].whole.members;
    check_records(group,records[i],nullptr);
    put(frame,members,group_prefix_bytes);
    append_header(frame,reply_magic,group.id,members,members*match::tiles,execution,group.source_digest);
    for(const auto word:records[i])put(frame,word,sizeof(word));
  }
  require(frame.size()==plan.reply_bytes,"anchor batch reply length");
  return frame;
}

std::vector<AnchorMatchAdvice> accept_anchor_match_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest& request) {
  check_header(frame,batch_reply_magic,execution);
  const auto groups=request.groups();
  const auto& plan=request.plan();
  const auto& digest=request.digest();
  require(!groups.empty() && frame.size()==plan.reply_bytes && get(frame,8,8)==request.id() &&
      get(frame,16,4)==groups.size() && get(frame,20,4)==plan.launches &&
      std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor batch reply binding");
  std::vector<AnchorMatchAdvice> result;
  result.reserve(groups.size());
  std::size_t at=header_size;
  for(std::size_t i=0;i<groups.size();++i) {
    const auto layout=plan.groups[i].whole;
    require(frame.size()-at>=group_prefix_bytes && get(frame,at,group_prefix_bytes)==layout.members,
        "anchor batch reply group prefix");
    at+=group_prefix_bytes;
    require(frame.size()-at>=layout.reply_bytes,"truncated anchor batch reply group");
    result.push_back(accept_anchor_match_reply(frame.subspan(at,layout.reply_bytes),groups[i]));
    at+=layout.reply_bytes;
  }
  require(at==frame.size(),"anchor batch reply trailing bytes");
  return result;
}

} // namespace flitzfs::detail
