#include "winfsp_session.hpp"
#include <new>
#include <stdexcept>

namespace flitzfs {
namespace {
WinFspSessionApi checked(WinFspSessionApi api) {
  if (!api.preflight || !api.create || !api.start_dispatcher || !api.set_mount_point ||
      !api.stop_dispatcher || !api.remove_mount_point || !api.delete_filesystem)
    throw std::invalid_argument("incomplete filesystem lifecycle provider");
  return api;
}
}
WinFspReadOnlySession::WinFspReadOnlySession(const std::vector<CatalogSource>& sources,
    WinFspSessionApi api) : api_(checked(api)), adapter_(sources) {}
WinFspReadOnlySession::~WinFspReadOnlySession() { stop(); }

NTSTATUS WinFspReadOnlySession::start(const ReadOnlyMountPlan& plan) noexcept {
  if (state_ != SessionState::ready) return STATUS_INVALID_DEVICE_STATE;
  if (!valid_mount_plan(plan)) return STATUS_INVALID_PARAMETER;
  auto fail = [&](NTSTATUS status) noexcept {
    release();
    state_ = SessionState::failed;
    return status;
  };
  try {
    // All potentially throwing local work precedes the first native operation.
    auto directory = plan.directory;
    auto parameters = adapter_.parameters();
    wchar_t device[] = L"" FSP_FSCTL_DISK_DEVICE_NAME;
    state_ = SessionState::starting;
    auto status = api_.preflight(device, directory.data());
    if (!NT_SUCCESS(status)) return fail(status);
    status = api_.create(device, &parameters, &adapter_.callbacks(), &filesystem_);
    if (!NT_SUCCESS(status)) return fail(status);
    if (!filesystem_) return fail(STATUS_INTERNAL_ERROR);
    adapter_.bind_callbacks(*filesystem_);
    // Start servicing before exposing a namespace; never expose a mount with
    // no dispatcher. A failed start still receives an idempotent stop call.
    dispatcher_attempted_ = true;
    status = api_.start_dispatcher(filesystem_, kMountDispatcherThreads);
    if (!NT_SUCCESS(status)) return fail(status);
    if (adapter_.dispatcher_stopped()) return fail(STATUS_DEVICE_NOT_READY);
    status = api_.set_mount_point(filesystem_, directory.data());
    if (!NT_SUCCESS(status)) return fail(status);
    mount_owned_ = true;
    if (adapter_.dispatcher_stopped()) return fail(STATUS_DEVICE_NOT_READY);
    state_ = SessionState::running;
    return STATUS_SUCCESS;
  } catch (const std::bad_alloc&) { return fail(STATUS_INSUFFICIENT_RESOURCES); }
  catch (...) { return fail(STATUS_INTERNAL_ERROR); }
}
void WinFspReadOnlySession::release() noexcept {
  if (!filesystem_) return;
  if (dispatcher_attempted_) api_.stop_dispatcher(filesystem_);
  if (mount_owned_) api_.remove_mount_point(filesystem_);
  api_.delete_filesystem(filesystem_);
  filesystem_ = nullptr;
  dispatcher_attempted_ = false;
  mount_owned_ = false;
}
void WinFspReadOnlySession::stop() noexcept {
  release();
  if (state_ != SessionState::failed) state_ = SessionState::stopped;
}
bool WinFspReadOnlySession::dispatcher_stopped() const noexcept {
  return adapter_.dispatcher_stopped();
}
bool WinFspReadOnlySession::dispatcher_stopped_unexpectedly() const noexcept {
  return adapter_.dispatcher_stopped_unexpectedly();
}
} // namespace flitzfs
