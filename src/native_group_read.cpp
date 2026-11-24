#include "native_group_read.hpp"
#include <algorithm>
#include <utility>

namespace flitzfs::detail {
std::size_t NativeGroupReadyReads::descriptor_bytes() noexcept {
  return sizeof(Member)+sizeof(std::vector<std::byte>)+
      sizeof(NativeGroupReadBatch::Member)+sizeof(NativeGroupReadBatch::Base);
}

NativeGroupReadyReads::Member* NativeGroupReadyReads::find(std::uint64_t inode,std::uint64_t block,
    std::uint64_t object) noexcept {
  if(inode!=inode_)return nullptr;
  const auto found=std::ranges::lower_bound(members_,block,{},&Member::block);
  return found!=members_.end() && found->block==block && found->record.object==object &&
      !found->bytes.empty()?&*found:nullptr;
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
    ready->members_.push_back({member.record,batch.bases_[member.base].record,member.block,std::move(bytes)});
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
    decoded.push_back(decode_native_group_member(member.frame,batch.bases_[member.base].bytes,
        NativeGroupMethod::packed_xor));
  }
  return verify(batch,std::move(decoded),interrupted);
}
}
