#include "winfsp_readonly.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/directory_order.hpp"
#include "flitzfs/projection.hpp"

#include <sddl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {
namespace {
// Convert all C++ failures to native statuses; never cross a C ABI by throwing.
template<class Function> NTSTATUS boundary(Function&& run) noexcept {
  try { return run(); }
  catch (const CatalogError& e) {
    switch (e.code()) {
      case CatalogErrorCode::invalid_path: return STATUS_OBJECT_NAME_INVALID;
      case CatalogErrorCode::not_found: return STATUS_OBJECT_NAME_NOT_FOUND;
      case CatalogErrorCode::not_directory: return STATUS_NOT_A_DIRECTORY;
      case CatalogErrorCode::is_directory: return STATUS_FILE_IS_A_DIRECTORY;
      case CatalogErrorCode::limit: return STATUS_INSUFFICIENT_RESOURCES;
    }
  } catch (const std::bad_alloc&) { return STATUS_INSUFFICIENT_RESOURCES; }
  catch (...) { return STATUS_DATA_ERROR; }
  return STATUS_DATA_ERROR;
}
// Kernel-supplied strings are NUL-terminated; additionally cap scans and copies.
std::wstring_view bounded_name(PCWSTR value, std::size_t maximum = 241) {
  if (!value) throw CatalogError(CatalogErrorCode::invalid_path, "null virtual name");
  for (std::size_t i = 0; i <= maximum; ++i)
    if (value[i] == 0) return {value, i};
  throw CatalogError(CatalogErrorCode::invalid_path, "virtual name exceeds limit");
}
std::string fold(std::string value) {
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return value;
}
std::wstring windows_path(const std::string& path) {
  std::wstring result = L"\\";
  for (const auto c : path) result.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
  return result;
}
struct TokenHandle { HANDLE value = nullptr; ~TokenHandle() { if (value) CloseHandle(value); } };
struct LocalMemory { void* value = nullptr; ~LocalMemory() { if (value) LocalFree(value); } };
// Build one protected read/execute ACE for the process user; never a null DACL.
std::vector<std::byte> owner_descriptor() {
  TokenHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
    throw std::runtime_error("cannot query process identity");
  DWORD length = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &length);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 16 * 1024)
    throw std::runtime_error("invalid token information size");
  std::vector<std::byte> bytes(length);
  if (!GetTokenInformation(token.value, TokenUser, bytes.data(), length, &length))
    throw std::runtime_error("cannot read process identity");
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid_text))
    throw std::runtime_error("cannot encode process SID");
  LocalMemory sid{sid_text};
  const std::wstring identity(sid_text);
  const auto sddl = L"O:" + identity + L"G:" + identity + L"D:P(A;;FRFX;;;" + identity + L")";
  LocalMemory descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
        &descriptor.value, &length) || length > 4096 ||
      !IsValidSecurityDescriptor(descriptor.value))
    throw std::runtime_error("cannot create owner-only descriptor");
  const auto* begin = static_cast<const std::byte*>(descriptor.value);
  return {begin, begin + length};
}
} // namespace

struct WinFspReadOnly::Impl {
  struct Node {
    std::wstring path;
    std::string name;
    FSP_FSCTL_FILE_INFO info{};
    const Node* parent = nullptr;
    std::vector<const Node*> children;
  };
  struct Slot { std::uintptr_t token = 0; const Node* node = nullptr; };
  ReadOnlyProjection projection;
  std::vector<std::byte> security;
  std::map<std::string, Node> nodes;
  std::array<Slot, max_open_handles> handles{};
  std::mutex handle_guard;
  std::uintptr_t next_token = 1;
  std::atomic<unsigned> active_reads{0};
  UINT64 timestamp = 0;
  UINT64 total_bytes = 0;

  explicit Impl(const std::vector<CatalogSource>& sources)
      : projection(ReadOnlyProjection::create(sources)), security(owner_descriptor()) {
    projection.verify_all(kDefaultCatalogVerificationBytes);
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    timestamp = (static_cast<UINT64>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
    add_node(projection.stat(L"\\"), nullptr);
  }
  // Fixed catalog limits bound the recursion, nodes and child vectors.
  void add_node(const CatalogEntry& entry, Node* parent) {
    if (nodes.size() >= 257) throw CatalogError(CatalogErrorCode::limit, "node budget");
    Node node;
    node.path = windows_path(entry.path);
    const auto slash = entry.path.rfind('/');
    node.name = entry.path.substr(slash == std::string::npos ? 0 : slash + 1);
    node.parent = parent;
    node.info.FileAttributes = FILE_ATTRIBUTE_READONLY |
        (entry.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    node.info.FileSize = entry.logical_bytes;
    node.info.AllocationSize = (entry.logical_bytes + 4095ULL) & ~4095ULL;
    node.info.CreationTime = node.info.LastAccessTime = timestamp;
    node.info.LastWriteTime = node.info.ChangeTime = timestamp;
    node.info.IndexNumber = nodes.size() + 1;
    total_bytes += node.info.AllocationSize;
    auto [it, inserted] = nodes.emplace(fold(entry.path), std::move(node));
    if (!inserted) throw std::runtime_error("duplicate adapter node");
    if (parent) parent->children.push_back(&it->second);
    if (entry.directory)
      for (const auto& child : projection.list(it->second.path)) add_node(child, &it->second);
  }
  static Impl& self(FSP_FILE_SYSTEM* fs) {
    if (!fs || !fs->UserContext) throw std::runtime_error("unbound callback context");
    return *static_cast<Impl*>(fs->UserContext);
  }
  const Node& lookup(PWSTR path) {
    const auto entry = projection.stat(bounded_name(path));
    return nodes.at(fold(entry.path));
  }
  const Node* resolve(PVOID context) {
    const auto token = reinterpret_cast<std::uintptr_t>(context);
    std::lock_guard guard(handle_guard);
    if (token) for (const auto& slot : handles) if (slot.token == token) return slot.node;
    return nullptr;
  }
  NTSTATUS security_copy(PSECURITY_DESCRIPTOR destination, SIZE_T* size) const {
    if (!size) return destination ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
    const auto capacity = *size;
    *size = security.size();
    if (!destination || capacity < security.size()) return STATUS_BUFFER_OVERFLOW;
    std::memcpy(destination, security.data(), security.size());
    return STATUS_SUCCESS;
  }
  static NTSTATUS volume(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* info) noexcept {
    return boundary([&]() -> NTSTATUS {
      if (!info) return STATUS_INVALID_PARAMETER;
      auto& s = self(fs);
      *info = {};
      info->TotalSize = (std::max)(s.total_bytes, UINT64{4096});
      constexpr wchar_t label[] = L"FlitzFS read-only";
      info->VolumeLabelLength = sizeof(label) - sizeof(wchar_t);
      std::memcpy(info->VolumeLabel, label, sizeof(label));
      return STATUS_SUCCESS;
    });
  }
  static NTSTATUS security_by_name(FSP_FILE_SYSTEM* fs, PWSTR name, PUINT32 attributes,
      PSECURITY_DESCRIPTOR descriptor, SIZE_T* size) noexcept {
    return boundary([&]() -> NTSTATUS {
      auto& s = self(fs);
      const auto& node = s.lookup(name);
      if (attributes) *attributes = node.info.FileAttributes;
      return s.security_copy(descriptor, size);
    });
  }
  static NTSTATUS open(FSP_FILE_SYSTEM* fs, PWSTR name, UINT32 options, UINT32 access,
                        PVOID* context, FSP_FSCTL_FILE_INFO* info) noexcept {
    if (context) *context = nullptr;
    if (info) *info = {};
    return boundary([&]() -> NTSTATUS {
      if (!context || !info) return STATUS_INVALID_PARAMETER;
      constexpr UINT32 writes = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
          FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER |
          GENERIC_WRITE | GENERIC_ALL;
      if ((access & writes) || (options & FILE_DELETE_ON_CLOSE)) return STATUS_MEDIA_WRITE_PROTECTED;
      auto& s = self(fs);
      const auto& node = s.lookup(name);
      const bool directory = (node.info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if ((options & FILE_DIRECTORY_FILE) && !directory) return STATUS_NOT_A_DIRECTORY;
      if ((options & FILE_NON_DIRECTORY_FILE) && directory) return STATUS_FILE_IS_A_DIRECTORY;
      std::lock_guard guard(s.handle_guard);
      if (s.next_token == (std::numeric_limits<std::uintptr_t>::max)()) return STATUS_INSUFFICIENT_RESOURCES;
      for (auto& slot : s.handles) if (slot.token == 0) {
        slot = {s.next_token++, &node};
        *context = reinterpret_cast<PVOID>(slot.token);
        *info = node.info;
        return STATUS_SUCCESS;
      }
      return STATUS_INSUFFICIENT_RESOURCES;
    });
  }
  static VOID close(FSP_FILE_SYSTEM* fs, PVOID context) noexcept {
    (void)boundary([&]() -> NTSTATUS {
      auto& s = self(fs);
      const auto token = reinterpret_cast<std::uintptr_t>(context);
      std::lock_guard guard(s.handle_guard);
      for (auto& slot : s.handles) if (token && slot.token == token) { slot = {}; break; }
      return STATUS_SUCCESS;
    });
  }
  static NTSTATUS file_info(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* info) noexcept {
    return boundary([&]() -> NTSTATUS {
      if (!info) return STATUS_INVALID_PARAMETER;
      const auto* node = self(fs).resolve(context);
      if (!node) return STATUS_INVALID_HANDLE;
      *info = node->info;
      return STATUS_SUCCESS;
    });
  }
  static NTSTATUS get_security(FSP_FILE_SYSTEM* fs, PVOID context,
                               PSECURITY_DESCRIPTOR descriptor, SIZE_T* size) noexcept {
    return boundary([&]() -> NTSTATUS {
      if (!size) return STATUS_INVALID_PARAMETER;
      auto& s = self(fs);
      if (!s.resolve(context)) return STATUS_INVALID_HANDLE;
      return s.security_copy(descriptor, size);
    });
  }
  static NTSTATUS read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer,
                       UINT64 offset, ULONG length, PULONG transferred) noexcept {
    if (transferred) *transferred = 0;
    return boundary([&]() -> NTSTATUS {
      if (!transferred || (!buffer && length) || length > kMaxSingleReadBytes)
        return STATUS_INVALID_PARAMETER;
      auto& s = self(fs);
      const auto* node = s.resolve(context);
      if (!node) return STATUS_INVALID_HANDLE;
      if (node->info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) return STATUS_FILE_IS_A_DIRECTORY;
      if (!length) return STATUS_SUCCESS;
      if (offset >= node->info.FileSize) return STATUS_END_OF_FILE;
      auto count = s.active_reads.load();
      do { if (count >= 2) return STATUS_DEVICE_BUSY; }
      while (!s.active_reads.compare_exchange_weak(count, count + 1));
      struct Release { std::atomic<unsigned>& n; ~Release() { n.fetch_sub(1); } } release{s.active_reads};
      const auto bytes = s.projection.read(node->path, offset, length);
      std::memcpy(buffer, bytes.data(), bytes.size());
      *transferred = static_cast<ULONG>(bytes.size());
      return STATUS_SUCCESS;
    });
  }
  static NTSTATUS directory(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR /*pattern*/, PWSTR marker,
                            PVOID buffer, ULONG length, PULONG transferred) noexcept {
    if (transferred) *transferred = 0;
    return boundary([&]() -> NTSTATUS {
      if (!transferred || (!buffer && length) || length > kMaxSingleReadBytes)
        return STATUS_INVALID_PARAMETER;
      const auto* node = self(fs).resolve(context);
      if (!node) return STATUS_INVALID_HANDLE;
      if (!(node->info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return STATUS_NOT_A_DIRECTORY;
      std::string after;
      if (marker) for (auto c : bounded_name(marker, 100)) {
        if (c < 0x20 || c > 0x7e) return STATUS_OBJECT_NAME_INVALID;
        after.push_back(static_cast<char>(c));
      }
      after = fold(after);
      auto append = [&](const Node& value, const std::string& name) {
        if (marker && !directory_name_after(name, after)) return true;
        alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte, sizeof(FSP_FSCTL_DIR_INFO) + 200> scratch{};
        auto* item = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(scratch.data());
        item->Size = static_cast<UINT16>(sizeof(FSP_FSCTL_DIR_INFO) + name.size() * sizeof(wchar_t));
        item->FileInfo = value.info;
        for (std::size_t i = 0; i < name.size(); ++i) item->FileNameBuf[i] = static_cast<wchar_t>(name[i]);
        return FspFileSystemAddDirInfo(item, buffer, length, transferred) != FALSE;
      };
      const auto full = [&]() { return *transferred ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW; };
      if (node->parent && (!append(*node, ".") || !append(*node->parent, ".."))) return full();
      for (const auto* child : node->children) if (!append(*child, child->name)) return full();
      if (!FspFileSystemAddDirInfo(nullptr, buffer, length, transferred)) return full();
      return STATUS_SUCCESS;
    });
  }
};

WinFspReadOnly::WinFspReadOnly(const std::vector<CatalogSource>& sources) : impl_(std::make_unique<Impl>(sources)) {}
WinFspReadOnly::~WinFspReadOnly() = default;
void WinFspReadOnly::bind_callbacks(FSP_FILE_SYSTEM& fs) noexcept { fs.UserContext = impl_.get(); }
FSP_FSCTL_VOLUME_PARAMS WinFspReadOnly::parameters() const noexcept {
  FSP_FSCTL_VOLUME_PARAMS result{};
  result.Version = sizeof(result);
  result.SectorSize = 512;
  result.SectorsPerAllocationUnit = 8;
  result.MaxComponentLength = 100; // v2.1 sys/volinfo.c exports this as character count
  result.VolumeCreationTime = impl_->timestamp;
  result.VolumeSerialNumber = 0x464c4653;
  result.IrpCapacity = 100; // documented WinFsp minimum
  result.FileInfoTimeout = 1000;
  result.CasePreservedNames = 1;
  result.PersistentAcls = 1;
  result.ReadOnlyVolume = 1;
  result.UmFileContextIsUserContext2 = 1;
  constexpr wchar_t name[] = L"FlitzFS";
  std::memcpy(result.FileSystemName, name, sizeof(name));
  return result;
}
const FSP_FILE_SYSTEM_INTERFACE& WinFspReadOnly::callbacks() noexcept {
  static const auto table = [] {
    FSP_FILE_SYSTEM_INTERFACE result{};
    result.GetVolumeInfo = Impl::volume;
    result.GetSecurityByName = Impl::security_by_name;
    result.Open = Impl::open;
    result.Close = Impl::close;
    result.Read = Impl::read;
    result.GetFileInfo = Impl::file_info;
    result.GetSecurity = Impl::get_security;
    result.ReadDirectory = Impl::directory;
    // Cleanup is NOT Close: cached/paging reads can arrive afterwards.
    result.Cleanup = [](FSP_FILE_SYSTEM*, PVOID, PWSTR, ULONG) {};
    result.Flush = [](FSP_FILE_SYSTEM* fs, PVOID ctx, FSP_FSCTL_FILE_INFO* info) -> NTSTATUS {
      return ctx ? Impl::file_info(fs, ctx, info) : STATUS_SUCCESS;
    };
    result.SetVolumeLabel = [](FSP_FILE_SYSTEM*, PWSTR, FSP_FSCTL_VOLUME_INFO*) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.Create = [](FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32, PSECURITY_DESCRIPTOR,
        UINT64, PVOID* ctx, FSP_FSCTL_FILE_INFO*) -> NTSTATUS {
      if (ctx) *ctx = nullptr;
      return STATUS_MEDIA_WRITE_PROTECTED;
    };
    result.Overwrite = [](FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.Write = [](FSP_FILE_SYSTEM*, PVOID, PVOID, UINT64, ULONG, BOOLEAN, BOOLEAN, PULONG n,
        FSP_FSCTL_FILE_INFO*) -> NTSTATUS {
      if (n) *n = 0;
      return STATUS_MEDIA_WRITE_PROTECTED;
    };
    result.SetBasicInfo = [](FSP_FILE_SYSTEM*, PVOID, UINT32, UINT64, UINT64, UINT64, UINT64, FSP_FSCTL_FILE_INFO*) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.SetFileSize = [](FSP_FILE_SYSTEM*, PVOID, UINT64, BOOLEAN, FSP_FSCTL_FILE_INFO*) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.CanDelete = [](FSP_FILE_SYSTEM*, PVOID, PWSTR) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.SetDelete = [](FSP_FILE_SYSTEM*, PVOID, PWSTR, BOOLEAN) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.Rename = [](FSP_FILE_SYSTEM*, PVOID, PWSTR, PWSTR, BOOLEAN) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    result.SetSecurity = [](FSP_FILE_SYSTEM*, PVOID, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR) -> NTSTATUS { return STATUS_MEDIA_WRITE_PROTECTED; };
    return result;
  }();
  return table;
}
} // namespace flitzfs
