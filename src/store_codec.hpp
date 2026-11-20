#pragma once
#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include <memory>
#include <optional>
#include <span>
#include <vector>
namespace flitzfs::detail {
inline constexpr std::uint32_t kStoreRaw=0,kStoreZstd=1,kStoreZero=2,kStorePattern=3,kStoreLz4=4;
inline constexpr std::size_t kStoreMaxPatternBytes=4096;
inline constexpr std::size_t kStoreMaxMatchInputBytes=64U*1024U;
inline constexpr std::size_t kStoreMaxMatchProposals=4096;
bool store_match_sequences_available() noexcept;
inline constexpr std::size_t kStoreEncoderMemoryCap=16U*1024U*1024U;
inline constexpr std::size_t kStoreDecoderMemoryCap=2U*1024U*1024U;
inline constexpr std::size_t kStorePrefixEncoderMemoryCap=2U*1024U*1024U;
// Advance when indexed cold eligibility, trials or their resource envelope change.
inline constexpr std::uint32_t kIndexedColdEncodingRevision=1;
bool store_prefix_frame(std::span<const std::byte>,std::size_t expected) noexcept;
std::optional<std::vector<std::byte>> encode_store_prefix(std::span<const std::byte> input,
    std::span<const std::byte> prefix,std::size_t exclusive_limit);
std::vector<std::byte> decode_store_prefix(std::span<const std::byte> input,
    std::span<const std::byte> prefix);
void validate_store_compression(StoreCompressionOptions options);
bool store_codec_framing(std::uint32_t codec,std::uint32_t logical,std::uint32_t stored) noexcept;
// Prefix a standalone LZ4 block whose entire decoded suffix was checked.
// Literal insertion shifts both ends of each back-reference equally.
std::vector<std::byte> prepend_lz4_literals(std::span<const std::byte> prefix,
    std::span<const std::byte> block,std::size_t decoded_body_bytes);
struct EncodedStoreObject {
  std::uint32_t codec=kStoreRaw;
  std::vector<std::byte> payload;
  Digest decoded_digest{};
  bool used_match_proposals=false;
  std::uint32_t match_baseline_bytes=0; // CPU baseline only when hints were selected.
};
struct ColdStoreEncoding {
  std::optional<EncodedStoreObject> smaller;
  bool evaluated=false; // A resource or codec failure has no usable result.
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
  EncodedStoreObject encode_with_lz4(std::span<const std::byte> input,std::span<const std::byte> candidate,
      std::uint32_t literal_prefix_bytes=0);
  EncodedStoreObject encode_with_matches(std::vector<std::byte> input,std::span<const StoreMatchProposal> matches);
  ColdStoreEncoding try_cold_encoding(std::span<const std::byte> input,std::size_t old_size);
  std::size_t peak_bytes() const noexcept;
 private:
  EncodedStoreObject encode_compressed(std::span<const std::byte> input,bool cpu_fast);
  EncodedStoreObject verify_encoding(EncodedStoreObject,std::span<const std::byte> input);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
