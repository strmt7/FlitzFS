#pragma once

#include <utility>
#include "flitzfs/catalog.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace flitzfs {

// A pure C++ adapter for prospective WinFsp read-only callbacks.
// DOES NOT include or load WinFsp, mount any volume, or write to storage.
// Accepts root-relative Windows-style paths with one leading backslash.
// ASCII-only by design; unsafe, ambiguous, or unsupported paths fail closed.
class ReadOnlyProjection final {
 public:
  static ReadOnlyProjection create(const std::vector<CatalogSource>& files);
  ReadOnlyProjection(const ReadOnlyProjection&) = delete;
  ReadOnlyProjection& operator=(const ReadOnlyProjection&) = delete;
  ReadOnlyProjection(ReadOnlyProjection&&) noexcept = default;
  ReadOnlyProjection& operator=(ReadOnlyProjection&&) noexcept = default;

  CatalogEntry stat(std::wstring_view winfsp_path) const;
  std::vector<CatalogEntry> list(std::wstring_view winfsp_directory) const;

  // Mimics normal short-read behavior: when offset >= EOF return zero bytes;
  // a request spanning EOF returns only remaining bytes. Requests must be
  // within the 4 MiB per-call bound even at EOF. The projection must not be
  // moved/destroyed while its operations are active.
  std::vector<std::byte> read(std::wstring_view winfsp_path,
                              std::uint64_t offset, std::uint32_t length);

  void verify_all(std::uint64_t max_total_logical_bytes = kDefaultCatalogVerificationBytes);

 private:
  explicit ReadOnlyProjection(ReadOnlyCatalog catalog) : catalog_(std::move(catalog)) {}
  ReadOnlyCatalog catalog_;
};

} // namespace flitzfs
