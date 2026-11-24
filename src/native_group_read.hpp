#pragma once
#include "native_group_codec.hpp"
#include <memory>

namespace flitzfs::detail {
// Identity of the protected backing record, before applying the native codec.
struct PagedGroupReadRecord {
  std::uint64_t object=0,generation=0,primary=0,peer=0;
  Digest stored_digest{};
  bool operator==(const PagedGroupReadRecord&) const=default;
};
struct PagedGroupReadFrame {
  PagedGroupReadRecord record;
  std::vector<std::byte> bytes;
};

// The indexed reader alone constructs the batch from one verified checkpoint.
// Each distinct base is owned once, even if several captured members use it.
class NativeGroupReadBatch final {
 public:
  NativeGroupReadBatch(const NativeGroupReadBatch&)=delete;
  NativeGroupReadBatch& operator=(const NativeGroupReadBatch&)=delete;
  struct Base {
    PagedGroupReadRecord record;
    NativeGroupBase bytes;
  };
  struct Member {
    PagedGroupReadRecord record;
    std::uint64_t block=0;
    std::uint32_t maximum_payload=0;
    std::size_t base=0;
    Digest decoded_digest{};
    NativePackedXorOffsets offsets;
    std::vector<std::byte> frame;
  };
  std::span<const Base> bases() const noexcept{return bases_;}
  std::span<const Member> members() const noexcept{return members_;}

 private:
  friend class IndexedVolumeView;
  friend class NativeGroupCaptureLease;
  friend class NativeGroupReadyReads;
  friend class NativeGroupReadExchange;
  NativeGroupReadBatch()=default;
  std::uint64_t inode_=0;
  std::vector<Base> bases_;
  std::vector<Member> members_;
  std::shared_ptr<PagedReadMetadata> metadata_;
};

class NativeGroupReadyReads final {
 public:
  NativeGroupReadyReads(const NativeGroupReadyReads&)=delete;
  NativeGroupReadyReads& operator=(const NativeGroupReadyReads&)=delete;
  // The caller must bound reply allocation and bind it to this specific
  // capture. Every complete result is checked against its committed digest.
  static std::unique_ptr<NativeGroupReadyReads> verify(NativeGroupReadBatch&,
      std::vector<std::vector<std::byte>> decoded,const std::function<bool()>& interrupted={});
  static std::unique_ptr<NativeGroupReadyReads> decode_captured(NativeGroupReadBatch&,
      const std::function<bool()>& interrupted={});
  bool pending() const noexcept{return consumed_<members_.size();}
  std::size_t consumed() const noexcept{return consumed_;}

 private:
  friend class IndexedVolumeView;
  friend class ::flitzfs::NativeFilesystemWriter;
  NativeGroupReadyReads()=default;
  struct Member {
    PagedGroupReadRecord record,base;
    std::uint64_t block;
    std::vector<std::byte> bytes;
  };
  static std::size_t descriptor_bytes() noexcept;
  Member* find(std::uint64_t inode,std::uint64_t block,std::uint64_t object) noexcept;
  std::uint64_t inode_=0;
  std::shared_ptr<PagedReadMetadata> metadata_;
  std::vector<Member> members_;
  std::size_t consumed_=0;
};
struct NativeGroupReadRoute {
  NativeGroupReadyReads* ready=nullptr;
  bool packed=false;
};

}
