#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace flitzfs {

// Phase 1: a single-file, read-only, independently compressed chunk container.
// This is NOT a mounted filesystem, encryption implementation, or GPU backend.
constexpr std::uint32_t kDefaultChunkBytes = 128u * 1024u;
constexpr std::uint32_t kMinChunkBytes = 4u * 1024u;
constexpr std::uint32_t kMaxChunkBytes = 1024u * 1024u;
constexpr std::uint32_t kMaxSingleReadBytes = 4u * 1024u * 1024u;
constexpr std::uint64_t kMaxChunks = 1024u * 1024u;
constexpr std::uint64_t kDefaultMaxInputBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxExplicitInputBytes = 4096ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kFreeSpaceReserveBytes = 1024ULL * 1024ULL * 1024ULL;

struct PackOptions {
  std::uint32_t chunk_bytes = kDefaultChunkBytes;
  int zstd_level = 3;
  std::uint64_t max_input_bytes = kDefaultMaxInputBytes;
};

struct ContainerInfo {
  std::uint32_t chunk_bytes{};
  std::uint64_t logical_bytes{};
  std::uint64_t container_bytes{};
  std::uint64_t chunk_count{};
  std::uint64_t zstd_chunk_count{};
};

// Creates ONLY a NEW output; refuses to overwrite any existing output, including links.
// The output is invalid until the footer has been written. If interrupted, a partial
// newly created output may remain; this function never deletes existing files.
ContainerInfo pack_new(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       const PackOptions& options = {});

// A deliberately single-reader interface. Do not concurrently invoke Reader methods.
class Reader {
 public:
  static Reader open(const std::filesystem::path& container);
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) noexcept = default;
  Reader& operator=(Reader&&) noexcept = default;

  const ContainerInfo& info() const noexcept { return info_; }

  // Strictly bounded read. Out-of-range requests throw; no silent truncation.
  std::vector<std::byte> read(std::uint64_t offset, std::uint32_t length);
  // Reads and checks every chunk, without materializing the entire logical file.
  void verify_all();
  // Creates a NEW file only, with bounded-memory streaming. Never overwrites.
  void extract_new(const std::filesystem::path& output,
                   std::uint64_t max_output_bytes = kDefaultMaxInputBytes);

 private:
  struct Entry {
    std::uint64_t offset{};
    std::uint32_t stored_bytes{};
    std::uint32_t decoded_bytes{};
    std::uint32_t crc32{};
    std::uint8_t codec{};
  };

  Reader(std::ifstream file, ContainerInfo info, std::vector<Entry> entries)
    : file_(std::move(file)), info_(info), entries_(std::move(entries)) {}

  std::vector<std::byte> read_chunk(std::size_t index);
  std::ifstream file_;
  ContainerInfo info_;
  std::vector<Entry> entries_;
  // Single most-recently used decoded chunk, bounded by kMaxChunkBytes.
  std::size_t cached_index_ = static_cast<std::size_t>(-1);
  std::vector<std::byte> cached_chunk_;
};

} // namespace flitzfs
