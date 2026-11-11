#include "flitzfs/mount_plan.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}
int main() {
  try {
    flitzfs::ReadOnlyMountPlan plan;
    plan.directory = L"C:\\flitzfs-fixture\\mount";
    require(!flitzfs::valid_mount_plan(plan), "default plan must not authorize a mount");
    plan.allow_driver_start = true;
    require(flitzfs::valid_mount_plan(plan), "ordinary absolute directory rejected");
    for (const auto* path : {L"", L"C:", L"C:\\", L"R:", L"*:", L"relative\\mount",
        L"\\rooted\\mount", L"\\\\server\\share\\mount", L"\\\\?\\C:\\mount",
        L"\\\\.\\PhysicalDrive0", L"C:\\fixture\\mount\\", L"C:/fixture/mount",
        L"C:\\fixture\\.\\mount", L"C:\\fixture\\..\\mount", L"C:\\fixture\\NUL.txt",
        L"C:\\fixture\\mount:stream", L"C:\\fixture\\bad?", L"C:\\fixture\\bad.",
        L"C:\\fixture\\bad ", L"C:\\fixture\\\\mount"}) {
      plan.directory = path;
      require(!flitzfs::valid_mount_plan(plan), "unsafe mount target accepted");
    }
    plan.directory = std::wstring(L"C:\\fixture\\bad\0name", 19);
    require(!flitzfs::valid_mount_plan(plan), "embedded NUL accepted");
    plan.directory = L"C:\\" + std::wstring(238, L'a');
    require(!flitzfs::valid_mount_plan(plan), "overlong mount target accepted");
    plan.directory = L"C:\\fixture\\mount";
    for (const auto seconds : {0U, 31U, 60U, 0xffffffffU}) {
      plan.lifetime_seconds = seconds;
      require(!flitzfs::valid_mount_plan(plan), "unbounded lifetime accepted");
    }
    for (const auto seconds : {1U, 15U, 30U}) {
      plan.lifetime_seconds = seconds;
      require(flitzfs::valid_mount_plan(plan), "valid lifetime rejected");
    }
    require(flitzfs::kMountDispatcherThreads == 2, "dispatcher budget changed");
    std::cout << "Mount plan rejection and admission tests passed; no filesystem operations.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
