// Experimental disposable-VM writable qualification. Never a product mount CLI.
#include "winfsp_session.hpp"
#include "winfsp_native.hpp"
#include "bounded_process.hpp"
#include "flitzfs/native_filesystem.hpp"
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
  return {text.begin(),text.end()};
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
      <<",\"dispatcher_stopped\":"<<(stopped?"true":"false")<<",\"gpu\":false}";
  const auto text=out.str();require(text.size()<=report_cap,"writable report budget");
  const auto* first=reinterpret_cast<const std::byte*>(text.data());
  write_new(fixture.root/(phase=="write"?L"write.json":L"verify.json"),{first,first+text.size()});
}
void write_probe() {
  require_probe_job();stage=3;Fixture fixture;
  const auto image=fixture.root/L"native.fltx",mount=fixture.root/L"mount";
  flitzfs::WinFspNativeSession session(flitzfs::WinFspNativeWritable::create_new_hosted(
      image,identity(),{flitzfs::StoreCompressionMode::automatic,6,true},image_cap),live_api());
  stage=4;
  require(session.start({mount.native(),15,true,true})==STATUS_SUCCESS,"writable mount startup");
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
  Fixture fixture(source_process);const auto leaf=phase=="write"?L"write.json":L"verify.json";
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
  require(fs::file_size(fixture.root/L"native.fltx")+fs::file_size(fixture.root/L"write.json")+
      (phase=="verify"?fs::file_size(fixture.root/L"verify.json"):0)<=image_cap+2*report_cap,
      "writable aggregate fixture cap");
  std::cout<<"Windows writable mount evidence: "<<text<<"\n";
}
} // namespace
int wmain(int argc,wchar_t** argv) {try {
  require_hosted_opt_in();require(argc==2||argc==3,"explicit writable probe mode required");
  const std::wstring_view mode(argv[1]);
  if(mode==L"--write-child"){require(argc==2,"unexpected writer argument");write_probe();return 0;}
  if(mode==L"--verify-child"){require(argc==3,"missing source identity");verify_probe(process_number(argv[2]));return 0;}
  require(argc==2&&mode==L"--run-disposable-ci","unknown writable probe mode");
  wchar_t path[512]{};const auto n=GetModuleFileNameW(nullptr,path,512);require(n&&n<512,"probe executable identity");
  const fs::path executable(std::wstring(path,n));flitzfs::ProcessBudget budget;budget.timeout_ms=30000;
  const auto written=flitzfs::run_bounded_process(executable,{L"--write-child"},budget);
  std::cout<<"writable_probe exit="<<written.exit_code<<" stage="<<(written.exit_code>>16U)
      <<" detail="<<(written.exit_code&0xffffU)<<" timed_out="<<written.timed_out<<" cancelled="<<written.cancelled
      <<" exit_confirmed="<<written.exit_confirmed<<" peak_commit_bytes="<<written.peak_committed_bytes<<"\n";
  require(written.process_id&&written.exit_confirmed&&!written.timed_out&&!written.cancelled&&written.exit_code==0,
      "supervised writable helper failed");emit(written.process_id,"write");
  const auto verified=flitzfs::run_bounded_process(executable,{L"--verify-child",std::to_wstring(written.process_id)},budget);
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
