// Disposable hosted-CI integration probe, NOT a general-purpose mount command.
#include "winfsp_session.hpp"
#include "bounded_process.hpp"
#include "flitzfs/container.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "native_windows_fixture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
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
// Exercise offsets beyond 32 bits without exposing a terabyte-sized cache map.
constexpr std::uint64_t mount_sparse_bytes=(1ULL<<32)+65536;
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
  explicit Fixture(std::uint32_t previous_process = 0) {
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
    root = fs::path(base) / (L"flitzfs-probe-" +
        std::to_wstring(previous_process ? previous_process : GetCurrentProcessId()));
    require(flitzfs::valid_mount_plan({(root / L"mount").native(), 15, true}), "fixture path too long");
    if (!previous_process)
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
void require_probe_job() {
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
}
std::vector<std::byte> synthetic_data() {
  std::vector<std::byte> data(192 * 1024 + 17);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::byte>((i / 31 + i % 251) % 256);
  return data;
}
void check_sparse_tail(const fs::path& path) {
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
      OPEN_EXISTING,FILE_FLAG_RANDOM_ACCESS,nullptr));
  checked(file.value!=INVALID_HANDLE_VALUE,"native sparse file open");
  LARGE_INTEGER size{},offset{};checked(GetFileSizeEx(file.value,&size),"native sparse file length");
  require(size.QuadPart==static_cast<LONGLONG>(mount_sparse_bytes),"native sparse logical length");
  offset.QuadPart=size.QuadPart-31;
  checked(SetFilePointerEx(file.value,offset,nullptr,FILE_BEGIN),"native sparse tail seek");
  std::array<std::byte,64> bytes{};DWORD got=0;
  checked(ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr),"native sparse tail read");
  const auto suffix=native_windows_fixture::tail();
  require(got==31 && std::all_of(bytes.begin(),bytes.begin()+14,[](auto b){return b==std::byte{0};}) &&
      std::equal(suffix.begin(),suffix.end(),bytes.begin()+14),"native sparse tail byte parity");
}
void check_inode(const fs::path& path,std::uint64_t expected) {
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
  checked(file.value!=INVALID_HANDLE_VALUE,"native file identity open");
  BY_HANDLE_FILE_INFORMATION info{};
  checked(GetFileInformationByHandle(file.value,&info),"native file identity query");
  FILE_BASIC_INFO basic{};checked(GetFileInformationByHandleEx(file.value,FileBasicInfo,&basic,sizeof(basic)),
      "mounted persisted native basic query");
  const auto wanted=native_windows_fixture::sample_basic();
  require(basic.CreationTime.QuadPart==static_cast<LONGLONG>(wanted.creation_time) &&
      basic.LastAccessTime.QuadPart==static_cast<LONGLONG>(wanted.last_access_time) &&
      basic.LastWriteTime.QuadPart==static_cast<LONGLONG>(wanted.last_write_time) &&
      basic.ChangeTime.QuadPart==static_cast<LONGLONG>(wanted.change_time) &&
      basic.FileAttributes==wanted.attributes,"mounted Windows basic tuple differs from durable native source");
  require(((std::uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow)==expected &&
      info.dwVolumeSerialNumber==0x5700004eU,"mounted identity differs from native UUID/inode");
}
void probe(bool abandon_mount) {
  require_probe_job();
  stage = 3;
  Fixture fixture;
  const auto data = synthetic_data();
  const auto input = fixture.root / L"synthetic.bin";
  const auto image = fixture.root / L"native.fltx";
  const auto empty = fixture.root / L"empty.bin";
  write_new(input,data);write_new(empty,{});
  const auto expected=native_windows_fixture::make_image(image,true,mount_sparse_bytes);
  require(expected.storage.stored_payload_bytes<expected.storage.logical_bytes,
      "native mount fixture must contain actual compressed payloads");
  const auto image_bytes=read_all(image,static_cast<std::size_t>(expected.storage.physical_end));
  // Source image + immutable crash baseline + original input remain <4 MiB.
  require(2*image_bytes.size()+data.size()+1024<4*1024*1024,"native mount fixture aggregate byte budget");
  auto projection=[&] {return flitzfs::ReadOnlyProjection::open_native_file(
      image,native_windows_fixture::identity());};
  stage = 4;
  const auto occupied = fixture.root / L"occupied";
  checked(CreateDirectoryW(occupied.c_str(), nullptr), "collision fixture creation");
  const auto sentinel = occupied / L"sentinel.bin";
  const std::vector<std::byte> sentinel_data{std::byte{11}, std::byte{29}};
  write_new(sentinel, sentinel_data);
  {
    flitzfs::WinFspReadOnlySession refused(projection(), live_api());
    require(!NT_SUCCESS(refused.start({occupied.native(), 15, true})), "existing directory adopted as mount");
  }
  require(read_all(sentinel, sentinel_data.size()) == sentinel_data, "collision fixture changed");
  const auto mount = fixture.root / L"mount";
  stage = 5;
  flitzfs::WinFspReadOnlySession session(projection(), live_api());
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
  check_inode(virtual_file,expected.sample_inode);
  check_sparse_tail(mount/L"sparse.bin");
  stage = 9;
  require(enumerate(mount / L"*") == std::set<std::wstring>{L"Data", L"empty.bin", L"sparse.bin"}, "root enumeration");
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
  if (abandon_mount) {
    // Readiness is recorded only after real mounted reads and write denial pass.
    // These are synthetic snapshots, not user backups or durable-write evidence.
    write_new(fixture.root / L"baseline.fltx", image_bytes);
    write_new(fixture.root / L"mounted.ready", {std::byte{0x46}, std::byte{0x53}, std::byte{2}});
    Sleep(30000); // Supervisor must kill this child at 15 seconds, without teardown.
    throw std::runtime_error("supervisor did not terminate the mounted helper");
  }
  session.stop(); session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "dispatcher shutdown");
  require(GetFileAttributesW(mount.c_str()) == INVALID_FILE_ATTRIBUTES, "owned mount directory remained");
  stage = 13;
  require(read_all(input, data.size()) == data, "source file changed");
  require(read_all(image, image_bytes.size()) == image_bytes, "native image changed");
  require(read_all(sentinel, sentinel_data.size()) == sentinel_data, "unrelated collision fixture changed");
  flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image)).verify_all();
}

// The only recovery selector is the PID returned by this executable's supervisor.
// No path, namespace, volume, or existing user file is accepted as an argument.
std::uint32_t process_number(std::wstring_view text) {
  require(!text.empty() && text.size() <= 10, "invalid recovery process identity");
  std::uint64_t result = 0;
  for (const auto c : text) {
    require(c >= L'0' && c <= L'9', "non-numeric recovery process identity");
    result = result * 10 + static_cast<unsigned>(c - L'0');
    require(result <= (std::numeric_limits<std::uint32_t>::max)(), "recovery process identity overflow");
  }
  require(result != 0, "zero recovery process identity");
  return static_cast<std::uint32_t>(result);
}
void recover(std::uint32_t previous_process) {
  require_probe_job();
  stage = 14;
  Fixture fixture(previous_process);
  const std::vector<std::byte> ready{std::byte{0x46}, std::byte{0x53}, std::byte{2}};
  require(read_all(fixture.root / L"mounted.ready", ready.size()) == ready,
      "killed helper did not reach verified mounted state");
  const auto mount = fixture.root / L"mount";
  const auto absent = [&]() {
    if (GetFileAttributesW(mount.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const auto error = GetLastError();
    require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND, "mount disappearance query failed");
    return true;
  };
  // Bounded observation only. Never delete or forcibly repair a stale junction.
  const auto deadline = GetTickCount64() + 2000;
  while (!absent() && GetTickCount64() < deadline) Sleep(20);
  require(absent(), "terminated helper left a mount directory behind");
  stage = 15;
  const auto data = synthetic_data();
  const auto image=fixture.root/L"native.fltx";
  require(read_all(fixture.root/L"synthetic.bin",data.size())==data,"source changed after termination");
  auto recovered=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));
  recovered.verify_all();const auto info=recovered.storage_info();
  require(recovered.identity()==native_windows_fixture::identity() && recovered.sequence()==3 &&
      recovered.stat("Data/sample.bin").inode==4 && recovered.stat("sparse.bin").logical_bytes==mount_sparse_bytes &&
      info.replicated_checkpoint &&
      !info.degraded_checkpoint && info.physical_end<=1536*1024 &&
      info.stored_payload_bytes<info.logical_bytes,"recovered native metadata differs");
  require(recovered.read("Data/sample.bin",0,static_cast<std::uint32_t>(data.size()))==data,
      "independent native CPU recovery parity");
  const auto n=static_cast<std::size_t>(info.physical_end);
  require(read_all(image,n)==read_all(fixture.root/L"baseline.fltx",n),
      "native image changed after forced termination");
  const std::vector<std::byte> sentinel{std::byte{11},std::byte{29}};
  require(read_all(fixture.root/L"occupied"/L"sentinel.bin",sentinel.size())==sentinel,
      "collision sentinel changed after forced termination");
  stage=16;
  flitzfs::WinFspReadOnlySession session(flitzfs::ReadOnlyProjection::open_native_file(
      image,native_windows_fixture::identity()),live_api());
  const auto status=session.start({mount.native(),30,true});
  if(!NT_SUCCESS(status))throw std::system_error(static_cast<int>(FspWin32FromNtStatus(status)),
      std::system_category(),"native remount after terminated helper");
  stage = 17;
  const auto file = mount / L"Data" / L"sample.bin";
  require(read_all(file, data.size()) == data, "remounted read parity");
  check_mapping(file, data);
  check_inode(file,4);check_sparse_tail(mount/L"sparse.bin");
  require(read_all(mount / L"empty.bin", 0).empty(), "remounted empty file");
  stage = 18;
  session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "remount clean shutdown");
  require(absent(), "remount directory remained");
  require(read_all(image,n)==read_all(fixture.root/L"baseline.fltx",n),"remount altered native backing");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    require_hosted_opt_in();
    require(argc == 2 || argc == 3, "explicit probe mode required");
    const std::wstring_view mode(argv[1]);
    if (mode == L"--probe-recover-child") {
      require(argc == 3, "recovery identity missing");
      recover(process_number(argv[2])); return 0;
    }
    require(argc == 2, "unexpected probe argument");
    if (mode == L"--probe-child") { probe(false); return 0; }
    if (mode == L"--probe-crash-child") { probe(true); return 0; }
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
    std::cout << "PASS: real mounted native transactional image reads, sparse tails, persistent inode IDs, mapped I/O, enumeration, write rejection and backing parity.\n";
    budget.timeout_ms = 15000;
    const auto killed = flitzfs::run_bounded_process(fs::path(std::wstring(path, n)), {L"--probe-crash-child"}, budget);
    std::cout << "crash_probe exit=" << killed.exit_code << " timed_out=" << killed.timed_out
        << " exit_confirmed=" << killed.exit_confirmed << " peak_commit_bytes=" << killed.peak_committed_bytes << '\n';
    require(killed.process_id != 0 && killed.timed_out && killed.exit_confirmed &&
        killed.exit_code == ERROR_TIMEOUT, "mounted helper was not forcibly terminated");
    budget.timeout_ms = 30000;
    const auto recovered = flitzfs::run_bounded_process(fs::path(std::wstring(path, n)),
        {L"--probe-recover-child", std::to_wstring(killed.process_id)}, budget);
    std::cout << "recovery_probe exit=" << recovered.exit_code << " stage=" << (recovered.exit_code >> 16U)
        << " detail=" << (recovered.exit_code & 0xffffU) << " timed_out=" << recovered.timed_out
        << " exit_confirmed=" << recovered.exit_confirmed << " peak_commit_bytes=" << recovered.peak_committed_bytes << '\n';
    require(recovered.exit_confirmed && !recovered.timed_out && recovered.exit_code == 0,
        "post-termination verification/remount failed");
    std::cout << "PASS: native-image mount termination, automatic removal, unchanged source, independent native CPU recovery and same-path remount.\n";
    return 0;
  } catch (const std::system_error& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | (static_cast<unsigned>(error.code().value()) & 0xffffU));
  } catch (const std::exception& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | 1U);
  }
}
