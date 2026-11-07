#pragma once
#include "winfsp_readonly.hpp"
#include "flitzfs/mount_plan.hpp"

namespace flitzfs {
// Explicit dependency injection: normal tests pass fake operations. No live
// operation table is selected implicitly by construction or by a default arg.
// Providers must implement the WinFsp ABI, never throw, and join callbacks in
// stop_dispatcher. delete_filesystem must accept only a provider-owned object.
struct WinFspSessionApi {
  decltype(&FspFileSystemPreflight) preflight = nullptr;
  decltype(&FspFileSystemCreate) create = nullptr;
  decltype(&FspFileSystemStartDispatcher) start_dispatcher = nullptr;
  decltype(&FspFileSystemSetMountPoint) set_mount_point = nullptr;
  decltype(&FspFileSystemStopDispatcher) stop_dispatcher = nullptr;
  decltype(&FspFileSystemRemoveMountPoint) remove_mount_point = nullptr;
  decltype(&FspFileSystemDelete) delete_filesystem = nullptr;
};
enum class SessionState { ready, starting, running, stopped, failed };

// Single controlling thread; callbacks may run concurrently. Destruction
// stops/joins the dispatcher before the adapter or its backing handles die.
// A native stop can block inside WinFsp: use an external process supervisor
// for live experiments. A plan lifetime is NOT a kernel shutdown guarantee.
class WinFspReadOnlySession final {
 public:
  WinFspReadOnlySession(const std::vector<CatalogSource>& sources, WinFspSessionApi api);
  WinFspReadOnlySession(ReadOnlyProjection projection, WinFspSessionApi api);
  ~WinFspReadOnlySession();
  WinFspReadOnlySession(const WinFspReadOnlySession&) = delete;
  WinFspReadOnlySession& operator=(const WinFspReadOnlySession&) = delete;
  // Start once, or fail closed. Invalid plans make no provider calls. Runtime
  // failures roll back owned resources and permanently consume the session.
  NTSTATUS start(const ReadOnlyMountPlan& plan) noexcept;
  void stop() noexcept;
  SessionState state() const noexcept { return state_; }
  bool dispatcher_stopped() const noexcept;
  bool dispatcher_stopped_unexpectedly() const noexcept;
 private:
  void release() noexcept;
  WinFspSessionApi api_;
  WinFspReadOnly adapter_;
  FSP_FILE_SYSTEM* filesystem_ = nullptr;
  SessionState state_ = SessionState::ready;
  bool dispatcher_attempted_ = false;
  bool mount_owned_ = false;
};
} // namespace flitzfs
