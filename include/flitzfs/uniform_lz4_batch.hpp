#pragma once
#include "flitzfs/gpu_lz4_window.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"

namespace flitzfs {

struct UniformLz4BatchInput {
  std::uint64_t request_id=0;
  UniformLz4Bytes source;
};
struct UniformLz4BatchWindow {
  GpuLz4WindowPlan window;
  std::size_t requests=0;
};
// Consecutive independent requests in submission order. Only the final
// request can have a partial chunk. Select a complete prefix using the same
// live headroom and kernel limits as a single finite sweep.
UniformLz4BatchWindow plan_uniform_lz4_batch(std::span<const UniformLz4BatchInput>,std::uint32_t chunk_bytes,
    const GpuHealthEvidence&,std::uint64_t now,const GpuLz4ResidentCost&,GpuLz4WindowPolicy,
    GpuLz4SweepGrant,std::uint32_t deadline_ms) noexcept;

struct UniformLz4BatchMember {
  std::uint64_t request_id=0;
  std::uint32_t first_chunk=0,chunks=0;
  std::size_t source_bytes=0;
  bool operator==(const UniformLz4BatchMember&) const=default;
};
// Own an immutable ordinary wire source with the original request boundaries
// retained by the parent. The worker executes the existing serial sweep.
class PreparedUniformLz4Batch final {
 public:
  static PreparedUniformLz4Batch prepare(std::span<const UniformLz4BatchInput>,const UniformLz4BatchWindow&);
  // Own an exact layout already funded by the caller, including calibration
  // before workload-specific device costs are known.
  static PreparedUniformLz4Batch prepare(std::span<const UniformLz4BatchInput>,const uniform_lz4::Layout&);
  const PreparedUniformLz4& source() const noexcept{return source_;}
  std::span<const UniformLz4BatchMember> members() const noexcept{return members_;}
  UniformLz4Bytes input(std::size_t member) const;
  PreparedUniformLz4 prepare_member(std::size_t member) const;
 private:
  PreparedUniformLz4Batch(PreparedUniformLz4&&,std::vector<UniformLz4BatchMember>&&) noexcept;
  PreparedUniformLz4 source_;
  std::vector<UniformLz4BatchMember> members_;
};

struct UniformLz4BatchReply {
  std::uint64_t request_id=0;
  std::span<const std::uint32_t> sizes;
  UniformLz4Bytes payload;
};
class VerifiedUniformLz4Batch final {
 public:
  // Call only after the supervisor has confirmed successful child retirement.
  // All bytes and the combined exchange identity must verify before any
  // individual result becomes available.
  static VerifiedUniformLz4Batch accept(UniformLz4Bytes,const PreparedUniformLz4Batch&,
      std::uint64_t exchange_id,WorkerExecution);
  // Each frame must match the exact member and its original request identity.
  // A complete sequence is verified before exposing any individual candidate.
  static VerifiedUniformLz4Batch accept_members(std::span<const UniformLz4Bytes>,
      const PreparedUniformLz4Batch&,WorkerExecution);
  std::size_t size() const noexcept{return blocks_.size();}
  bool matches(const PreparedUniformLz4Batch&) const noexcept;
  UniformLz4BatchReply reply(std::size_t member) const;
 private:
  struct Block {
    UniformLz4BatchMember member;
    std::size_t compressed_offset=0,compressed_bytes=0;
  };
  friend UniformLz4BatchWindow plan_uniform_lz4_batch(std::span<const UniformLz4BatchInput>,std::uint32_t,
      const GpuHealthEvidence&,std::uint64_t,const GpuLz4ResidentCost&,GpuLz4WindowPolicy,
      GpuLz4SweepGrant,std::uint32_t) noexcept;
  static VerifiedUniformLz4Batch bind(const PreparedUniformLz4Batch&,UniformLz4Candidates&&);
  UniformLz4Limits limits_{};
  std::array<std::byte,32> input_digest_{};
  UniformLz4Candidates candidates_;
  std::vector<Block> blocks_;
};

}
