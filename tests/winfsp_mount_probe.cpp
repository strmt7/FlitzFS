// Disposable hosted-CI integration probe, NOT a general-purpose mount command.
#include "winfsp_session.hpp"
#include "bounded_process.hpp"
#include "flitzfs/container.hpp"
#include "flitzfs/catalog_manifest.hpp"
#include "flitzfs/flzip_reader.hpp"
#include "flzip_native_fixture.hpp"
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
std::string synthetic_manifest() {
  return flitzfs::serialize_catalog_manifest({
      {"Data/sample.bin", "synthetic.flfs"}, {"Data/other.txt", "empty.flfs"}, {"empty.bin", "empty.flfs"}});
}
void probe(bool abandon_mount) {
  require_probe_job();
  stage = 3;
  Fixture fixture;
  const auto data = synthetic_data();
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
  const auto manifest = synthetic_manifest();
  const auto* text_begin = reinterpret_cast<const std::byte*>(manifest.data());
  write_new(fixture.root / L"collection.flcatalog", {text_begin, text_begin + manifest.size()});
  const auto sources = flitzfs::read_catalog_manifest_sources(fixture.root / L"collection.flcatalog");
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
  if (abandon_mount) {
    // Readiness is recorded only after real mounted reads and write denial pass.
    // These are synthetic snapshots, not user backups or durable-write evidence.
    write_new(fixture.root / L"baseline.flfs", archive_bytes);
    write_new(fixture.root / L"mounted.ready", {std::byte{0x46}, std::byte{0x53}, std::byte{1}});
    Sleep(30000); // Supervisor must kill this child at 15 seconds, without teardown.
    throw std::runtime_error("supervisor did not terminate the mounted helper");
  }
  session.stop(); session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "dispatcher shutdown");
  require(GetFileAttributesW(mount.c_str()) == INVALID_FILE_ATTRIBUTES, "owned mount directory remained");
  stage = 13;
  require(read_all(input, data.size()) == data, "source file changed");
  require(read_all(archive, archive_bytes.size()) == archive_bytes, "backing container changed");
  require(read_all(sentinel, sentinel_data.size()) == sentinel_data, "unrelated collision fixture changed");
  flitzfs::Reader::open(archive).verify_all();
}
// Same native mount interface, now fed by a verified immutable .flzip snapshot.
// All bytes, paths and codec choices are synthetic and fixed by this test.
void flzip_probe() {
  require_probe_job(); stage = 20;
  Fixture fixture;
  const auto data = synthetic_data();
  using namespace flzip_fixture;
  const Bytes first(data.begin(), data.begin() + 128 * 1024);
  const Bytes second(data.begin() + 128 * 1024, data.end());
  const auto patterned = pattern(9001);
  const auto deflated = compressed(2, pattern(8193, "Deflate native interoperability").plain);
  const Record native_record{"native-codecs.bin", false, native_samples()};
  const auto native_bytes = plain(native_record);
  require(flitzfs::flzip_codec_available(6), "mount lane requires the LZ4 decoder");
  const auto zip = archive({
      {"Data/sample.bin", false, {compressed(8, first), compressed(8, second)}},
      {"filled.bin", false, {fill(4097, 19)}},
      {"pattern.bin", false, {patterned}},
      {"deflated.bin", false, {deflated}},
      {"empty.bin", false, {}}, {"EmptyDir", true, {}}, native_record}, 11);
  const auto source = fixture.root / L"native.flzip";
  write_new(source, zip.bytes);
  stage = 21;
  auto projection = flitzfs::ReadOnlyProjection::open_flzip(source);
  const auto mount = fixture.root / L"mount";
  flitzfs::WinFspReadOnlySession session(std::move(projection), live_api());
  const auto status = session.start({mount.native(), 30, true});
  if (!NT_SUCCESS(status)) throw std::system_error(static_cast<int>(FspWin32FromNtStatus(status)),
      std::system_category(), "FLZIP session startup");
  stage = 22;
  const auto virtual_file = mount / L"data" / L"SAMPLE.BIN";
  require(read_all(virtual_file, data.size()) == data, "mounted FLZIP Zstandard parity");
  require(read_all(mount / L"filled.bin", 4097) == Bytes(4097, std::byte{19}), "mounted FLZIP fill parity");
  require(read_all(mount / L"pattern.bin", patterned.plain.size()) == patterned.plain, "mounted FLZIP pattern parity");
  require(read_all(mount / L"deflated.bin", deflated.plain.size()) == deflated.plain, "mounted FLZIP Deflate parity");
  require(read_all(mount / L"empty.bin", 0).empty(), "mounted FLZIP empty file");
  require(read_all(mount / L"native-codecs.bin", native_bytes.size()) == native_bytes, "mounted native codec byte parity");
  check_mapping(mount / L"native-codecs.bin", native_bytes);
  check_mapping(virtual_file, data);
  {
    Handle file(CreateFileW(virtual_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr));
    checked(file.value != INVALID_HANDLE_VALUE, "FLZIP random open");
    LARGE_INTEGER offset{}; offset.QuadPart = 128 * 1024 - 13;
    checked(SetFilePointerEx(file.value, offset, nullptr, FILE_BEGIN), "FLZIP random seek");
    std::array<std::byte, 64> bytes{}; DWORD got = 0;
    checked(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr), "FLZIP random read");
    require(got == bytes.size() && std::equal(bytes.begin(), bytes.end(), data.begin() + offset.QuadPart), "FLZIP cross-block parity");
    Handle denied(CreateFileW(virtual_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(denied.value == INVALID_HANDLE_VALUE, "FLZIP write grant");
    require(!DeleteFileW(virtual_file.c_str()), "FLZIP delete allowed");
  }
  stage = 23;
  require(enumerate(mount / L"*") == std::set<std::wstring>{L"Data",L"EmptyDir",L"deflated.bin",L"empty.bin",L"filled.bin",L"native-codecs.bin",L"pattern.bin"}, "FLZIP directory enumeration");
  require(enumerate(mount / L"Data" / L"s*.bin") == std::set<std::wstring>{L"sample.bin"}, "FLZIP wildcard filtering");
  std::atomic<bool> parity{true};
  auto worker = [&]() {try {for(unsigned i=0;i<2;++i) if(read_all(virtual_file,data.size()) != data) parity=false;} catch(...) {parity=false;}};
  {std::jthread one(worker);std::jthread two(worker);}
  require(parity, "FLZIP concurrent mounted parity");
  stage = 24;
  session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "FLZIP clean shutdown");
  require(GetFileAttributesW(mount.c_str()) == INVALID_FILE_ATTRIBUTES, "FLZIP mount remained");
  require(read_all(source,zip.bytes.size()) == zip.bytes, "FLZIP input changed");
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
  const std::vector<std::byte> ready{std::byte{0x46}, std::byte{0x53}, std::byte{1}};
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
  const auto archive = fixture.root / L"synthetic.flfs";
  require(read_all(fixture.root / L"synthetic.bin", data.size()) == data, "source changed after termination");
  auto checked_reader = flitzfs::Reader::open(archive);
  const auto& info = checked_reader.info();
  require(info.logical_bytes == data.size() && info.container_bytes <= data.size() + 4096 &&
      info.zstd_chunk_count > 0, "recovered container metadata differs");
  checked_reader.verify_all();
  require(checked_reader.read(0, static_cast<std::uint32_t>(data.size())) == data, "independent recovery parity");
  const auto n = static_cast<std::size_t>(info.container_bytes);
  require(read_all(archive, n) == read_all(fixture.root / L"baseline.flfs", n),
      "backing bytes changed after forced termination");
  const std::vector<std::byte> sentinel{std::byte{11}, std::byte{29}};
  require(read_all(fixture.root / L"occupied" / L"sentinel.bin", sentinel.size()) == sentinel,
      "collision sentinel changed after forced termination");
  stage = 16;
  const auto manifest = synthetic_manifest();
  const auto persisted = read_all(fixture.root / L"collection.flcatalog", manifest.size());
  require(std::memcmp(persisted.data(), manifest.data(), manifest.size()) == 0,
      "catalog manifest changed after termination");
  const auto sources = flitzfs::read_catalog_manifest_sources(fixture.root / L"collection.flcatalog");
  flitzfs::WinFspReadOnlySession session(sources, live_api());
  const auto status = session.start({mount.native(), 30, true});
  if (!NT_SUCCESS(status)) throw std::system_error(static_cast<int>(FspWin32FromNtStatus(status)),
      std::system_category(), "remount after terminated helper");
  stage = 17;
  const auto file = mount / L"Data" / L"sample.bin";
  require(read_all(file, data.size()) == data, "remounted read parity");
  check_mapping(file, data);
  require(read_all(mount / L"empty.bin", 0).empty(), "remounted empty file");
  stage = 18;
  session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "remount clean shutdown");
  require(absent(), "remount directory remained");
  require(read_all(archive, n) == read_all(fixture.root / L"baseline.flfs", n), "remount altered backing");
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
    if (mode == L"--probe-flzip-child") { flzip_probe(); return 0; }
    if (mode == L"--probe-child") { probe(false); return 0; }
    if (mode == L"--probe-crash-child") { probe(true); return 0; }
    require(mode == L"--run-disposable-ci", "unknown probe mode");
    wchar_t path[512]{};
    const auto n = GetModuleFileNameW(nullptr, path, 512);
    require(n && n < 512, "probe executable path unavailable");
    flitzfs::ProcessBudget budget; budget.timeout_ms = 30000;
    const auto native_zip = flitzfs::run_bounded_process(fs::path(std::wstring(path, n)), {L"--probe-flzip-child"}, budget);
    std::cout << "flzip_probe exit=" << native_zip.exit_code << " stage=" << (native_zip.exit_code >> 16U)
        << " detail=" << (native_zip.exit_code & 0xffffU) << " timed_out=" << native_zip.timed_out
        << " exit_confirmed=" << native_zip.exit_confirmed << " peak_commit_bytes=" << native_zip.peak_committed_bytes << '\n';
    require(native_zip.exit_confirmed && !native_zip.timed_out && native_zip.exit_code == 0, "supervised FLZIP mount failed");
    std::cout << "PASS: real mounted FLZIP native entropy/dictionary/composition and CPU codecs, mapped/random/concurrent reads, write rejection and unchanged source.\n";
    const auto result = flitzfs::run_bounded_process(fs::path(std::wstring(path, n)), {L"--probe-child"}, budget);
    std::cout << "mount_probe exit=" << result.exit_code << " stage=" << (result.exit_code >> 16U)
        << " detail=" << (result.exit_code & 0xffffU) << " timed_out=" << result.timed_out
        << " exit_confirmed=" << result.exit_confirmed << " peak_commit_bytes=" << result.peak_committed_bytes << '\n';
    require(result.exit_confirmed && !result.timed_out && result.exit_code == 0, "supervised mount probe failed");
    std::cout << "PASS: real mounted Zstd reads, EOF, random/mapped I/O, enumeration, write rejection, shutdown and backing parity.\n";
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
    std::cout << "PASS: supervisor termination of a verified live mount, automatic mount removal, unchanged backing, independent CPU recovery and same-path remount.\n";
    return 0;
  } catch (const std::system_error& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | (static_cast<unsigned>(error.code().value()) & 0xffffU));
  } catch (const std::exception& error) {
    std::cerr << "stage=" << stage << " " << error.what() << '\n';
    return static_cast<int>((stage << 16U) | 1U);
  }
}
