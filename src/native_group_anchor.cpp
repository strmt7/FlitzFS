#include "native_group_anchor.hpp"
#include "native_group_codec.hpp"
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
constexpr std::string_view wide_xor_request_magic="FLFSABQ\2";
constexpr std::string_view wide_xor_batch_magic="FLFSAXP\2",wide_xor_group_magic="FLFSAXG\2";
constexpr std::string_view delta_request_magic="FLFSABQ\3";
constexpr std::string_view delta_batch_magic="FLFSADP\1",delta_group_magic="FLFSADG\1";
constexpr std::string_view sparse_request_magic="FLFSABQ\4";
constexpr std::string_view sparse_batch_magic="FLFSASP\1",sparse_group_magic="FLFSASG\1";
constexpr std::string_view collective_request_magic="FLFSABQ\5";
constexpr std::string_view collective_batch_magic="FLFSACP\1",collective_group_magic="FLFSACG\1";
constexpr std::string_view reference_request_magic="FLFSABQ\6";
constexpr std::string_view reference_batch_magic="FLFSARP\1",reference_group_magic="FLFSARG\1";
constexpr std::uint32_t group_prefix_bytes=4;
constexpr auto minimum_group_request=group_prefix_bytes+header_size+2*match::extent_bytes;
constexpr auto execution=static_cast<std::uint32_t>(WorkerExecution::hip);
static_assert(match::extent_bytes==kNativeExtentBytes);
static_assert(header_size+(match::max_members+1)*match::extent_bytes<=kWorkerRequestLimit);
static_assert(header_size+match::max_members*match::tiles*sizeof(match::u32)<=kWorkerReplyLimit);
static_assert(maximum_members*match::tiles<=(maximum_channel_bytes-header_size)/record_bytes);
static_assert(std::uint64_t(maximum_members+1)*match::extent_bytes+match::index_bytes+
    std::uint64_t(maximum_members)*match::tiles*record_bytes<=(std::numeric_limits<std::uint32_t>::max)());
static_assert((std::uint64_t(maximum_members)+1)*match::extent_bytes+
    std::uint64_t(maximum_members)*match::xor_wide_slot_bytes<=(std::numeric_limits<std::uint32_t>::max)());

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
    std::uint32_t backend,const Digest& digest,std::uint32_t reference=0) {
  require(id!=0,"missing anchor match request identity");
  for (const auto byte:magic) frame.push_back(static_cast<std::byte>(byte));
  put(frame,id,8);
  put(frame,count,4);
  put(frame,work,4);
  put(frame,backend,4);
  put(frame,reference,4);
  frame.insert(frame.end(),digest.begin(),digest.end());
}

Bytes header(std::string_view magic,std::uint64_t id,std::uint32_t count,std::uint32_t work,
    std::uint32_t backend,const Digest& digest,std::size_t payload_size) {
  Bytes frame;
  frame.reserve(header_size+payload_size);
  append_header(frame,magic,id,count,work,backend,digest);
  return frame;
}

void check_header(std::span<const std::byte> frame,std::string_view magic,std::uint32_t backend,bool reference=false) {
  require(frame.size()>=header_size && std::memcmp(frame.data(),magic.data(),magic.size())==0 &&
      get(frame,8,8)!=0 && get(frame,24,4)==backend && (reference || get(frame,28,4)==0),
      "invalid anchor match frame header");
}

std::string_view batch_request_tag(match::BatchOutput output) noexcept {
  if(output==match::BatchOutput::reference_xor)return reference_request_magic;
  if(output==match::BatchOutput::collective)return collective_request_magic;
  if(output==match::BatchOutput::modular_delta)return delta_request_magic;
  if(output==match::BatchOutput::sparse_xor)return sparse_request_magic;
  return match::wide_output(output)?wide_xor_request_magic:batch_request_magic;
}
std::string_view batch_reply_tag(match::BatchOutput output) noexcept {
  if(output==match::BatchOutput::reference_xor)return reference_batch_magic;
  if(output==match::BatchOutput::collective)return collective_batch_magic;
  if(output==match::BatchOutput::sparse_xor)return sparse_batch_magic;
  if(output==match::BatchOutput::modular_delta)return delta_batch_magic;
  if(output==match::BatchOutput::wide_packed_xor)return wide_xor_batch_magic;
  return output==match::BatchOutput::packed_xor?xor_batch_magic:batch_reply_magic;
}
std::string_view group_reply_tag(match::BatchOutput output) noexcept {
  if(output==match::BatchOutput::reference_xor)return reference_group_magic;
  if(output==match::BatchOutput::collective)return collective_group_magic;
  if(output==match::BatchOutput::sparse_xor)return sparse_group_magic;
  if(output==match::BatchOutput::modular_delta)return delta_group_magic;
  if(output==match::BatchOutput::wide_packed_xor)return wide_xor_group_magic;
  return output==match::BatchOutput::packed_xor?xor_group_magic:reply_magic;
}
bool valid_packed_payload_size(std::size_t bytes,match::BatchOutput output) noexcept {
  if(!bytes)return true;
  if(output==match::BatchOutput::collective)
    return bytes>=match::collective_body_tag_bytes+match::delta_parameters_bytes &&
        bytes<=match::collective_body_tag_bytes+match::delta_max_payload_bytes;
  if(output==match::BatchOutput::modular_delta)
    return bytes>=match::delta_parameters_bytes && bytes<=match::delta_max_payload_bytes;
  if(output==match::BatchOutput::sparse_xor || output==match::BatchOutput::reference_xor)
    return bytes>=match::sparse_xor_min_body_bytes && bytes<=match::sparse_xor_max_body_bytes;
  return bytes>=match::xor_tiles+match::xor_plane_bytes && (bytes-match::xor_tiles)%match::xor_plane_bytes==0;
}
bool valid_packed_payload(std::span<const std::byte> body,match::BatchOutput output) noexcept {
  return valid_packed_payload_size(body.size(),output) &&
      (body.empty() || output!=match::BatchOutput::collective || parse_collective_group_body(body).has_value());
}
bool valid_reference_member(std::uint32_t member,std::uint32_t candidates,std::uint32_t members) noexcept {
  if(member>members || candidates<2)return false;
  for(std::uint32_t candidate=0;candidate<candidates;++candidate)
    if(match::reference_member_at(candidate,candidates,members)==member)return true;
  return false;
}
std::span<const std::byte> packed_slot(std::span<const std::byte> slot,match::BatchOutput output) {
  require(slot.size()==match::reply_member_bytes(output),"packed reply slot extent");
  const auto size=static_cast<std::size_t>(get(slot,0,record_bytes));
  require(size<=slot.size()-record_bytes && valid_packed_payload_size(size,output),"packed reply body length");
  require(std::ranges::all_of(slot.subspan(record_bytes+size),[](auto byte){return byte==std::byte{255};}),
      "packed reply slot padding");
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

AnchorMatchLayout anchor_match_layout(std::uint32_t members,match::BatchOutput output) {
  require(output==match::BatchOutput::anchors || output==match::BatchOutput::packed_xor ||
      match::wide_output(output),"unknown anchor reply layout");
  const auto member_bytes=match::reply_member_bytes(output);
  require(members && members<=maximum_members &&
      members<=(maximum_channel_bytes-header_size)/member_bytes,"invalid anchor match member count");
  return {members,header_size+(members+1)*match::extent_bytes,header_size+members*member_bytes};
}

AnchorMatchSweepPlan plan_anchor_match_sweep(std::uint32_t members,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_windows,match::BatchOutput output) {
  const auto whole=anchor_match_layout(members,output);
  const auto fixed_bytes=match::device_bytes(0,output);
  const auto per_member_bytes=match::extent_bytes+match::reply_member_bytes(output);
  const auto demand=(std::min)({members,maximum_quantum,match::dispatch_member_limit(output)});
  const auto fit=maximum_device_bytes>=fixed_bytes?(maximum_device_bytes-fixed_bytes)/per_member_bytes:0;
  const auto quantum=static_cast<std::uint32_t>((std::min)(std::uint64_t(demand),fit));
  require(quantum!=0,"anchor match sweep cannot admit one search window");
  const auto windows=1+(members-1)/quantum;
  require(windows<=maximum_windows,"anchor match sweep exceeds its search window grant");
  return {whole,quantum,windows,match::device_bytes(quantum,output)};
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

std::uint32_t anchor_match_batch_member_limit() noexcept {return maximum_members;}

std::uint32_t anchor_reference_pricing_launches(const AnchorMatchBatchPlan& plan) noexcept {
  std::uint32_t result=0;
  for(const auto& group:plan.groups)result+=1+group.whole.members/group.quantum_members;
  return result;
}

AnchorMatchBatchPlan plan_anchor_match_batch(std::span<const std::uint32_t> members,
    std::uint32_t maximum_quantum,std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches,
    match::BatchOutput output) {
  require(!members.empty() && members.size()<=(maximum_channel_bytes-header_size)/minimum_group_request,
      "invalid anchor batch group count");
  AnchorMatchBatchPlan plan{};
  plan.request_bytes=plan.reply_bytes=header_size;
  plan.output=output;
  plan.groups.reserve(members.size());
  const auto index_launches=match::wide_output(output)?0U:1U;
  for(const auto count:members) {
    const auto group=plan_anchor_match_sweep(count,maximum_quantum,maximum_device_bytes,maximum_launches,output);
    require(std::uint64_t(plan.request_bytes)+group_prefix_bytes+group.whole.request_bytes<=maximum_channel_bytes &&
        std::uint64_t(plan.reply_bytes)+group_prefix_bytes+group.whole.reply_bytes<=maximum_channel_bytes,
        "anchor batch channel capacity");
    require(std::uint64_t(plan.launches)+index_launches+group.windows<=maximum_launches,
        "anchor batch exceeds its launch grant");
    plan.request_bytes+=group_prefix_bytes+group.whole.request_bytes;
    plan.reply_bytes+=group_prefix_bytes+group.whole.reply_bytes;
    plan.launches+=index_launches+group.windows;
    plan.quantum_members=(std::max)(plan.quantum_members,group.quantum_members);
    plan.device_bytes=(std::max)(plan.device_bytes,group.device_bytes);
    plan.groups.push_back(group);
  }
  if(output==match::BatchOutput::reference_xor) {
    const auto pricing_launches=anchor_reference_pricing_launches(plan);
    const auto eligible=*std::ranges::min_element(members)+1;
    plan.reference_candidates=(std::min)(eligible,(maximum_launches-plan.launches)/pricing_launches);
    require(plan.reference_candidates>=2,"anchor reference search exceeds its launch grant");
    plan.launches+=plan.reference_candidates*pricing_launches;
  }
  return plan;
}

AnchorMatchTransferPlan plan_anchor_match_transfers(const AnchorMatchBatchPlan& batch,
    std::uint64_t maximum_device_bytes) {
  const auto quantum=batch.quantum_members;
  require(!batch.groups.empty() && quantum && quantum<=match::dispatch_member_limit(batch.output) &&
      batch.device_bytes==match::device_bytes(quantum,batch.output) && maximum_device_bytes>=batch.device_bytes,
      "invalid anchor transfer allocation grant");
  std::uint32_t largest=0;
  for(const auto& group:batch.groups) {
    const auto members=group.whole.members;
    require(members && members<=maximum_members && group.quantum_members==(std::min)(members,quantum),
        "invalid anchor transfer group geometry");
    largest=(std::max)(largest,members);
  }
  require(largest>=quantum,"anchor transfer does not contain its declared quantum");

  const auto fixed_bytes=match::device_bytes(0,batch.output);
  const auto per_member_bytes=match::extent_bytes+match::reply_member_bytes(batch.output);
  const auto fit=(maximum_device_bytes-fixed_bytes)/per_member_bytes;
  auto members=static_cast<std::uint32_t>((std::min)(fit,std::uint64_t(largest)));
  if(members<largest)members-=members%quantum;
  require(members>=quantum,"anchor transfer cannot fit one search dispatch");
  require(batch.output!=match::BatchOutput::reference_xor || members==largest,
      "anchor reference search requires whole resident groups");
  std::uint64_t transfers=0;
  for(const auto& group:batch.groups)transfers+=1+(group.whole.members-1)/members;
  const auto index_launches=match::wide_output(batch.output)?0:batch.groups.size();
  require(batch.launches>=index_launches && transfers<=batch.launches-index_launches,
      "anchor transfer work exceeds batch launch grant");
  return {members,match::device_bytes(members,batch.output),static_cast<std::uint32_t>(transfers)};
}

AnchorMatchBatchBuilder::AnchorMatchBatchBuilder(std::uint64_t id,const AnchorMatchBatchPlan& plan) {
  require(id!=0,"missing anchor batch identity");
  std::vector<std::uint32_t> counts;
  counts.reserve(plan.groups.size());
  for(const auto& group:plan.groups)counts.push_back(group.whole.members);
  require(plan==plan_anchor_match_batch(counts,plan.quantum_members,plan.device_bytes,plan.launches,plan.output),
      "invalid anchor batch construction plan");
  request_.id_=id;
  request_.plan_=plan;
  request_.groups_.reserve(counts.size());
  const auto magic=batch_request_tag(plan.output);
  frame_=header(magic,id,static_cast<std::uint32_t>(counts.size()),plan.launches,0,
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
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches,match::BatchOutput output) {
  require(snapshot!=nullptr,"missing anchor batch snapshot");
  const auto frame=snapshot->bytes();
  const auto magic=batch_request_tag(output);
  check_header(frame,magic,0);
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
  auto plan=plan_anchor_match_batch(counts,maximum_quantum,maximum_device_bytes,maximum_launches,output);
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
    const auto decoded=inspect_anchor_match_request(frame.subspan(at,group.whole.request_bytes),
        anchor_match_layout(group.whole.members));
    require(decoded.id>previous_id,"unordered anchor batch group identity");
    require(record_digest(decoded.source)==decoded.source_digest,"anchor match request digest");
    previous_id=decoded.id;
    request.groups_.push_back(decoded);
    at+=group.whole.request_bytes;
  }
  return request;
}

namespace {
template<class ReadBody>
Bytes encode_compact_batch_reply(const AnchorMatchBatchRequest& request,ReadBody read_body,
    std::size_t reserve_tail_bytes,std::size_t known_body_bytes=0,std::span<const std::uint32_t> references={}) {
  const auto groups=request.groups();
  const auto& plan=request.plan();
  const auto slot_bytes=match::reply_member_bytes(plan.output);
  const auto words_per_member=slot_bytes/record_bytes;
  require(reserve_tail_bytes<=maximum_channel_bytes-plan.reply_bytes,"anchor batch reply tail capacity");
  auto frame=header(batch_reply_tag(plan.output),request.id(),static_cast<std::uint32_t>(groups.size()),
      plan.launches,execution,request.digest(),groups.size()*(group_prefix_bytes+header_size)+
      known_body_bytes+reserve_tail_bytes);
  for(std::size_t i=0;i<groups.size();++i) {
    const auto members=plan.groups[i].whole.members;
    const auto prefix=frame.size();
    put(frame,0,group_prefix_bytes);
    append_header(frame,group_reply_tag(plan.output),groups[i].id,members,members*words_per_member,
        execution,groups[i].source_digest,references.empty()?0:references[i]);
    for(std::uint32_t member=0;member<members;++member) {
      const auto body=read_body(i,member);
      put(frame,body.size(),record_bytes);
      frame.insert(frame.end(),body.begin(),body.end());
    }
    const auto group_bytes=frame.size()-prefix-group_prefix_bytes;
    for(unsigned j=0;j<group_prefix_bytes;++j)frame[prefix+j]=static_cast<std::byte>(group_bytes>>(8*j));
  }
  require(frame.size()<=plan.reply_bytes,"anchor batch reply length");
  return frame;
}
}

Bytes encode_anchor_match_batch_reply(const AnchorMatchBatchRequest& request,
    std::span<const std::vector<match::u32>> records,std::size_t reserve_tail_bytes) {
  const auto groups=request.groups();
  const auto& plan=request.plan();
  const bool packed=plan.output!=match::BatchOutput::anchors;
  const auto slot_bytes=match::reply_member_bytes(plan.output);
  const auto words_per_member=slot_bytes/record_bytes;
  const auto batch_magic=batch_reply_tag(plan.output);
  const auto group_magic=group_reply_tag(plan.output);
  require(plan.output!=match::BatchOutput::collective && plan.output!=match::BatchOutput::reference_xor &&
      !records.empty() && records.size()==groups.size(),
      "anchor batch result count or layout");
  if(match::compact_output(plan.output)) {
    for(std::size_t i=0;i<records.size();++i)
      require(records[i].size()==plan.groups[i].whole.members*words_per_member,"packed reply record count");
    return encode_compact_batch_reply(request,[&](auto group,auto member) {
      return packed_slot(std::as_bytes(std::span(records[group])).subspan(member*slot_bytes,slot_bytes),plan.output);
    },reserve_tail_bytes);
  }
  require(reserve_tail_bytes<=maximum_channel_bytes-plan.reply_bytes,"anchor batch reply tail capacity");
  auto frame=header(batch_magic,request.id(),static_cast<std::uint32_t>(records.size()),
      plan.launches,execution,request.digest(),plan.reply_bytes-header_size+reserve_tail_bytes);
  for(std::size_t i=0;i<records.size();++i) {
    const auto& group=groups[i];
    const auto members=plan.groups[i].whole.members;
    if(packed)require(records[i].size()==members*words_per_member,"packed reply record count");
    else check_records(group,records[i],nullptr);
    put(frame,members,group_prefix_bytes);
    append_header(frame,group_magic,group.id,members,members*words_per_member,execution,group.source_digest);
    const auto start=frame.size();
    append_records(frame,records[i]);
    if(packed)for(auto at=start;at<frame.size();at+=slot_bytes)
      (void)packed_slot(std::span<const std::byte>(frame).subspan(at,slot_bytes),plan.output);
  }
  require(frame.size()==plan.reply_bytes,"anchor batch reply length");
  return frame;
}

std::vector<AnchorArenaBody> check_anchor_arena_directory(
    std::span<const match::u32> directory,match::u32 arena_capacity_words,match::BatchOutput output) {
  const auto header=match::sparse_xor_arena_header_words;
  const auto stride=match::sparse_xor_arena_entry_words;
  require(match::arena_output(output) && directory.size()>header && (directory.size()-header)%stride==0 &&
      (directory.size()-header)/stride<=maximum_members,"anchor device directory extent");
  const auto used_words=directory.front();
  require(used_words<=arena_capacity_words,"anchor device arena extent");
  std::vector<AnchorArenaBody> bodies;
  bodies.reserve((directory.size()-header)/stride);
  for(auto at=static_cast<std::size_t>(header);at<directory.size();at+=stride) {
    const auto offset=directory[at];
    const auto size=directory[at+1];
    require(valid_packed_payload_size(size,output),"anchor device body size");
    if(!size) {
      require(!offset,"empty anchor device body offset");
      continue;
    }
    require(offset<=used_words && match::sparse_xor_arena_words(size)<=used_words-offset,
        "anchor device body range");
    bodies.push_back({offset,size,static_cast<match::u32>((at-header)/stride)});
  }
  std::ranges::sort(bodies,{},&AnchorArenaBody::offset_words);
  match::u32 next=0;
  for(const auto& body:bodies) {
    require(body.offset_words==next,"anchor device arena gap or overlap");
    next+=match::sparse_xor_arena_words(body.size_bytes);
  }
  require(next==used_words,"anchor device arena trailing bytes");
  return bodies;
}

void take_anchor_arena_bodies(std::span<const AnchorArenaBody> bodies,
    std::span<const std::byte> arena,std::span<Bytes> members) {
  for(const auto& body:bodies) {
    const auto at=std::size_t{body.offset_words}*record_bytes;
    const auto aligned=std::size_t{match::sparse_xor_arena_words(body.size_bytes)}*record_bytes;
    require(body.member<members.size() && at<=arena.size() && aligned<=arena.size()-at,
        "anchor device readback extent");
    const auto slot=arena.subspan(at,aligned);
    require(std::ranges::all_of(slot.subspan(body.size_bytes),[](auto byte){return byte==std::byte{255};}),
        "anchor device arena padding");
    members[body.member].assign(slot.begin(),slot.begin()+body.size_bytes);
  }
}

Bytes encode_anchor_arena_batch_reply(const AnchorMatchBatchRequest& request,
    std::span<const AnchorPackedAdvice> groups,std::size_t reserve_tail_bytes,std::span<const std::uint32_t> references) {
  const auto& plan=request.plan();
  require(match::arena_output(plan.output) && !groups.empty() &&
      groups.size()==plan.groups.size(),"anchor device reply groups");
  require(plan.reference_candidates?references.size()==groups.size():references.empty(),"anchor reply reference count");
  std::size_t members_bytes=0;
  for(std::size_t i=0;i<groups.size();++i) {
    require(groups[i].size()==plan.groups[i].whole.members,"anchor device reply members");
    if(!references.empty())require(valid_reference_member(references[i],plan.reference_candidates,plan.groups[i].whole.members),
        "anchor reply reference outside search");
    for(const auto& body:groups[i]) {
      require(valid_packed_payload(body,plan.output),"anchor device payload");
      members_bytes+=record_bytes+body.size();
    }
  }
  return encode_compact_batch_reply(request,[&](auto group,auto member) {
    return std::span<const std::byte>(groups[group][member]);
  },reserve_tail_bytes,members_bytes,references);
}

namespace {
template<class ReadGroup>auto read_anchor_batch_reply(std::span<const std::byte> frame,
    const AnchorMatchBatchRequest& request,std::string_view magic,ReadGroup read_group) {
  check_header(frame,magic,execution);
  const auto groups=request.groups();
  const auto& plan=request.plan();
  const auto& digest=request.digest();
  const bool compact=match::compact_output(plan.output);
  require(!groups.empty() && (compact?frame.size()<=plan.reply_bytes:frame.size()==plan.reply_bytes) &&
      get(frame,8,8)==request.id() &&
      get(frame,16,4)==groups.size() && get(frame,20,4)==plan.launches &&
      std::equal(digest.begin(),digest.end(),frame.begin()+32),"anchor batch reply binding");
  using Advice=decltype(read_group(frame,groups.front(),plan.groups.front().whole));
  std::vector<Advice> result;
  result.reserve(groups.size());
  std::size_t at=header_size;
  for(std::size_t i=0;i<groups.size();++i) {
    const auto layout=plan.groups[i].whole;
    require(frame.size()-at>=group_prefix_bytes,"anchor batch reply group prefix");
    const auto prefix=get(frame,at,group_prefix_bytes);
    require(compact?(prefix>=header_size && prefix<=layout.reply_bytes):prefix==layout.members,
        "anchor batch reply group extent");
    at+=group_prefix_bytes;
    const auto group_bytes=compact?static_cast<std::size_t>(prefix):layout.reply_bytes;
    require(frame.size()-at>=group_bytes,"truncated anchor batch reply group");
    // The batch owns these source bytes and binds their digest during construction or parsing.
    const auto& group=groups[i];
    result.push_back(read_group(frame.subspan(at,group_bytes),group,layout));
    at+=group_bytes;
  }
  require(at==frame.size(),"anchor batch reply trailing bytes");
  return result;
}
}

std::vector<AnchorMatchAdvice> accept_anchor_match_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest& request) {
  require(request.plan().output==match::BatchOutput::anchors,"copy reply requested for a packed XOR batch");
  return read_anchor_batch_reply(frame,request,batch_reply_magic,
      [](auto bytes,const auto& group,auto) {
        return read_anchor_match_reply(bytes,group,&group.source_digest);
      });
}
std::vector<AnchorPackedAdvice> decode_anchor_packed_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest& request,std::span<std::uint32_t> references) {
  const auto output=request.plan().output;
  require(output!=match::BatchOutput::anchors,"packed reply requested for a copy batch");
  const bool compact=match::compact_output(output);
  const auto candidates=request.plan().reference_candidates;
  require(candidates?references.size()==request.groups().size():references.empty(),"anchor reply reference count");
  const auto slot_bytes=match::reply_member_bytes(output);
  return read_anchor_batch_reply(frame,request,batch_reply_tag(output),
      [output,slot_bytes,compact,candidates,&references](auto bytes,const auto& group,auto layout) {
        check_header(bytes,group_reply_tag(output),execution,candidates!=0);
        require(get(bytes,8,8)==group.id && get(bytes,16,4)==layout.members &&
            get(bytes,20,4)==layout.members*(slot_bytes/record_bytes) &&
            std::ranges::equal(group.source_digest,bytes.subspan(32,32)),"packed reply group binding");
        if(candidates) {
          const auto reference=static_cast<std::uint32_t>(get(bytes,28,4));
          require(valid_reference_member(reference,candidates,layout.members),"anchor reply reference outside search");
          references.front()=reference;
          references=references.subspan(1);
        }
        AnchorPackedAdvice result;
        result.reserve(layout.members);
        std::size_t at=header_size;
        for(std::uint32_t i=0;i<layout.members;++i) {
          if(compact) {
            require(bytes.size()-at>=record_bytes,"truncated compact payload length");
            const auto size=static_cast<std::size_t>(get(bytes,at,record_bytes));
            at+=record_bytes;
            require(valid_packed_payload_size(size,output) && size<=bytes.size()-at,"invalid compact payload length");
            const auto body=bytes.subspan(at,size);
            require(valid_packed_payload(body,output),"invalid compact payload codec");
            result.emplace_back(body.begin(),body.end());
            at+=size;
            continue;
          }
          const auto body=packed_slot(bytes.subspan(at,slot_bytes),output);
          result.emplace_back(body.begin(),body.end());
          at+=slot_bytes;
        }
        require(at==bytes.size(),"packed reply group trailing bytes");
        return result;
      });
}

AnchorPackedBatchCheck inspect_anchor_packed_checks(std::span<const NativeSharedBaseSearchReply> replies) noexcept {
  std::size_t verified_groups=0;
  bool pending=false;
  for(const auto& reply:replies) {
    const auto bodies=reply.packed_bodies;
    const auto checks=reply.packed_checks;
    if(bodies.size()!=checks.size() || bodies.size()!=reply.window.members().size())
      return {AnchorPackedBatchStatus::invalid,0};
    bool verified=false;
    for(std::size_t i=0;i<bodies.size();++i) {
      if(bodies[i].empty())continue;
      if(checks[i]==NativePackedGroupCheck::invalid)return {AnchorPackedBatchStatus::invalid,0};
      if(checks[i]==NativePackedGroupCheck::verified)verified=true;
      else pending=true;
    }
    if(verified)++verified_groups;
  }
  if(pending)return {AnchorPackedBatchStatus::pending,0};
  return {verified_groups?AnchorPackedBatchStatus::verified:AnchorPackedBatchStatus::empty,verified_groups};
}

} // namespace flitzfs::detail
