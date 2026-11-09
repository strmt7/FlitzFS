#pragma once
#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include <memory>
#include <span>
#include <vector>
namespace flitzfs::detail {
inline constexpr std::uint32_t kStoreRaw=0,kStoreZstd=1,kStoreZero=2,kStorePattern=3;
inline constexpr std::size_t kStoreMaxPatternBytes=4096;
inline constexpr std::size_t kStoreEncoderMemoryCap=16U*1024U*1024U;
inline constexpr std::size_t kStoreDecoderMemoryCap=2U*1024U*1024U;
void validate_store_compression(StoreCompressionOptions options);
bool store_codec_framing(std::uint32_t codec,std::uint32_t logical,std::uint32_t stored) noexcept;
struct EncodedStoreObject {
  std::uint32_t codec=kStoreRaw;
  std::vector<std::byte> payload;
  Digest decoded_digest{};
};
class StoreCodecDecoder final {
 public:
  explicit StoreCodecDecoder(std::size_t allocation_cap=kStoreDecoderMemoryCap);
  ~StoreCodecDecoder();
  StoreCodecDecoder(const StoreCodecDecoder&)=delete;
  StoreCodecDecoder& operator=(const StoreCodecDecoder&)=delete;
  std::vector<std::byte> decode(std::uint32_t codec,std::span<const std::byte> payload,
                               std::uint32_t logical);
  std::size_t peak_bytes() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class StoreCodecEncoder final {
 public:
  explicit StoreCodecEncoder(StoreCompressionOptions options,std::size_t allocation_cap=kStoreEncoderMemoryCap);
  ~StoreCodecEncoder();
  StoreCodecEncoder(const StoreCodecEncoder&)=delete;
  StoreCodecEncoder& operator=(const StoreCodecEncoder&)=delete;
  // Takes ownership. Compression/round-trip validation finish before I/O.
  EncodedStoreObject encode(std::vector<std::byte> input);
  std::size_t peak_bytes() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
