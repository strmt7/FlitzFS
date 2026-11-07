#pragma once

#include "flitzfs/container.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {

// In-memory, read-only directory view over independently indexed .flfs files.
// Persistence is supplied separately by catalog_manifest.hpp; this class owns the live view.
struct CatalogSource {
  std::string virtual_path;  // relative '/'-separated conservative ASCII path
  std::filesystem::path container;
};
struct CatalogEntry {
  std::string path;          // original case as presented to Windows clients
  bool directory = false;
  std::uint64_t logical_bytes = 0;
};

constexpr std::size_t kMaxCatalogFiles = 16;
constexpr std::uint64_t kMaxCatalogIndexBytes = 4ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultCatalogVerificationBytes = 32ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxCatalogVerificationBytes = 256ULL * 1024ULL * 1024ULL;

// Pure namespace validation/case key; never resolves an OS path.
std::string canonical_catalog_path(std::string_view path, bool allow_root = false);

class FlzipReader;
class ReadOnlyCatalog final {
 public:
  // Validate namespace/count collisions without opening any backing file.
  static void validate_layout(const std::vector<CatalogSource>& files);
  static ReadOnlyCatalog create(const std::vector<CatalogSource>& files);
  // Separate, verified immutable FLZIP route; does not change .flcatalog grammar.
  static ReadOnlyCatalog open_flzip(const std::filesystem::path& archive);
  ReadOnlyCatalog(const ReadOnlyCatalog&) = delete;
  ReadOnlyCatalog& operator=(const ReadOnlyCatalog&) = delete;
  ReadOnlyCatalog(ReadOnlyCatalog&&) noexcept = default;
  ReadOnlyCatalog& operator=(ReadOnlyCatalog&&) noexcept = default;

  CatalogEntry stat(std::string_view path) const;
  std::vector<CatalogEntry> list(std::string_view directory) const;
  // Concurrent calls to read() on the same catalog are safe: each backing
  // Reader's mutable chunk cache and Windows file pointer are serialized.
  // The catalog itself must not be moved or destroyed during active calls.
  std::vector<std::byte> read(std::string_view path, std::uint64_t offset, std::uint32_t bytes);

  // Explicitly verifies every byte of every backing container, with a hard
  // TOTAL logical-byte admission budget checked before any decoding begins.
  // This API does not modify archives or write data.
  void verify_all(std::uint64_t max_total_logical_bytes = kDefaultCatalogVerificationBytes);

 private:
  struct Node {
    CatalogEntry entry;
    std::unique_ptr<Reader> reader;
    std::unique_ptr<std::mutex> read_guard;
  };
  static ReadOnlyCatalog namespace_for(const std::vector<CatalogSource>& files);
  ReadOnlyCatalog() = default;
  std::map<std::string, Node> nodes_;
  std::shared_ptr<FlzipReader> flzip_;
  std::unique_ptr<std::mutex> flzip_guard_;
};

}  // namespace flitzfs
