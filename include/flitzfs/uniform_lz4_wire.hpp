#pragma once
#include "flitzfs/gpu_lz4_uniform.hpp"
#include "flitzfs/worker_wire.hpp"
#include <array>
#include <span>
#include <vector>
namespace flitzfs {
using UniformLz4Bytes=std::span<const std::byte>;
inline constexpr std::size_t kUniformLz4HeaderBytes=80;
// Exact capacities derived from one caller-admitted window. Representation
// bounds are transport constraints, not product allocation/operation ceilings.
struct UniformLz4Limits {
  std::uint64_t input_bytes=0,device_bytes=0,request_bytes=0,reply_bytes=0;
  std::uint32_t chunk_bytes=0,chunks=0;
  bool operator==(const UniformLz4Limits&) const=default;
};
UniformLz4Limits uniform_lz4_limits(const uniform_lz4::Layout&);
bool valid_uniform_lz4_limits(const UniformLz4Limits&) noexcept;
class PreparedUniformLz4 final {
 public:
  static PreparedUniformLz4 prepare(UniformLz4Bytes,const UniformLz4Limits&);
  PreparedUniformLz4(const PreparedUniformLz4&)=delete;
  PreparedUniformLz4& operator=(const PreparedUniformLz4&)=delete;
  PreparedUniformLz4(PreparedUniformLz4&&) noexcept;
  PreparedUniformLz4& operator=(PreparedUniformLz4&&) noexcept;
  bool ready() const noexcept;
  const uniform_lz4::Layout& layout() const noexcept{return layout_;}
  const UniformLz4Limits& limits() const noexcept{return limits_;}
  UniformLz4Bytes plaintext() const noexcept{return source_;}
  UniformLz4Bytes input(std::uint32_t) const;
  const std::array<std::byte,32>& digest() const noexcept{return digest_;}
 private:
  PreparedUniformLz4()=default;
  void swap(PreparedUniformLz4&) noexcept;
  uniform_lz4::Layout layout_{};
  UniformLz4Limits limits_{};
  std::vector<std::byte> source_;
  std::array<std::byte,32> digest_{};
};
struct UniformLz4Request {std::uint64_t request_id;PreparedUniformLz4 batch;};
// Untrusted until independently verified. Dense payload, one size per chunk;
// no thousands of individually allocated block vectors.
struct UniformLz4Candidates {
  std::vector<std::byte> payload;
  std::vector<std::uint32_t> sizes;
};
std::vector<std::byte> encode_uniform_lz4_request(const PreparedUniformLz4&,std::uint64_t);
// Inspect an already owned immutable request against independently supplied
// channel capacities, without allocating or trusting shared metadata.
UniformLz4Limits inspect_uniform_lz4_request(UniformLz4Bytes,std::uint64_t request_capacity,
    std::uint64_t reply_capacity);
UniformLz4Request decode_uniform_lz4_request(UniformLz4Bytes,const UniformLz4Limits&);
void verify_uniform_lz4_candidates(const PreparedUniformLz4&,const UniformLz4Candidates&);
std::vector<std::byte> encode_uniform_lz4_reply(const PreparedUniformLz4&,std::uint64_t,
    const UniformLz4Candidates&,WorkerExecution);
void verify_uniform_lz4_reply(UniformLz4Bytes,const PreparedUniformLz4&,
    std::uint64_t,WorkerExecution);
UniformLz4Candidates accept_uniform_lz4_reply(UniformLz4Bytes,const PreparedUniformLz4&,
    std::uint64_t,WorkerExecution);
// Same canonical shared layout as the typed device entry. Every output guard,
// immutable input and alignment gap is checked; scratch may change. No launch.
std::vector<std::byte> uniform_lz4_upload(const PreparedUniformLz4&);
UniformLz4Candidates verify_uniform_lz4_readback(const PreparedUniformLz4&,UniformLz4Bytes);
}
