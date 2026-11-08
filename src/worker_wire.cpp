#include "flitzfs/worker_wire.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace flitzfs {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::array<std::byte,8> request_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'Z'},std::byte{'R'},
    std::byte{'E'},std::byte{'Q'},std::byte{0},std::byte{0}};
constexpr std::array<std::byte,8> reply_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'Z'},std::byte{'R'},
    std::byte{'S'},std::byte{'P'},std::byte{0},std::byte{0}};
[[noreturn]] void invalid(const char* why) { throw std::invalid_argument(why); }
void put(Bytes& out, std::uint64_t n, unsigned width) {
  for (unsigned i=0;i<width;++i) out.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
struct Cursor {
  std::span<const std::byte> bytes;
  std::span<const std::byte> take(std::size_t count) {
    if (count>bytes.size()) invalid("truncated worker frame");
    const auto result=bytes.first(count);bytes=bytes.subspan(count);return result;
  }
  std::uint64_t number(unsigned width) {
    const auto in=take(width);std::uint64_t result=0;
    for (unsigned i=0;i<width;++i) result|=std::uint64_t(std::to_integer<unsigned>(in[i]))<<(8*i);
    return result;
  }
};
void header(std::span<const std::byte> frame, std::size_t maximum,
    const std::array<std::byte,8>& magic) {
  if (frame.size()<kWorkerHeaderBytes || frame.size()>maximum ||
      !std::equal(magic.begin(),magic.end(),frame.begin())) invalid("invalid worker frame header");
}
void backend(WorkerExecution execution) {
  if (execution!=WorkerExecution::cpu_semantics && execution!=WorkerExecution::hip)
    invalid("unknown worker execution backend");
}
}
std::vector<std::byte> encode_worker_request(const PreparedGpuBatch& batch, std::uint64_t id) {
  if (!id || batch.tasks().empty() || batch.tasks().size()>gpu_detail::max_tasks ||
      batch.payload().size()>gpu_detail::max_batch_bytes || batch.cpu_reference().empty() ||
      batch.cpu_reference().size()>gpu_detail::max_batch_bytes) invalid("invalid worker request batch");
  std::array<const gpu_detail::Task*,kGpuMaxBlocks> blocks{};
  std::size_t count=0;
  for (const auto& task:batch.tasks()) if (!task.segment_offset) {
    if (count==blocks.size()) invalid("worker block count exceeds bound");
    blocks[count++]=&task;
  }
  if (!count) invalid("worker batch contains no blocks");
  Bytes result;result.reserve(kWorkerHeaderBytes+count*kWorkerRecordBytes+batch.payload().size());
  result.insert(result.end(),request_magic.begin(),request_magic.end());
  put(result,id,8);put(result,count,4);put(result,batch.payload().size(),4);
  put(result,batch.cpu_reference().size(),4);put(result,0,4);
  for (std::size_t i=0;i<count;++i) {
    const auto& b=*blocks[i];
    put(result,b.kind,1);put(result,b.fill,1);put(result,0,2);
    put(result,b.decoded_bytes,4);put(result,b.input_bytes,4);
  }
  result.insert(result.end(),batch.payload().begin(),batch.payload().end());
  return result;
}
WorkerBatchRequest decode_worker_request(std::span<const std::byte> frame) {
  header(frame,kWorkerRequestLimit,request_magic);Cursor cursor{frame.subspan(8)};
  const auto id=cursor.number(8), count=cursor.number(4), encoded=cursor.number(4), decoded=cursor.number(4);
  if (!id || !count || count>kGpuMaxBlocks || encoded>gpu_detail::max_batch_bytes ||
      !decoded || decoded>gpu_detail::max_batch_bytes || cursor.number(4)) invalid("invalid worker request bounds");
  const auto table_size=static_cast<std::size_t>(count)*kWorkerRecordBytes;
  if (cursor.bytes.size()!=table_size+encoded) invalid("worker request extent mismatch");
  Cursor records{cursor.take(table_size)};
  const auto payload=cursor.bytes;
  std::array<GpuBlockView,kGpuMaxBlocks> views{};
  std::size_t input=0,output=0;
  for (std::size_t i=0;i<count;++i) {
    auto& view=views[i];view.kind=static_cast<std::uint8_t>(records.number(1));
    view.fill=static_cast<std::uint8_t>(records.number(1));
    if (records.number(2)) invalid("worker block reserved field is not zero");
    const auto size=records.number(4), stored=records.number(4);
    if (!size || size>gpu_detail::max_block_bytes || size>decoded-output ||
        stored>payload.size()-input) invalid("invalid worker block extent");
    view.decoded_bytes=static_cast<std::uint32_t>(size);
    view.payload=payload.subspan(input,static_cast<std::size_t>(stored));
    input+=static_cast<std::size_t>(stored);output+=static_cast<std::size_t>(size);
  }
  if (input!=encoded || output!=decoded) invalid("worker block totals mismatch");
  return {id,PreparedGpuBatch::prepare(std::span<const GpuBlockView>(views).first(static_cast<std::size_t>(count)))};
}
std::vector<std::byte> encode_worker_reply(const GpuBatchReply& reply, WorkerExecution execution) {
  backend(execution);
  if (!reply.generation || reply.status.empty() || reply.status.size()>gpu_detail::max_tasks ||
      reply.bytes.empty() || reply.bytes.size()>gpu_detail::max_batch_bytes) invalid("invalid worker reply bounds");
  Bytes result;result.reserve(kWorkerHeaderBytes+reply.status.size()*4+reply.bytes.size());
  result.insert(result.end(),reply_magic.begin(),reply_magic.end());
  put(result,reply.generation,8);put(result,static_cast<std::uint32_t>(execution),4);
  put(result,reply.status.size(),4);put(result,reply.bytes.size(),4);put(result,0,4);
  for (const auto status:reply.status) put(result,status,4);
  result.insert(result.end(),reply.bytes.begin(),reply.bytes.end());return result;
}
GpuBatchReply accept_worker_reply(std::span<const std::byte> frame, const PreparedGpuBatch& expected,
    std::uint64_t id, WorkerExecution execution) {
  backend(execution);header(frame,kWorkerReplyLimit,reply_magic);
  if (!id || expected.tasks().empty() || expected.tasks().size()>gpu_detail::max_tasks ||
      expected.cpu_reference().empty() || expected.cpu_reference().size()>gpu_detail::max_batch_bytes)
    invalid("invalid expected worker reply");
  Cursor cursor{frame.subspan(8)};
  if (cursor.number(8)!=id) invalid("stale worker reply");
  if (cursor.number(4)!=static_cast<std::uint32_t>(execution)) invalid("worker backend mismatch");
  const auto tasks=cursor.number(4), size=cursor.number(4);
  if (tasks!=expected.tasks().size() || size!=expected.cpu_reference().size() || cursor.number(4) ||
      cursor.bytes.size()!=tasks*4+size) invalid("worker reply extent mismatch");
  for (std::size_t i=0;i<tasks;++i)
    if (cursor.number(4)!=gpu_detail::success) invalid("worker task failed or incomplete");
  if (!std::equal(cursor.bytes.begin(),cursor.bytes.end(),expected.cpu_reference().begin()))
    invalid("worker output disagrees with independent CPU reference");
  GpuBatchReply result;result.generation=id;
  result.status.assign(static_cast<std::size_t>(tasks),gpu_detail::success);
  result.bytes.assign(cursor.bytes.begin(),cursor.bytes.end());return result;
}
} // namespace flitzfs
