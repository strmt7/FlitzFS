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
  // Qualification replays take independently read bytes from the same
  // private selected roots. No live writer or cached ready read participates.
  std::vector<std::byte> read_range(std::uint32_t bytes,const std::function<bool()>& interrupted) const;
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
}
