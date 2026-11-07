// Disposable hosted-CI integration probe, NOT a general-purpose mount command.
#include "winfsp_session.hpp"
#include "bounded_process.hpp"
#include "flitzfs/container.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <utility>

namespace {
namespace fs = std::filesystem;
unsigned stage = 1;
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void checked(BOOL ok, const char* message) {
  if (!ok) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(other.value) { other.value = INVALID_HANDLE_VALUE; }
  void reset() noexcept {
    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    value = INVALID_HANDLE_VALUE;
  }
};
std::wstring environment(const wchar_t* name) {
  wchar_t value[512]{};
  const auto n = GetEnvironmentVariableW(name, value, 512);
  require(n > 0 && n < 512, "missing or oversized probe environment value");
  return {value, n};
}
void require_hosted_opt_in() {
  require(environment(L"GITHUB_ACTIONS") == L"true" &&
      environment(L"RUNNER_ENVIRONMENT") == L"github-hosted" &&
      environment(L"RUNNER_OS") == L"Windows" &&
      environment(L"FLITZFS_ALLOW_DISPOSABLE_MOUNT") == L"1",
      "probe requires explicit disposable hosted Windows CI opt-in");
}
// Pin each existing ancestor without delete sharing and reject reparse points.
// Only a trusted hosted-runner directory is accepted, not arbitrary user paths.
struct Fixture {
  fs::path root;
  std::vector<Handle> pins;
  void pin(const std::wstring& path) {
    require(pins.size() < 24, "fixture ancestor budget");
    Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    checked(handle.value != INVALID_HANDLE_VALUE, "pin fixture ancestor");
    FILE_ATTRIBUTE_TAG_INFO info{};
    checked(GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &info, sizeof(info)),
        "inspect fixture ancestor");
    require((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        GetFileType(handle.value) == FILE_TYPE_DISK, "unsafe fixture ancestor");
    pins.push_back(std::move(handle));
  }
  Fixture() {
    const auto base = environment(L"RUNNER_TEMP");
    require(flitzfs::valid_mount_plan({base + L"\\probe", 15, true}), "invalid runner temporary path");
    pin(base.substr(0, 3));
    for (std::size_t end = base.find(L'\\', 3); end != base.npos; end = base.find(L'\\', end + 1))
      pin(base.substr(0, end));
    pin(base);
    ULARGE_INTEGER available{};
    checked(GetDiskFreeSpaceExW(base.c_str(), &available, nullptr, nullptr), "fixture free space");
    require(available.QuadPart >= flitzfs::kFreeSpaceReserveBytes + 4 * 1024 * 1024,
        "fixture storage reserve unavailable");
    root = fs::path(base) / (L"flitzfs-probe-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()));
    require(flitzfs::valid_mount_plan({(root / L"mount").native(), 15, true}), "fixture path too long");
    checked(CreateDirectoryW(root.c_str(), nullptr), "exclusive fixture directory creation");
    pin(root.native());
  }
  // Leave fixture files for VM disposal; never recursively remove anything.
};
void write_new(const fs::path& path, const std::vector<std::byte>& bytes) {
  Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr));
  checked(file.value != INVALID_HANDLE_VALUE, "exclusive synthetic file creation");
  DWORD written = 0;
  if (!bytes.empty()) {
    checked(WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr),
        "synthetic file write");
    require(written == bytes.size(), "short fixture write");
  }
}
std::vector<std::byte> read_all(const fs::path& path, std::size_t expected) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  checked(file.value != INVALID_HANDLE_VALUE, "mounted file open");
  LARGE_INTEGER size{};
  checked(GetFileSizeEx(file.value, &size), "mounted file size");
  require(size.QuadPart == static_cast<LONGLONG>(expected), "unexpected file length");
  std::vector<std::byte> result(expected + 37);
  DWORD got = 0;
  checked(ReadFile(file.value, result.data(), static_cast<DWORD>(result.size()), &got, nullptr),
      "mounted file read");
  require(got == expected, "short/full read mismatch");
  DWORD eof = 99;
  std::byte extra{};
  checked(ReadFile(file.value, &extra, 1, &eof, nullptr), "read at EOF");
  require(eof == 0, "EOF did not return zero bytes");
  result.resize(expected);
  return result;
}
std::set<std::wstring> enumerate(const fs::path& path) {
  WIN32_FIND_DATAW item{};
  const auto find = FindFirstFileW(path.c_str(), &item);
  checked(find != INVALID_HANDLE_VALUE, "mounted directory enumeration");
  struct Finish { HANDLE h; ~Finish() { FindClose(h); } } finish{find};
  std::set<std::wstring> result;
  do {
    const std::wstring name(item.cFileName);
    if (name != L"." && name != L"..") result.insert(name);
    require(result.size() <= 16, "unexpected directory growth");
  } while (FindNextFileW(find, &item));
  require(GetLastError() == ERROR_NO_MORE_FILES, "directory enumeration failed");
  return result;
}
void check_mapping(const fs::path& path, const std::vector<std::byte>& expected) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  checked(file.value != INVALID_HANDLE_VALUE, "mapped file open");
  Handle mapping(CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr));
  checked(mapping.value != nullptr, "read-only mapping creation");
  // Exercise mapped access after the original file handle has closed.
  file.reset();
  const auto* view = MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, expected.size());
  checked(view != nullptr, "read-only view creation");
  struct Unmap { const void* p; ~Unmap() { UnmapViewOfFile(p); } } unmap{view};
  require(std::memcmp(view, expected.data(), expected.size()) == 0, "mapped byte parity");
}
flitzfs::WinFspSessionApi live_api() {
  return {FspFileSystemPreflight, FspFileSystemCreate, FspFileSystemStartDispatcher,
      FspFileSystemSetMountPoint, FspFileSystemStopDispatcher,
      FspFileSystemRemoveMountPoint, FspFileSystemDelete};
}
void probe() {
  stage = 2;
  BOOL in_job = FALSE;
  checked(IsProcessInJob(GetCurrentProcess(), nullptr, &in_job), "probe job membership");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  checked(QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr),
      "probe memory job limits");
  checked(QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu), nullptr),
      "probe CPU job limits");
  require(in_job && limits.ProcessMemoryLimit == 128ULL * 1024 * 1024 &&
      limits.JobMemoryLimit == limits.ProcessMemoryLimit &&
      limits.BasicLimitInformation.ActiveProcessLimit == 1 &&
      (limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) &&
      cpu.ControlFlags == (JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) &&
      cpu.CpuRate == 2000, "probe launched outside required job limits");
  stage = 3;
  Fixture fixture;
  std::vector<std::byte> data(192 * 1024 + 17);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>((i / 31 + i % 251) % 256);
  const auto input = fixture.root / L"synthetic.bin";
  const auto archive = fixture.root / L"synthetic.flfs";
  const auto empty = fixture.root / L"empty.bin";
  const auto empty_archive = fixture.root / L"empty.flfs";
  write_new(input, data);
  write_new(empty, {});
  const auto packed = flitzfs::pack_new(input, archive);
  require(packed.zstd_chunk_count > 0, "mount probe requires actual Zstandard chunks");
  (void)flitzfs::pack_new(empty, empty_archive);
  const auto archive_bytes = read_all(archive, static_cast<std::size_t>(packed.container_bytes));
  const std::vector<flitzfs::CatalogSource> sources{
      {"Data/sample.bin", archive}, {"Data/other.txt", empty_archive}, {"empty.bin", empty_archive}};
  stage = 4;
  const auto occupied = fixture.root / L"occupied";
  checked(CreateDirectoryW(occupied.c_str(), nullptr), "collision fixture creation");
  const auto sentinel = occupied / L"sentinel.bin";
  const std::vector<std::byte> sentinel_data{std::byte{11}, std::byte{29}};
  write_new(sentinel, sentinel_data);
  {
    flitzfs::WinFspReadOnlySession refused(sources, live_api());
    require(!NT_SUCCESS(refused.start({occupied.native(), 15, true})), "existing directory adopted as mount");
  }
  require(read_all(sentinel, sentinel_data.size()) == sentinel_data, "collision fixture changed");
  const auto mount = fixture.root / L"mount";
  stage = 5;
  flitzfs::WinFspReadOnlySession session(sources, live_api());
  const auto status = session.start({mount.native(), 30, true});
  if (!NT_SUCCESS(status)) throw std::system_error(static_cast<int>(FspWin32FromNtStatus(status)),
      std::system_category(), "live session startup");
  stage = 6;
  const auto virtual_file = mount / L"data" / L"SAMPLE.BIN";
  require(read_all(virtual_file, data.size()) == data, "sequential mounted byte parity");
  require(read_all(mount / L"empty.bin", 0).empty(), "empty mounted file");
  stage = 7;
  {
    Handle file(CreateFileW(virtual_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr));
    checked(file.value != INVALID_HANDLE_VALUE, "random file open");
    LARGE_INTEGER offset{}; offset.QuadPart = 128 * 1024 - 13;
    checked(SetFilePointerEx(file.value, offset, nullptr, FILE_BEGIN), "random file seek");
    std::array<std::byte, 64> bytes{};
    DWORD got = 0;
    checked(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr), "cross-chunk read");
    require(got == bytes.size() && std::equal(bytes.begin(), bytes.end(), data.begin() + offset.QuadPart),
        "cross-chunk byte parity");
  }
  stage = 8;
  check_mapping(virtual_file, data);
  stage = 9;
  require(enumerate(mount / L"*") == std::set<std::wstring>{L"Data", L"empty.bin"}, "root enumeration");
  require(enumerate(mount / L"Data" / L"*") == std::set<std::wstring>{L"other.txt", L"sample.bin"}, "nested enumeration");
  require(enumerate(mount / L"Data" / L"s*.bin") == std::set<std::wstring>{L"sample.bin"}, "wildcard filtering");
  stage = 10;
  {
    Handle denied(CreateFileW(virtual_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(denied.value == INVALID_HANDLE_VALUE, "write access granted");
    Handle created(CreateFileW((mount / L"new.bin").c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(created.value == INVALID_HANDLE_VALUE, "create allowed");
    require(!DeleteFileW(virtual_file.c_str()), "delete allowed");
    require(!MoveFileW(virtual_file.c_str(), (mount / L"moved.bin").c_str()), "rename allowed");
    require(!CreateDirectoryW((mount / L"new-dir").c_str(), nullptr), "directory creation allowed");
  }
  stage = 11;
  std::atomic<bool> parallel_ok{true};
  auto reader = [&]() {
    try { for (unsigned i = 0; i != 4; ++i)
      if (read_all(virtual_file, data.size()) != data) parallel_ok.store(false);
    } catch (...) { parallel_ok.store(false); }
  };
  { std::jthread first(reader); std::jthread second(reader); }
  require(parallel_ok.load(), "concurrent mounted reads");
  stage = 12;
  session.stop(); session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "dispatcher shutdown");
  require(GetFileAttributesW(mount.c_str()) == INVALID_FILE_ATTRIBUTES, "owned mount directory remained");
  stage = 13;
  require(read_all(input, data.size()) == data, "source file changed");
  require(read_all(archive, archive_bytes.size()) == archive_bytes, "backing container changed");
  require(read_all(sentinel, sentinel_data.size()) == sentinel_data, "unrelated collision fixture changed");
  flitzfs::Reader::open(archive).verify_all();
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    require_hosted_opt_in();
    require(argc == 2, "explicit probe mode required");
    const std::wstring_view mode(argv[1]);
    if (mode == L"--probe-child") { probe(); return 0; }
    require(mode == L"--run-disposable-ci", "unknown probe mode");
    wchar_t path[512]{};
    const auto n = GetModuleFileNameW(nullptr, path, 512);
    require(n && n < 512, "probe executable path unavailable");
    flitzfs::ProcessBudget budget; budget.timeout_ms = 30000;
    const auto result = flitzfs::run_bounded_process(fs::path(std::wstring(path, n)), {L"--probe-child"}, budget);
    std::cout << "mount_probe exit=" << result.exit_code << " stage=" << (result.exit_code >> 16U)
        << " detail=" << (result.exit_code & 0xffffU) << " timed_out=" << result.timed_out
        << " exit_confirmed=" << result.exit_confirmed << " peak_commit_bytes=" << result.peak_committed_bytes << '\n';
    require(result.exit_confirmed && !result.timed_out && result.exit_code == 0, "supervised mount probe failed");
    std::cout << "PASS: real mounted Zstd reads, EOF, random/mapped I/O, enumeration, write rejection, shutdown and backing parity.\n";
    return 0;
  } catch (const std::system_error& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | (static_cast<unsigned>(error.code().value()) & 0xffffU));
  } catch (const std::exception& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | 1U);
  }
}
