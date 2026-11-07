#include "flitzfs/catalog.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flitzfs {
namespace {
[[noreturn]] void reject() { throw std::runtime_error("FlitzFS: invalid or conflicting virtual catalog path"); }

char ascii_lower(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
  return c;
}
std::string key_for(std::string_view path, bool allow_root) {
  if (allow_root && path.empty()) return {};
  if (path.empty() || path.size() > 240 || path[0] == '/' || path.back() == '/') reject();
  std::string key;
  key.reserve(path.size());
  std::size_t pos = 0;
  unsigned parts = 0;
  while (pos < path.size()) {
    const auto stop = path.find('/', pos);
    const auto len = (stop == std::string_view::npos ? path.size() : stop) - pos;
    if (len == 0 || len > 100 || ++parts > 16) reject();
    auto component = path.substr(pos, len);
    if (component == "." || component == ".." || component.back() == '.' || component.back() == ' ') reject();
    std::string stem;
    for (char c : component) {
      const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
          c == ' ' || c == '(' || c == ')';
      if (!allowed) reject();
      if (c == '.') break;
      stem.push_back(ascii_lower(c));
    }
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        stem == "conin$" || stem == "conout$" || stem == "clock$" ||
        (stem.size() == 4 && ((stem.substr(0, 3) == "com") || (stem.substr(0, 3) == "lpt")) &&
         stem[3] >= '1' && stem[3] <= '9')) reject();
    for (const char c : component) key.push_back(ascii_lower(c));
    if (stop == std::string_view::npos) break;
    key.push_back('/');
    pos = stop + 1;
  }
  return key;
}
std::vector<std::string> ancestor_keys(const std::string& key) {
  std::vector<std::string> result;
  for (std::size_t i = 0; i < key.size(); ++i)
    if (key[i] == '/') result.push_back(key.substr(0, i));
  return result;
}
}  // namespace

ReadOnlyCatalog ReadOnlyCatalog::create(const std::vector<CatalogSource>& files) {
  if (files.size() > kMaxCatalogFiles)
    throw std::runtime_error("FlitzFS: catalog exceeds open-handle count budget");
  ReadOnlyCatalog out;
  out.nodes_.emplace("", Node{{"", true, 0}, nullptr});
  // Validate all paths and file/directory collisions BEFORE opening archives.
  for (const auto& file : files) {
    const auto key = key_for(file.virtual_path, false);
    for (const auto& parent : ancestor_keys(key)) {
      const auto display_prefix = file.virtual_path.substr(0, parent.size());
      auto [it, inserted] = out.nodes_.try_emplace(parent, Node{{display_prefix, true, 0}, nullptr});
      if (!inserted && !it->second.entry.directory) reject();
    }
    if (!out.nodes_.emplace(key, Node{{file.virtual_path, false, 0}, nullptr}).second) reject();
  }

  std::uint64_t indexed = 0;
  for (const auto& file : files) {
    auto reader = std::make_unique<Reader>(Reader::open(file.container));
    const auto footprint = reader->info().chunk_count * 32ULL;
    if (footprint > kMaxCatalogIndexBytes - indexed)
      throw std::runtime_error("FlitzFS: catalog index-memory budget exceeded");
    indexed += footprint;
    auto& node = out.nodes_.at(key_for(file.virtual_path, false));
    node.entry.logical_bytes = reader->info().logical_bytes;
    node.reader = std::move(reader);
  }
  return out;
}

CatalogEntry ReadOnlyCatalog::stat(std::string_view path) const {
  const auto key = key_for(path, true);
  const auto it = nodes_.find(key);
  if (it == nodes_.end()) throw std::runtime_error("FlitzFS: virtual entry not found");
  return it->second.entry;
}

std::vector<CatalogEntry> ReadOnlyCatalog::list(std::string_view directory) const {
  const auto key = key_for(directory, true);
  const auto it = nodes_.find(key);
  if (it == nodes_.end() || !it->second.entry.directory)
    throw std::runtime_error("FlitzFS: virtual directory not found");
  std::vector<CatalogEntry> result;
  for (const auto& [entry_key, node] : nodes_) {
    if (entry_key.empty()) continue;
    const auto slash = entry_key.rfind('/');
    const auto parent = slash == std::string::npos ? std::string{} : entry_key.substr(0, slash);
    if (parent == key) result.push_back(node.entry);
  }
  return result;
}

std::vector<std::byte> ReadOnlyCatalog::read(std::string_view path,
                                             std::uint64_t offset, std::uint32_t bytes) {
  const auto key = key_for(path, false);
  const auto it = nodes_.find(key);
  if (it == nodes_.end() || !it->second.reader)
    throw std::runtime_error("FlitzFS: virtual regular file not found");
  return it->second.reader->read(offset, bytes);
}
}  // namespace flitzfs
