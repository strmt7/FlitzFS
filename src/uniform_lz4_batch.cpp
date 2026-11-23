#include "flitzfs/uniform_lz4_batch.hpp"
#include <stdexcept>

namespace flitzfs {
namespace {
std::uint64_t input_bytes(std::span<const UniformLz4BatchInput> requests,std::uint32_t chunk_bytes) noexcept {
  if(!chunk_bytes)return 0;
  std::uint64_t bytes=0,last_id=0;
  for(const auto& request:requests) {
    if(request.request_id<=last_id || request.source.empty() || bytes%chunk_bytes ||
        !uniform_lz4::add(bytes,request.source.size(),bytes))return 0;
    last_id=request.request_id;
  }
  return bytes;
}
}

UniformLz4BatchWindow plan_uniform_lz4_batch(std::span<const UniformLz4BatchInput> requests,
    std::uint32_t chunk_bytes,const GpuHealthEvidence& evidence,std::uint64_t now,const GpuLz4ResidentCost& cost,
    GpuLz4WindowPolicy policy,GpuLz4SweepGrant grant,std::uint32_t deadline) noexcept {
  const auto bytes=input_bytes(requests,chunk_bytes);
  auto charged=cost;
  constexpr auto member_overhead=sizeof(UniformLz4BatchMember)+sizeof(VerifiedUniformLz4Batch::Block);
  if(!bytes || !uniform_lz4::add(charged.per_chunk_bytes,member_overhead,charged.per_chunk_bytes))return {};
  if(!evidence.supervisor_quiescent)
    return {{.code=GpuGateCode::wrong_state,.remaining_input_bytes=bytes},0};
  auto available=plan_gpu_lz4_sweep_window(bytes,chunk_bytes,evidence,now,charged,policy,grant,deadline);
  if(available.code!=GpuGateCode::admitted)return {available,0};
  if(!available.remaining_input_bytes)return {available,requests.size()};

  std::size_t admitted=0;
  std::uint64_t prefix=0;
  for(const auto& request:requests) {
    if(request.source.size()>available.layout.input_bytes-prefix)break;
    prefix+=request.source.size();
    ++admitted;
  }
  const auto refused=GpuLz4WindowPlan{.code=GpuGateCode::insufficient_headroom,.remaining_input_bytes=bytes};
  if(!admitted)return {refused,0};
  if(prefix!=available.layout.input_bytes)
    available=plan_gpu_lz4_sweep_window(prefix,chunk_bytes,evidence,now,charged,policy,grant,deadline);
  if(available.code!=GpuGateCode::admitted || available.layout.input_bytes!=prefix)return {refused,0};
  available.remaining_input_bytes=bytes-prefix;
  return {available,admitted};
}

PreparedUniformLz4Batch::PreparedUniformLz4Batch(PreparedUniformLz4&& source,
    std::vector<UniformLz4BatchMember>&& members) noexcept:
    source_(std::move(source)),members_(std::move(members)) {}
PreparedUniformLz4Batch PreparedUniformLz4Batch::prepare(std::span<const UniformLz4BatchInput> requests,
    const UniformLz4BatchWindow& planned) {
  const auto& window=planned.window;
  if(window.code!=GpuGateCode::admitted || !planned.requests || planned.requests>requests.size())
    throw std::invalid_argument("Invalid uniform batch admission");
  return prepare(requests.first(planned.requests),window.layout);
}

PreparedUniformLz4Batch PreparedUniformLz4Batch::prepare(std::span<const UniformLz4BatchInput> requests,
    const uniform_lz4::Layout& layout) {
  const auto limits=uniform_lz4_limits(layout);
  if(input_bytes(requests,limits.chunk_bytes)!=limits.input_bytes)
    throw std::invalid_argument("Uniform batch differs from admitted source extent");

  std::vector<std::byte> owned;
  owned.reserve(static_cast<std::size_t>(limits.input_bytes));
  std::vector<UniformLz4BatchMember> members;
  members.reserve(requests.size());
  for(const auto& request:requests) {
    const auto bytes=request.source.size();
    const auto chunks=1+(bytes-1)/limits.chunk_bytes;
    members.push_back({request.request_id,static_cast<std::uint32_t>(owned.size()/limits.chunk_bytes),
        static_cast<std::uint32_t>(chunks),bytes});
    owned.insert(owned.end(),request.source.begin(),request.source.end());
  }
  return {PreparedUniformLz4::prepare_owned(std::move(owned),limits),std::move(members)};
}

UniformLz4Bytes PreparedUniformLz4Batch::input(std::size_t index) const {
  const auto& member=members_.at(index);
  if(!source_.ready())throw std::invalid_argument("Uniform batch source retired");
  return source_.plaintext().subspan(std::size_t(member.first_chunk)*source_.layout().chunk_bytes,member.source_bytes);
}

VerifiedUniformLz4Batch VerifiedUniformLz4Batch::accept(UniformLz4Bytes wire,
    const PreparedUniformLz4Batch& batch,std::uint64_t exchange_id,WorkerExecution execution) {
  VerifiedUniformLz4Batch result;
  result.candidates_=accept_uniform_lz4_reply(wire,batch.source(),exchange_id,execution);
  result.limits_=batch.source().limits();
  result.input_digest_=batch.source().digest();
  const auto sizes=std::span(result.candidates_.sizes);
  result.blocks_.reserve(batch.members().size());
  std::size_t offset=0;
  for(const auto& member:batch.members()) {
    std::size_t bytes=0;
    for(const auto length:sizes.subspan(member.first_chunk,member.chunks))bytes+=length;
    result.blocks_.push_back({member,offset,bytes});
    offset+=bytes;
  }
  return result;
}

bool VerifiedUniformLz4Batch::matches(const PreparedUniformLz4Batch& input) const noexcept {
  const auto members=input.members();
  if(!input.source().ready() || input.source().limits()!=limits_ || input.source().digest()!=input_digest_ ||
      members.size()!=blocks_.size())return false;
  for(std::size_t index=0;index<members.size();++index)
    if(blocks_[index].member!=members[index])return false;
  return true;
}

UniformLz4BatchReply VerifiedUniformLz4Batch::reply(std::size_t index) const {
  const auto& block=blocks_.at(index);
  return {block.member.request_id,std::span(candidates_.sizes).subspan(block.member.first_chunk,block.member.chunks),
      std::span(candidates_.payload).subspan(block.compressed_offset,block.compressed_bytes)};
}

}
