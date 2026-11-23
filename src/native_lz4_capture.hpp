#pragma once
#include "native_indexed_volume.hpp"
#include "native_lz4_read.hpp"
#include <utility>

namespace flitzfs::detail {
// Private selected roots and a strong backing pin. The snapshot may be read
// by one capture owner after the serialized session has released control.
class NativeLz4CaptureLease final {
 public:
  bool capture(NativeLz4CapturedFrame&) const;
  // The grant covers encoded and decoded frame bytes together.
  std::vector<NativeLz4CapturedFrame> capture_range(std::uint32_t bytes,std::size_t maximum_frame_bytes,
      const std::function<bool()>& interrupted={},std::shared_ptr<PagedReadMetadata>* metadata=nullptr) const;

 private:
  friend class ::flitzfs::NativeFilesystemWriter;
  NativeLz4CaptureLease(PagedObjectStoreSnapshot snapshot,IndexedVolumeRoot root,
      std::uint64_t inode,std::uint64_t offset,std::size_t maximum_encoded):
      snapshot_(std::move(snapshot)),root_(root),inode_(inode),offset_(offset),maximum_encoded_(maximum_encoded) {}
  PagedObjectStoreSnapshot snapshot_;
  IndexedVolumeRoot root_;
  std::uint64_t inode_,offset_;
  std::size_t maximum_encoded_;
};
struct NativeCommittedWriteRange {
  std::uint64_t sequence=0,inode=0,revision=0,offset=0;
  std::uint32_t bytes=0;
};
struct NativeCapturedWrite {
  std::uint64_t sequence=0;
  std::vector<std::byte> bytes;
};
// One selected checkpoint for a funded set of completed write receipts.
// The first member is required. Older stale members can be dropped.
class NativeWriteCaptureLease final {
 public:
  std::vector<NativeCapturedWrite> read(std::span<const NativeCommittedWriteRange>,
      const std::function<bool(std::size_t)>& admit,const std::function<bool()>& interrupted) const;
 private:
  friend class ::flitzfs::NativeFilesystemWriter;
  NativeWriteCaptureLease(PagedObjectStoreSnapshot snapshot,IndexedVolumeRoot root):
      snapshot_(std::move(snapshot)),root_(root) {}
  PagedObjectStoreSnapshot snapshot_;
  IndexedVolumeRoot root_;
};
}
