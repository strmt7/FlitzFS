#pragma once

#include <winfsp/winfsp.h>
#include "flitzfs/catalog.hpp"
#include "flitzfs/projection.hpp"
#include <memory>
#include <vector>

namespace flitzfs {
// Optional native callback backend. Does not create a filesystem, load a
// driver, assign a drive letter, install a service, or start a dispatcher.
// Owns read-only backing handles. Keep alive until all callbacks have stopped.
class WinFspReadOnly final {
 public:
  // Fully verify at most 32 MiB of logical content before exposing callbacks.
  explicit WinFspReadOnly(const std::vector<CatalogSource>& sources);
  explicit WinFspReadOnly(ReadOnlyProjection projection);
  ~WinFspReadOnly();
  WinFspReadOnly(const WinFspReadOnly&) = delete;
  WinFspReadOnly& operator=(const WinFspReadOnly&) = delete;
  // Bind an existing user-mode struct; this performs no WinFsp operation.
  void bind_callbacks(FSP_FILE_SYSTEM& filesystem) noexcept;
  static const FSP_FILE_SYSTEM_INTERFACE& callbacks() noexcept;
  FSP_FSCTL_VOLUME_PARAMS parameters() const noexcept;
  // Written by the last dispatcher callback; observing does not tear down resources.
  bool dispatcher_stopped() const noexcept;
  bool dispatcher_stopped_unexpectedly() const noexcept;
  static constexpr std::size_t max_open_handles = 256;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace flitzfs
