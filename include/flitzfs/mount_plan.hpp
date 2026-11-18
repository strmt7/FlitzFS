#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace flitzfs {
// A plan is inert data: validating it never queries or mutates the host.
// A true consent flag is necessary, not sufficient, for a live mount.
struct ReadOnlyMountPlan {
  std::wstring directory;
  // Finite lifetime declaration for supervised read-only trials.
  std::uint32_t lifetime_seconds = 15;
  bool allow_driver_start = false;
};
inline constexpr unsigned kMountDispatcherThreads = 2;
// Accept only bounded, fully-qualified local directory paths. Drive-letter
// aliases, namespaces, UNC, wildcards, streams and traversal are unsupported.
// Existence, parent safety and atomic leaf creation require native checks.
bool valid_mount_directory(std::wstring_view path) noexcept;
bool valid_mount_plan(const ReadOnlyMountPlan& plan) noexcept;
} // namespace flitzfs
