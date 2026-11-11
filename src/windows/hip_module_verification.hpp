#pragma once
#include "hip_worker_exit.hpp"
#include "flitzfs/read_only_file.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace flitzfs::hip_detail {
struct DirectoryPin {
  explicit DirectoryPin(const std::filesystem::path& path)
      : handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
        !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      throw HipWorkerFailure(HipWorkerExit::runtime_unavailable, "unsafe module directory");
    }
  }
  ~DirectoryPin() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  DirectoryPin(DirectoryPin&& other) noexcept
      : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
  DirectoryPin(const DirectoryPin&) = delete;
  DirectoryPin& operator=(const DirectoryPin&) = delete;
  HANDLE handle;
};

// The production constructor always supplies Windows' real verifier. The
// typed callback also permits CPU tests of every trust/close status.
bool verify_cached_signature(const std::filesystem::path& path,
    decltype(&WinVerifyTrust) verify) noexcept;
bool valid_sha256(std::string_view expected) noexcept;
bool matches_sha256(const ReadOnlyFile& file, std::string_view expected);

// Metadata only. This class never loads the subject or initializes a GPU.
// Directory and read-only file handles remain owned for its entire lifetime.
class VerifiedSystemModule final {
 public:
  VerifiedSystemModule(std::wstring_view basename, std::string_view expected_sha256);
  VerifiedSystemModule(const VerifiedSystemModule&) = delete;
  VerifiedSystemModule& operator=(const VerifiedSystemModule&) = delete;
  VerifiedSystemModule(VerifiedSystemModule&&) = delete;
  VerifiedSystemModule& operator=(VerifiedSystemModule&&) = delete;
  const std::filesystem::path& path() const noexcept { return path_; }
  std::uint64_t size() const noexcept { return file_ ? file_->size() : 0; }
 private:
  std::filesystem::path path_;
  std::vector<DirectoryPin> directories_;
  std::optional<ReadOnlyFile> file_;
};
}
