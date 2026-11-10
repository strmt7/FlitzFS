#pragma once
#include "flitzfs/catalog.hpp"
#include "flitzfs/object_store.hpp"
#include "flitzfs/native_filesystem.hpp"
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace flitzfs {
struct NativeProjectionInfo {
  StoreIdentity identity{};
  StoreCommitInfo storage;
  std::uint64_t namespace_sequence=0;
  std::uint64_t image_bytes=0; // Full backing logical length, not allocated clusters.
};
// Windows virtual namespace only. No driver, dispatcher, mount or write.
// Accepts one leading backslash and existing conservative ASCII path rules.
class ReadOnlyProjection final {
 public:
  static ReadOnlyProjection create(const std::vector<CatalogSource>& files);
  // Windows-only native image admission: existing regular file, read-only
  // pinned handle, exact nonzero identity, full CPU namespace/content checks.
  // Deny write/delete sharing on ordinary handles for this lifetime. Integrity
  // checks still guard every read; this is not a freeze of pre-existing mappings.
  // No caller-owned mutable backend/snapshot can enter the callback route.
  static ReadOnlyProjection open_native_file(const std::filesystem::path& image,
      StoreIdentity expected_identity);
  ~ReadOnlyProjection();
  ReadOnlyProjection(const ReadOnlyProjection&)=delete;
  ReadOnlyProjection& operator=(const ReadOnlyProjection&)=delete;
  ReadOnlyProjection(ReadOnlyProjection&&) noexcept;
  ReadOnlyProjection& operator=(ReadOnlyProjection&&) noexcept;

  CatalogEntry stat(std::wstring_view winfsp_path) const;
  std::vector<CatalogEntry> list(std::wstring_view winfsp_directory) const;
  // Concurrent calls are supported. Native image operations share one mutex
  // around the underlying single-control-thread positional reader/decoder.
  // Full private read/integrity verification precedes caller-byte exposure.
  // 4 MiB per call, short at EOF, sparse holes stay bounded zero reads.
  std::vector<std::byte> read(std::wstring_view winfsp_path,
      std::uint64_t offset,std::uint32_t length);
  // Catalog: total logical content. Native: decoded LIVE objects, including
  // metadata; never scan/materialize logical sparse holes. Hard store caps stay.
  void verify_all(std::uint64_t max_verification_bytes=kDefaultCatalogVerificationBytes);
  // Metadata for the helper/adapter; none for a legacy read-only catalog.
  std::optional<NativeProjectionInfo> native_info() const;
  std::optional<NativeSecurityDescriptor> security_descriptor(std::wstring_view path) const;
  // Explicit persisted tuple only. Legacy catalogs and unstamped native nodes
  // have no such tuple; paths are still validated/resolved in either case.
  std::optional<NativeBasicMetadata> basic_metadata(std::wstring_view path) const;
 private:
  struct Impl;
  explicit ReadOnlyProjection(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
