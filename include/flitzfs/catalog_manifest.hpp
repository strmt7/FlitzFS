#pragma once
#include "flitzfs/catalog.hpp"

namespace flitzfs {
// Experimental, plaintext configuration. No authentication, encryption or writes.
inline constexpr std::size_t kMaxManifestBytes = 8192;
struct ManifestEntry {
  std::string virtual_path;
  std::string container_name; // one sibling .flfs filename, never a path
  bool operator==(const ManifestEntry&) const = default;
};
// Pure bounded parsing/serialization. Validate the complete namespace before I/O.
std::vector<ManifestEntry> parse_catalog_manifest(std::string_view text);
std::string serialize_catalog_manifest(const std::vector<ManifestEntry>& entries);
// Read a regular manifest, then bind leaf names to its absolute parent directory.
// Final symlinks/reparse points are rejected by the underlying readers. Parents
// must be trusted; this is not an adversarial namespace/hardlink sandbox.
std::vector<CatalogSource> read_catalog_manifest_sources(const std::filesystem::path& manifest);
ReadOnlyCatalog load_catalog_manifest(const std::filesystem::path& manifest);
} // namespace flitzfs
