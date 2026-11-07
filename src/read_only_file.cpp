#include "flitzfs/read_only_file.hpp"
#include "flitzfs/windows_path.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#else
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace flitzfs {
namespace {
[[noreturn]] void fail(const char* reason) {
  throw std::runtime_error(std::string("FlitzFS: ") + reason);
}
}

#ifdef _WIN32
ReadOnlyFile::ReadOnlyFile(void* h, std::uint64_t n) noexcept : handle_(h), size_(n) {}
ReadOnlyFile ReadOnlyFile::open(const std::filesystem::path& path) {
  if (path.empty() || path.filename().empty()) fail("read path must refer to a file");
  if (!ordinary_windows_file_path(path.native()))
    fail("unsafe Windows file path rejected before OS open");

  // No write/delete access. Reparse points remain reparse points at the opened handle.
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail("unable to open existing read-only file");
  FILE_ATTRIBUTE_TAG_INFO attr{};
  LARGE_INTEGER sz{};
  const bool valid = GetFileType(h) == FILE_TYPE_DISK &&
      GetFileInformationByHandleEx(h, FileAttributeTagInfo, &attr, sizeof(attr)) != 0 &&
      (attr.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
      GetFileSizeEx(h, &sz) != 0 && sz.QuadPart >= 0;
  if (!valid) { CloseHandle(h); fail("input handle is not an ordinary regular file"); }
  return ReadOnlyFile(h, static_cast<std::uint64_t>(sz.QuadPart));
}
ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), size_(std::exchange(other.size_, 0)) {}
ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = std::exchange(other.handle_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}
ReadOnlyFile::~ReadOnlyFile() { if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_)); }

void ReadOnlyFile::read_exact_at(std::uint64_t offset, void* dst, std::size_t length) const {
  if (offset > size_ || length > size_ - offset) fail("read is outside the pinned file length");
  if (length != 0 && dst == nullptr) fail("null destination");
  auto* p = static_cast<std::uint8_t*>(dst);
  while (length != 0) {
    LARGE_INTEGER pos{};
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()))
      fail("read offset too large");
    pos.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), pos, nullptr, FILE_BEGIN)) fail("seek failed");
    const DWORD request = static_cast<DWORD>((std::min)(length, std::size_t{0x7ffff000}));
    DWORD actual = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), p, request, &actual, nullptr) || actual == 0)
      fail("short read or modified backing file");
    p += actual;
    offset += actual;
    length -= actual;
  }
}
#else
ReadOnlyFile::ReadOnlyFile(int fd, std::uint64_t n) noexcept : fd_(fd), size_(n) {}
ReadOnlyFile ReadOnlyFile::open(const std::filesystem::path& path) {
  if (path.empty() || path.filename().empty()) fail("read path must refer to a file");
  if (path.native().find(char(0)) != std::string::npos) fail("embedded NUL in read path");
  // O_NONBLOCK avoids an attacker-controlled FIFO/other special file freezing open().
  // O_NOFOLLOW validates the exact final path component at open time.
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) fail("unable to open ordinary read-only file without following links");
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    ::close(fd);
    fail("opened file is not an ordinary regular file");
  }
  return ReadOnlyFile(fd, static_cast<std::uint64_t>(st.st_size));
}
ReadOnlyFile::ReadOnlyFile(ReadOnlyFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), size_(std::exchange(other.size_, 0)) {}
ReadOnlyFile& ReadOnlyFile::operator=(ReadOnlyFile&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = std::exchange(other.fd_, -1);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}
ReadOnlyFile::~ReadOnlyFile() { if (fd_ >= 0) ::close(fd_); }

void ReadOnlyFile::read_exact_at(std::uint64_t offset, void* dst, std::size_t length) const {
  if (offset > size_ || length > size_ - offset) fail("read is outside the pinned file length");
  if (length != 0 && dst == nullptr) fail("null destination");
  auto* p = static_cast<std::uint8_t*>(dst);
  while (length != 0) {
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()))
      fail("read offset too large");
    const auto request = (std::min)(length, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
    const auto received = ::pread(fd_, p, request, static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) fail("short read or modified backing file");
    const auto used = static_cast<std::size_t>(received);
    p += used;
    offset += used;
    length -= used;
  }
}
#endif
}  // namespace flitzfs
