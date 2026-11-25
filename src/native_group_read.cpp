#include "native_group_read.hpp"
#include <algorithm>
#include <utility>

namespace flitzfs::detail {
bool NativeGroupReadBatch::mixed_methods() const noexcept {
  return std::ranges::any_of(members_,[](const auto& member){return member.method!=NativeGroupMethod::packed_xor;});
}

bool NativeGroupReadBatch::has_data_after(std::uint64_t offset) const noexcept {
  if(members_.empty())return false;
  const auto& last=members_.back();
  const auto block=offset/kNativeExtentBytes;
  return last.block>block || (last.block==block && offset%kNativeExtentBytes<last.maximum_payload);
}

std::size_t NativeGroupReadyReads::descriptor_bytes() noexcept {
  return sizeof(Member)+sizeof(std::vector<std::byte>)+
      sizeof(NativeGroupReadBatch::Member)+sizeof(NativeGroupReadBatch::Base);
}

NativeGroupReadyReads::Member* NativeGroupReadyReads::find(std::uint64_t inode,std::uint64_t block,
    std::uint64_t object,std::uint64_t offset) noexcept {
  if(inode!=inode_)return nullptr;
  const auto found=std::ranges::lower_bound(members_,block,{},&Member::block);
  return found!=members_.end() && found->block==block && found->record.object==object &&
      !found->bytes.empty() && offset<found->logical_bytes &&
      (!found->next_offset || found->next_offset==offset)?&*found:nullptr;
}

std::uint32_t NativeGroupReadyReads::delivered(Member& member,std::uint64_t begin,std::uint64_t end) noexcept {
  const auto through=(std::min)(end,std::uint64_t{member.logical_bytes});
  pending_bytes_-=through-member.next_offset;
  if(!member.next_offset)++consumed_;
  if(through==member.logical_bytes)std::vector<std::byte>{}.swap(member.bytes);
  else member.next_offset=static_cast<std::uint32_t>(through);
  return static_cast<std::uint32_t>(through-begin);
}

void NativeGroupReadyReads::discard_before(std::uint64_t offset) noexcept {
  const auto keep=std::ranges::upper_bound(members_,offset,{},[](const Member& member) {
    return member.block*kNativeExtentBytes+member.logical_bytes;
  });
  for(auto it=members_.begin();it!=keep;++it) {
    if(!it->bytes.empty())pending_bytes_-=it->logical_bytes-it->next_offset;
  }
  members_.erase(members_.begin(),keep);
}

std::unique_ptr<NativeGroupReadyReads> NativeGroupReadyReads::verify(NativeGroupReadBatch& batch,
    std::vector<std::vector<std::byte>> decoded,const std::function<bool()>& interrupted) {
  if(!batch.metadata_ || batch.members_.empty() || decoded.size()!=batch.members_.size())return {};
  auto ready=std::unique_ptr<NativeGroupReadyReads>(new NativeGroupReadyReads);
  ready->inode_=batch.inode_;
  ready->members_.reserve(decoded.size());
  for(std::size_t i=0;i<decoded.size();++i) {
    if(interrupted && interrupted())return {};
    const auto& member=batch.members_[i];
    auto& bytes=decoded[i];
    if(bytes.size()!=kNativeExtentBytes || record_digest(bytes)!=member.decoded_digest)return {};
    while(!bytes.empty() && bytes.back()==std::byte{})bytes.pop_back();
    if(bytes.empty() || bytes.size()>member.maximum_payload)return {};
    ready->pending_bytes_+=member.maximum_payload;
    ready->members_.push_back({member.record,batch.bases_[member.base].record,member.block,
        member.maximum_payload,0,std::move(bytes)});
  }
  if(interrupted && interrupted())return {};
  ready->metadata_=std::move(batch.metadata_);
  return ready;
}

std::unique_ptr<NativeGroupReadyReads> NativeGroupReadyReads::decode_captured(NativeGroupReadBatch& batch,
    const std::function<bool()>& interrupted) {
  if(!batch.metadata_)return {};
  std::vector<std::vector<std::byte>> decoded;
  decoded.reserve(batch.members_.size());
  for(const auto& member:batch.members_) {
    if(interrupted && interrupted())return {};
    decoded.push_back(decode_native_group_member(member.frame,batch.bases_[member.base].bytes,member.method));
  }
  return verify(batch,std::move(decoded),interrupted);
}
}
