#include "flitzfs/projection.hpp"
#include "flitzfs/catalog_error.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace flitzfs {
namespace {
// Validates a **virtual** WinFsp relative name, never an OS path.
// The catalog independently validates component lengths and Windows reserved
// names. No normalization of '..', drives, UNC, or Unicode is permitted.
std::string virtual_path(std::wstring_view name) {
  if (name == L"\\") return {};
  if (name.size() < 2U || name.size() > 241U || name.front() != L'\\')
    throw CatalogError(CatalogErrorCode::invalid_path, "FlitzFS: invalid virtual Windows path boundary");
  std::string out;
  out.reserve(name.size() - 1U);
  for (std::size_t i = 1; i < name.size(); ++i) {
    const auto c = name[i];
    if (c == L'\\') { out.push_back('/'); continue; }
    if (c < 0x20 || c > 0x7e || c == L'/')
      throw CatalogError(CatalogErrorCode::invalid_path, "FlitzFS: unsupported virtual Windows path character");
    out.push_back(static_cast<char>(c));
  }
  return out;
}
} // namespace

ReadOnlyProjection ReadOnlyProjection::create(const std::vector<CatalogSource>& files) {
  return ReadOnlyProjection(ReadOnlyCatalog::create(files));
}
ReadOnlyProjection ReadOnlyProjection::open_flzip(const std::filesystem::path& archive) {
  return ReadOnlyProjection(ReadOnlyCatalog::open_flzip(archive));
}
CatalogEntry ReadOnlyProjection::stat(std::wstring_view winfsp_path) const {
  return catalog_.stat(virtual_path(winfsp_path));
}
std::vector<CatalogEntry> ReadOnlyProjection::list(std::wstring_view winfsp_directory) const {
  return catalog_.list(virtual_path(winfsp_directory));
}
std::vector<std::byte> ReadOnlyProjection::read(std::wstring_view winfsp_path,
                                                 std::uint64_t offset, std::uint32_t length) {
  if (length > kMaxSingleReadBytes)
    throw CatalogError(CatalogErrorCode::limit, "FlitzFS: virtual read exceeds 4 MiB budget");
  auto path = virtual_path(winfsp_path);
  auto entry = catalog_.stat(path);
  if (entry.directory)
    throw CatalogError(CatalogErrorCode::is_directory, "FlitzFS: cannot read a virtual directory");
  if (offset >= entry.logical_bytes || length == 0) return {};
  const auto admitted = static_cast<std::uint32_t>(
      (std::min)(static_cast<std::uint64_t>(length), entry.logical_bytes - offset));
  return catalog_.read(path, offset, admitted);
}
void ReadOnlyProjection::verify_all(std::uint64_t max_total_logical_bytes) {
  catalog_.verify_all(max_total_logical_bytes);
}
} // namespace flitzfs
