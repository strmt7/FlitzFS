#include "flitzfs/gpu_lz4_slab.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
[[noreturn]] void invalid(const char* why){throw std::invalid_argument(why);}
const GpuLz4DeviceLayout& admit(const PreparedLz4WorkerBatch& batch,std::uint64_t address) {
  const auto& layout=batch.device_layout(); // Refuses moved/empty ownership.
  if(!address || (address&7U) || layout.total_bytes>gpu_detail::max_batch_bytes ||
      address>(std::numeric_limits<std::uint64_t>::max)()-layout.total_bytes)
    invalid("LZ4 slab device address or allocation");
  return layout;
}
void put64(Bytes& bytes,std::uint32_t at,std::uint64_t value) noexcept {
  for(unsigned i=0;i<8;++i)bytes[at+i]=static_cast<std::byte>((value>>(8U*i))&255U);
}
std::uint64_t get64(Lz4WorkerBytes bytes,std::uint32_t at) noexcept {
  std::uint64_t value=0;
  for(unsigned i=0;i<8;++i)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(8U*i);
  return value;
}
void protected_bytes(const PreparedLz4WorkerBatch& batch,std::uint64_t address,
    Lz4WorkerBytes observed,const GpuLz4DeviceLayout& layout) {
  // The only mutable extents are the size table, output capacities and scratch.
  // Everything else includes input bytes, pointer/size tables, padding and ALL
  // 32-byte guards. Rebuild the canonical upload instead of trusting readback.
  const auto expected=gpu_lz4_slab_upload(batch,address);
  std::size_t cursor=0;
  const auto skip=[&](std::uint32_t start,std::uint32_t bytes) {
    if(start<cursor || start>observed.size() || bytes>observed.size()-start)
      invalid("LZ4 slab internal mutable extent");
    if(!std::equal(expected.begin()+cursor,expected.begin()+start,observed.begin()+cursor))
      invalid("LZ4 slab protected bytes changed");
    cursor=static_cast<std::size_t>(start)+bytes;
  };
  skip(layout.output_sizes,layout.count*8U);
  for(std::uint32_t i=0;i<layout.count;++i)
    skip(layout.chunks[i].output_offset,layout.chunks[i].output_capacity);
  skip(layout.scratch_offset,layout.scratch_bytes);
  if(!std::equal(expected.begin()+cursor,expected.end(),observed.begin()+cursor))
    invalid("LZ4 slab protected tail changed");
}
}
Bytes gpu_lz4_slab_upload(const PreparedLz4WorkerBatch& batch,std::uint64_t address) {
  const auto& layout=admit(batch,address); // All arithmetic before allocation.
  Bytes result(layout.total_bytes,std::byte{0});
  for(std::uint32_t i=0;i<layout.count;++i) {
    const auto& chunk=layout.chunks[i];
    put64(result,layout.input_pointers+8U*i,address+chunk.input_offset);
    put64(result,layout.input_sizes+8U*i,chunk.input_bytes);
    put64(result,layout.output_pointers+8U*i,address+chunk.output_offset);
    const auto input=batch.input(i);
    std::copy(input.begin(),input.end(),result.begin()+chunk.input_offset);
    const auto guard=chunk.output_offset+chunk.output_capacity;
    std::fill_n(result.begin()+guard,gpu_lz4_detail::output_guard_bytes,std::byte{0xa5});
  }
  return result;
}
std::vector<Bytes> verify_gpu_lz4_slab(const PreparedLz4WorkerBatch& batch,
    std::uint64_t address,Lz4WorkerBytes observed) {
  const auto& layout=admit(batch,address);
  if(observed.size()!=layout.total_bytes)invalid("LZ4 slab exact readback size");
  std::array<Lz4WorkerBytes,8> candidates{};
  std::size_t largest=0;
  for(std::uint32_t i=0;i<layout.count;++i) {
    const auto& chunk=layout.chunks[i];
    const auto size=get64(observed,layout.output_sizes+8U*i);
    if(!size || size>chunk.output_capacity)invalid("LZ4 slab unwritten or excessive result size");
    candidates[i]=observed.subspan(chunk.output_offset,static_cast<std::size_t>(size));
    largest=(std::max)(largest,static_cast<std::size_t>(chunk.input_bytes));
  }
  protected_bytes(batch,address,observed,layout); // Temporary upload retired here.
  if(!gpu_lz4_cpu_verification_available())
    throw std::runtime_error("independent CPU LZ4 verification unavailable");
  Bytes scratch(largest);
  for(std::uint32_t i=0;i<layout.count;++i)
    if(verify_gpu_lz4_candidate(batch.input(i),candidates[i],
          std::span(scratch).first(layout.chunks[i].input_bytes))!=GpuLz4Verification::verified)
      invalid("LZ4 slab candidate does not reproduce immutable source");
  std::vector<Bytes> result;result.reserve(layout.count);
  for(std::uint32_t i=0;i<layout.count;++i)
    result.emplace_back(candidates[i].begin(),candidates[i].end());
  return result;
}
} // namespace flitzfs
