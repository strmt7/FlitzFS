#include "flitzfs/catalog_manifest.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/read_only_file.hpp"
#include <charconv>
#include <utility>

namespace flitzfs {
namespace {
constexpr std::string_view header = "FLITZFS-CATALOG-1\t";
[[noreturn]] void reject() {
  throw CatalogError(CatalogErrorCode::invalid_path, "invalid or unsupported FlitzFS catalog manifest");
}
void validate_entries(const std::vector<ManifestEntry>& entries) {
  if (entries.size() > kMaxCatalogFiles)
    throw CatalogError(CatalogErrorCode::limit, "manifest exceeds catalog file-count budget");
  std::vector<CatalogSource> names;
  names.reserve(entries.size());
  for (const auto& entry : entries) {
    const auto& leaf = entry.container_name;
    if (entry.virtual_path.size() > 240 || leaf.size() < 6 || leaf.size() > 100 ||
        leaf.find_first_of("/\\") != std::string::npos) reject();
    constexpr std::string_view extension = ".flfs";
    for (std::size_t i = 0; i < extension.size(); ++i) {
      auto c = leaf[leaf.size() - extension.size() + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != extension[i]) reject();
    }
    // Reuse the canonical path rules, including reserved Windows device names.
    ReadOnlyCatalog::validate_layout({{leaf, {}}});
    names.push_back({entry.virtual_path, {}});
  }
  ReadOnlyCatalog::validate_layout(names);
}
std::string_view next_line(std::string_view text, std::size_t& position) {
  const auto end = text.find('\n', position);
  if (end == text.npos) reject(); // Terminal newline is mandatory, including header.
  auto line = text.substr(position, end - position);
  position = end + 1;
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return line;
}
} // namespace

std::vector<ManifestEntry> parse_catalog_manifest(std::string_view text) {
  if (text.size() > kMaxManifestBytes)
    throw CatalogError(CatalogErrorCode::limit, "manifest exceeds 8 KiB serialized budget");
  std::size_t position = 0;
  const auto first = next_line(text, position);
  if (!first.starts_with(header)) reject();
  const auto count_text = first.substr(header.size());
  if (count_text.empty() || (count_text.size() > 1 && count_text[0] == '0')) reject();
  std::size_t count = 0;
  const auto parsed = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
  if (parsed.ec != std::errc{} || parsed.ptr != count_text.data() + count_text.size() ||
      count > kMaxCatalogFiles) reject();
  std::vector<ManifestEntry> entries;
  entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto line = next_line(text, position);
    const auto tab = line.find('\t');
    if (tab == line.npos || line.find('\t', tab + 1) != line.npos) reject();
    const auto path = line.substr(0, tab);
    const auto leaf = line.substr(tab + 1);
    if (path.empty() || path.size() > 240 || leaf.empty() || leaf.size() > 100) reject();
    entries.push_back({std::string(path), std::string(leaf)});
  }
  if (position != text.size()) reject();
  validate_entries(entries);
  return entries;
}

std::string serialize_catalog_manifest(const std::vector<ManifestEntry>& entries) {
  validate_entries(entries);
  std::string result(header);
  result += std::to_string(entries.size()) + "\n";
  for (const auto& entry : entries)
    result += entry.virtual_path + "\t" + entry.container_name + "\n";
  if (result.size() > kMaxManifestBytes)
    throw CatalogError(CatalogErrorCode::limit, "serialized manifest exceeds budget");
  return result;
}

std::vector<CatalogSource> read_catalog_manifest_sources(const std::filesystem::path& manifest) {
  // Capture CWD once so callers do not receive mutable-CWD-relative bindings.
  const auto absolute = std::filesystem::absolute(manifest);
  auto file = ReadOnlyFile::open(absolute);
  if (file.size() > kMaxManifestBytes)
    throw CatalogError(CatalogErrorCode::limit, "manifest exceeds 8 KiB serialized budget");
  std::string text(static_cast<std::size_t>(file.size()), '\0');
  file.read_exact_at(0, text.data(), text.size());
  const auto entries = parse_catalog_manifest(text);
  std::vector<CatalogSource> result;
  result.reserve(entries.size());
  for (const auto& entry : entries)
    result.push_back({entry.virtual_path, absolute.parent_path() / entry.container_name});
  return result;
}
ReadOnlyCatalog load_catalog_manifest(const std::filesystem::path& manifest) {
  return ReadOnlyCatalog::create(read_catalog_manifest_sources(manifest));
}
} // namespace flitzfs
