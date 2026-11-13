#include "flitzfs/lz4_worker_wire.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
constexpr std::array<std::byte,8> request_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'F'},std::byte{'S'},
    std::byte{'L'},std::byte{'R'},std::byte{'Q'},std::byte{1}};
constexpr std::array<std::byte,8> reply_magic{
    std::byte{'F'},std::byte{'L'},std::byte{'F'},std::byte{'S'},
    std::byte{'L'},std::byte{'R'},std::byte{'P'},std::byte{1}};
[[noreturn]] void invalid(const char* why){throw std::invalid_argument(why);}
void put(Bytes& frame,std::uint64_t value,unsigned width) {
  for(unsigned i=0;i<width;++i)frame.push_back(static_cast<std::byte>((value>>(8U*i))&255U));
}
std::uint32_t u32(Lz4WorkerBytes frame,std::size_t at) noexcept {
  std::uint32_t result=0;
  for(unsigned i=0;i<4;++i)result|=std::uint32_t(std::to_integer<unsigned>(frame[at+i]))<<(8U*i);
  return result;
}
std::uint64_t u64(Lz4WorkerBytes frame,std::size_t at) noexcept {
  std::uint64_t result=0;
  for(unsigned i=0;i<8;++i)result|=std::uint64_t(std::to_integer<unsigned>(frame[at+i]))<<(8U*i);
  return result;
}
void batch_id(const PreparedLz4WorkerBatch& batch,std::uint64_t id) {
  if(!id || !batch.ready())invalid("invalid LZ4 worker identity or owned batch");
}
void backend(WorkerExecution execution) {
  if(execution!=WorkerExecution::cpu_semantics && execution!=WorkerExecution::hip)
    invalid("unknown LZ4 worker backend");
}
void header(Lz4WorkerBytes frame,std::size_t limit,const std::array<std::byte,8>& magic) {
  if(frame.size()<kLz4WorkerHeaderBytes || frame.size()>limit ||
      !std::equal(magic.begin(),magic.end(),frame.begin()) || u32(frame,24)!=0)
    invalid("invalid LZ4 worker header");
}
Bytes make_header(const PreparedLz4WorkerBatch& batch,std::uint64_t id,
    std::uint32_t execution,bool reply,std::size_t total) {
  batch_id(batch,id);
  Bytes result;result.reserve(total);
  const auto& magic=reply?reply_magic:request_magic;
  result.insert(result.end(),magic.begin(),magic.end());
  put(result,id,8);put(result,batch.count(),4);put(result,batch.input_bytes(),4);
  put(result,0,4);put(result,execution,4);
  result.insert(result.end(),batch.source_digest().begin(),batch.source_digest().end());
  return result;
}
std::size_t verify_all(const PreparedLz4WorkerBatch& batch,std::span<const Lz4WorkerBytes> chunks) {
  if(!batch.ready() || chunks.size()!=batch.count())invalid("LZ4 candidate chunk count");
  std::size_t total=0,largest=0;
  const auto& layout=batch.device_layout();
  for(std::size_t i=0;i<chunks.size();++i) {
    if(chunks[i].empty() || chunks[i].size()>layout.chunks[i].output_capacity ||
        chunks[i].size()>gpu_detail::max_batch_bytes-total)invalid("LZ4 candidate extent");
    total+=chunks[i].size();
    largest=std::max(largest,batch.input(i).size());
  }
  if(!gpu_lz4_cpu_verification_available())
    throw std::runtime_error("independent CPU LZ4 verification unavailable");
  Bytes scratch(largest);
  for(std::size_t i=0;i<chunks.size();++i)
    if(verify_gpu_lz4_candidate(batch.input(i),chunks[i],
          std::span(scratch).first(batch.input(i).size()))!=GpuLz4Verification::verified)
      invalid("LZ4 candidate does not reproduce immutable source");
  return total;
}
}
void PreparedLz4WorkerBatch::swap(PreparedLz4WorkerBatch& other) noexcept {
  input_.swap(other.input_);sizes_.swap(other.sizes_);offsets_.swap(other.offsets_);
  std::swap(layout_,other.layout_);digest_.swap(other.digest_);
}
PreparedLz4WorkerBatch::PreparedLz4WorkerBatch(PreparedLz4WorkerBatch&& other) noexcept {
  swap(other); // Exchange with explicit empty/default state.
}
PreparedLz4WorkerBatch& PreparedLz4WorkerBatch::operator=(PreparedLz4WorkerBatch&& other) noexcept {
  if(this!=&other){PreparedLz4WorkerBatch replacement(std::move(other));swap(replacement);}
  return *this;
}
bool PreparedLz4WorkerBatch::ready() const noexcept {
  return layout_.count>0 && layout_.count<=8 && !input_.empty() &&
      input_.size()==layout_.input_bytes;
}
std::uint32_t PreparedLz4WorkerBatch::count() const noexcept {return ready()?layout_.count:0;}
std::uint32_t PreparedLz4WorkerBatch::input_bytes() const noexcept {return ready()?layout_.input_bytes:0;}
Lz4WorkerBytes PreparedLz4WorkerBatch::canonical_sizes() const noexcept {
  return std::span(sizes_).first(ready()?4U+4U*layout_.count:0U);
}
Lz4WorkerBytes PreparedLz4WorkerBatch::input(std::size_t index) const {
  if(!ready() || index>=layout_.count)invalid("LZ4 worker chunk index or moved batch");
  return std::span(input_).subspan(offsets_[index],layout_.chunks[index].input_bytes);
}
const GpuLz4DeviceLayout& PreparedLz4WorkerBatch::device_layout() const {
  if(!ready())invalid("moved LZ4 worker batch");
  return layout_;
}
PreparedLz4WorkerBatch PreparedLz4WorkerBatch::prepare(std::span<const Lz4WorkerBytes> chunks) {
  if(chunks.empty() || chunks.size()>8)invalid("LZ4 worker preparation count");
  std::array<std::uint32_t,8> sizes{};
  std::uint32_t total=0;
  for(std::size_t i=0;i<chunks.size();++i) {
    if(chunks[i].empty() || chunks[i].size()>gpu_detail::max_block_bytes ||
        chunks[i].size()>gpu_detail::max_batch_bytes-total)invalid("LZ4 worker preparation extent");
    sizes[i]=static_cast<std::uint32_t>(chunks[i].size());total+=sizes[i];
  }
  auto layout=gpu_lz4_device_layout(std::span(sizes).first(chunks.size()));
  PreparedLz4WorkerBatch result;result.layout_=layout;
  result.input_.reserve(total);
  for(std::size_t i=0;i<chunks.size();++i) {
    result.offsets_[i]=static_cast<std::uint32_t>(result.input_.size());
    result.input_.insert(result.input_.end(),chunks[i].begin(),chunks[i].end());
  }
  const auto word=[&](std::size_t at,std::uint32_t value) {
    for(unsigned j=0;j<4;++j)result.sizes_[at+j]=static_cast<std::byte>((value>>(8U*j))&255U);
  };
  word(0,layout.count);
  for(std::size_t i=0;i<chunks.size();++i)word(4+4*i,sizes[i]);
  result.digest_=detail::record_digest(result.canonical_sizes(),result.input_);
  return result;
}
Bytes encode_lz4_worker_request(const PreparedLz4WorkerBatch& batch,std::uint64_t id) {
  batch_id(batch,id);
  auto result=make_header(batch,id,0,false,
      kLz4WorkerHeaderBytes+batch.canonical_sizes().size()+batch.input_bytes());
  result.insert(result.end(),batch.canonical_sizes().begin(),batch.canonical_sizes().end());
  result.insert(result.end(),batch.plaintext().begin(),batch.plaintext().end());
  return result;
}
Lz4WorkerRequest decode_lz4_worker_request(Lz4WorkerBytes frame) {
  header(frame,kLz4WorkerRequestLimit,request_magic);
  const auto id=u64(frame,8);const auto count=u32(frame,16),total=u32(frame,20);
  if(!id || !count || count>8 || !total || total>gpu_detail::max_batch_bytes ||
      u32(frame,28)!=0)invalid("invalid LZ4 worker request shape");
  const std::size_t directory=4U+4U*count;
  if(frame.size()!=kLz4WorkerHeaderBytes+directory+total || u32(frame,64)!=count)
    invalid("invalid LZ4 source directory");
  std::array<std::uint32_t,8> sizes{};
  std::uint32_t sum=0;
  for(std::size_t i=0;i<count;++i) {
    sizes[i]=u32(frame,68+4*i);
    if(!sizes[i] || sizes[i]>gpu_detail::max_block_bytes ||
        sizes[i]>total-sum)invalid("invalid LZ4 source extent");
    sum+=sizes[i];
  }
  if(sum!=total)invalid("non-dense LZ4 source payload");
  (void)gpu_lz4_device_layout(std::span(sizes).first(count));
  const auto source=frame.subspan(kLz4WorkerHeaderBytes+directory);
  const auto digest=detail::record_digest(frame.subspan(kLz4WorkerHeaderBytes,directory),source);
  if(!std::equal(digest.begin(),digest.end(),frame.begin()+32))invalid("LZ4 source digest mismatch");
  std::array<Lz4WorkerBytes,8> chunks{};std::size_t at=0;
  for(std::size_t i=0;i<count;++i){chunks[i]=source.subspan(at,sizes[i]);at+=sizes[i];}
  auto batch=PreparedLz4WorkerBatch::prepare(std::span(chunks).first(count));
  if(batch.source_digest()!=digest)invalid("LZ4 source changed during preparation");
  return {id,std::move(batch)};
}
Bytes encode_lz4_worker_reply(const PreparedLz4WorkerBatch& batch,std::uint64_t id,
    std::span<const Lz4WorkerBytes> chunks,WorkerExecution execution) {
  batch_id(batch,id);backend(execution);
  const auto total=verify_all(batch,chunks);
  auto result=make_header(batch,id,static_cast<std::uint32_t>(execution),true,
      kLz4WorkerHeaderBytes+8U*batch.count()+total);
  for(std::size_t i=0;i<chunks.size();++i) {
    put(result,batch.input(i).size(),4);put(result,chunks[i].size(),4);
  }
  for(const auto chunk:chunks)result.insert(result.end(),chunk.begin(),chunk.end());
  return result;
}
std::vector<Bytes> accept_lz4_worker_reply(Lz4WorkerBytes frame,
    const PreparedLz4WorkerBatch& batch,std::uint64_t id,WorkerExecution execution) {
  batch_id(batch,id);backend(execution);
  header(frame,kLz4WorkerReplyLimit,reply_magic);
  if(u64(frame,8)!=id || u32(frame,16)!=batch.count() || u32(frame,20)!=batch.input_bytes() ||
      u32(frame,28)!=static_cast<std::uint32_t>(execution) ||
      !std::equal(batch.source_digest().begin(),batch.source_digest().end(),frame.begin()+32))
    invalid("LZ4 reply identity mismatch");
  const std::size_t directory=8U*batch.count();
  if(frame.size()<kLz4WorkerHeaderBytes+directory)invalid("truncated LZ4 result directory");
  std::array<Lz4WorkerBytes,8> chunks{};std::size_t total=0;
  for(std::size_t i=0;i<batch.count();++i) {
    const auto size=u32(frame,64+8*i),stored=u32(frame,68+8*i);
    if(size!=batch.input(i).size() || !stored || stored>batch.device_layout().chunks[i].output_capacity ||
        stored>gpu_detail::max_batch_bytes-total)invalid("invalid LZ4 result extent");
    total+=stored;
  }
  if(frame.size()!=kLz4WorkerHeaderBytes+directory+total)invalid("non-dense LZ4 result payload");
  std::size_t at=kLz4WorkerHeaderBytes+directory;
  for(std::size_t i=0;i<batch.count();++i) {
    const auto stored=u32(frame,68+8*i);chunks[i]=frame.subspan(at,stored);at+=stored;
  }
  (void)verify_all(batch,std::span(chunks).first(batch.count()));
  std::vector<Bytes> result;result.reserve(batch.count());
  for(std::size_t i=0;i<batch.count();++i)result.emplace_back(chunks[i].begin(),chunks[i].end());
  return result;
}
} // namespace flitzfs
