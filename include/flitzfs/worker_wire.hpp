#pragma once
#include "flitzfs/gpu_batch.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace flitzfs {
// A private request can be shared with the supervisor for its whole lifetime.
// Transfer only privately owned bytes with no outstanding writable aliases.
class WorkerRequestSnapshot final {
 public:
  explicit WorkerRequestSnapshot(std::span<const std::byte> bytes):bytes_(bytes.begin(),bytes.end()) {}
  explicit WorkerRequestSnapshot(std::vector<std::byte>&& bytes) noexcept:bytes_(std::move(bytes)) {}
  WorkerRequestSnapshot(const WorkerRequestSnapshot&)=delete;
  WorkerRequestSnapshot& operator=(const WorkerRequestSnapshot&)=delete;
  std::span<const std::byte> bytes() const & noexcept {return bytes_;}
  std::span<const std::byte> bytes() const && = delete;
 private:
  const std::vector<std::byte> bytes_;
};
using SharedWorkerRequest=std::shared_ptr<const WorkerRequestSnapshot>;

// Fixed, bounded IPC frames. No paths, pointers, native structs or CPU-reference
// bytes cross this boundary. The helper rebuilds and validates its own batch.
inline constexpr std::size_t kWorkerHeaderBytes = 32;
inline constexpr std::size_t kWorkerRecordBytes = 12;
inline constexpr std::size_t kWorkerRequestLimit = kWorkerHeaderBytes +
    kGpuMaxBlocks * kWorkerRecordBytes + gpu_detail::max_batch_bytes;
inline constexpr std::size_t kWorkerReplyLimit = kWorkerHeaderBytes +
    gpu_detail::max_tasks * 4 + gpu_detail::max_batch_bytes;
enum class WorkerExecution : std::uint32_t { cpu_semantics = 1, hip = 2 };
struct WorkerBatchRequest {
  std::uint64_t request_id;
  PreparedGpuBatch batch;
};
std::vector<std::byte> encode_worker_request(const PreparedGpuBatch& batch,
    std::uint64_t request_id);
WorkerBatchRequest decode_worker_request(std::span<const std::byte> frame);
std::vector<std::byte> encode_worker_reply(const GpuBatchReply& reply,
    WorkerExecution execution);
// Reject wrong request/backend, truncation, trailing data, pending/failed tasks,
// and unequal output BEFORE returning any candidate bytes. Backend tags are
// declarations by a trusted helper, not attestation of GPU execution.
GpuBatchReply accept_worker_reply(std::span<const std::byte> frame,
    const PreparedGpuBatch& expected, std::uint64_t request_id,
    WorkerExecution execution);
} // namespace flitzfs
