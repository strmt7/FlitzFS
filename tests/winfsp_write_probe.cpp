// Experimental disposable-VM writable qualification. Never a product mount CLI.
#include "winfsp_session.hpp"
#include "winfsp_native.hpp"
#include "bounded_process.hpp"
#include "maintenance_host.hpp"
#include "supervised_source_fixture.hpp"
#include "ram_client_channel.hpp"
#include "locked_ram_fixture.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/record_digest.hpp"
#include "../src/store_file.hpp"
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
#include <thread>
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
  require(environment(L"FLITZFS_ALLOW_DISPOSABLE_MOUNT") == L"1" &&
      environment(L"FLITZFS_ALLOW_DISPOSABLE_WRITE") == L"1",
      "probe requires explicit qualified disposable mount/write fixture opt-in");
}
// Pin each existing ancestor without delete sharing and reject reparse points.
// Only a trusted qualified fixture directory is accepted, not arbitrary paths.
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
  auto api=flitzfs::linked_winfsp_session_api();api.create=create_source_filesystem;return api;
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
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"writable mount startup");
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
      // Real SDK TLS request hints and real FSD responses, still CPU-only.
      // Default factories and the other mount profiles remain unchanged.
      adapter->enable_deferred_writes_hosted(flitzfs::NativeDeferredWriteOptions{});
      const auto* observer=adapter.get();
      serial=observer->parameters().VolumeSerialNumber;
      flitzfs::WinFspNativeSession session(std::move(adapter),source_api());
      require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"locked-RAM mount startup");
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
      stage=75;
      finish_session(session,mount);
      stage=76;
      const auto deferred=session.deferred_write_stats();
      require(deferred.queued>0&&deferred.completed==deferred.queued&&
          deferred.response_attempts==deferred.queued&&deferred.thread_exited&&deferred.retirement_confirmed&&
          !deferred.native_pending&&!deferred.worker_busy&&!deferred.disabled&&
          !deferred.worker_submitted&&!deferred.advised,"RAM deferred CPU write lifecycle");
      stage=77;
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
    stage=78;
    working.restore();
  }
  stage=79;
  require(locked_ram_fixture::locked_bytes.load()==0,"RAM mounted storage retained locked bytes");
  stage=80;
  require(locked_ram_fixture::cleanup_ok.load(),"RAM mounted storage release failed");
  stage=81;
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
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"extended namespace mount");
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
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"restart source mount");
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
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"persisted writable remount");
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
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"external source mount");
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
    if(result.process_id!=child->process_id()||!result.exit_confirmed||result.timed_out||
        result.cancelled||result.exit_code||!child->retirement_confirmed()) {
      throw std::runtime_error("RAM exact source/supervisor exit="+std::to_string(result.exit_code)+
          " stage="+std::to_string(result.exit_code>>16U)+" detail="+std::to_string(result.exit_code&0xffffU)+
          " confirmed="+std::to_string(result.exit_confirmed)+" timed_out="+std::to_string(result.timed_out)+
          " cancelled="+std::to_string(result.cancelled)+" retired="+std::to_string(child->retirement_confirmed())+
          " peak_commit="+std::to_string(result.peak_committed_bytes));
    }
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

// These modes are admitted only inside the disposable Windows Sandbox fixture.
// The normal probe and the application never arm this byte-I/O wrapper.
void require_cut_guest() {
  DWORD type=0,size=sizeof(type);
  require(RegGetValueW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Control",L"ContainerType",
      RRF_RT_REG_DWORD,nullptr,&type,&size)==ERROR_SUCCESS && type==4,"Publication cuts require Windows Sandbox");
  const auto nonce=environment(L"FLITZFS_DISPOSABLE_GUEST_MOUNT");
  require(source_fixture::valid_token(nonce),"Publication cut guest nonce");
  const auto parent=L"C:\\FlitzFS-guest-"+nonce+L"\\mount-parent";
  require(environment(L"RUNNER_TEMP")==parent,"Publication cut runner directory is outside the guest fixture");
  wchar_t temp[MAX_PATH]{};
  const auto length=GetTempPath2W(static_cast<DWORD>(std::size(temp)),temp);
  require(length<std::size(temp) && std::wstring_view(temp,length)==parent+L"\\","Publication cut temporary directory differs");
}

enum class PublicationCut {before_commit,after_first_copy,after_second_copy};
enum class PublicationOperation {write,shrink,rename,image_switch};
constexpr std::array cut_points{PublicationCut::before_commit,PublicationCut::after_first_copy,
    PublicationCut::after_second_copy};
constexpr std::array<std::wstring_view,3> cut_names{L"before-payload-flush",L"after-first-root",L"after-second-root"};
constexpr std::array<std::wstring_view,4> operation_names{L"write",L"eof-shrink",L"rename",L"image-switch"};
constexpr std::array<std::wstring_view,3> selector_cut_names{
    L"before-first-selector-write",L"after-first-selector-flush",L"after-second-selector-flush"};
constexpr auto cut_bytes=8192U;
constexpr auto cut_shrunk_bytes=3751U;
constexpr unsigned cut_neighbors=32;
constexpr flitzfs::PagedStoreLimits cut_write_limits{8ULL*1024*1024};
// Create, write and cleanup each leave append history in the isolated fixture.
constexpr flitzfs::PagedStoreLimits cut_metadata_limits{
    cut_write_limits.maximum_physical_bytes+cut_neighbors*16*flitzfs::kStoreCommitUnit};
constexpr flitzfs::StoreIoContract cut_contract{static_cast<std::uint32_t>(flitzfs::kStoreCommitUnit),true};
constexpr flitzfs::StoreIoContract selector_contract{cut_contract.write_failure_unit_bytes,true,true};
constexpr flitzfs::PagedStoreLimits selector_limits{16ULL*1024*1024};
constexpr auto selector_alternate_offset=flitzfs::kNativeVolumeFileImageOffset+selector_limits.maximum_physical_bytes;
constexpr flitzfs::StoreCompressionOptions cut_compression{flitzfs::StoreCompressionMode::raw,1,false};
std::vector<std::byte> cut_data(unsigned salt) {
  std::vector<std::byte> bytes(cut_bytes);
  for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i/31+i%251+salt)%256);
  return bytes;
}
flitzfs::StoreIdentity cut_volume_identity(DWORD owner) {
  auto volume=identity();
  for(unsigned i=0;i<4;++i)volume[4+i]=static_cast<std::byte>((owner>>(8*i))&0xffU);
  return volume;
}

[[noreturn]] void park_publication(const source_fixture::Events& protocol) {
  protocol.signal(source_fixture::Phase::ack);
  Sleep(15000);
  throw std::runtime_error("Supervisor did not retire the paused publication");
}
class PublicationIo final:public flitzfs::StoreByteIo {
 public:
  PublicationIo(std::shared_ptr<flitzfs::StoreByteIo> backing,PublicationCut cut,PublicationOperation operation,
      const source_fixture::Events& protocol):
      backing_(std::move(backing)),cut_(cut),operation_(operation),protocol_(protocol),replacement_(cut_data(119)) {}
  std::uint64_t extent() const override{return backing_->extent();}
  flitzfs::StoreIoContract contract() const noexcept override{return backing_->contract();}
  void admit_write(std::uint64_t end) override{backing_->admit_write(end);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{backing_->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override {
    if(active==this) {
      constexpr auto image=flitzfs::kNativeVolumeFileImageOffset;
      constexpr auto unit=flitzfs::kStoreCommitUnit;
      require(at>=image,"Armed write reached the volume selector or advisory area");
      const auto relative=at-image;
      if(relative<2*unit) {
        // The paged root is 4 KiB at the start of its isolated commit unit.
        require(barriers_==roots_+1 && (relative==0 || relative==unit) && bytes.size()==4096,
            "Armed write did not publish the expected image root after its preceding barrier");
        const auto slot=1U<<static_cast<unsigned>(relative/unit);
        require(!(root_slots_&slot),"Armed write reused an image root slot");
        root_slots_|=slot;++roots_;
      } else {
        require(!barriers_ && !roots_,"Armed write appended data after root publication began");
        ++payload_writes_;
      }
    }
    backing_->write_exact(at,bytes);
  }
  void flush() override {
    if(active!=this){backing_->flush();return;}
    require(payload_writes_ && roots_==barriers_ && roots_<=2,"Unexpected armed image flush boundary");
    if(cut_==PublicationCut::before_commit && roots_==0)park_publication(protocol_);
    backing_->flush();
    ++barriers_;
    if((cut_==PublicationCut::after_first_copy && roots_==1) ||
        (cut_==PublicationCut::after_second_copy && roots_==2))park_publication(protocol_);
  }
  bool armed_for(PublicationOperation operation) const noexcept {
    return operation==operation_ && WaitForSingleObject(protocol_.get(source_fixture::Phase::written),0)==WAIT_OBJECT_0;
  }
  bool matches_write(PVOID data,UINT64 offset,ULONG length) const noexcept {
    return armed_for(PublicationOperation::write) && offset==0 && length==replacement_.size() && data &&
        std::equal(replacement_.begin(),replacement_.end(),static_cast<const std::byte*>(data));
  }
  bool claim() noexcept{return !claimed_.exchange(true,std::memory_order_acq_rel);}
  static thread_local PublicationIo* active;
 private:
  std::shared_ptr<flitzfs::StoreByteIo> backing_;
  const PublicationCut cut_;
  const PublicationOperation operation_;
  const source_fixture::Events& protocol_;
  const std::vector<std::byte> replacement_;
  std::atomic<bool> claimed_{false};
  unsigned payload_writes_=0,roots_=0,root_slots_=0,barriers_=0;
};
thread_local PublicationIo* PublicationIo::active=nullptr;
PublicationIo* cut_backing=nullptr;
const FSP_FILE_SYSTEM_INTERFACE* cut_original=nullptr;
FSP_FILE_SYSTEM_INTERFACE cut_callbacks{};
template<class Invoke>
NTSTATUS publication_callback(bool matches,Invoke&& invoke) noexcept {
  if(!matches)return invoke();
  if(!cut_backing->claim())return STATUS_INVALID_DEVICE_STATE;
  PublicationIo::active=cut_backing;
  const auto status=invoke();
  PublicationIo::active=nullptr;
  return status;
}
NTSTATUS cut_write(FSP_FILE_SYSTEM* fs,PVOID context,PVOID buffer,UINT64 offset,ULONG length,
    BOOLEAN append,BOOLEAN constrained,PULONG written,FSP_FSCTL_FILE_INFO* info) noexcept {
  return publication_callback(cut_backing->matches_write(buffer,offset,length),[&] {
    return cut_original->Write(fs,context,buffer,offset,length,append,constrained,written,info);
  });
}
NTSTATUS cut_shrink(FSP_FILE_SYSTEM* fs,PVOID context,UINT64 size,BOOLEAN allocation,
    FSP_FSCTL_FILE_INFO* info) noexcept {
  const bool matches=cut_backing->armed_for(PublicationOperation::shrink) && !allocation && size==cut_shrunk_bytes;
  return publication_callback(matches,[&] {return cut_original->SetFileSize(fs,context,size,allocation,info);});
}
NTSTATUS cut_rename(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR path,PWSTR destination,BOOLEAN replace) noexcept {
  // WinFsp can supply a normalized source name and a caller-spelled destination.
  const bool matches=cut_backing->armed_for(PublicationOperation::rename) && path && destination &&
      CompareStringOrdinal(path,-1,L"\\payload.bin",-1,TRUE)==CSTR_EQUAL &&
      CompareStringOrdinal(destination,-1,L"\\renamed.bin",-1,TRUE)==CSTR_EQUAL;
  return publication_callback(matches,[&] {return cut_original->Rename(fs,context,path,destination,replace);});
}
NTSTATUS create_cut_filesystem(PWSTR device,const FSP_FSCTL_VOLUME_PARAMS* params,
    const FSP_FILE_SYSTEM_INTERFACE* callbacks,FSP_FILE_SYSTEM** output) noexcept {
  if(!cut_backing || cut_original || !callbacks || !callbacks->Write || !callbacks->SetFileSize || !callbacks->Rename)
    return STATUS_INVALID_PARAMETER;
  cut_original=callbacks;
  cut_callbacks=*callbacks;
  cut_callbacks.Write=cut_write;
  cut_callbacks.SetFileSize=cut_shrink;
  cut_callbacks.Rename=cut_rename;
  return FspFileSystemCreate(device,params,&cut_callbacks,output);
}


class SelectorPublicationObserver final:public flitzfs::detail::VolumeFileObserver {
 public:
  SelectorPublicationObserver(PublicationCut cut,const source_fixture::Events& protocol):
      cut_(cut),protocol_(protocol) {}
  static thread_local const SelectorPublicationObserver* active;
  void before_write(std::uint64_t at,std::size_t bytes) override {
    if(active!=this)return;
    constexpr auto unit=flitzfs::kStoreCommitUnit;
    constexpr auto image=flitzfs::kNativeVolumeFileImageOffset;
    if(at<flitzfs::kVolumeHeadBytes) {
      require(alternate_flushed_ && roots_==barriers_ && roots_<2 && (at==0 || at==unit) && bytes==unit,
          "Unqualified selector write or unflushed replacement");
      const auto mask=1U<<static_cast<unsigned>(at/unit);
      require(!(root_slots_&mask),"Selector wrote the same slot twice");
      if(roots_==0 && cut_==PublicationCut::before_commit)park_publication(protocol_);
      root_slots_|=mask;++roots_;
    } else if(at>=selector_alternate_offset && at-selector_alternate_offset<selector_limits.maximum_physical_bytes) {
      alternate_written_=true;
      alternate_flushed_=false;
    } else if(at>=image && at<selector_alternate_offset) {
      throw std::runtime_error("Mounted reclamation attempted to write the retained source");
    }
  }
  void after_flush() override {
    if(active!=this)return;
    if(alternate_written_)alternate_flushed_=true;
    if(roots_==barriers_)return;
    require(roots_==barriers_+1,"Selector slots advanced without ordered flushes");
    ++barriers_;
    if((cut_==PublicationCut::after_first_copy && roots_==1) ||
        (cut_==PublicationCut::after_second_copy && roots_==2))park_publication(protocol_);
  }
  void before_reclaim(std::uint64_t at,std::uint64_t bytes) override {
    if(active!=this)return;
    constexpr auto image=flitzfs::kNativeVolumeFileImageOffset;
    require(at>=selector_alternate_offset || (at<=image && bytes<=image-at),
        "Mounted reclamation attempted to retire the selected source before publication");
  }
 private:
  const PublicationCut cut_;
  const source_fixture::Events& protocol_;
  unsigned roots_=0,root_slots_=0,barriers_=0;
  bool alternate_written_=false,alternate_flushed_=false;
};
thread_local const SelectorPublicationObserver* SelectorPublicationObserver::active=nullptr;

void advance_selector_publication(flitzfs::WinFspNativeSession& session,SelectorPublicationObserver& observer) {
  flitzfs::WindowsMaintenanceHost host;
  const auto deadline=GetTickCount64()+15000;
  auto continuation=flitzfs::WinFspMaintenanceContinuation::none;
  struct ActivePublication {
    explicit ActivePublication(const SelectorPublicationObserver& observer){SelectorPublicationObserver::active=&observer;}
    ~ActivePublication(){SelectorPublicationObserver::active=nullptr;}
  } active(observer);
  while(GetTickCount64()<deadline) {
    const auto observed=host.observe(false);
    if(observed.valid) {
      flitzfs::MaintenanceThreadScope priority;
      require(priority.active(),"Selector fixture could not lower maintenance priority");
      auto result=session.advance_background_maintenance(observed,GetTickCount64(),{},continuation,false);
      require(!result.compacted && !result.compaction_failed &&
          result.state!=flitzfs::WinFspMaintenanceState::unavailable,"Mounted reclamation bypassed selector cut or failed");
      continuation=result.continuation;
      while(continuation==flitzfs::WinFspMaintenanceContinuation::compaction) {
        const auto fresh=host.continue_preparation(250);
        if(!fresh)break;
        result=session.advance_background_maintenance(*fresh,GetTickCount64(),{},continuation,false);
        require(!result.compacted && !result.compaction_failed &&
            result.state!=flitzfs::WinFspMaintenanceState::unavailable,"Mounted selector preparation failed");
        continuation=result.continuation;
      }
      require(priority.finish(),"Selector fixture could not restore maintenance priority");
    }
    require(host.status()!=flitzfs::MaintenanceHostStatus::unavailable,"Selector fixture could not read guest resources");
    Sleep(40);
  }
  throw std::runtime_error("Mounted reclamation did not reach the selector boundary");
}
void publication_source(std::wstring_view token,PublicationCut cut,PublicationOperation operation) {
  require_probe_job();stage=70;
  source_fixture::Events protocol(false,std::wstring(token));
  Fixture fixture;
  try {
    const auto mount=fixture.root/L"mount",path=fixture.root/L"inflight.flfs";
    const bool selector=operation==PublicationOperation::image_switch;
    const auto limits=selector?selector_limits:(operation==PublicationOperation::write?cut_write_limits:cut_metadata_limits);
    auto api=flitzfs::linked_winfsp_session_api();
    std::shared_ptr<SelectorPublicationObserver> selector_observer;
    std::shared_ptr<flitzfs::StoreByteIo> backing;
    if(selector) {
      selector_observer=std::make_shared<SelectorPublicationObserver>(cut,protocol);
      backing=flitzfs::detail::new_observed_volume_file(path,selector_contract,
          flitzfs::kNativeVolumeFileImageOffset+3*limits.maximum_physical_bytes,selector_observer);
    } else {
      auto provider=flitzfs::detail::new_volume_file(path,cut_contract,
          flitzfs::kNativeVolumeFileImageOffset+limits.maximum_physical_bytes);
      auto tap=std::make_shared<PublicationIo>(std::move(provider),cut,operation,protocol);
      cut_backing=tap.get();
      backing=std::move(tap);
      api.create=create_cut_filesystem;
    }
    const auto volume=cut_volume_identity(GetCurrentProcessId());
    auto image=volume;image.back()^=std::byte{1};
    flitzfs::WinFspNativeSession session(flitzfs::WinFspNativeWritable::create_file_volume(
        backing,volume,image,limits,cut_compression),api);
    require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"Publication-cut source mount");
    protocol.signal(source_fixture::Phase::ready);
    if(operation==PublicationOperation::write)protocol.wait_written();
    else require(WaitForSingleObject(protocol.get(source_fixture::Phase::written),15000)==WAIT_OBJECT_0,
        "Mounted population exceeded its preparation deadline");
    if(selector_observer)advance_selector_publication(session,*selector_observer);
    Sleep(20000);
    throw std::runtime_error("Publication-cut source was not retired");
  }catch(const std::exception& failure) {
    try {
      const auto message=std::string_view(failure.what()).substr(0,1024);
      const auto* data=reinterpret_cast<const std::byte*>(message.data());
      write_new(fixture.root/L"publication-source-error.txt",std::vector<std::byte>(data,data+message.size()));
    }catch(...) {}
    throw;
  }
}
void publication_resumer(std::wstring_view token,DWORD source,flitzfs::StoreIoContract contract) {
  require_probe_job();stage=71;
  source_fixture::Events protocol(false,std::wstring(token));
  Fixture fixture(source);
  require_removed(fixture.root/L"mount");
  const auto mount=fixture.root/L"resume-mount";
  auto adapter=flitzfs::WinFspNativeWritable::resume_file_volume(fixture.root/L"inflight.flfs",
      contract,cut_volume_identity(source),cut_compression);
  flitzfs::WinFspNativeSession session(std::move(adapter),flitzfs::linked_winfsp_session_api());
  require(session.start({mount.native(),true,true})==STATUS_SUCCESS,"Publication-cut continuation mount");
  protocol.signal(source_fixture::Phase::ready);
  protocol.wait_written();
  finish_session(session,mount);
}
std::uint64_t mounted_cut_inode(HANDLE file) {
  BY_HANDLE_FILE_INFORMATION info{};
  checked(GetFileInformationByHandle(file,&info),"Inspect pending publication inode");
  return (static_cast<std::uint64_t>(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
}
class PendingMountedWrite final {
 public:
  explicit PendingMountedWrite(const fs::path& path):
      done_(CreateEventW(nullptr,TRUE,FALSE,nullptr)),
      file_(CreateFileW(path.c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED|FILE_FLAG_NO_BUFFERING|FILE_FLAG_WRITE_THROUGH,nullptr)) {
    checked(file_.value!=INVALID_HANDLE_VALUE,"Open existing file for pending publication");
    checked(done_.value!=nullptr,"Pending publication event");
    const auto replacement=cut_data(119);
    std::copy(replacement.begin(),replacement.end(),bytes_.begin());
    operation_.hEvent=done_.value;
  }
  ~PendingMountedWrite() {
    if(!pending_)return;
    (void)CancelIoEx(file_.value,&operation_);
    if(WaitForSingleObject(done_.value,5000)!=WAIT_OBJECT_0)std::terminate();
    DWORD transferred=0;
    if(!GetOverlappedResult(file_.value,&operation_,&transferred,FALSE) && GetLastError()==ERROR_IO_INCOMPLETE)
      std::terminate(); // The kernel still owns the request and buffer. Keep both until process exit.
  }
  std::uint64_t inode() const {return mounted_cut_inode(file_.value);}
  void submit() {
    const auto accepted=WriteFile(file_.value,bytes_.data(),static_cast<DWORD>(bytes_.size()),nullptr,&operation_);
    pending_=!accepted && GetLastError()==ERROR_IO_PENDING;
    require(pending_,"Publication write did not enter the pending state");
  }
  void verify_pending() {
    DWORD transferred=0;
    require(!GetOverlappedResult(file_.value,&operation_,&transferred,FALSE) &&
        GetLastError()==ERROR_IO_INCOMPLETE,"Windows replied before the publication cut");
  }
  DWORD retire_after_owner() {
    require(WaitForSingleObject(done_.value,5000)==WAIT_OBJECT_0,"Pending write did not complete after owner retirement");
    DWORD transferred=0;
    const auto accepted=GetOverlappedResult(file_.value,&operation_,&transferred,FALSE);
    const auto error=accepted?ERROR_SUCCESS:GetLastError();
    pending_=error==ERROR_IO_INCOMPLETE;
    require(!pending_ && error,"Interrupted write was acknowledged or remained pending");
    checked(file_.close(),"Close interrupted publication handle");
    return error;
  }
 private:
  source_fixture::Handle done_,file_;
  OVERLAPPED operation_{};
  alignas(4096) std::array<std::byte,cut_bytes> bytes_{};
  bool pending_=false;
};
// The ordinary metadata APIs are synchronous. Keep their thread and exact
// handle alive until completion or until the fixture process is retired.
class PendingMountedMetadata final {
 public:
  PendingMountedMetadata(const fs::path& path,PublicationOperation operation):
      path_(path),destination_(path.parent_path()/L"renamed.bin"),operation_(operation),
      file_(CreateFileW(path.c_str(),GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,nullptr)) {
    checked(file_.value!=INVALID_HANDLE_VALUE,"Open existing file for pending metadata publication");
  }
  ~PendingMountedMetadata() {
    if(!thread_.joinable())return;
    (void)CancelSynchronousIo(thread_.native_handle());
    if(WaitForSingleObject(thread_.native_handle(),5000)!=WAIT_OBJECT_0)std::terminate();
    thread_.join();
  }
  std::uint64_t inode() const {return mounted_cut_inode(file_.value);}
  void submit() {
    thread_=std::thread([this] {
      FILE_END_OF_FILE_INFO eof{};
      eof.EndOfFile.QuadPart=cut_shrunk_bytes;
      const BOOL success=operation_==PublicationOperation::shrink?
          SetFileInformationByHandle(file_.value,FileEndOfFileInfo,&eof,sizeof(eof)):
          MoveFileExW(path_.c_str(),destination_.c_str(),0);
      result_=success?ERROR_SUCCESS:GetLastError();
      responded_.store(true,std::memory_order_release);
    });
  }
  void await_cut(source_fixture::Child& source,source_fixture::Events& protocol) {
    const HANDLE waits[]{protocol.get(source_fixture::Phase::ack),thread_.native_handle(),source.result_event()};
    const auto wait=WaitForMultipleObjects(static_cast<DWORD>(std::size(waits)),waits,FALSE,5000);
    if(wait==WAIT_OBJECT_0) {
      require(!responded_.load(std::memory_order_acquire),"Windows replied before the metadata publication cut");
      return;
    }
    if(wait==WAIT_OBJECT_0+1) {
      thread_.join();
      throw std::runtime_error("Windows completed the metadata operation before a publication cut, status="+
          std::to_string(result_));
    }
    if(wait==WAIT_TIMEOUT)source.request_cancel();
    if(wait==WAIT_TIMEOUT || wait==WAIT_OBJECT_0+2) {
      const auto ended=source.result();
      throw std::runtime_error("Metadata source did not reach the publication cut, wait="+std::to_string(wait)+
          " exit="+std::to_string(ended.exit_code)+" confirmed="+std::to_string(ended.exit_confirmed));
    }
    checked(FALSE,"Metadata publication wait");
  }
  DWORD retire_after_owner() {
    require(WaitForSingleObject(thread_.native_handle(),5000)==WAIT_OBJECT_0,
        "Metadata operation did not complete after owner retirement");
    thread_.join();
    require(responded_.load(std::memory_order_acquire) && result_!=ERROR_SUCCESS,
        "Interrupted metadata operation was acknowledged");
    checked(file_.close(),"Close interrupted metadata handle");
    return result_;
  }
 private:
  const fs::path path_,destination_;
  const PublicationOperation operation_;
  source_fixture::Handle file_;
  std::thread thread_;
  std::atomic<bool> responded_{false};
  DWORD result_=ERROR_SUCCESS;
};
void seed_cut_file(const fs::path& path,const std::vector<std::byte>& bytes) {
  Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
  if(file.value==INVALID_HANDLE_VALUE) {
    const auto error=GetLastError();
    std::wcerr<<L"publication_fixture_create_failed name="<<path.filename().native()<<L" error="<<error<<L'\n';
    throw std::system_error(static_cast<int>(error),std::system_category(),"Create new publication fixture file");
  }
  write_bytes(file.value,bytes);
}
std::string cut_neighbor_name(unsigned index) {return "neighbor-"+std::to_string(index)+".bin";}
std::vector<std::byte> cut_neighbor_data(unsigned index) {
  auto bytes=cut_data(151+index);
  bytes.resize(257+index*11);
  return bytes;
}

void run_publication_continuation(const fs::path& executable,const Fixture& fixture,DWORD source_id,
    const std::string& target,const std::vector<std::byte>& expected,bool paired) {
  constexpr flitzfs::ProcessBudget budget{128,2000,30000};
  source_fixture::Events continuation(true);
  source_fixture::Child resumer(executable,
      {paired?L"--publication-selector-resumer":L"--publication-cut-resumer",
       continuation.token(),std::to_wstring(source_id)},budget);
  resumer.await(continuation,source_fixture::Phase::ready);
  const auto mount=fixture.root/L"resume-mount";
  mounted_identity(mount);
  require(read_all(mount/target,expected.size())==expected,"Fresh mount read different recovered bytes");
  seed_cut_file(mount/L"continuation.bin",cut_data(61));
  continuation.signal(source_fixture::Phase::written);
  const auto ended=resumer.result();
  require(ended.process_id && ended.process_id!=source_id && ended.exit_code==0 && ended.exit_confirmed &&
      !ended.cancelled && !ended.timed_out && resumer.retirement_confirmed(),"Publication continuation retirement unconfirmed");
  require_removed(mount);
}
void run_publication_cut(const fs::path& executable,PublicationCut point,PublicationOperation operation) {
  stage=72;
  const auto name=cut_names[static_cast<std::size_t>(point)];
  const auto operation_name=operation_names[static_cast<std::size_t>(operation)];
  const bool metadata=operation!=PublicationOperation::write;
  source_fixture::Events protocol(true);
  constexpr flitzfs::ProcessBudget budget{128,2000,30000};
  // On exception the source supervisor retires before either client's buffer or handle can be released.
  std::optional<PendingMountedWrite> pending_write;
  std::optional<PendingMountedMetadata> pending_metadata;
  source_fixture::Child source(executable,{L"--publication-cut-source",protocol.token(),std::wstring(name),
      std::wstring(operation_name)},budget);
  source.await(protocol,source_fixture::Phase::ready);
  const auto source_id=source.process_id();
  require(source_id && source_id!=GetCurrentProcessId(),"Publication owner and client alias");
  Fixture fixture(source_id);
  const auto mount=fixture.root/L"mount",path=fixture.root/L"inflight.flfs";
  mounted_identity(mount);
  seed_cut_file(mount/L"payload.bin",cut_data(7));
  seed_cut_file(mount/L"witness.bin",cut_data(29));
  if(metadata) {
    const auto folder=mount/L"population";
    checked(CreateDirectoryW(folder.c_str(),nullptr),"Create mounted population directory");
    for(unsigned i=0;i<cut_neighbors;++i)seed_cut_file(folder/cut_neighbor_name(i),cut_neighbor_data(i));
    pending_metadata.emplace(mount/L"payload.bin",operation);
  } else pending_write.emplace(mount/L"payload.bin");
  const auto inode=metadata?pending_metadata->inode():pending_write->inode();
  require(inode!=0,"Publication trial lacked a stable file index");
  protocol.signal(source_fixture::Phase::written);
  if(metadata)pending_metadata->submit();
  else pending_write->submit();
  if(metadata)pending_metadata->await_cut(source,protocol);
  else {
    source.await(protocol,source_fixture::Phase::ack);
    pending_write->verify_pending();
  }
  require(source.request_cancel(),"Publication source cancellation was not admitted");
  const auto retired=source.result();
  require(retired.process_id==source_id && retired.cancelled && !retired.timed_out && retired.exit_confirmed &&
      retired.exit_code==ERROR_CANCELLED && source.retirement_confirmed(),"Publication source retirement unconfirmed");
  const auto error=metadata?pending_metadata->retire_after_owner():pending_write->retire_after_owner();
  pending_metadata.reset();
  pending_write.reset();
  require_removed(mount);
  const bool expects_new=point!=PublicationCut::before_commit;
  const bool renamed=expects_new && operation==PublicationOperation::rename;
  const std::string name_on_disk=renamed?"renamed.bin":"payload.bin";
  auto expected=cut_data(expects_new && operation==PublicationOperation::write?119:7);
  if(expects_new && operation==PublicationOperation::shrink)expected.resize(cut_shrunk_bytes);
  const auto verify=[&](bool continued) {
    const auto view=flitzfs::NativeVolumeFile::inspect(path,cut_volume_identity(source_id));
    const auto& snapshot=view.filesystem();snapshot.verify_all();
    const auto target=snapshot.stat(name_on_disk);
    const auto entries=snapshot.list();
    require(target.inode==inode && target.logical_bytes==expected.size() && !target.directory &&
        entries.size()==2U+static_cast<unsigned>(metadata)+static_cast<unsigned>(continued) &&
        snapshot.read(name_on_disk,0,cut_bytes+1)==expected &&
        snapshot.read("witness.bin",0,cut_bytes+1)==cut_data(29),
        "Independent CPU recovery did not select the complete expected checkpoint");
    const std::string_view absent=renamed?"payload.bin":"renamed.bin";
    require(std::ranges::any_of(entries,[&](const auto& entry){return entry.path==name_on_disk;}) &&
        std::ranges::none_of(entries,[&](const auto& entry){return entry.path==absent;}),
        "Recovered directory does not contain exactly the expected target spelling");
    if(metadata) {
      require(snapshot.list("population").size()==cut_neighbors,"Independently recovered population size differs");
      for(unsigned i=0;i<cut_neighbors;++i)
        require(snapshot.read("population/"+cut_neighbor_name(i),0,cut_bytes+1)==cut_neighbor_data(i),
            "A neighboring acknowledged file changed");
    }
    if(continued)require(snapshot.read("continuation.bin",0,cut_bytes+1)==cut_data(61),
        "Independent CPU readback of the fresh mounted write differs");
    return snapshot.storage_info().generation;
  };
  const auto recovered=verify(false);
  const auto recovered_extent=fs::file_size(path);
  run_publication_continuation(executable,fixture,source_id,name_on_disk,expected,false);
  const auto continued=verify(true);
  require(continued>recovered,"Writable continuation did not advance the image");
  std::wcout<<L"mounted_publication_cut boundary="<<name<<L" pending_before_exit=1 client_error="<<error
      <<L" recovered="<<(expects_new?L"next":L"previous")<<L" generation="<<recovered
      <<L" continuation_generation="<<continued<<L" cpu_readback=1 retired=1 operation="<<operation_name
      <<L" neighbors="<<(metadata?cut_neighbors:0)<<L" recovered_extent="<<recovered_extent
      <<L" continued_extent="<<fs::file_size(path)<<L'\n';
}


// Read both reserved image regions only after the writer's process has exited.
class SelectorImageReader final:public flitzfs::StoreByteIo {
 public:
  SelectorImageReader(std::shared_ptr<const flitzfs::ReadOnlyFile> file,std::uint64_t offset):
      file_(std::move(file)),offset_(offset) {
    require(file_->size()>offset_,"Reclamation did not retain the requested image range");
  }
  std::uint64_t extent() const override {
    return (std::min)(selector_limits.maximum_physical_bytes,file_->size()-offset_);
  }
  void read_exact(std::uint64_t at,std::span<std::byte> output) const override {
    require(at<=extent() && output.size()<=extent()-at,"Selector image read range");
    file_->read_exact_at(offset_+at,output.data(),output.size());
  }
  void write_exact(std::uint64_t,std::span<const std::byte>) override {read_only();}
  void flush() override {read_only();}
  void admit_write(std::uint64_t) override {read_only();}
  flitzfs::StoreIoContract contract() const noexcept override{return {};}
 protected:
  bool allows_writer() const noexcept override{return false;}
 private:
  [[noreturn]] static void read_only(){throw std::runtime_error("Selector image verification is read only");}
  const std::shared_ptr<const flitzfs::ReadOnlyFile> file_;
  const std::uint64_t offset_;
};
void run_selector_cut(const fs::path& executable,PublicationCut point) {
  stage=73;
  const auto index=static_cast<std::size_t>(point);
  source_fixture::Events protocol(true);
  constexpr flitzfs::ProcessBudget budget{128,2000,30000};
  source_fixture::Child source(executable,{L"--publication-cut-source",protocol.token(),
      std::wstring(cut_names[index]),L"image-switch"},budget);
  source.await(protocol,source_fixture::Phase::ready);
  const auto source_id=source.process_id();
  require(source_id && source_id!=GetCurrentProcessId(),"Selector source and client alias");
  Fixture fixture(source_id);
  const auto mount=fixture.root/L"mount",path=fixture.root/L"inflight.flfs";
  mounted_identity(mount);
  seed_cut_file(mount/L"payload.bin",cut_data(7));
  seed_cut_file(mount/L"witness.bin",cut_data(29));
  auto expected=cut_data(7);
  std::uint64_t inode=0;
  {
    Handle file(CreateFileW((mount/L"payload.bin").c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,nullptr));
    checked(file.value!=INVALID_HANDLE_VALUE,"Open selector history workload");
    inode=mounted_cut_inode(file.value);
    for(unsigned i=0;i<12;++i) {
      expected=cut_data(181+i);
      LARGE_INTEGER zero{};
      checked(SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN),"Selector history seek");
      write_bytes(file.value,expected);
    }
  }
  protocol.signal(source_fixture::Phase::written);
  const HANDLE waits[]{protocol.get(source_fixture::Phase::ack),source.result_event()};
  const auto reached=WaitForMultipleObjects(static_cast<DWORD>(std::size(waits)),waits,FALSE,18000);
  if(reached!=WAIT_OBJECT_0) {
    if(reached==WAIT_TIMEOUT)source.request_cancel();
    const auto no_cut=source.result();
    throw std::runtime_error("Selector cut was not observed, wait="+std::to_string(reached)+
        " source_exit="+std::to_string(no_cut.exit_code));
  }
  require(source.request_cancel(),"Selector source cancellation was not admitted");
  const auto retired=source.result();
  require(retired.process_id==source_id && retired.cancelled && !retired.timed_out && retired.exit_confirmed &&
      retired.exit_code==ERROR_CANCELLED && source.retirement_confirmed(),"Selector source retirement unconfirmed");
  require_removed(mount);
  auto initial=cut_volume_identity(source_id);
  initial.back()^=std::byte{1};
  const bool selected_next=point!=PublicationCut::before_commit;
  const auto inspect_payload=[&](const flitzfs::NativeFilesystemSnapshot& image,bool continued) {
    image.verify_all();
    require(image.stat("payload.bin").inode==inode && image.list().size()==(continued?3U:2U) &&
        image.read("payload.bin",0,cut_bytes+1)==expected &&
        image.read("witness.bin",0,cut_bytes+1)==cut_data(29),
        "Selector recovery did not preserve the acknowledged namespace and bytes");
    if(continued)require(image.read("continuation.bin",0,cut_bytes+1)==cut_data(61),
        "Recovered selector did not support a fresh acknowledged write");
  };
  flitzfs::StoreIdentity next{};
  std::uint64_t old_end=0,next_end=0;
  {
    const auto selected=flitzfs::NativeVolumeFile::inspect(path,cut_volume_identity(source_id));
    const auto info=selected.info();
    require(flitzfs::paired_volume_file(info.selection.placement) &&
        info.selection.epoch==(selected_next?2U:1U) && info.degraded==(point==PublicationCut::after_first_copy),
        "Selector recovery chose the wrong epoch or peer state");
    inspect_payload(selected.filesystem(),false);
    const auto file=std::make_shared<flitzfs::ReadOnlyFile>(flitzfs::ReadOnlyFile::open(path));
    const auto original=flitzfs::NativeFilesystemSnapshot::open_image(
        std::make_shared<SelectorImageReader>(file,flitzfs::kNativeVolumeFileImageOffset),info.selection.storage);
    const auto alternate=flitzfs::NativeFilesystemSnapshot::open_image(
        std::make_shared<SelectorImageReader>(file,selector_alternate_offset),info.selection.storage);
    inspect_payload(original,false);inspect_payload(alternate,false);
    next=alternate.identity();
    old_end=original.storage_info().physical_end;next_end=alternate.storage_info().physical_end;
    require(original.identity()==initial && next!=initial && next_end<old_end &&
        info.selection.image_identity==(selected_next?next:initial) &&
        (!selected_next || info.selection.parent_identity==initial),
        "Mounted handoff did not retain the old image and bind the compacted successor");
  }
  run_publication_continuation(executable,fixture,source_id,"payload.bin",expected,true);
  const auto recovered=flitzfs::NativeVolumeFile::inspect(path,cut_volume_identity(source_id));
  inspect_payload(recovered.filesystem(),true);
  require(!recovered.info().degraded && recovered.info().selection.image_identity==(selected_next?next:initial),
      "Writable continuation changed the selected image or left a damaged selector peer");
  std::wcout<<L"mounted_selector_cut boundary="<<selector_cut_names[index]<<L" epoch="<<(selected_next?2:1)
      <<L" source_end="<<old_end<<L" successor_end="<<next_end<<L" bytes_verified=1 retired=1\n";
}

} // namespace
int wmain(int argc,wchar_t** argv) {try {
  require_hosted_opt_in();require(argc>=2&&argc<=5,"explicit writable probe mode required");
  const std::wstring_view mode(argv[1]);
  if(mode==L"--publication-cut-source") {
    require_cut_guest();require(argc==5,"Publication-cut source arguments");
    const auto cut=std::ranges::find(cut_names,std::wstring_view(argv[3]));
    const auto operation=std::ranges::find(operation_names,std::wstring_view(argv[4]));
    require(cut!=cut_names.end() && operation!=operation_names.end(),"Unknown publication-cut operation or boundary");
    publication_source(argv[2],cut_points[static_cast<std::size_t>(cut-cut_names.begin())],
        static_cast<PublicationOperation>(operation-operation_names.begin()));return 0;
  }
  if(mode==L"--publication-cut-resumer" || mode==L"--publication-selector-resumer") {
    require_cut_guest();require(argc==4,"Publication-cut resumer arguments");
    publication_resumer(argv[2],process_number(argv[3]),
        mode==L"--publication-selector-resumer"?selector_contract:cut_contract);return 0;
  }
  if(mode==L"--run-publication-cuts" || mode==L"--run-metadata-publication-cuts" ||
      mode==L"--run-selector-publication-cuts") {
    require_cut_guest();require(argc==2,"Publication-cut controller arguments");
    const source_fixture::ControllerBudget controller_budget;
    wchar_t path[512]{};const auto n=GetModuleFileNameW(nullptr,path,512);
    require(n&&n<512,"Publication-cut executable identity");
    const fs::path executable(std::wstring(path,n));
    if(mode==L"--run-publication-cuts") {
      for(const auto point:cut_points)run_publication_cut(executable,point,PublicationOperation::write);
    } else if(mode==L"--run-metadata-publication-cuts") {
      for(const auto operation:{PublicationOperation::shrink,PublicationOperation::rename})
        for(const auto point:cut_points)run_publication_cut(executable,point,operation);
    } else {
      for(const auto point:cut_points)run_selector_cut(executable,point);
    }
    require(source_fixture::cleanup_ok.load()&&!source_fixture::source_busy.load(),"Publication-cut final cleanup");
    return 0;
  }
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
  (void)commit(); // Report-producing modes require the source binding before starting a child.
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
