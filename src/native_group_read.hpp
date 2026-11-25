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
    NativeGroupMethod method;
    NativeGroupTileParameters parameters;
    NativeGroupTileOffsets offsets;
    std::vector<std::byte> frame;
  };
  std::span<const Base> bases() const noexcept{return bases_;}
  std::span<const Member> members() const noexcept{return members_;}
  bool mixed_methods() const noexcept;
  bool has_data_after(std::uint64_t offset) const noexcept;

 private:
  friend class IndexedVolumeView;
  friend class NativeGroupCaptureLease;
  friend class NativeGroupReadyReads;
  friend class NativeGroupReadExchange;
  friend class NativeGroupReplyVerifier;
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
  // Before publication, release members already served by the CPU.
  void discard_before(std::uint64_t offset) noexcept;
  bool pending() const noexcept{return pending_bytes_!=0;}
  std::uint64_t pending_bytes() const noexcept{return pending_bytes_;}
  std::size_t consumed() const noexcept{return consumed_;}

 private:
  friend class IndexedVolumeView;
  friend class ::flitzfs::NativeFilesystemWriter;
  friend class NativeGroupReplyVerifier;
  NativeGroupReadyReads()=default;
  bool append_checked(const NativeGroupReadBatch&,std::vector<std::byte>);
  struct Member {
    PagedGroupReadRecord record,base;
    std::uint64_t block;
    std::uint32_t logical_bytes,next_offset=0;
    std::vector<std::byte> bytes;
  };
  static std::size_t descriptor_bytes() noexcept;
  Member* find(std::uint64_t inode,std::uint64_t block,std::uint64_t object,std::uint64_t offset) noexcept;
  std::uint32_t delivered(Member&,std::uint64_t begin,std::uint64_t end) noexcept;
  std::uint64_t inode_=0;
  std::shared_ptr<PagedReadMetadata> metadata_;
  std::vector<Member> members_;
  std::uint64_t pending_bytes_=0;
  std::size_t consumed_=0;
};
struct NativeGroupReadRoute {
  NativeGroupReadyReads* ready=nullptr;
  bool decoded_tiled=false;
  std::uint32_t tiled_suffix_bytes=0;
  std::uint32_t ready_bytes=0;
};

}
