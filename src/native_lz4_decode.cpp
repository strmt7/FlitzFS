#include "native_lz4_decode.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace flitzfs::detail {
namespace {
using namespace native_lz4_decode;
constexpr std::byte guard{0xa5};
[[noreturn]] void fail(const char* why) { throw std::invalid_argument(why); }
u32 aligned(u32 bytes) noexcept {
  return (bytes+alignment-1)&~(alignment-1);
}
Record descriptor(std::span<const std::byte> data,std::size_t index) noexcept {
  Record entry{};
  std::memcpy(&entry,data.data()+index*sizeof(Record),sizeof(entry));
  return entry;
}
}

NativeLz4DecodeBatch NativeLz4DecodeBatch::prepare(std::span<const NativeLz4DecodeSource> entries,
    NativeLz4DecodeGrant grant) {
  if(!store_lz4_available() || entries.empty() || entries.size()>grant.maximum_frames)
    fail("native LZ4 decode frame admission");
  u32 stored=0,logical=0;
  for(const auto& entry:entries) {
    const auto& key=entry.key;
    if(!key.object || !key.generation || key.image==StoreIdentity{} ||
        key.decoded_bytes>kStoreMaxObjectBytes || entry.encoded.size()>=key.decoded_bytes ||
        !valid_block(reinterpret_cast<const u8*>(entry.encoded.data()),
            static_cast<u32>(entry.encoded.size()),key.decoded_bytes))
      fail("invalid native LZ4 decode input");
    stored=(std::max)(stored,static_cast<u32>(entry.encoded.size()));
    logical=(std::max)(logical,key.decoded_bytes);
  }
  NativeLz4DecodeBatch batch;
  batch.layout_={static_cast<u32>(entries.size()),aligned(stored),aligned(logical)+alignment,version};
  const auto bytes=output_at(batch.layout_,batch.layout_.chunks);
  if(bytes>grant.maximum_slab_bytes || bytes>(std::numeric_limits<std::size_t>::max)() ||
      !valid_layout(batch.layout_,bytes))fail("native LZ4 decode slab admission");
  batch.upload_.assign(static_cast<std::size_t>(bytes),guard);
  batch.keys_.reserve(entries.size());
  for(u32 index=0;index<batch.layout_.chunks;++index) {
    const auto& entry=entries[index];
    const Record shape{static_cast<u32>(entry.encoded.size()),entry.key.decoded_bytes,0,pending};
    std::memcpy(batch.upload_.data()+std::size_t(index)*sizeof(shape),&shape,sizeof(shape));
    std::ranges::copy(entry.encoded,batch.upload_.begin()+static_cast<std::ptrdiff_t>(input_at(batch.layout_,index)));
    batch.keys_.push_back(entry.key);
  }
  return batch;
}

std::vector<NativeLz4DecodedFrame> NativeLz4DecodeBatch::verify(std::span<const std::byte> slab) const {
  if(slab.size()!=upload_.size() || !valid_layout(layout_,slab.size()))
    fail("native LZ4 readback extent");
  std::vector<NativeLz4DecodedFrame> frames;
  frames.reserve(keys_.size());
  for(u32 index=0;index<layout_.chunks;++index) {
    const auto result=descriptor(slab,index),expected=descriptor(upload_,index);
    if(result.input_bytes!=expected.input_bytes || result.output_bytes!=expected.output_bytes ||
        result.actual_bytes!=expected.output_bytes || result.status)
      fail("native LZ4 readback descriptor");
    const auto out=slab.subspan(static_cast<std::size_t>(output_at(layout_,index)),layout_.output_stride);
    const auto data=out.first(expected.output_bytes);
    if(!std::ranges::all_of(out.subspan(data.size()),[](auto b){return b==guard;}) ||
        record_digest(data)!=keys_[index].decoded_digest)
      fail("native LZ4 readback guard or digest");
    frames.push_back({keys_[index],{data.begin(),data.end()}});
  }
  return frames;
}
}
