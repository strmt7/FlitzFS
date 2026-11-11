#include "flitzfs/projection.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/native_filesystem.hpp"
#include <mutex>
#include <variant>

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

struct ReadOnlyProjection::Impl {
  std::variant<ReadOnlyCatalog,NativeFilesystemSnapshot> source;
  mutable std::mutex native_guard;
  std::uint64_t image_bytes=0;
  explicit Impl(ReadOnlyCatalog catalog):source(std::move(catalog)){}
  Impl(NativeFilesystemSnapshot snapshot,std::uint64_t bytes):source(std::move(snapshot)),image_bytes(bytes){}
};
ReadOnlyProjection::ReadOnlyProjection(std::unique_ptr<Impl> impl) noexcept:impl_(std::move(impl)){}
ReadOnlyProjection::~ReadOnlyProjection()=default;
ReadOnlyProjection::ReadOnlyProjection(ReadOnlyProjection&&) noexcept=default;
ReadOnlyProjection& ReadOnlyProjection::operator=(ReadOnlyProjection&&) noexcept=default;
ReadOnlyProjection ReadOnlyProjection::create(const std::vector<CatalogSource>& files) {
  return ReadOnlyProjection(std::make_unique<Impl>(ReadOnlyCatalog::create(files)));
}
ReadOnlyProjection ReadOnlyProjection::open_native_file(const std::filesystem::path& image,
    StoreIdentity expected) {
#ifdef _WIN32
  if(expected==StoreIdentity{})throw std::invalid_argument("native projection needs an explicit nonzero volume identity");
  auto objects=ObjectStoreSnapshot::open_file(image);const auto bytes=objects.source_bytes();
  auto native=NativeFilesystemSnapshot::open(std::move(objects));
  if(native.identity()!=expected)throw std::invalid_argument("native projection volume identity mismatch");
  if(native.needs_session_recovery())
    throw std::invalid_argument("native projection requires closed-session file-lifetime recovery");
  // Snapshot open checked all records/references. Retained sharing denies
  // ordinary writer/delete handles; every read still verifies immutable hashes.
  return ReadOnlyProjection(std::make_unique<Impl>(std::move(native),bytes));
#else
  (void)image;(void)expected;
  throw std::invalid_argument("native image projection is qualified on Windows only");
#endif
}
CatalogEntry ReadOnlyProjection::stat(std::wstring_view path) const {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  const auto relative=virtual_path(path);
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    const auto entry=native->stat(relative);
    return {entry.path,entry.directory,entry.logical_bytes,entry.inode};
  }
  return std::get<ReadOnlyCatalog>(impl_->source).stat(relative);
}
std::vector<CatalogEntry> ReadOnlyProjection::list(std::wstring_view path) const {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  const auto relative=virtual_path(path);
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    const auto entries=native->list(relative);
    std::vector<CatalogEntry> result;result.reserve(entries.size());
    for(const auto& entry:entries)
      result.push_back({entry.path,entry.directory,entry.logical_bytes,entry.inode});
    return result;
  }
  return std::get<ReadOnlyCatalog>(impl_->source).list(relative);
}
std::vector<std::byte> ReadOnlyProjection::read(std::wstring_view path,
    std::uint64_t offset,std::uint32_t length) {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  if(length>kMaxSingleReadBytes)
    throw CatalogError(CatalogErrorCode::limit,"FlitzFS: virtual read exceeds 4 MiB budget");
  const auto relative=virtual_path(path);
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    return native->read(relative,offset,length);
  }
  auto& catalog=std::get<ReadOnlyCatalog>(impl_->source);
  const auto entry=catalog.stat(relative);
  if(entry.directory)
    throw CatalogError(CatalogErrorCode::is_directory,"FlitzFS: cannot read a virtual directory");
  if(offset>=entry.logical_bytes || !length)return {};
  const auto admitted=static_cast<std::uint32_t>(
      (std::min)(static_cast<std::uint64_t>(length),entry.logical_bytes-offset));
  return catalog.read(relative,offset,admitted);
}
void ReadOnlyProjection::verify_all(std::uint64_t maximum) {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    if(maximum>kMaxCatalogVerificationBytes || native->storage_info().logical_bytes>maximum)
      throw CatalogError(CatalogErrorCode::limit,"native live decoded verification budget");
    native->verify_all();return;
  }
  std::get<ReadOnlyCatalog>(impl_->source).verify_all(maximum);
}
std::optional<NativeBasicMetadata> ReadOnlyProjection::basic_metadata(std::wstring_view path) const {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  const auto relative=virtual_path(path);
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    return native->basic_metadata(native->stat(relative).inode);
  }
  (void)std::get<ReadOnlyCatalog>(impl_->source).stat(relative);return {};
}
std::optional<NativeSecurityDescriptor> ReadOnlyProjection::security_descriptor(std::wstring_view path) const {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  const auto relative=virtual_path(path);
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    return native->security_descriptor(native->stat(relative).inode);
  }
  (void)std::get<ReadOnlyCatalog>(impl_->source).stat(relative);return {};
}
std::optional<NativeProjectionInfo> ReadOnlyProjection::native_info() const {
  if(!impl_)throw std::logic_error("moved-from read-only projection");
  if(const auto* native=std::get_if<NativeFilesystemSnapshot>(&impl_->source)) {
    std::lock_guard lock(impl_->native_guard);
    return NativeProjectionInfo{native->identity(),native->storage_info(),native->sequence(),impl_->image_bytes};
  }
  return {};
}
} // namespace flitzfs
