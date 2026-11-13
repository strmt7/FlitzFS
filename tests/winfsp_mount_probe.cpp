// Disposable hosted-CI integration probe, NOT a general-purpose mount command.
#include "winfsp_session.hpp"
#include "bounded_process.hpp"
#include "supervised_source_fixture.hpp"
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
#include <sstream>
#include <locale>
#include <iterator>
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
constexpr std::size_t space_report_max_bytes=8192;
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
      FspFileSystemRemoveMountPoint, FspFileSystemDelete,FspFileSystemOperationProcessId};
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
// Read-only observations only: no compression toggle, allocation request,
// raw volume handle or mutation API is used. Secondary compression failures
// are evidence, not a reason to fabricate a physical size.
void record_space_reporting(const fs::path& mount,const fs::path& image,
    std::size_t image_bytes,std::string_view phase,const fs::path& report) {
  require(phase=="normal" || phase=="crash" || phase=="recovery","invalid reporting phase");
  const auto native=flitzfs::NativeFilesystemSnapshot::open(
      flitzfs::ObjectStoreSnapshot::open_file(image));
  require(native.identity()==native_windows_fixture::identity() && native.sequence()==3,
      "space oracle native identity/sequence");
  const auto storage=native.storage_info();
  Handle backing(CreateFileW(image.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
  checked(backing.value!=INVALID_HANDLE_VALUE,"backing image observation open");
  FILE_STANDARD_INFO backing_standard{};
  checked(GetFileInformationByHandleEx(backing.value,FileStandardInfo,&backing_standard,sizeof(backing_standard)),
      "backing image standard allocation");
  require(!backing_standard.Directory && !backing_standard.DeletePending &&
      backing_standard.EndOfFile.QuadPart==static_cast<LONGLONG>(image_bytes) &&
      backing_standard.AllocationSize.QuadPart>=0,"backing image observation differs from owned source");
  const auto commit_wide=environment(L"GITHUB_SHA");
  require(commit_wide.size()==40 && std::all_of(commit_wide.begin(),commit_wide.end(),
      [](wchar_t c){return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');}),
      "invalid source commit in hosted report");
  const std::string commit(commit_wide.begin(),commit_wide.end());
  const auto root=mount.native()+L"\\";
  DWORD serial=0,maximum_name=0,flags=0;
  wchar_t filesystem[32]{};
  checked(GetVolumeInformationW(root.c_str(),nullptr,0,&serial,&maximum_name,&flags,
      filesystem,32),"mounted volume attributes");
  const auto name_end=std::find(std::begin(filesystem),std::end(filesystem),L'\0');
  require(name_end!=std::end(filesystem) &&
      std::wstring_view(filesystem,static_cast<std::size_t>(name_end-std::begin(filesystem)))==L"FlitzFS" &&
      serial==0x5700004eU,
      "space report reached wrong filesystem/volume identity");
  DWORD sectors=0,sector_bytes=0,free_clusters=0,total_clusters=0;
  checked(GetDiskFreeSpaceW(root.c_str(),&sectors,&sector_bytes,&free_clusters,&total_clusters),
      "mounted allocation geometry");
  const auto unit=std::uint64_t(sectors)*sector_bytes;
  require(unit==4096,"mounted geometry differs from selected adapter");
  ULARGE_INTEGER available{},total{},free{};
  checked(GetDiskFreeSpaceExW(root.c_str(),&available,&total,&free),"mounted space totals");
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"{\"schema\":1,\"phase\":\""<<phase<<"\",\"commit\":\""<<commit
      <<"\",\"filesystem\":\"FlitzFS\",\"source_uuid\":\"4e000000000000000000000000000057\","
      <<"\"native_namespace_sequence\":"<<native.sequence()
      <<",\"native_image_bytes\":"<<image_bytes
      <<",\"backing_standard_allocation_bytes\":"<<backing_standard.AllocationSize.QuadPart
      <<",\"native_generation\":"<<storage.generation
      <<",\"native_committed_end\":"<<storage.physical_end
      <<",\"native_live_decoded_bytes\":"<<storage.logical_bytes
      <<",\"native_live_stored_payload_bytes\":"<<storage.stored_payload_bytes
      <<",\"volume_serial\":"<<serial<<",\"volume_flags\":"<<flags
      <<",\"volume_read_only\":"<<((flags&FILE_READ_ONLY_VOLUME)?"true":"false")
      <<",\"volume_file_compression\":"<<((flags&FILE_FILE_COMPRESSION)?"true":"false")
      <<",\"maximum_component_characters\":"<<maximum_name
      <<",\"allocation_unit_bytes\":"<<unit<<",\"total_clusters\":"<<total_clusters
      <<",\"free_clusters\":"<<free_clusters<<",\"total_bytes\":"<<total.QuadPart
      <<",\"free_bytes\":"<<free.QuadPart<<",\"caller_available_bytes\":"<<available.QuadPart
      <<",\"records\":[";
  struct Item {PCWSTR windows;const char* native;const char* label;};
  const std::array<Item,3> items{{{L"Data\\sample.bin","Data/sample.bin","sample"},
      {L"empty.bin","empty.bin","empty"},{L"sparse.bin","sparse.bin","sparse"}}};
  unsigned count=0;
  for(const auto& item:items) {
    const auto path=mount/item.windows;const auto wanted=native.stat(item.native);
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"space observation file open");
    FILE_STANDARD_INFO standard{};
    checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&standard,sizeof(standard)),
        "mounted standard allocation/EOF query");
    require(!standard.Directory && !standard.DeletePending &&
        standard.EndOfFile.QuadPart==static_cast<LONGLONG>(wanted.logical_bytes) &&
        standard.AllocationSize.QuadPart==static_cast<LONGLONG>(
            (wanted.logical_bytes+unit-1)/unit*unit),
        "standard allocation differs from current logical-presentation contract");
    FILE_COMPRESSION_INFO compression{};
    SetLastError(ERROR_SUCCESS);
    const auto compression_ok=GetFileInformationByHandleEx(file.value,FileCompressionInfo,
        &compression,sizeof(compression));
    const auto compression_error=compression_ok?ERROR_SUCCESS:GetLastError();
    if(compression_ok)require(compression.CompressedFileSize.QuadPart>=0,"negative compression information");
    DWORD high=0;SetLastError(ERROR_SUCCESS);
    const auto low=GetCompressedFileSizeW(path.c_str(),&high);
    const auto compressed_last_error=GetLastError();
    if(count++)out<<",";
    out<<"{\"file\":\""<<item.label<<"\",\"inode\":"<<wanted.inode
        <<",\"logical_bytes\":"<<wanted.logical_bytes
        <<",\"standard_allocation_bytes\":"<<standard.AllocationSize.QuadPart
        <<",\"standard_eof_bytes\":"<<standard.EndOfFile.QuadPart
        <<",\"standard_links\":"<<standard.NumberOfLinks
        <<",\"compression_info_ok\":"<<(compression_ok?"true":"false")
        <<",\"compression_info_error\":"<<compression_error
        <<",\"compression_info_bytes\":"<<(compression_ok?compression.CompressedFileSize.QuadPart:0)
        <<",\"compression_format\":"<<(compression_ok?unsigned(compression.CompressionFormat):0)
        <<",\"compression_unit_shift\":"<<(compression_ok?unsigned(compression.CompressionUnitShift):0)
        <<",\"compression_chunk_shift\":"<<(compression_ok?unsigned(compression.ChunkShift):0)
        <<",\"compression_cluster_shift\":"<<(compression_ok?unsigned(compression.ClusterShift):0)
        <<",\"get_compressed_low\":"<<low<<",\"get_compressed_high\":"<<high
        <<",\"get_compressed_last_error\":"<<compressed_last_error<<"}";
  }
  require(count==3,"space observation omitted a native file");
  out<<"],\"record_count\":"<<count<<",\"complete\":true}";
  const auto text=out.str();
  require(text.size()>0 && text.size()<=space_report_max_bytes,"space report byte budget");
  const auto* begin=reinterpret_cast<const std::byte*>(text.data());
  write_new(report,{begin,begin+text.size()});
}
// The supervisor reads only the exact child's newly owned fixture after
// confirmed termination. Child output handles are deliberately not inherited.
void emit_space_report(std::uint32_t process,std::string_view phase) {
  require(process && (phase=="normal" || phase=="crash" || phase=="recovery"),"invalid report selector");
  Fixture fixture(process);
  const auto leaf=phase=="normal"?L"space-normal.json":phase=="crash"?L"space-crash.json":L"space-recovery.json";
  const auto path=fixture.root/leaf;
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
  checked(file.value!=INVALID_HANDLE_VALUE,"open owned child space report");
  FILE_ATTRIBUTE_TAG_INFO tag{};FILE_STANDARD_INFO standard{};
  checked(GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&tag,sizeof(tag)),
      "inspect owned child report type");
  checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&standard,sizeof(standard)),
      "inspect owned child report size");
  require(GetFileType(file.value)==FILE_TYPE_DISK && !standard.Directory &&
      !(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && standard.NumberOfLinks==1 &&
      standard.EndOfFile.QuadPart>0 && static_cast<std::uint64_t>(standard.EndOfFile.QuadPart)<=space_report_max_bytes,
      "unsafe/oversized child space report");
  std::string text(static_cast<std::size_t>(standard.EndOfFile.QuadPart),'\0');DWORD got=0;
  checked(ReadFile(file.value,text.data(),static_cast<DWORD>(text.size()),&got,nullptr),"read owned child report");
  require(got==text.size() && text.starts_with("{\"schema\":1,\"phase\":\""+std::string(phase)+"\",") &&
      text.ends_with("\"record_count\":3,\"complete\":true}") &&
      text.find('\n')==std::string::npos && text.find('\r')==std::string::npos &&
      text.find('\0')==std::string::npos,"incomplete or wrong-phase child space report");
  std::cout<<"Windows space evidence: "<<text<<"\n";
}
void reject_host_self_open(flitzfs::WinFspReadOnlySession& session,const fs::path& mount) {
  const auto path=mount/L"Data"/L"sample.bin";
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
  const auto error=GetLastError();
  require(file.value==INVALID_HANDLE_VALUE&&error==ERROR_ACCESS_DENIED&&session.self_origin_rejected(),
      "readonly host self-open was not refused by product adapter");
}
void mounted_client(const Fixture& fixture,bool abandon_mount) {
  const auto mount=fixture.root/L"mount",image=fixture.root/L"native.fltx";
  const auto data=synthetic_data();
  auto native=flitzfs::NativeFilesystemSnapshot::open(
      flitzfs::ObjectStoreSnapshot::open_file(image));
  const auto image_bytes=static_cast<std::size_t>(native.storage_info().physical_end);
  require(image_bytes<=1536*1024,"external readonly image budget");
  const auto expected_inode=native.stat("Data/sample.bin").inode;
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
  check_inode(virtual_file,expected_inode);
  check_sparse_tail(mount/L"sparse.bin");
  record_space_reporting(mount,image,image_bytes,abandon_mount?"crash":"normal",
      fixture.root/(abandon_mount?L"space-crash.json":L"space-normal.json"));
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
}
void probe(bool abandon_mount,std::wstring_view token) {
  require_probe_job();source_fixture::Events protocol(false,std::wstring(token));
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
  // All retained normal/crash images, the crash baseline, BOTH inputs and
  // all three reports remain <4 MiB across the complete supervised campaign.
  require(3*image_bytes.size()+2*data.size()+3*space_report_max_bytes+1024<4*1024*1024,"native mount fixture aggregate byte budget");
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
  reject_host_self_open(session,mount);
  protocol.signal(source_fixture::Phase::ready);protocol.wait_written();
  stage = 12;
  if (abandon_mount) {
    // Readiness is recorded only after real mounted reads and write denial pass.
    // These are synthetic snapshots, not user backups or durable-write evidence.
    write_new(fixture.root / L"baseline.fltx", image_bytes);
    write_new(fixture.root / L"mounted.ready", {std::byte{0x46}, std::byte{0x53}, std::byte{2}});
    protocol.signal(source_fixture::Phase::ack);
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
  protocol.signal(source_fixture::Phase::ack);
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
void recovered_client(const Fixture& fixture) {
  const auto mount=fixture.root/L"mount",image=fixture.root/L"native.fltx";
  const auto data=synthetic_data();
  const auto native=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));
  const auto n=static_cast<std::size_t>(native.storage_info().physical_end);
  require(n<=1536*1024,"external recovered image budget");
  stage = 17;
  const auto file = mount / L"Data" / L"sample.bin";
  require(read_all(file, data.size()) == data, "remounted read parity");
  check_mapping(file, data);
  check_inode(file,4);check_sparse_tail(mount/L"sparse.bin");
  require(read_all(mount / L"empty.bin", 0).empty(), "remounted empty file");
  record_space_reporting(mount,image,n,"recovery",fixture.root/L"space-recovery.json");
}
void recover(std::uint32_t previous_process,std::wstring_view token) {
  require_probe_job();source_fixture::Events protocol(false,std::wstring(token));
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
  reject_host_self_open(session,mount);
  protocol.signal(source_fixture::Phase::ready);protocol.wait_written();
  stage = 18;
  session.stop();
  require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "remount clean shutdown");
  require(absent(), "remount directory remained");
  require(read_all(image,n)==read_all(fixture.root/L"baseline.fltx",n),"remount altered native backing");
  protocol.signal(source_fixture::Phase::ack);
}
struct ExternalReadonly {
  source_fixture::Events protocol{true};
  std::unique_ptr<source_fixture::Child> child;
  flitzfs::ProcessResult result;
  ExternalReadonly(const fs::path& executable,bool crash,std::uint32_t recover_source=0) {
    require(!crash||recover_source==0,"readonly recovery/crash selector");
    std::vector<std::wstring> arguments{recover_source?L"--probe-recover-child":
        crash?L"--probe-crash-child":L"--probe-child"};
    if(recover_source)arguments.push_back(std::to_wstring(recover_source));
    arguments.push_back(protocol.token());
    child=std::make_unique<source_fixture::Child>(executable,arguments,
        flitzfs::ProcessBudget{128,2000,crash?15000U:30000U});
    child->await(protocol,source_fixture::Phase::ready);
    require(child->process_id()&&child->process_id()!=GetCurrentProcessId()&&
        (!recover_source||child->process_id()!=recover_source),"readonly actor alias");
    Fixture fixture(recover_source?recover_source:child->process_id());
    if(recover_source)recovered_client(fixture);else mounted_client(fixture,crash);
    protocol.signal(source_fixture::Phase::written);child->await(protocol,source_fixture::Phase::ack);
    result=child->result();
    require(result.process_id==child->process_id()&&result.exit_confirmed&&!result.cancelled&&
        (crash?(result.timed_out&&result.exit_code==ERROR_TIMEOUT):(!result.timed_out&&result.exit_code==0))&&
        child->retirement_confirmed(),"readonly exact child/supervisor retirement");
    require(source_fixture::cleanup_ok.load(),"readonly protocol cleanup");
    std::cout<<"readonly_actors source="<<child->process_id()<<" client="<<GetCurrentProcessId()
        <<" recovery_source="<<recover_source<<" exit_confirmed="<<result.exit_confirmed
        <<" supervisor_retired="<<child->retirement_confirmed()<<"\n";
  }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    require_hosted_opt_in();
    require(argc >= 2 && argc <= 4, "explicit probe mode required");
    const std::wstring_view mode(argv[1]);
    if (mode == L"--probe-recover-child") {
      require(argc == 4, "recovery identity/protocol missing");
      recover(process_number(argv[2]),argv[3]); return 0;
    }
    if(mode==L"--probe-child"||mode==L"--probe-crash-child") {
      require(argc==3,"missing readonly client protocol");probe(mode==L"--probe-crash-child",argv[2]);return 0;
    }
    require(argc==2,"unexpected controller argument");
    require(mode == L"--run-disposable-ci", "unknown probe mode");
    const source_fixture::ControllerBudget controller_budget;
    wchar_t path[512]{};
    const auto n = GetModuleFileNameW(nullptr, path, 512);
    require(n && n < 512, "probe executable path unavailable");
    const fs::path executable(std::wstring(path,n));
    const ExternalReadonly normal(executable,false);const auto result=normal.result;
    std::cout << "mount_probe exit=" << result.exit_code << " stage=" << (result.exit_code >> 16U)
        << " detail=" << (result.exit_code & 0xffffU) << " timed_out=" << result.timed_out
        << " exit_confirmed=" << result.exit_confirmed << " peak_commit_bytes=" << result.peak_committed_bytes << '\n';
    require(result.exit_confirmed && !result.timed_out && result.exit_code == 0, "supervised mount probe failed");
    emit_space_report(result.process_id,"normal");
    std::cout << "PASS: real mounted native transactional image reads, sparse tails, persistent inode IDs, mapped I/O, enumeration, write rejection and backing parity.\n";
    const ExternalReadonly crash(executable,true);const auto killed=crash.result;
    std::cout << "crash_probe exit=" << killed.exit_code << " timed_out=" << killed.timed_out
        << " exit_confirmed=" << killed.exit_confirmed << " peak_commit_bytes=" << killed.peak_committed_bytes << '\n';
    require(killed.process_id != 0 && killed.timed_out && killed.exit_confirmed &&
        killed.exit_code == ERROR_TIMEOUT, "mounted helper was not forcibly terminated");
    emit_space_report(killed.process_id,"crash");
    const ExternalReadonly resumed(executable,false,killed.process_id);const auto recovered=resumed.result;
    std::cout << "recovery_probe exit=" << recovered.exit_code << " stage=" << (recovered.exit_code >> 16U)
        << " detail=" << (recovered.exit_code & 0xffffU) << " timed_out=" << recovered.timed_out
        << " exit_confirmed=" << recovered.exit_confirmed << " peak_commit_bytes=" << recovered.peak_committed_bytes << '\n';
    require(recovered.exit_confirmed && !recovered.timed_out && recovered.exit_code == 0,
        "post-termination verification/remount failed");
    emit_space_report(killed.process_id,"recovery");
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
