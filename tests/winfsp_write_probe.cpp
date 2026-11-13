// Experimental disposable-VM writable qualification. Never a product mount CLI.
#include "winfsp_session.hpp"
#include "winfsp_native.hpp"
#include "bounded_process.hpp"
#include "supervised_source_fixture.hpp"
#include "ram_client_channel.hpp"
#include "locked_ram_fixture.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "../src/record_digest.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
namespace {
namespace fs = std::filesystem;
unsigned stage = 1;
std::uint32_t source_client_process=0;
// Independent writable fixture: image <=4 MiB, two reports <=2048 bytes each.
constexpr std::uint64_t image_cap=4U*1024*1024;
constexpr std::size_t report_cap=2048;
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
      environment(L"FLITZFS_ALLOW_DISPOSABLE_MOUNT") == L"1" &&
      environment(L"FLITZFS_ALLOW_DISPOSABLE_WRITE") == L"1",
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
    root = fs::path(base) / (L"flitzfs-write-probe-" +
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
flitzfs::WinFspSessionApi live_api() {
  return {FspFileSystemPreflight, FspFileSystemCreate, FspFileSystemStartDispatcher,
      FspFileSystemSetMountPoint, FspFileSystemStopDispatcher,
      FspFileSystemRemoveMountPoint, FspFileSystemDelete,FspFileSystemOperationProcessId};
}
// This guard is specific to the forced-termination source, not a replacement
// for the native callback authorization/adoption/rollback checks.
struct SourceGuard {
  const FSP_FILE_SYSTEM_INTERFACE* original=nullptr;
  FSP_FILE_SYSTEM_INTERFACE callbacks{};
  std::atomic<DWORD> expected{0},observed{0},self_denied{0},flush_succeeded{0};
  std::atomic<bool> armed{false};
} source_guard;
bool source_origin_allowed() noexcept {
  if(!source_guard.armed.load(std::memory_order_acquire))return true;
  // The pinned SDK permits this query ONLY in Create/Open (or eligible Rename).
  const auto origin=FspFileSystemOperationProcessId();
  if(origin==GetCurrentProcessId())return true; // Product adapter must refuse this request.
  const auto expected=source_guard.expected.load(std::memory_order_acquire);
  if(origin==expected&&origin){source_guard.observed.store(origin,std::memory_order_release);return true;}
  return origin==0; // Kernel-origin operations do not fabricate a user actor.
}
NTSTATUS source_create(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 granted,UINT32 attributes,
    PSECURITY_DESCRIPTOR security,UINT64 allocation,PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
  if(!source_origin_allowed())return STATUS_ACCESS_DENIED;
  const auto status=source_guard.original->Create(fs,name,options,granted,attributes,security,allocation,context,info);
  if(status==STATUS_ACCESS_DENIED&&source_guard.armed.load(std::memory_order_acquire)&&
      FspFileSystemOperationProcessId()==GetCurrentProcessId())source_guard.self_denied.fetch_add(1);
  return status;
}
NTSTATUS source_open(FSP_FILE_SYSTEM* fs,PWSTR name,UINT32 options,UINT32 granted,
    PVOID* context,FSP_FSCTL_FILE_INFO* info) noexcept {
  if(!source_origin_allowed())return STATUS_ACCESS_DENIED;
  const auto status=source_guard.original->Open(fs,name,options,granted,context,info);
  if(status==STATUS_ACCESS_DENIED&&source_guard.armed.load(std::memory_order_acquire)&&
      FspFileSystemOperationProcessId()==GetCurrentProcessId())source_guard.self_denied.fetch_add(1);
  return status;
}
NTSTATUS source_flush(FSP_FILE_SYSTEM* fs,PVOID context,FSP_FSCTL_FILE_INFO* info) noexcept {
  const auto status=source_guard.original->Flush(fs,context,info);
  if(status==STATUS_SUCCESS&&source_guard.armed.load(std::memory_order_acquire))
    source_guard.flush_succeeded.fetch_add(1,std::memory_order_release);
  return status;
}
NTSTATUS create_source_filesystem(PWSTR device,const FSP_FSCTL_VOLUME_PARAMS* params,
    const FSP_FILE_SYSTEM_INTERFACE* callbacks,FSP_FILE_SYSTEM** output) noexcept {
  if(source_guard.original||!callbacks||!callbacks->Create||!callbacks->Open||
      !callbacks->Flush||callbacks->CreateEx||!source_guard.expected.load())return STATUS_INVALID_PARAMETER;
  source_guard.original=callbacks;source_guard.callbacks=*callbacks;
  source_guard.callbacks.Create=source_create;source_guard.callbacks.Open=source_open;
  source_guard.callbacks.Flush=source_flush;
  return FspFileSystemCreate(device,params,&source_guard.callbacks,output);
}
flitzfs::WinFspSessionApi source_api() {
  auto api=live_api();api.create=create_source_filesystem;return api;
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
      cpu.CpuRate == 2000 && GetPriorityClass(GetCurrentProcess())==BELOW_NORMAL_PRIORITY_CLASS, "probe launched outside required job limits");
}

flitzfs::StoreIdentity identity() {
  flitzfs::StoreIdentity id{};id[0]=std::byte{0x57};id[1]=std::byte{0x52};id[15]=std::byte{0x58};return id;
}
std::vector<std::byte> expected_data() {
  std::vector<std::byte> bytes(7001);
  for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i/31+i%251)%256);
  return bytes;
}
std::string commit() {
  const auto text=environment(L"GITHUB_SHA");
  require(text.size()==40&&std::all_of(text.begin(),text.end(),[](wchar_t c){
    return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),"invalid source commit");
  std::string result;result.reserve(text.size());
  for(auto c:text)result.push_back(static_cast<char>(c));return result;
}
bool mount_absent(const fs::path& mount) {
  if(GetFileAttributesW(mount.c_str())!=INVALID_FILE_ATTRIBUTES)return false;
  const auto error=GetLastError();
  require(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND,"mount absence inspection failed");
  return true;
}
void require_removed(const fs::path& mount) {
  const auto deadline=GetTickCount64()+2000;
  while(!mount_absent(mount)&&GetTickCount64()<deadline)Sleep(10);
  require(mount_absent(mount),"owned writable mount remained after stop"); // Never remove a stale junction.
}
void report(const Fixture& fixture,std::string_view phase,std::uint32_t source_process,
    const flitzfs::NativeFilesystemSnapshot& view,bool stopped) {
  const auto image=fixture.root/L"native.fltx";const auto info=view.storage_info();
  const auto inode=view.stat("sample.bin").inode;const auto basic=view.basic_metadata(inode);
  const auto security=view.security_descriptor(inode);const auto allocation=view.allocation_info(inode);
  require(view.identity()==identity()&&view.list().size()==1&&inode==2&&!view.needs_session_recovery()&&
      view.read("sample.bin",0,10000)==expected_data(),"independent native bytes/namespace/identity");
  require(basic&&basic->creation_time&&security&&IsValidSecurityDescriptor(
      const_cast<std::byte*>(security->data()))&&allocation.reserved_bytes&&
      *allocation.reserved_bytes>=expected_data().size(),"persisted metadata/security/reservation");
  require(info.replicated_checkpoint&&!info.degraded_checkpoint&&
      info.physical_end<=image_cap&&fs::file_size(image)<=image_cap,"native checkpoint/image cap");
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"{\"schema\":1,\"phase\":\""<<phase<<"\",\"commit\":\""<<commit()
      <<"\",\"source_process\":"<<source_process<<",\"observer_process\":"<<GetCurrentProcessId()
      <<",\"user_bytes\":7001,\"inode\":"<<inode<<",\"namespace_sequence\":"<<view.sequence()
      <<",\"generation\":"<<info.generation<<",\"image_bytes\":"<<fs::file_size(image)
      <<",\"image_cap\":"<<image_cap<<",\"contexts\":0,\"explicit_flush_observed\":"
      <<(phase=="write"?"true":"false")<<",\"cpu_verified\":true"
      <<",\"dispatcher_stopped\":"<<(stopped?"true":"false")<<",\"gpu\":false";
  if(source_client_process)out<<",\"source_protocol\":\"external-client\",\"client_process\":"<<source_client_process
      <<",\"host_self_io_rejected\":"<<(phase=="write"?"true":"false")
      <<",\"client_flush_callback_observed\":"<<(phase=="write"?"true":"false");
  out<<'}';
  const auto text=out.str();require(text.size()<=report_cap,"writable report budget");
  const auto* first=reinterpret_cast<const std::byte*>(text.data());
  write_new(fixture.root/(phase=="write"?L"write.json":L"verify.json"),{first,first+text.size()});
}
void prepare_external_actor(std::uint32_t client) {
  require(client&&client!=GetCurrentProcessId(),"external client/source identity");
  source_client_process=client;source_guard.expected.store(client,std::memory_order_release);
}
void self_open_probe(flitzfs::WinFspNativeSession& session,
    const flitzfs::WinFspNativeWritable* observer,const fs::path& mount,bool empty=true) {
  require(session.stats().contexts==0,"source setup retained namespace handles");
  source_guard.armed.store(true,std::memory_order_release);
  // Prove the guard using the actual SDK/driver. This attempt MUST NOT acquire
  // a namespace handle or mutate the newly created native filesystem.
  {
    const auto before=observer->snapshot();
    const auto denied_path=mount/L"host-self-io-probe.bin";
    const auto attempted=CreateFileW(denied_path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    const auto error=GetLastError();Handle denied(attempted);
    require(denied.value==INVALID_HANDLE_VALUE&&error==ERROR_ACCESS_DENIED&&
        source_guard.self_denied.load()>0&&session.self_origin_rejected(),
        "filesystem host self-I/O was not refused by product adapter");
    const auto after=observer->snapshot();
    require(before.storage_info().generation==after.storage_info().generation&&
        before.storage_info().physical_end==after.storage_info().physical_end&&
        (empty?after.list().empty():after.list().size()==before.list().size())&&
        session.stats().contexts==0,"self-I/O guard changed native state");
  }
}
void external_origin_proof(const flitzfs::WinFspNativeSession& session) {
  require(source_guard.observed.load(std::memory_order_acquire)==source_client_process&&
      source_guard.flush_succeeded.load(std::memory_order_acquire)>0&&
      !session.requires_offline()&&!session.dispatcher_stopped_unexpectedly(),
      "external client/flush proof missing");
}
void basic_client(const fs::path& mount) {
  wchar_t filesystem[32]{};DWORD flags=0,serial=0;
  checked(GetVolumeInformationW((mount.native()+L"\\").c_str(),nullptr,0,&serial,nullptr,&flags,filesystem,32),
      "writable volume identity");
  require(std::wstring_view(filesystem)==L"FlitzFS"&&serial&&!(flags&FILE_READ_ONLY_VOLUME),
      "wrong/read-only mounted target");
  stage=5;
  const auto expected=expected_data();const auto target=mount/L"sample.bin";
  {
    Handle file(CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"create mounted synthetic file");
    DWORD accepted=0;checked(WriteFile(file.value,expected.data(),static_cast<DWORD>(expected.size()),&accepted,nullptr),
        "mounted cached WriteFile");
    require(accepted==expected.size(),"mounted accepted byte count");
    stage=6;
    // Plain cached WriteFile return alone is NOT our persistence oracle.
    checked(FlushFileBuffers(file.value),"mounted explicit FlushFileBuffers");
    FILE_STANDARD_INFO standard{};FILE_BASIC_INFO basic{};BY_HANDLE_FILE_INFORMATION id{};
    checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&standard,sizeof(standard)),"mounted file size");
    checked(GetFileInformationByHandleEx(file.value,FileBasicInfo,&basic,sizeof(basic)),"mounted basic metadata");
    checked(GetFileInformationByHandle(file.value,&id),"mounted persistent identity");
    require(standard.EndOfFile.QuadPart==7001&&standard.AllocationSize.QuadPart>=7001&&
        standard.AllocationSize.QuadPart%4096==0&&basic.CreationTime.QuadPart>0&&
        id.nFileIndexHigh==0&&id.nFileIndexLow==2,"mounted metadata/cache reservation/identity");
    LARGE_INTEGER zero{};checked(SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN),"mounted seek");
    std::vector<std::byte> got(expected.size()+37);DWORD count=0;
    checked(ReadFile(file.value,got.data(),static_cast<DWORD>(got.size()),&count,nullptr),"mounted same-handle readback");
    require(count==expected.size()&&std::equal(expected.begin(),expected.end(),got.begin()),"mounted byte parity");
  }
  stage=7;
  require(read_all(target,expected.size())==expected,"mounted close/reopen parity");
}
void write_probe(std::wstring_view token,std::uint32_t client) {
  require_probe_job();stage=3;Fixture fixture;
  const auto image=fixture.root/L"native.fltx",mount=fixture.root/L"mount";
  prepare_external_actor(client);
  source_fixture::Events protocol(false,std::wstring(token));
  auto adapter=flitzfs::WinFspNativeWritable::create_new_hosted(
      image,identity(),{flitzfs::StoreCompressionMode::automatic,6,true},image_cap);
  const auto* observer=adapter.get();
  flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
  stage=4;
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"writable mount startup");
  self_open_probe(session,observer,mount);
  stage=5;protocol.signal(source_fixture::Phase::ready);protocol.wait_written();
  external_origin_proof(session);
  const auto deadline=GetTickCount64()+2000;
  while(session.stats().contexts&&GetTickCount64()<deadline&&!session.requires_offline()&&
      !session.dispatcher_stopped_unexpectedly())Sleep(10);
  require(!session.requires_offline()&&!session.dispatcher_stopped_unexpectedly()&&!session.stats().contexts,
      "writable contexts did not retire cleanly");
  stage=8;session.stop();session.stop();
  require(session.state()==flitzfs::SessionState::stopped&&session.dispatcher_stopped()&&
      !session.dispatcher_stopped_unexpectedly()&&!session.requires_offline()&&!session.stats().contexts,
      "writable dispatcher/writer shutdown");
  require_removed(mount);stage=9;
  auto view=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));view.verify_all();
  report(fixture,"write",GetCurrentProcessId(),view,true);
}
std::uint32_t process_number(std::wstring_view text) {
  require(!text.empty()&&text.size()<=10,"invalid verifier source identity");std::uint64_t n=0;
  for(auto c:text){require(c>=L'0'&&c<=L'9',"invalid verifier digit");n=n*10+static_cast<unsigned>(c-L'0');
    require(n<=UINT32_MAX,"verifier identity overflow");}
  require(n!=0,"zero verifier identity");return static_cast<std::uint32_t>(n);
}
void verify_probe(std::uint32_t source_process) {
  require_probe_job();stage=10;Fixture fixture(source_process);require_removed(fixture.root/L"mount");
  auto view=flitzfs::NativeFilesystemSnapshot::open(
      flitzfs::ObjectStoreSnapshot::open_file(fixture.root/L"native.fltx"));view.verify_all();
  report(fixture,"verify",source_process,view,false); // No dispatcher in the independent CPU verifier.
}
void emit(std::uint32_t source_process,std::string_view phase) {
  Fixture fixture(source_process);const auto leaf=phase=="ram-handoff"?L"ram-handoff.json":phase=="write"?L"write.json":L"verify.json";
  Handle file(CreateFileW((fixture.root/leaf).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,nullptr));checked(file.value!=INVALID_HANDLE_VALUE,"open owned writable report");
  FILE_ATTRIBUTE_TAG_INFO attr{};FILE_STANDARD_INFO size{};
  checked(GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attr,sizeof(attr)),"report attributes");
  checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&size,sizeof(size)),"report size");
  require(GetFileType(file.value)==FILE_TYPE_DISK&&!(attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
      size.NumberOfLinks==1&&size.EndOfFile.QuadPart>0&&size.EndOfFile.QuadPart<=report_cap,"unsafe/oversized report");
  std::string text(static_cast<std::size_t>(size.EndOfFile.QuadPart),'\0');DWORD got=0;
  checked(ReadFile(file.value,text.data(),static_cast<DWORD>(text.size()),&got,nullptr),"read owned writable report");
  require(got==text.size()&&text.find('\0')==text.npos&&text.find("\"phase\":\""+std::string(phase)+"\"")!=text.npos&&
      text.find("\"commit\":\""+commit()+"\"")!=text.npos,"incomplete/wrong-source report");
  if(phase=="ram-handoff") {
    require(mount_absent(fixture.root/L"mount"),"RAM mount remained after child exit");
    std::cout<<"Windows RAM handoff evidence: "<<text<<"\n";
    return;
  }
  require(fs::file_size(fixture.root/L"native.fltx")+fs::file_size(fixture.root/L"write.json")+
      (phase=="verify"?fs::file_size(fixture.root/L"verify.json"):0)<=image_cap+2*report_cap,
      "writable aggregate fixture cap");
  std::cout<<"Windows writable mount evidence: "<<text<<"\n";
}

bool restart_profile(std::string_view profile){return profile=="restart"||profile=="crash-restart";}
std::vector<std::byte> continuation_data() {
  std::vector<std::byte> data(3073);
  for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::byte>((i*17+i/7)%256);
  return data;
}
std::vector<std::byte> profile_data(std::string_view profile) {
  if(profile=="crash"||restart_profile(profile))return expected_data();
  if(profile=="delete")return std::vector<std::byte>(37,std::byte{0x4b});
  auto bytes=expected_data();bytes.resize(17);bytes.resize(89,std::byte{0});return bytes;
}
std::string hex_digest(std::span<const std::byte> bytes) {
  const auto digest=flitzfs::detail::record_digest(bytes);
  constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(64);
  for(auto byte:digest){const auto n=std::to_integer<unsigned>(byte);result.push_back(digits[n>>4]);result.push_back(digits[n&15]);}
  return result;
}
void missing(const flitzfs::NativeFilesystemSnapshot& view,std::string_view path) {
  bool absent=false;try{(void)view.stat(path);}catch(const flitzfs::NativeFsError& e){
    absent=e.code()==flitzfs::NativeFsErrorCode::not_found;}
  require(absent,"removed/renamed native path remained");
}
std::string extended_text(std::string_view profile,std::string_view phase,std::uint32_t source_process,
    std::uint32_t observer_process,const flitzfs::NativeFilesystemSnapshot& view,std::uint32_t contexts) {
  const auto expected=profile_data(profile);const auto path=profile=="mutations"?"renamed.bin":
      profile=="delete"?"keep.bin":"sample.bin";
  const auto inode=view.stat(path).inode;const auto info=view.storage_info();
  const auto basic=view.basic_metadata(inode);const auto allocation=view.allocation_info(inode).reserved_bytes;
  const auto security=view.security_descriptor(inode);const auto bytes=view.read(path,0,10000);
  const std::vector<std::byte> kept(37,std::byte{0x4b});
  require(view.identity()==identity()&&!view.needs_session_recovery()&&bytes==expected&&
      view.list().size()==(profile=="mutations"||(restart_profile(profile)&&phase=="verify")?2U:1U)&&inode==(profile=="delete"?3U:2U),
      "extended native namespace/data/identity");
  if(profile=="mutations"){
    require(view.read("keep.bin",0,100)==kept,"EOF/rename lost unrelated acknowledged file");
    missing(view,"sample.bin");
  }else if(profile=="delete"){missing(view,"sample.bin");missing(view,"renamed.bin");}
  if(restart_profile(profile)&&phase=="verify")
    require(view.stat("continued.bin").inode==3&&view.read("continued.bin",0,10000)==continuation_data(),
        "restart lost continued bytes/inode");
  require(basic&&basic->creation_time&&security&&IsValidSecurityDescriptor(
      const_cast<std::byte*>(security->data()))&&allocation&&*allocation>=bytes.size()&&
      info.replicated_checkpoint&&!info.degraded_checkpoint&&info.physical_end<=image_cap,
      "extended metadata/security/reservation/checkpoint");
  require((phase=="ack"&&contexts>0&&contexts<=256)||(phase!="ack"&&contexts==0),"extended context phase");
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"{\"schema\":2,\"profile\":\""<<profile<<"\",\"phase\":\""<<phase<<"\",\"commit\":\""<<commit()
      <<"\",\"source_process\":"<<source_process<<",\"observer_process\":"<<observer_process
      <<",\"user_bytes\":"<<bytes.size()<<",\"inode\":"<<inode<<",\"file_count\":"<<view.list().size()
      <<",\"namespace_sequence\":"<<view.sequence()<<",\"generation\":"<<info.generation
      <<",\"image_bytes\":"<<info.physical_end<<",\"image_cap\":"<<image_cap<<",\"contexts\":"<<contexts
      <<",\"creation_time\":"<<basic->creation_time<<",\"last_access_time\":"<<basic->last_access_time
      <<",\"last_write_time\":"<<basic->last_write_time<<",\"change_time\":"<<basic->change_time
      <<",\"attributes\":"<<basic->attributes
      <<",\"reservation\":"<<*allocation<<",\"data_digest\":\""<<hex_digest(bytes)
      <<"\",\"security_digest\":\""<<hex_digest(*security)<<"\",\"keep_digest\":\""
      <<(profile=="mutations"?hex_digest(kept):restart_profile(profile)&&phase=="verify"?hex_digest(continuation_data()):std::string(64,'0'))
      <<"\",\"explicit_flush_observed\":"<<(phase=="verify"?"false":"true")
      <<",\"cpu_verified\":true,\"dispatcher_stopped\":"<<(phase=="state"?"true":"false")<<",\"gpu\":false";
  if(source_client_process) {
    require(((profile=="crash"||profile=="crash-restart")&&(phase=="ack"||phase=="verify"))||
        ((profile=="mutations"||profile=="delete"||profile=="restart")&&(phase=="state"||phase=="verify")),
        "external client on wrong evidence phase");
    out<<",\"source_protocol\":\"external-client\",\"client_process\":"<<source_client_process
        <<",\"host_self_io_rejected\":"<<(phase!="verify"?"true":"false")
        <<",\"client_flush_callback_observed\":"<<(phase!="verify"?"true":"false");
  }
  out<<'}';
  return out.str();
}
std::string read_state(const Fixture& fixture);
void extended_report(const Fixture& fixture,std::string_view profile,std::string_view phase,
    std::uint32_t source_process,const flitzfs::NativeFilesystemSnapshot& view,std::uint32_t contexts,
    std::uint32_t resumer_process=0) {
  auto text=extended_text(profile,phase,source_process,GetCurrentProcessId(),view,contexts);
  if(restart_profile(profile)&&phase=="verify") {
    const auto canonical=extended_text(profile,phase,source_process,0,view,contexts);
    const auto* first=reinterpret_cast<const std::byte*>(canonical.data());
    const auto state_digest=hex_digest({first,canonical.size()});
    const auto source=read_state(fixture);const auto end=source.find('\n');
    require(end!=source.npos&&end>0,"missing continuation acknowledgement boundary");
    const auto* ack=reinterpret_cast<const std::byte*>(source.data());
    require(resumer_process!=0,"missing actual resumer identity");
    text.pop_back();text+=",\"state_digest\":\""+state_digest+"\",\"source_ack_digest\":\""+
        hex_digest({ack,end})+"\",\"resumer_process\":"+std::to_string(resumer_process)+"}";
  }
  require(text.size()<=report_cap,"extended report cap");
  Handle file(CreateFileW((fixture.root/(phase=="verify"?L"state-verify.json":L"state.json")).c_str(),
      GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
  checked(file.value!=INVALID_HANDLE_VALUE,"exclusive extended report");DWORD accepted=0;
  checked(WriteFile(file.value,text.data(),static_cast<DWORD>(text.size()),&accepted,nullptr),"extended report write");
  require(accepted==text.size(),"short extended report");
  checked(FlushFileBuffers(file.value),"extended acknowledgement report flush");
}
void mounted_identity(const fs::path& mount) {
  wchar_t name[32]{};DWORD flags=0,serial=0;
  checked(GetVolumeInformationW((mount.native()+L"\\").c_str(),nullptr,0,&serial,nullptr,&flags,name,32),
      "extended volume identity");
  require(std::wstring_view(name)==L"FlitzFS"&&serial&&!(flags&FILE_READ_ONLY_VOLUME),"wrong extended volume");
}
void write_bytes(HANDLE file,const std::vector<std::byte>& bytes) {
  DWORD count=0;checked(WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr),"extended mounted write");
  require(count==bytes.size(),"short extended mounted write");checked(FlushFileBuffers(file),"extended mounted explicit flush");
}
void read_handle(HANDLE file,const std::vector<std::byte>& expected) {
  LARGE_INTEGER zero{};checked(SetFilePointerEx(file,zero,nullptr,FILE_BEGIN),"extended mounted seek");
  std::vector<std::byte> bytes(expected.size()+1);DWORD got=0;
  checked(ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr),"extended mounted read");
  require(got==expected.size()&&std::equal(expected.begin(),expected.end(),bytes.begin()),"extended mounted byte/zero-tail parity");
}
void finish_session(flitzfs::WinFspNativeSession& session,const fs::path& mount) {
  const auto deadline=GetTickCount64()+2000;
  while(session.stats().contexts&&GetTickCount64()<deadline&&!session.requires_offline()&&
      !session.dispatcher_stopped_unexpectedly())Sleep(10);
  require(!session.stats().contexts&&!session.requires_offline()&&!session.dispatcher_stopped_unexpectedly(),
      "extended contexts did not retire");
  session.stop();session.stop();
  require(session.state()==flitzfs::SessionState::stopped&&session.dispatcher_stopped()&&
      !session.requires_offline()&&!session.dispatcher_stopped_unexpectedly(),"extended normal shutdown");
  require_removed(mount);
}
struct RamClient {
  const fs::path mount;
  Handle writer,reader;
  const std::vector<std::byte> original=std::vector<std::byte>(65536,std::byte{81});
  BY_HANDLE_FILE_INFORMATION before{};
  bool finished=false;
  explicit RamClient(fs::path path):mount(std::move(path)){}
  void perform(ram_client_fixture::Step step) {
    using Step=ram_client_fixture::Step;
    switch(step) {
      case Step::initial: {
        mounted_identity(mount);
        constexpr DWORD share=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE;
        const auto path=mount/L"sample.bin";
        writer.value=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,share,nullptr,
            CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        checked(writer.value!=INVALID_HANDLE_VALUE,"external RAM create");
        write_bytes(writer.value,original);
        reader.value=CreateFileW(path.c_str(),GENERIC_READ,share,nullptr,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        checked(reader.value!=INVALID_HANDLE_VALUE,"external RAM peer open");
        read_handle(reader.value,original);
        checked(GetFileInformationByHandle(writer.value,&before),"external RAM initial identity");
        // Exact serial of this fixture's fixed UUID, not host hardware/configuration.
        require(before.dwVolumeSerialNumber==0x58005257U&&
            (before.nFileIndexHigh||before.nFileIndexLow),"external RAM volume/file ID");
        break;
      }
      case Step::recovered:case Step::copy_read:
        read_handle(reader.value,original);break;
      case Step::rewrite: {
        LARGE_INTEGER start{};
        checked(SetFilePointerEx(writer.value,start,nullptr,FILE_BEGIN),"external RAM intervening seek");
        write_bytes(writer.value,original);break;
      }
      case Step::published: {
        BY_HANDLE_FILE_INFORMATION after{};
        checked(GetFileInformationByHandle(writer.value,&after),"external RAM retained identity");
        require(after.dwVolumeSerialNumber==before.dwVolumeSerialNumber&&
            after.nFileIndexHigh==before.nFileIndexHigh&&after.nFileIndexLow==before.nFileIndexLow,
            "external RAM published identity changed");
        read_handle(reader.value,original);break;
      }
      case Step::finish: {
        LARGE_INTEGER end{};
        checked(SetFilePointerEx(writer.value,end,nullptr,FILE_END),"external RAM continuation seek");
        const std::vector<std::byte> tail(73,std::byte{0x3d});write_bytes(writer.value,tail);
        auto final=original;final.insert(final.end(),tail.begin(),tail.end());
        read_handle(reader.value,final);writer.reset();reader.reset();finished=true;break;
      }
      default:throw std::runtime_error("foreign RAM client step");
    }
  }
};
void ram_handoff_probe(std::wstring_view token,std::uint32_t client) {
  require_probe_job();prepare_external_actor(client);
  ram_client_fixture::Channel channel(false,std::wstring(token));
  using Step=ram_client_fixture::Step;
  stage=70;
  Fixture fixture;
  const auto mount=fixture.root/L"mount";
  auto volume_id=identity();
  auto source_id=volume_id;
  auto target_id=volume_id;
  auto rejected_id=volume_id;
  auto stale_id=volume_id;
  source_id[0]=std::byte{1};
  target_id[0]=std::byte{2};
  rejected_id[0]=std::byte{3};
  stale_id[0]=std::byte{4};
  std::uint64_t raw_payload=0,compressed_payload=0,final_generation=0;
  DWORD serial=0;
  std::size_t copy_steps=0;
  {
    locked_ram_fixture::WorkingSet working;
    {
      auto head=std::make_shared<locked_ram_fixture::Store>(flitzfs::kVolumeHeadBytes);
      auto source=std::make_shared<locked_ram_fixture::Store>(image_cap);
      auto target=std::make_shared<locked_ram_fixture::Store>(image_cap);
      auto adapter=flitzfs::WinFspNativeWritable::create_managed_hosted(
          head,volume_id,source,source_id,{flitzfs::StoreCompressionMode::raw,6,false});
      const auto* observer=adapter.get();
      serial=observer->parameters().VolumeSerialNumber;
      flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
      require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"locked-RAM mount startup");
      self_open_probe(session,observer,mount);
      stage=71;channel.request(Step::initial);external_origin_proof(session);
      const std::vector<std::byte> original(65536,std::byte{81});
      require(session.stats().contexts==2,"RAM live context count");
      auto pinned=observer->snapshot();
      raw_payload=pinned.storage_info().stored_payload_bytes;
      stage=72;
      {
        auto undersized_candidate=std::make_shared<locked_ram_fixture::Store>(flitzfs::kVolumeHeadBytes);
        auto recovered=session.recompress_managed_hosted(undersized_candidate,rejected_id);
        require(recovered.recovered&&recovered.head.selection.epoch==1&&
            recovered.head.selection.image_identity==source_id&&undersized_candidate->extent()==0,
            "RAM capacity failure did not recover original authority");
      }
      channel.request(Step::recovered);
      require(session.stats().contexts==2,"RAM recovery lost mounted contexts");
      stage=73;
      using Action=flitzfs::HostedIdleMaintenanceAction;
      using State=flitzfs::HostedIdleMaintenanceState;
      const flitzfs::StoreCompressionOptions compression{flitzfs::StoreCompressionMode::automatic,6,true};
      require(session.begin_idle_recompression_hosted(target,stale_id,compression).state==State::begun&&
          session.idle_recompression_hosted(Action::copy).state==State::copying,
          "RAM idle candidate did not begin");
      // An actual acknowledged write advances the source while an unpublished
      // candidate exists. Reuse the bytes to keep the independent oracle fixed.
      channel.request(Step::rewrite);
      require(session.idle_recompression_hosted(Action::copy).state==State::stale,
          "RAM idle candidate ignored a foreground write");
      target.reset();
      target=std::make_shared<locked_ram_fixture::Store>(image_cap);
      require(session.begin_idle_recompression_hosted(target,target_id,compression).state==State::begun,
          "RAM later idle opportunity did not begin");
      const auto objects=observer->snapshot().storage_info().objects;
      require(objects>0&&objects<=ram_client_fixture::Sequence::max_copy_rounds,"RAM copy round bound");
      for(std::size_t step=1;step<=objects;++step) {
        require(session.idle_recompression_hosted(Action::copy).state==
            (step==objects?State::ready:State::copying),"RAM idle copy progress");
        ++copy_steps;
        channel.request(Step::copy_read);
      }
      const auto handoff=session.idle_recompression_hosted(Action::publish);
      require(handoff.state==State::published&&handoff.head.selection.epoch==2&&
          handoff.head.selection.volume_identity==volume_id&&handoff.head.selection.image_identity==target_id,
          "RAM mounted handoff did not select verified candidate");
      require(session.begin_idle_recompression_hosted({},stale_id,compression).state==State::suppressed,
          "RAM idle policy reconsidered the image it just published");
      compressed_payload=observer->snapshot().storage_info().stored_payload_bytes;
      require(compressed_payload<raw_payload,"RAM mounted recompression did not reduce payload");
      channel.request(Step::published);
      require(session.stats().contexts==2,"RAM handoff lost contexts");
      stage=74;
      channel.request(Step::finish);
      require(channel.complete()&&channel.copy_rounds()==copy_steps,"RAM source protocol completion");
      auto final=original;const std::vector<std::byte> tail(73,std::byte{0x3d});
      final.insert(final.end(),tail.begin(),tail.end());
      finish_session(session,mount);
      stage=75;
      auto selected=flitzfs::NativeVolumeHeadView::open(head,volume_id,
          [&](const auto& id)->std::shared_ptr<flitzfs::StoreByteIo>{
            require(id==target_id,"RAM CPU reader selected wrong image");
            return target;
          });
      const auto& view=selected.filesystem();
      require(view.read("sample.bin",0,65609)==final&&
          pinned.read("sample.bin",0,65536)==original&&view.stat("sample.bin").inode==2,
          "RAM independent final or pinned bytes");
      require(view.basic_metadata(1)==pinned.basic_metadata(1)&&
          view.security_descriptor(1)==pinned.security_descriptor(1),
          "RAM root metadata changed");
      final_generation=view.storage_info().generation;
      source->verify();
      target->verify();
      head->verify();
    }
    working.restore();
  }
  require(locked_ram_fixture::locked_bytes.load()==0&&locked_ram_fixture::cleanup_ok.load(),
      "RAM mounted storage cleanup");
  std::ostringstream report;
  report<<"{\"schema\":1,\"phase\":\"ram-handoff\",\"commit\":\""<<commit()
      <<"\",\"source_process\":"<<GetCurrentProcessId()<<",\"volume_serial\":"<<serial
      <<",\"idle_copy_steps\":"<<copy_steps<<",\"stale_candidate_rejected\":true,\"repeat_suppressed\":true"
      <<",\"epoch\":2,\"user_bytes\":65609,\"raw_payload\":"<<raw_payload
      <<",\"compressed_payload\":"<<compressed_payload<<",\"generation\":"<<final_generation
      <<",\"locked_peak\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after\":0,\"contexts\":0,\"cpu_verified\":true,\"mounted\":true"
      <<",\"unmounted\":true,\"recovered_capacity_failure\":true,\"gpu\":false,\"complete\":true"
      <<",\"source_protocol\":\"external-client\",\"client_process\":"<<source_client_process
      <<",\"host_self_io_rejected\":true,\"client_flush_callback_observed\":true}";
  const auto body=report.str();
  require(body.size()<=report_cap,"RAM handoff report cap");
  const auto* first=reinterpret_cast<const std::byte*>(body.data());
  write_new(fixture.root/L"ram-handoff.json",std::vector<std::byte>(first,first+body.size()));
}
void namespace_client(const fs::path& mount,bool deleting) {
  const std::string_view profile=deleting?"delete":"mutations";
  mounted_identity(mount);
  stage=21;
  {
    Handle file(CreateFileW((mount/L"sample.bin").c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"extended target create");
    auto original=expected_data();original.resize(73);write_bytes(file.value,original);
    {
      Handle keep(CreateFileW((mount/L"keep.bin").c_str(),GENERIC_READ|GENERIC_WRITE,
          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
      checked(keep.value!=INVALID_HANDLE_VALUE,"unrelated acknowledged file create");
      write_bytes(keep.value,std::vector<std::byte>(37,std::byte{0x4b}));
    }
    stage=22;
    if(deleting) {
      checked(DeleteFileW((mount/L"sample.bin").c_str()),"delete while original handle remains open");
      read_handle(file.value,original); // Pending/unlinked names must not redirect an owned inode.
    }else {
      LARGE_INTEGER offset{};offset.QuadPart=17;
      checked(SetFilePointerEx(file.value,offset,nullptr,FILE_BEGIN),"EOF shrink seek");
      checked(SetEndOfFile(file.value),"mounted EOF shrink");
      offset.QuadPart=89;checked(SetFilePointerEx(file.value,offset,nullptr,FILE_BEGIN),"EOF growth seek");
      checked(SetEndOfFile(file.value),"mounted EOF growth");
      checked(FlushFileBuffers(file.value),"mounted EOF explicit flush");
      read_handle(file.value,profile_data(profile));
      checked(MoveFileExW((mount/L"sample.bin").c_str(),(mount/L"renamed.bin").c_str(),0),"rename with held stable inode");
      BY_HANDLE_FILE_INFORMATION info{};checked(GetFileInformationByHandle(file.value,&info),"renamed stable identity");
      require(info.nFileIndexHigh==0&&info.nFileIndexLow==2,"rename changed owned inode");
      read_handle(file.value,profile_data(profile));
    }
  }
}
void namespace_probe(bool deleting,std::wstring_view token,std::uint32_t client) {
  require_probe_job();stage=20;Fixture fixture;const auto mount=fixture.root/L"mount",image=fixture.root/L"native.fltx";
  const std::string_view profile=deleting?"delete":"mutations";
  prepare_external_actor(client);
  source_fixture::Events protocol(false,std::wstring(token));
  auto adapter=flitzfs::WinFspNativeWritable::create_new_hosted(
      image,identity(),{flitzfs::StoreCompressionMode::automatic,6,true},image_cap);
  const auto* observer=adapter.get();
  flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"extended namespace mount");
  self_open_probe(session,observer,mount);
  stage=21;protocol.signal(source_fixture::Phase::ready);protocol.wait_written();
  external_origin_proof(session);
  stage=23;finish_session(session,mount);
  auto view=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));view.verify_all();
  extended_report(fixture,profile,"state",GetCurrentProcessId(),view,0);
}

void wait_for_supervised_termination(flitzfs::WinFspNativeSession& session) {
  const auto deadline=GetTickCount64()+30000;
  while(GetTickCount64()<deadline) {
    require(!session.requires_offline()&&!session.dispatcher_stopped(),
        "source dispatcher failed before supervised termination");
    Sleep(20); // Atomic health only: no storage/GPU/probing work after acknowledgement.
  }
  throw std::runtime_error("supervisor did not terminate acknowledged helper");
}
std::string read_state(const Fixture& fixture) {
  Handle file(CreateFileW((fixture.root/L"state.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,
      OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));checked(file.value!=INVALID_HANDLE_VALUE,"open source acknowledgement");
  FILE_ATTRIBUTE_TAG_INFO attr{};FILE_STANDARD_INFO info{};
  checked(GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attr,sizeof(attr)),"source acknowledgement attributes");
  checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&info,sizeof(info)),"source acknowledgement size");
  require(GetFileType(file.value)==FILE_TYPE_DISK&&info.NumberOfLinks==1&&
      !(attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
      info.EndOfFile.QuadPart>0&&info.EndOfFile.QuadPart<=report_cap,"source acknowledgement bounds");
  std::string text(static_cast<std::size_t>(info.EndOfFile.QuadPart),'\0');DWORD got=0;
  checked(ReadFile(file.value,text.data(),static_cast<DWORD>(text.size()),&got,nullptr),"source acknowledgement read");
  require(got==text.size()&&text.find('\0')==text.npos,"incomplete source acknowledgement");return text;
}
void restart_client(const fs::path& mount) {
  mounted_identity(mount);
  {
    Handle file(CreateFileW((mount/L"sample.bin").c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"restart source create");
    write_bytes(file.value,expected_data());read_handle(file.value,expected_data());stage=51;
  }
}
void restart_source(std::wstring_view token,std::uint32_t client) {
  require_probe_job();stage=50;Fixture fixture;const auto mount=fixture.root/L"mount",image=fixture.root/L"native.fltx";
  prepare_external_actor(client);
  source_fixture::Events protocol(false,std::wstring(token));
  auto adapter=flitzfs::WinFspNativeWritable::create_new_hosted(
      image,identity(),{flitzfs::StoreCompressionMode::automatic,6,true},image_cap);
  const auto* observer=adapter.get();
  flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"restart source mount");
  self_open_probe(session,observer,mount);
  protocol.signal(source_fixture::Phase::ready);protocol.wait_written();external_origin_proof(session);
  finish_session(session,mount);
  auto view=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));view.verify_all();
  extended_report(fixture,"restart","state",GetCurrentProcessId(),view,0);
}
void append_continuation(const Fixture& fixture,const std::string& original,std::string_view profile,
    std::uint32_t source_process,const flitzfs::NativeFilesystemSnapshot& view) {
  const auto canonical=extended_text(profile,"verify",source_process,0,view,0);
  const auto* data=reinterpret_cast<const std::byte*>(canonical.data());
  const auto* ack=reinterpret_cast<const std::byte*>(original.data());
  std::ostringstream out;out.imbue(std::locale::classic());
  out<<"\n{\"schema\":1,\"profile\":\""<<profile<<"\",\"phase\":\"continued\",\"commit\":\""<<commit()
      <<"\",\"source_process\":"<<source_process<<",\"observer_process\":"<<GetCurrentProcessId()
      <<",\"source_ack_digest\":\""<<hex_digest({ack,original.size()})
      <<"\",\"continued_state_digest\":\""<<hex_digest({data,canonical.size()})
      <<"\",\"generation\":"<<view.storage_info().generation<<",\"namespace_sequence\":"<<view.sequence()
      <<",\"image_bytes\":"<<view.storage_info().physical_end<<"}\n";
  const auto extra=out.str();require(original.size()+extra.size()<=report_cap,"continuation report budget");
  Handle file(CreateFileW((fixture.root/L"state.json").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
      OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));checked(file.value!=INVALID_HANDLE_VALUE,"exclusive continuation receipt");
  FILE_ATTRIBUTE_TAG_INFO attr{};FILE_STANDARD_INFO info{};
  checked(GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attr,sizeof(attr)),"continuation receipt attributes");
  checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&info,sizeof(info)),"continuation receipt size");
  require(GetFileType(file.value)==FILE_TYPE_DISK&&info.NumberOfLinks==1&&
      !(attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
      info.EndOfFile.QuadPart==static_cast<LONGLONG>(original.size()),"continuation receipt identity/size");
  std::string checked_text(original.size(),'\0');DWORD got=0;
  checked(ReadFile(file.value,checked_text.data(),static_cast<DWORD>(checked_text.size()),&got,nullptr),"recheck source receipt");
  require(got==checked_text.size()&&checked_text==original,"source acknowledgement changed");
  DWORD wrote=0;checked(WriteFile(file.value,extra.data(),static_cast<DWORD>(extra.size()),&wrote,nullptr),"append continuation receipt");
  require(wrote==extra.size(),"short continuation receipt");checked(FlushFileBuffers(file.value),"continuation receipt flush");
}
void continuation_client(const fs::path& mount) {
  mounted_identity(mount);
  require(read_all(mount/L"sample.bin",expected_data().size())==expected_data(),"remounted original bytes");
  {
    Handle file(CreateFileW((mount/L"continued.bin").c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"new continuation file");
    write_bytes(file.value,continuation_data());read_handle(file.value,continuation_data());
  }
}
void restart_resume(std::uint32_t source_process,bool killed,std::wstring_view token,std::uint32_t client) {
  require_probe_job();prepare_external_actor(client);
  source_fixture::Events protocol(false,std::wstring(token));
  stage=52;Fixture fixture(source_process);const auto image=fixture.root/L"native.fltx";
  require_removed(fixture.root/L"mount");const std::string_view profile=killed?"crash-restart":"restart";
  const auto original=read_state(fixture);std::uint64_t generation=0;
  {
    auto view=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));view.verify_all();
    const auto expected=extended_text(profile,killed?"ack":"state",source_process,source_process,view,killed?1U:0U);
    require(original==expected,"restart source checkpoint differs from exact acknowledgement");
    generation=view.storage_info().generation;
  } // Drop the read-only handle BEFORE acquiring the exclusive writer lease.
  auto adapter=flitzfs::WinFspNativeWritable::resume_retired_hosted(
      image,identity(),generation,{flitzfs::StoreCompressionMode::automatic,6,true},image_cap);
  stage=53;const auto mount=fixture.root/L"remount";
  const auto* observer=adapter.get();
  flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"persisted writable remount");
  self_open_probe(session,observer,mount,false);
  protocol.signal(source_fixture::Phase::ready);protocol.wait_written();external_origin_proof(session);
  stage=54;finish_session(session,mount);
  auto view=flitzfs::NativeFilesystemSnapshot::open(flitzfs::ObjectStoreSnapshot::open_file(image));view.verify_all();
  require(view.storage_info().generation>generation,"continuation did not publish a new checkpoint");
  append_continuation(fixture,original,profile,source_process,view);
}
void crash_source(std::wstring_view token,std::uint32_t client_process,std::string_view profile) {
  require_probe_job();stage=30;
  require(client_process&&client_process!=GetCurrentProcessId()&&
      (profile=="crash"||profile=="crash-restart"),"external source identity/profile");
  source_client_process=client_process;source_guard.expected.store(client_process,std::memory_order_release);
  source_fixture::Events protocol(false,std::wstring(token));
  Fixture fixture;const auto mount=fixture.root/L"mount",image=fixture.root/L"native.fltx";
  auto adapter=flitzfs::WinFspNativeWritable::create_new_hosted(
      image,identity(),{flitzfs::StoreCompressionMode::automatic,6,true},image_cap);
  auto* observer=adapter.get();
  flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"external source mount");
  self_open_probe(session,observer,mount);
  stage=31;protocol.signal(source_fixture::Phase::ready);protocol.wait_written();
  require(source_guard.observed.load(std::memory_order_acquire)==client_process&&
      source_guard.flush_succeeded.load(std::memory_order_acquire)>0&&
      session.stats().contexts==1&&!session.requires_offline()&&!session.dispatcher_stopped(),
      "external client/flush/context proof missing");
  {
    auto view=observer->snapshot();view.verify_all();
    extended_report(fixture,profile,"ack",GetCurrentProcessId(),view,session.stats().contexts);
  }
  stage=32;protocol.signal(source_fixture::Phase::ack);
  // No source process ever holds the mounted client file. The controller keeps
  // that file open until this exact host AND its supervisor have retired.
  wait_for_supervised_termination(session);
}
void extended_verify(std::uint32_t source_process,std::string_view profile,std::uint32_t resumer_process=0) {
  require_probe_job();stage=40;Fixture fixture(source_process);require_removed(fixture.root/L"mount");
  if(restart_profile(profile))require_removed(fixture.root/L"remount");
  auto view=flitzfs::NativeFilesystemSnapshot::open(
      flitzfs::ObjectStoreSnapshot::open_file(fixture.root/L"native.fltx"));view.verify_all();
  require(fs::file_size(fixture.root/L"native.fltx")<=image_cap,"recovered extended image cap");
  extended_report(fixture,profile,"verify",source_process,view,0,resumer_process);
}
void emit_extended(std::uint32_t source_process,std::string_view profile,std::string_view phase) {
  Fixture fixture(source_process);const auto path=fixture.root/(phase=="verify"?L"state-verify.json":L"state.json");
  Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT,nullptr));checked(file.value!=INVALID_HANDLE_VALUE,"open extended evidence");
  FILE_ATTRIBUTE_TAG_INFO attr{};FILE_STANDARD_INFO size{};
  checked(GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&attr,sizeof(attr)),"extended report attributes");
  checked(GetFileInformationByHandleEx(file.value,FileStandardInfo,&size,sizeof(size)),"extended report size");
  require(GetFileType(file.value)==FILE_TYPE_DISK&&!(attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&
      size.NumberOfLinks==1&&size.EndOfFile.QuadPart>0&&size.EndOfFile.QuadPart<=report_cap,"extended evidence bounds/type");
  std::string text(static_cast<std::size_t>(size.EndOfFile.QuadPart),'\0');DWORD got=0;
  checked(ReadFile(file.value,text.data(),static_cast<DWORD>(text.size()),&got,nullptr),"read extended evidence");
  require(got==text.size()&&text.find('\0')==text.npos&&text.find("\"commit\":\""+commit()+"\"")!=text.npos&&
      text.find("\"profile\":\""+std::string(profile)+"\"")!=text.npos&&
      text.find("\"phase\":\""+std::string(phase)+"\"")!=text.npos,"extended evidence source/phase");
  require(fs::file_size(fixture.root/L"native.fltx")+fs::file_size(fixture.root/L"state.json")+
      (phase=="verify"?fs::file_size(fixture.root/L"state-verify.json"):0)<=image_cap+2*report_cap,
      "extended aggregate fixture cap");
  std::cout<<"Windows writable extended evidence: "<<text<<"\n";
}
struct ExternalRam {
  ram_client_fixture::Channel channel{true};
  std::unique_ptr<source_fixture::Child> child;
  flitzfs::ProcessResult result;
  explicit ExternalRam(const fs::path& executable) {
    child=std::make_unique<source_fixture::Child>(executable,
        std::vector<std::wstring>{L"--ram-handoff-child",channel.token(),std::to_wstring(GetCurrentProcessId())},
        flitzfs::ProcessBudget{128,2000,30000});
    // The first actual request also proves host readiness; no synthetic data
    // operation starts until the exact child has created its fresh mount.
    const auto first=channel.next(child->result_event());
    require(first==ram_client_fixture::Step::initial&&child->process_id()&&
        child->process_id()!=GetCurrentProcessId(),"RAM source/client readiness or alias");
    Fixture fixture(child->process_id());RamClient client(fixture.root/L"mount");
    client.perform(first);channel.acknowledge(first);
    while(!channel.complete()) {
      const auto step=channel.next(child->result_event());
      client.perform(step);channel.acknowledge(step);
    }
    require(client.finished&&channel.copy_rounds()>0,"RAM client did not close before completion");
    result=child->result();
    require(result.process_id==child->process_id()&&result.exit_confirmed&&!result.timed_out&&
        !result.cancelled&&result.exit_code==0&&child->retirement_confirmed(),"RAM exact source/supervisor retirement");
    require(source_fixture::cleanup_ok.load(),"RAM client protocol cleanup");
    std::cout<<"ram_client source="<<child->process_id()<<" client="<<GetCurrentProcessId()
        <<" copy_reads="<<channel.copy_rounds()<<" exit_confirmed="<<result.exit_confirmed<<"\n";
  }
};
struct ExternalOrderedSource {
  source_fixture::Events protocol{true};
  std::unique_ptr<source_fixture::Child> child;
  flitzfs::ProcessResult written;
  ExternalOrderedSource(const fs::path& executable,std::string_view profile) {
    require(profile=="basic"||profile=="mutations"||profile=="delete"||profile=="restart","ordered external profile");
    const auto client=GetCurrentProcessId();
    const auto mode=profile=="basic"?L"--write-child":profile=="delete"?L"--delete-child":
        profile=="restart"?L"--restart-source-child":L"--mutations-child";
    child=std::make_unique<source_fixture::Child>(executable,
        std::vector<std::wstring>{mode,protocol.token(),std::to_wstring(client)},
        flitzfs::ProcessBudget{128,2000,30000});
    child->await(protocol,source_fixture::Phase::ready);
    require(child->process_id()&&child->process_id()!=client,"ordered source/client alias");
    Fixture fixture(child->process_id());const auto mount=fixture.root/L"mount";
    if(profile=="basic")basic_client(mount);
    else if(profile=="restart")restart_client(mount);
    else namespace_client(mount,profile=="delete");
    // All actual client handles are closed before authorizing ordered stop.
    protocol.signal(source_fixture::Phase::written);
    written=child->result();
    require(written.process_id==child->process_id()&&written.exit_confirmed&&
        !written.timed_out&&!written.cancelled&&written.exit_code==0&&child->retirement_confirmed(),
        "ordered external source exact retirement failed");
    require(source_fixture::cleanup_ok.load(),"ordered external protocol cleanup");
  }
};
struct ExternalResumer {
  source_fixture::Events protocol{true};
  std::unique_ptr<source_fixture::Child> child;
  flitzfs::ProcessResult resumed;
  ExternalResumer(const fs::path& executable,std::uint32_t source,bool killed) {
    child=std::make_unique<source_fixture::Child>(executable,
        std::vector<std::wstring>{killed?L"--crash-resume-child":L"--resume-child",
            std::to_wstring(source),protocol.token(),std::to_wstring(GetCurrentProcessId())},
        flitzfs::ProcessBudget{128,2000,30000});
    child->await(protocol,source_fixture::Phase::ready);
    require(child->process_id()&&child->process_id()!=source&&
        child->process_id()!=GetCurrentProcessId(),"resumer actor alias");
    Fixture fixture(source);continuation_client(fixture.root/L"remount");
    protocol.signal(source_fixture::Phase::written);
    resumed=child->result();
    require(resumed.process_id==child->process_id()&&resumed.exit_confirmed&&
        !resumed.timed_out&&!resumed.cancelled&&resumed.exit_code==0&&child->retirement_confirmed(),
        "external resumer exact retirement failed");
    require(source_fixture::cleanup_ok.load(),"external resumer protocol cleanup");
  }
};
struct ExternalSource {
  source_fixture::Events protocol{true};
  std::unique_ptr<source_fixture::Child> child;
  flitzfs::ProcessResult written;
  ExternalSource(const fs::path& executable,std::string_view profile) {
    stage=60;const auto client=GetCurrentProcessId();
    const auto mode=profile=="crash-restart"?L"--crash-restart-source-child":L"--crash-child";
    child=std::make_unique<source_fixture::Child>(executable,
        std::vector<std::wstring>{mode,protocol.token(),std::to_wstring(client)},
        flitzfs::ProcessBudget{128,2000,15000});
    child->await(protocol,source_fixture::Phase::ready);
    require(child->process_id()&&child->process_id()!=client,"source/client process alias");
    Fixture fixture(child->process_id());const auto mount=fixture.root/L"mount";mounted_identity(mount);
    const auto client_path=mount/L"sample.bin";
    source_fixture::Handle file(CreateFileW(client_path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    checked(file.value&&file.value!=INVALID_HANDLE_VALUE,"external client create");
    write_bytes(file.value,expected_data());read_handle(file.value,expected_data());
    protocol.signal(source_fixture::Phase::written);
    child->await(protocol,source_fixture::Phase::ack);
    written=child->result();
    require(written.process_id==child->process_id()&&written.exit_confirmed&&
        written.timed_out&&!written.cancelled&&written.exit_code==ERROR_TIMEOUT&&
        child->retirement_confirmed(),"external source exact retirement failed");
    require(file.close(),"external client handle retirement failed");
    require(source_fixture::cleanup_ok.load(),"source protocol cleanup failed");
  }
};
void run_extended(const fs::path& executable,std::string_view profile) {
  const bool crash=profile=="crash"||profile=="crash-restart";flitzfs::ProcessBudget budget;budget.timeout_ms=crash?15000:30000;
  const std::wstring child=profile=="crash-restart"?L"--crash-restart-source-child":profile=="restart"?L"--restart-source-child":
      crash?L"--crash-child":profile=="delete"?L"--delete-child":L"--mutations-child";
  std::unique_ptr<ExternalSource> external;
  std::unique_ptr<ExternalOrderedSource> ordered;
  flitzfs::ProcessResult written;
  if(crash){external=std::make_unique<ExternalSource>(executable,profile);written=external->written;}
  else if(profile=="mutations"||profile=="delete"||profile=="restart") {
    ordered=std::make_unique<ExternalOrderedSource>(executable,profile);written=ordered->written;
  } else written=flitzfs::run_bounded_process(executable,{child},budget);
  std::cout<<"extended_writer profile="<<profile<<" exit="<<written.exit_code<<" stage="<<(written.exit_code>>16U)
      <<" detail="<<(written.exit_code&0xffffU)<<" timed_out="<<written.timed_out<<" cancelled="<<written.cancelled
      <<" exit_confirmed="<<written.exit_confirmed<<" peak_commit_bytes="<<written.peak_committed_bytes<<"\n";
  require(written.process_id&&written.exit_confirmed&&!written.cancelled&&
      (crash?(written.timed_out&&written.exit_code==ERROR_TIMEOUT):(!written.timed_out&&written.exit_code==0)),
      "extended writer/termination proof failed");emit_extended(written.process_id,profile,crash?"ack":"state");
  budget.timeout_ms=30000;std::uint32_t resumer_process=0;
  std::unique_ptr<ExternalResumer> resumer;
  if(restart_profile(profile)) {
    resumer=std::make_unique<ExternalResumer>(executable,written.process_id,crash);
    const auto resumed=resumer->resumed;
    std::cout<<"extended_resumer profile="<<profile<<" exit="<<resumed.exit_code
        <<" stage="<<(resumed.exit_code>>16U)<<" detail="<<(resumed.exit_code&0xffffU)
        <<" timed_out="<<resumed.timed_out<<" cancelled="<<resumed.cancelled
        <<" exit_confirmed="<<resumed.exit_confirmed<<" peak_commit_bytes="<<resumed.peak_committed_bytes<<"\n";
    require(resumed.process_id&&resumed.exit_confirmed&&!resumed.timed_out&&!resumed.cancelled&&resumed.exit_code==0,
        "supervised persisted writable continuation failed");
    resumer_process=resumed.process_id;
  }
  const std::wstring verifier=profile=="crash-restart"?L"--crash-restart-verify-child":profile=="restart"?L"--restart-verify-child":crash?L"--crash-verify-child":profile=="delete"?L"--delete-verify-child":L"--mutations-verify-child";
  std::vector<std::wstring> verify_arguments{verifier,std::to_wstring(written.process_id)};
  if(restart_profile(profile))verify_arguments.push_back(std::to_wstring(resumer_process));
  if(crash||ordered)verify_arguments.push_back(std::to_wstring(GetCurrentProcessId()));
  const auto verified=flitzfs::run_bounded_process(executable,verify_arguments,budget);
  std::cout<<"extended_verifier profile="<<profile<<" exit="<<verified.exit_code<<" stage="<<(verified.exit_code>>16U)
      <<" detail="<<(verified.exit_code&0xffffU)<<" timed_out="<<verified.timed_out<<" cancelled="<<verified.cancelled
      <<" exit_confirmed="<<verified.exit_confirmed<<" peak_commit_bytes="<<verified.peak_committed_bytes<<"\n";
  require(verified.process_id&&verified.exit_confirmed&&!verified.timed_out&&!verified.cancelled&&verified.exit_code==0,
      "independent extended CPU verification failed");emit_extended(written.process_id,profile,"verify");
}

} // namespace
int wmain(int argc,wchar_t** argv) {try {
  require_hosted_opt_in();require(argc>=2&&argc<=5,"explicit writable probe mode required");
  const std::wstring_view mode(argv[1]);
  if(mode==L"--ram-handoff-child"){require(argc==4,"missing RAM protocol/client identity");ram_handoff_probe(argv[2],process_number(argv[3]));return 0;}
  if(mode==L"--write-child"){require(argc==4,"missing external writer identity");write_probe(argv[2],process_number(argv[3]));return 0;}
  if(mode==L"--verify-child"){require(argc==4,"missing source/client identities");source_client_process=process_number(argv[3]);verify_probe(process_number(argv[2]));return 0;}
  if(mode==L"--restart-source-child") {
    require(argc==4,"missing restart source client identity");restart_source(argv[2],process_number(argv[3]));return 0;
  }
  if(mode==L"--crash-restart-source-child"||mode==L"--crash-child") {
    require(argc==4,"missing external source handshake/identity");
    crash_source(argv[2],process_number(argv[3]),mode==L"--crash-child"?"crash":"crash-restart");return 0;
  }
  if(mode==L"--resume-child"||mode==L"--crash-resume-child") {
    const bool killed=mode==L"--crash-resume-child";
    require(argc==5,"missing restart source/token/client identities");
    restart_resume(process_number(argv[2]),killed,argv[3],process_number(argv[4]));return 0;
  }
  if(mode==L"--restart-verify-child"||mode==L"--crash-restart-verify-child") {
    const bool killed=mode==L"--crash-restart-verify-child";
    require(argc==5,"missing restart verifier/client identities");
    source_client_process=process_number(argv[4]);
    extended_verify(process_number(argv[2]),killed?"crash-restart":"restart",process_number(argv[3]));return 0;
  }
  if(mode==L"--mutations-child"||mode==L"--delete-child") {
    require(argc==4,"missing external namespace identity");namespace_probe(mode==L"--delete-child",argv[2],process_number(argv[3]));return 0;
  }
  if(mode==L"--mutations-verify-child"||mode==L"--delete-verify-child"||mode==L"--crash-verify-child") {
    require(argc==4,"missing extended source/client identity");
    source_client_process=process_number(argv[3]);
    extended_verify(process_number(argv[2]),mode==L"--crash-verify-child"?"crash":
        mode==L"--delete-verify-child"?"delete":"mutations");return 0;
  }
  require(argc==2&&(mode==L"--run-disposable-ci"||mode==L"--run-mutations-ci"||
      mode==L"--run-delete-ci"||mode==L"--run-crash-ci"||mode==L"--run-restart-ci"||mode==L"--run-crash-restart-ci"||mode==L"--run-ram-handoff-ci"),"unknown writable probe mode");
  const source_fixture::ControllerBudget controller_budget;
  wchar_t path[512]{};const auto n=GetModuleFileNameW(nullptr,path,512);require(n&&n<512,"probe executable identity");
  const fs::path executable(std::wstring(path,n));
  if(mode==L"--run-ram-handoff-ci") {
    const ExternalRam ram(executable);const auto result=ram.result;
    std::cout<<"ram_handoff exit="<<result.exit_code<<" exit_confirmed="<<result.exit_confirmed
        <<" peak_commit_bytes="<<result.peak_committed_bytes<<"\n";
    require(result.process_id&&result.exit_confirmed&&!result.timed_out&&!result.cancelled&&
        result.exit_code==0,"supervised RAM handoff failed");
    emit(result.process_id,"ram-handoff");
    return 0;
  }
  if(mode!=L"--run-disposable-ci") {
    run_extended(executable,mode==L"--run-restart-ci"?"restart":mode==L"--run-crash-restart-ci"?"crash-restart":
        mode==L"--run-crash-ci"?"crash":mode==L"--run-delete-ci"?"delete":"mutations");
    require(source_fixture::cleanup_ok.load()&&!source_fixture::source_busy.load(),"source final cleanup");return 0;
  }
  flitzfs::ProcessBudget budget;budget.timeout_ms=30000;
  const ExternalOrderedSource ordered(executable,"basic");
  const auto written=ordered.written;
  std::cout<<"writable_probe exit="<<written.exit_code<<" stage="<<(written.exit_code>>16U)
      <<" detail="<<(written.exit_code&0xffffU)<<" timed_out="<<written.timed_out<<" cancelled="<<written.cancelled
      <<" exit_confirmed="<<written.exit_confirmed<<" peak_commit_bytes="<<written.peak_committed_bytes<<"\n";
  require(written.process_id&&written.exit_confirmed&&!written.timed_out&&!written.cancelled&&written.exit_code==0,
      "supervised writable helper failed");emit(written.process_id,"write");
  const auto verified=flitzfs::run_bounded_process(executable,{L"--verify-child",std::to_wstring(written.process_id),std::to_wstring(GetCurrentProcessId())},budget);
  std::cout<<"writable_verify exit="<<verified.exit_code<<" stage="<<(verified.exit_code>>16U)
      <<" detail="<<(verified.exit_code&0xffffU)<<" timed_out="<<verified.timed_out<<" cancelled="<<verified.cancelled
      <<" exit_confirmed="<<verified.exit_confirmed<<" peak_commit_bytes="<<verified.peak_committed_bytes<<"\n";
  require(verified.process_id&&verified.exit_confirmed&&!verified.timed_out&&!verified.cancelled&&verified.exit_code==0,
      "independent bounded writable-image verification failed");emit(written.process_id,"verify");
  return 0;
}catch(const std::system_error& e){
  std::cerr<<"writable stage="<<stage<<" "<<e.what()<<"\n";
  return static_cast<int>((stage<<16U)|(static_cast<unsigned>(e.code().value())&0xffffU));
}catch(const std::exception& e){
  std::cerr<<"writable stage="<<stage<<" "<<e.what()<<"\n";return static_cast<int>((stage<<16U)|1U);
}}
