#pragma once
#include "flitzfs/catalog.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace flitzfs {
inline constexpr std::size_t kMaxFlzipSnapshotBytes = 34U * 1024U * 1024U;
inline constexpr std::size_t kMaxFlzipIndexBytes = 1024U * 1024U;
inline constexpr std::uint64_t kMaxFlzipLogicalBytes = 32ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kMaxFlzipBlockBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxFlzipBlocks = 8192;
inline constexpr std::size_t kMaxFlzipRecords = 256;
enum class FlzipErrorCode { malformed, unsupported_version, unsupported_codec, limit, corrupt };
class FlzipError final : public std::runtime_error {
 public:
  FlzipError(FlzipErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}
  FlzipErrorCode code() const noexcept { return code_; }
 private:
  FlzipErrorCode code_;
};
struct FlzipInfo {
  std::uint32_t version = 0;
  std::uint64_t archive_bytes = 0;
  std::uint64_t logical_bytes = 0;
  std::size_t files = 0;
  std::size_t blocks = 0;
};
// Reports compiled outer-kind support, not GPU availability or full format support.
// A composed frame can still require an unavailable optional LZ4 inner stage.
bool flzip_codec_available(std::uint8_t kind) noexcept;

// Experimental, bounded CPU-only snapshot. Every file is CRC-checked before
// exposure. Input is read-only and later changes cannot change this snapshot.
// Direct calls are single-threaded; the catalog wrapper supplies synchronization.
class FlzipReader final {
 public:
  static FlzipReader open(const std::filesystem::path& input,
      std::uint64_t logical_budget = kMaxFlzipLogicalBytes);
  // Copies bounded bytes before parsing. Primarily useful for CPU parser tests.
  static FlzipReader from_bytes(std::span<const std::byte> input,
      std::uint64_t logical_budget = kMaxFlzipLogicalBytes);
  ~FlzipReader();
  FlzipReader(FlzipReader&&) noexcept;
  FlzipReader& operator=(FlzipReader&&) noexcept;
  FlzipReader(const FlzipReader&) = delete;
  FlzipReader& operator=(const FlzipReader&) = delete;
  const FlzipInfo& info() const noexcept;
  CatalogEntry stat(std::string_view path) const;
  std::vector<CatalogEntry> list(std::string_view directory) const;
  // Strict ranges; no decoded result is returned on error. One decoded-block cache.
  std::vector<std::byte> read(std::string_view file, std::uint64_t offset, std::uint32_t length);
  void verify_all();
 private:
  struct Impl;
  explicit FlzipReader(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
