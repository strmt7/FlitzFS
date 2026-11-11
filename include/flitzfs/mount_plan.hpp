#pragma once
#include <cstdint>
#include <string>

namespace flitzfs {
// A plan is inert data: validating it never queries or mutates the host.
// A true consent flag is necessary, not sufficient, for a live mount.
struct ReadOnlyMountPlan {
  std::wstring directory;
  std::uint32_t lifetime_seconds = 15;
  bool allow_driver_start = false;
};
inline constexpr unsigned kMountDispatcherThreads = 2;
inline constexpr std::uint32_t kMaxMountLifetimeSeconds = 30;
// Accept only bounded, fully-qualified local directory paths. Drive-letter
// aliases, namespaces, UNC, wildcards, streams and traversal are unsupported.
// Existence, parent safety and atomic leaf creation require native checks.
bool valid_mount_plan(const ReadOnlyMountPlan& plan) noexcept;
} // namespace flitzfs
