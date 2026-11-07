#pragma once

#include "flitzfs/container.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {

// In-memory, read-only directory view over independently indexed .flfs files.
// It is NOT a mounted filesystem and intentionally has no persistent manifest.
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

class ReadOnlyCatalog final {
 public:
  static ReadOnlyCatalog create(const std::vector<CatalogSource>& files);
  ReadOnlyCatalog(const ReadOnlyCatalog&) = delete;
  ReadOnlyCatalog& operator=(const ReadOnlyCatalog&) = delete;
  ReadOnlyCatalog(ReadOnlyCatalog&&) noexcept = default;
  ReadOnlyCatalog& operator=(ReadOnlyCatalog&&) noexcept = default;

  CatalogEntry stat(std::string_view path) const;
  std::vector<CatalogEntry> list(std::string_view directory) const;
  std::vector<std::byte> read(std::string_view path, std::uint64_t offset, std::uint32_t bytes);

 private:
  struct Node {
    CatalogEntry entry;
    std::unique_ptr<Reader> reader;
  };
  ReadOnlyCatalog() = default;
  std::map<std::string, Node> nodes_;
};

}  // namespace flitzfs
