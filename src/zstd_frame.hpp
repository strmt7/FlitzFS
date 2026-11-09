#pragma once
#include <cstddef>
#include <span>
namespace flitzfs::detail {
// Canonical Zstandard frame magic, public ZSTD_MAGICNUMBER since v0.8.0.
// Reject legacy/skippable formats before invoking optional library parsers.
inline bool native_zstd_frame(std::span<const std::byte> bytes) noexcept {
  return bytes.size()>=4 && bytes[0]==std::byte{0x28} && bytes[1]==std::byte{0xb5} &&
      bytes[2]==std::byte{0x2f} && bytes[3]==std::byte{0xfd};
}
}
