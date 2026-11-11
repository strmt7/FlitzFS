#include "flitzfs/mount_plan.hpp"
#include "flitzfs/windows_path.hpp"
#include <string_view>

namespace flitzfs {
bool valid_mount_plan(const ReadOnlyMountPlan& plan) noexcept {
  const std::wstring_view path(plan.directory);
  if (!plan.allow_driver_start || !plan.lifetime_seconds ||
      plan.lifetime_seconds > kMaxMountLifetimeSeconds || path.size() < 4 ||
      path.size() > 240 || !ordinary_windows_file_path(path)) return false;
  const auto drive = path[0];
  if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) ||
      path[1] != L':' || path[2] != L'\\') return false;
  if (path.find(L'/') != path.npos) return false;
  std::size_t start = 3;
  while (start < path.size()) {
    const auto stop = path.find(L'\\', start);
    const auto part = path.substr(start, stop == path.npos ? path.size() - start : stop - start);
    if (part.empty() || part == L"." || part == L"..") return false;
    if (stop == path.npos) break;
    start = stop + 1;
  }
  return true;
}
} // namespace flitzfs
