#pragma once
#include "native_lz4_read.hpp"
#include "gpu/native_lz4_decode_layout.hpp"

namespace flitzfs::detail {
struct NativeLz4DecodeSource {
  NativeLz4ReadKey key;
  std::span<const std::byte> encoded;
};
struct NativeLz4DecodeGrant {
  std::uint32_t maximum_frames=0;
  std::uint64_t maximum_slab_bytes=0;
};
struct NativeLz4DecodedFrame {
  NativeLz4ReadKey key;
  std::vector<std::byte> bytes;
};

class NativeLz4DecodeBatch final {
 public:
  static NativeLz4DecodeBatch prepare(std::span<const NativeLz4DecodeSource>,NativeLz4DecodeGrant);
  native_lz4_decode::Layout layout() const noexcept { return layout_; }
  std::span<const std::byte> upload() const noexcept { return upload_; }
  // Require matching descriptors, complete status, exact length and digest.
  // A failed batch yields no frames for the file reader.
  std::vector<NativeLz4DecodedFrame> verify(std::span<const std::byte> readback) const;
 private:
  native_lz4_decode::Layout layout_{};
  std::vector<NativeLz4ReadKey> keys_;
  std::vector<std::byte> upload_;
};
}
