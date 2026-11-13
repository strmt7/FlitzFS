#pragma once
#include "flitzfs/gpu_protocol.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace flitzfs {
// Experimental host-only metadata for the pinned byte-oriented hipCOMP candidate.
// No runtime, launch, wire ABI, stored codec or proportional allocation is added.
struct GpuLz4ChunkLayout {
  std::uint32_t input_offset=0,input_bytes=0,output_offset=0,output_capacity=0;
};
struct GpuLz4DeviceLayout {
  std::array<GpuLz4ChunkLayout,8> chunks{};
  std::uint32_t count=0,input_bytes=0,input_pointers=0,input_sizes=0;
  std::uint32_t output_pointers=0,output_sizes=0,scratch_offset=0;
  std::uint32_t hash_table_entries=0,scratch_bytes=0,total_bytes=0;
};
namespace gpu_lz4_detail {
// The selected candidate's AMD64 size_t and device-pointer tables use 8-byte
// entries. This layout does not serialize a host pointer or invoke the SDK.
inline constexpr std::uint32_t table_entry_bytes=8;
inline constexpr std::uint32_t hash_entry_limit=16384;
inline constexpr std::uint32_t output_guard_bytes=32;
inline constexpr std::uint32_t output_capacity(std::uint32_t size) noexcept {
  if(!size || size>gpu_detail::max_block_bytes)return 0;
  const auto maximum=size+1U+(size+254U)/255U;
  return (maximum+7U)&~7U;
}
inline std::uint32_t append(std::uint32_t& cursor,std::uint32_t bytes) {
  if(cursor>gpu_detail::max_batch_bytes)
    throw std::invalid_argument("GPU LZ4 invalid allocation cursor");
  const auto start=(cursor+7U)&~7U;
  if(start>gpu_detail::max_batch_bytes || bytes>gpu_detail::max_batch_bytes-start)
    throw std::invalid_argument("GPU LZ4 explicit allocation cap");
  cursor=start+bytes;
  return start;
}
}
// Caller must still obtain current resource admission and own immutable input.
// Unsupported shapes fail before any source copy or device/runtime operation.
inline GpuLz4DeviceLayout gpu_lz4_device_layout(std::span<const std::uint32_t> sizes) {
  if(sizes.empty() || sizes.size()>8)
    throw std::invalid_argument("GPU LZ4 chunk count");
  GpuLz4DeviceLayout result;
  result.count=static_cast<std::uint32_t>(sizes.size());
  std::uint32_t largest=0;
  for(const auto size:sizes) {
    if(!size || size>gpu_detail::max_block_bytes ||
        size>gpu_detail::max_batch_bytes-result.input_bytes)
      throw std::invalid_argument("GPU LZ4 input cap");
    result.input_bytes+=size;
    if(size>largest)largest=size;
  }
  // These bounded operands cannot overflow. Upstream rounds to a power of two,
  // caps at 16384 entries, then uses uint16 offsets and one table per chunk.
  std::uint32_t entries=1;
  while(entries<largest && entries<gpu_lz4_detail::hash_entry_limit)entries*=2;
  result.hash_table_entries=entries;
  result.scratch_bytes=entries*2U*result.count;
  std::uint32_t cursor=0;
  const auto table_bytes=result.count*gpu_lz4_detail::table_entry_bytes;
  result.input_pointers=gpu_lz4_detail::append(cursor,table_bytes);
  result.input_sizes=gpu_lz4_detail::append(cursor,table_bytes);
  result.output_pointers=gpu_lz4_detail::append(cursor,table_bytes);
  result.output_sizes=gpu_lz4_detail::append(cursor,table_bytes);
  for(std::uint32_t i=0;i<result.count;++i) {
    auto& chunk=result.chunks[i];
    chunk.input_bytes=sizes[i];
    chunk.input_offset=gpu_lz4_detail::append(cursor,chunk.input_bytes);
    // Pinned maxSizeOfStream: roundUp(size+1+ceil(size/255),sizeof(size_t)).
    chunk.output_capacity=gpu_lz4_detail::output_capacity(chunk.input_bytes);
    chunk.output_offset=gpu_lz4_detail::append(cursor,
        chunk.output_capacity+gpu_lz4_detail::output_guard_bytes);
  }
  result.scratch_offset=gpu_lz4_detail::append(cursor,result.scratch_bytes);
  result.total_bytes=cursor;
  return result;
}
} // namespace flitzfs
