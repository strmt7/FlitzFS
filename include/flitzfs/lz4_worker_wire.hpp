#pragma once
#include "flitzfs/gpu_lz4_layout.hpp"
#include "flitzfs/worker_wire.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace flitzfs {
using Lz4WorkerBytes=std::span<const std::byte>;
inline constexpr std::size_t kLz4WorkerHeaderBytes=64;
inline constexpr std::size_t kLz4WorkerRequestLimit=kLz4WorkerHeaderBytes+36+gpu_detail::max_batch_bytes;
inline constexpr std::size_t kLz4WorkerReplyLimit=kLz4WorkerHeaderBytes+64+gpu_detail::max_batch_bytes;
static_assert(kLz4WorkerRequestLimit<=kWorkerRequestLimit && kLz4WorkerReplyLimit<=kWorkerReplyLimit);
// Immutable CPU preparation only. No worker, runtime, device discovery or I/O.
// Input extents and the COMPLETE device layout are admitted before copying.
// One control thread; borrowed views expire at replacement/move/destruction.
// Caller frame/candidate spans remain valid and immutable for each entire call.
class PreparedLz4WorkerBatch final {
 public:
  static PreparedLz4WorkerBatch prepare(std::span<const Lz4WorkerBytes>);
  PreparedLz4WorkerBatch(const PreparedLz4WorkerBatch&)=default;
  PreparedLz4WorkerBatch& operator=(const PreparedLz4WorkerBatch&)=default;
  PreparedLz4WorkerBatch(PreparedLz4WorkerBatch&&) noexcept;
  PreparedLz4WorkerBatch& operator=(PreparedLz4WorkerBatch&&) noexcept;
  bool ready() const noexcept;
  std::uint32_t count() const noexcept;
  std::uint32_t input_bytes() const noexcept;
  Lz4WorkerBytes input(std::size_t index) const;
  Lz4WorkerBytes plaintext() const noexcept {return input_;}
  Lz4WorkerBytes canonical_sizes() const noexcept;
  const std::array<std::byte,32>& source_digest() const noexcept {return digest_;}
  const GpuLz4DeviceLayout& device_layout() const;
 private:
  PreparedLz4WorkerBatch()=default;
  void swap(PreparedLz4WorkerBatch&) noexcept;
  std::vector<std::byte> input_;
  std::array<std::byte,36> sizes_{};
  std::array<std::uint32_t,8> offsets_{};
  GpuLz4DeviceLayout layout_{};
  std::array<std::byte,32> digest_{};
};
struct Lz4WorkerRequest {std::uint64_t request_id;PreparedLz4WorkerBatch batch;};
std::vector<std::byte> encode_lz4_worker_request(const PreparedLz4WorkerBatch&,std::uint64_t);
Lz4WorkerRequest decode_lz4_worker_request(Lz4WorkerBytes);
// Both paths independently CPU-decode every block before copying candidate
// bytes. This does not authorize publication or a launch: callers still require
// successful exact child/supervisor retirement and fresh result/resource gates.
// Missing optional LZ4 rejects explicitly. Preflight its availability
// before any future worker launch. Backend tags do not attest GPU execution.
std::vector<std::byte> encode_lz4_worker_reply(const PreparedLz4WorkerBatch&,
    std::uint64_t,std::span<const Lz4WorkerBytes>,WorkerExecution);
std::vector<std::vector<std::byte>> accept_lz4_worker_reply(Lz4WorkerBytes,
    const PreparedLz4WorkerBatch&,std::uint64_t,WorkerExecution);
} // namespace flitzfs
