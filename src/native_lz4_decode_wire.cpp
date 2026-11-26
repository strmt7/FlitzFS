#include "native_lz4_decode_wire.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace flitzfs::detail {
namespace {
using Bytes=std::vector<std::byte>;
using View=std::span<const std::byte>;
constexpr std::size_t header_bytes=64,key_bytes=72;
constexpr auto wire_limit=std::size_t((std::numeric_limits<std::int32_t>::max)());
constexpr std::array request_magic{std::byte{'F'},std::byte{'L'},std::byte{'N'},std::byte{'L'},
    std::byte{'Z'},std::byte{'4'},std::byte{'Q'},std::byte{1}};
constexpr auto reply_magic=[] {auto value=request_magic;value[6]=std::byte{'P'};return value;}();

[[noreturn]] void invalid(const char* why){throw std::invalid_argument(why);}
void put(std::span<std::byte> out,std::size_t at,std::uint64_t value,unsigned width){
  for(unsigned i=0;i<width;++i)out[at+i]=static_cast<std::byte>((value>>(i*8))&255);
}
std::uint64_t get(View bytes,std::size_t at,unsigned width) noexcept {
  std::uint64_t n=0;
  for(unsigned i=0;i<width;++i)n|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(i*8);
  return n;
}
template<std::size_t N> std::array<std::byte,N> field(View bytes,std::size_t at){
  std::array<std::byte,N> result{};
  std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),N,result.begin());
  return result;
}
std::size_t add(std::size_t total,std::uint64_t count) {
  if(total>wire_limit || count>wire_limit-total)invalid("native LZ4 wire size");
  return total+static_cast<std::size_t>(count);
}
bool valid_key(const NativeLz4ReadKey& key) noexcept {
  return key.image!=StoreIdentity{} && key.object && key.generation &&
      key.decoded_bytes && key.decoded_bytes<=kStoreMaxObjectBytes;
}
bool valid_backend(WorkerExecution backend) noexcept {
  return backend==WorkerExecution::cpu_semantics || backend==WorkerExecution::hip;
}
void write_header(std::span<std::byte> bytes,const std::array<std::byte,8>& magic,
    std::uint64_t id,std::size_t count,std::uint32_t role,Digest digest={}) {
  std::ranges::copy(magic,bytes.begin());
  put(bytes,8,id,8);put(bytes,16,count,4);put(bytes,20,role,4);
  put(bytes,24,bytes.size()-header_bytes,8);
  std::ranges::copy(digest,bytes.begin()+32);
}
void require_header(View bytes,const std::array<std::byte,8>& magic) {
  if(bytes.size()<header_bytes || bytes.size()>wire_limit ||
      field<8>(bytes,0)!=magic || !get(bytes,8,8) || !get(bytes,16,4) ||
      get(bytes,24,8)!=bytes.size()-header_bytes)invalid("native LZ4 wire header");
}
void write_key(std::span<std::byte> out,const NativeLz4ReadKey& key,std::size_t encoded) {
  std::ranges::copy(key.image,out.begin());
  put(out,16,key.object,8);put(out,24,key.generation,8);
  put(out,32,key.decoded_bytes,4);put(out,36,encoded,4);
  std::ranges::copy(key.decoded_digest,out.begin()+40);
}
NativeLz4ReadKey read_key(View in) {
  return {field<16>(in,0),get(in,16,8),get(in,24,8),static_cast<std::uint32_t>(get(in,32,4)),
      field<32>(in,40)};
}
void require_unique(std::span<const NativeLz4ReadKey> keys){
  auto sorted=std::vector<NativeLz4ReadKey>(keys.begin(),keys.end());
  std::ranges::sort(sorted);
  if(std::ranges::adjacent_find(sorted)!=sorted.end())invalid("duplicate native LZ4 read key");
}
}

NativeLz4DispatchPlan plan_native_lz4_dispatches(std::span<const NativeLz4CapturedFrame> captured,
    NativeLz4DecodeGrant grant) noexcept {
  const auto frames=static_cast<std::uint32_t>((std::min)(captured.size(),std::size_t{grant.maximum_frames}));
  NativeLz4DispatchPlan best;
  // The wire decoder uses a fixed frame count per dispatch. Prefer the widest
  // such dispatch that admits the same prefix, since wider batches need fewer
  // launches. Each batch has its own encoded and decoded slot widths.
  for(auto width=frames;width;--width) {
    NativeLz4DispatchPlan candidate{0,width};
    std::uint64_t completed=0,slab=0;
    std::uint32_t count=0,encoded=0,decoded=0;
    for(const auto& frame:captured.first(frames)) {
      if(count==width) {
        completed+=slab;
        count=encoded=decoded=0;
      }
      encoded=(std::max)(encoded,static_cast<std::uint32_t>(frame.encoded().size()));
      decoded=(std::max)(decoded,frame.key().decoded_bytes);
      const auto layout=native_lz4_layout(++count,encoded,decoded);
      slab=native_lz4_decode::output_at(layout,layout.chunks);
      if(slab>grant.maximum_slab_bytes-completed)break;
      ++candidate.frames;
      candidate.aggregate_slab_bytes=completed+slab;
      candidate.peak_slab_bytes=(std::max)(candidate.peak_slab_bytes,slab);
    }
    if(candidate.frames>best.frames) {
      candidate.frames_per_dispatch=(std::min)(candidate.frames,candidate.frames_per_dispatch);
      best=candidate;
      if(best.frames==frames)break;
    }
  }
  return best;
}

NativeLz4ReadExchange NativeLz4ReadExchange::prepare(std::span<const NativeLz4DecodeSource> entries,
    std::uint64_t id,NativeLz4DecodeGrant grant){
  if(!store_lz4_available() || !id || entries.empty() || entries.size()>grant.maximum_frames)
    invalid("native LZ4 request admission");
  auto request=add(header_bytes,std::uint64_t(entries.size())*key_bytes);
  auto reply=header_bytes;
  std::uint64_t room=grant.maximum_slab_bytes;
  for(const auto& entry:entries){
    if(!valid_key(entry.key) || entry.encoded.empty() || entry.encoded.size()>=entry.key.decoded_bytes ||
        entry.encoded.size()+entry.key.decoded_bytes>room)invalid("native LZ4 request frame");
    room-=entry.encoded.size()+entry.key.decoded_bytes;
    request=add(request,entry.encoded.size());
    reply=add(reply,entry.key.decoded_bytes);
  }
  NativeLz4ReadExchange exchange;
  exchange.id_=id;
  exchange.reply_bytes_=reply;
  exchange.keys_.reserve(entries.size());
  Bytes bytes(request);
  write_header(bytes,request_magic,id,entries.size(),0);
  std::size_t descriptor=header_bytes,body=header_bytes+key_bytes*entries.size();
  for(const auto& entry:entries){
    write_key(std::span(bytes).subspan(descriptor,key_bytes),entry.key,entry.encoded.size());
    std::ranges::copy(entry.encoded,bytes.begin()+static_cast<std::ptrdiff_t>(body));
    descriptor+=key_bytes;body+=entry.encoded.size();
    exchange.keys_.push_back(entry.key);
  }
  require_unique(exchange.keys_);
  exchange.request_digest_=record_digest(bytes);
  exchange.request_=std::make_shared<const WorkerRequestSnapshot>(std::move(bytes));
  return exchange;
}

NativeLz4ReadExchange NativeLz4ReadExchange::prepare_captured(std::span<const NativeLz4CapturedFrame> frames,
    std::uint64_t id,NativeLz4DecodeGrant grant) {
  if(frames.empty() || frames.size()>grant.maximum_frames)
    invalid("native LZ4 captured request admission");
  std::vector<NativeLz4DecodeSource> sources;
  sources.reserve(frames.size());
  for(const auto& frame:frames)sources.push_back({frame.key(),frame.encoded()});
  auto request=prepare(sources,id,grant);
  request.stored_digests_.reserve(frames.size());
  for(const auto& frame:frames)request.stored_digests_.push_back(frame.stored_digest_);
  return request;
}

NativeLz4WireRequest decode_native_lz4_request(View bytes,NativeLz4DecodeGrant grant,
    std::uint32_t maximum_frames_per_batch){
  require_header(bytes,request_magic);
  const auto count=static_cast<std::size_t>(get(bytes,16,4));
  if(!maximum_frames_per_batch || count>grant.maximum_frames || get(bytes,20,4) ||
      field<32>(bytes,32)!=Digest{} || count>(bytes.size()-header_bytes)/key_bytes)
    invalid("native LZ4 worker request bounds");
  std::size_t cursor=header_bytes+key_bytes*count;
  auto room=grant.maximum_slab_bytes;
  NativeLz4WireRequest request;
  request.id=get(bytes,8,8);
  request.reply_bytes=header_bytes;
  request.request_digest=record_digest(bytes);
  request.keys.reserve(count);
  std::vector<NativeLz4DecodeSource> frames;
  frames.reserve((std::min)(count,std::size_t(maximum_frames_per_batch)));
  for(std::size_t i=0;i<count;++i){
    const auto descriptor=bytes.subspan(header_bytes+i*key_bytes,key_bytes);
    const auto key=read_key(descriptor);
    const auto encoded=get(descriptor,36,4);
    if(!valid_key(key) || !encoded || encoded>=key.decoded_bytes || encoded>bytes.size()-cursor)
      invalid("native LZ4 request frame extent");
    frames.push_back({key,bytes.subspan(cursor,static_cast<std::size_t>(encoded))});
    request.keys.push_back(key);
    request.reply_bytes=add(request.reply_bytes,key.decoded_bytes);
    cursor+=static_cast<std::size_t>(encoded);
    if(frames.size()==maximum_frames_per_batch || i+1==count){
      auto batch=NativeLz4DecodeBatch::prepare(frames,{maximum_frames_per_batch,room});
      room-=batch.slab_bytes();
      request.batches.push_back(std::move(batch));
      frames.clear();
    }
  }
  if(cursor!=bytes.size())invalid("native LZ4 request trailing bytes");
  require_unique(request.keys);
  return request;
}

Bytes encode_native_lz4_reply(const NativeLz4WireRequest& request,
    std::span<const NativeLz4DecodedFrame> frames,WorkerExecution backend){
  if(!request.id || frames.empty() || frames.size()!=request.keys.size() || !valid_backend(backend))
    invalid("invalid native LZ4 completion identity");
  auto size=header_bytes;
  for(std::size_t i=0;i<frames.size();++i){
    const auto& frame=frames[i];
    if(frame.key!=request.keys[i] || frame.bytes.size()!=frame.key.decoded_bytes)
      invalid("native LZ4 completion key or extent");
    size=add(size,frame.bytes.size());
  }
  if(size!=request.reply_bytes)invalid("native LZ4 completion wire length");
  Bytes result(size);
  write_header(result,reply_magic,request.id,frames.size(),static_cast<std::uint32_t>(backend),request.request_digest);
  std::size_t at=header_bytes;
  for(const auto& frame:frames){
    std::ranges::copy(frame.bytes,result.begin()+static_cast<std::ptrdiff_t>(at));
    at+=frame.bytes.size();
  }
  return result;
}

std::unique_ptr<NativeLz4ReadyReads> NativeLz4ReadExchange::accept(View bytes,WorkerExecution expected) const {
  require_header(bytes,reply_magic);
  if(!request_ || !valid_backend(expected) || bytes.size()!=reply_bytes_ || get(bytes,8,8)!=id_ ||
      get(bytes,16,4)!=keys_.size() || get(bytes,20,4)!=static_cast<std::uint32_t>(expected) ||
      field<32>(bytes,32)!=request_digest_)invalid("native LZ4 reply binding");
  auto ready=std::make_unique<NativeLz4ReadyReads>();
  ready->frames_.reserve(keys_.size());
  auto payload=bytes.subspan(header_bytes);
  for(std::size_t i=0;i<keys_.size();++i){
    const auto& key=keys_[i];
    const auto data=payload.first(key.decoded_bytes);
    if(record_digest(data)!=key.decoded_digest)invalid("native LZ4 reply digest");
    const auto stored=stored_digests_.empty()?std::optional<Digest>{}:stored_digests_[i];
    ready->frames_.push_back({key,{data.begin(),data.end()},stored});
    payload=payload.subspan(data.size());
  }
  std::ranges::sort(ready->frames_,{},&NativeLz4ReadyReads::Frame::key);
  return ready;
}

std::unique_ptr<NativeLz4ReadyReads> NativeLz4ReadyReads::decode_captured(const NativeLz4CapturedFrame& frame) {
  return decode_captured({&frame,1});
}

std::unique_ptr<NativeLz4ReadyReads> NativeLz4ReadyReads::decode_captured(
    std::span<const NativeLz4CapturedFrame> frames,const std::function<bool()>& interrupted) {
  if(frames.empty())return {};
  StoreCodecDecoder decoder;
  auto result=std::make_unique<NativeLz4ReadyReads>();
  result->frames_.reserve(frames.size());
  for(const auto& frame:frames) {
    if(interrupted && interrupted())return {};
    auto decoded=decoder.decode(kStoreLz4,frame.encoded(),frame.key().decoded_bytes);
    if(record_digest(decoded)!=frame.key().decoded_digest)invalid("captured native LZ4 decoded digest");
    result->frames_.push_back({frame.key(),std::move(decoded),frame.stored_digest_});
  }
  std::ranges::sort(result->frames_,{},&Frame::key);
  if(std::ranges::adjacent_find(result->frames_,{},&Frame::key)!=result->frames_.end())
    invalid("duplicate captured native LZ4 frame");
  return result;
}

std::optional<Bytes> NativeLz4ReadyReads::try_ready(const NativeLz4ReadKey& key,View) noexcept {
  if(rejected_)return {};
  const auto match=std::ranges::lower_bound(frames_,key,{},&Frame::key);
  if(match==frames_.end() || match->key!=key || match->bytes.empty())return {};
  return consume(*match);
}
std::optional<Bytes> NativeLz4ReadyReads::try_indexed(const StoreIdentity& image,std::uint64_t object,
    std::uint64_t generation,std::uint32_t size,const Digest& stored_digest) noexcept {
  if(rejected_)return {};
  const NativeLz4ReadKey prefix{image,object,generation,size,{}};
  const auto match=std::ranges::lower_bound(frames_,prefix,{},&Frame::key);
  if(match==frames_.end() || match->key.image!=image || match->key.object!=object ||
      match->key.generation!=generation || match->key.decoded_bytes!=size ||
      match->stored_digest!=stored_digest || match->bytes.empty())return {};
  if(match->bytes.size()!=size || record_digest(match->bytes)!=match->key.decoded_digest) {
    reject(match->key);
    return {};
  }
  ++indexed_consumed_;
  return consume(*match);
}
Bytes NativeLz4ReadyReads::consume(Frame& frame) noexcept {
  ++consumed_;
  return std::exchange(frame.bytes,Bytes{});
}
void NativeLz4ReadyReads::reject(const NativeLz4ReadKey&) noexcept {rejected_=true;}
}
