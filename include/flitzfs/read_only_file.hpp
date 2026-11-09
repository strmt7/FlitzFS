#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace flitzfs {

// Privately owned, strictly read-only operating-system file handle.
// Opens without following a final symlink/reparse point; validates the opened
// object is a regular disk file rather than trusting a pre-open path check.
// This single-reader building block does not support concurrent reads.
class ReadOnlyFile final {
 public:
  static ReadOnlyFile open(const std::filesystem::path& path);
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
  ReadOnlyFile(ReadOnlyFile&& other) noexcept;
  ReadOnlyFile& operator=(ReadOnlyFile&& other) noexcept;
  ~ReadOnlyFile();

  std::uint64_t size() const noexcept { return size_; }
  void read_exact_at(std::uint64_t offset, void* destination, std::size_t length) const;
  // Pin a cooperating store reader against exclusive writers, without waiting.
  // POSIX advisory flock; Windows already denies write/delete sharing.
  // Refresh size from the same opened handle after successful admission.
  void lock_shared_nonblocking();

 private:
#ifdef _WIN32
  explicit ReadOnlyFile(void* handle, std::uint64_t bytes) noexcept;
  void* handle_ = nullptr;
#else
  explicit ReadOnlyFile(int fd, std::uint64_t bytes) noexcept;
  int fd_ = -1;
#endif
  std::uint64_t size_ = 0;
};

}  // namespace flitzfs
