#include "store_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "flitzfs/windows_path.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include "windows/maintenance_backing.hpp"
#define NOMINMAX
#include "windows/positioned_file_io.hpp"
#include "windows/volume_file_lifetime.hpp"
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace flitzfs::detail {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("FlitzFS store file: ")+message); }
constexpr std::uint64_t kReserve=1024ULL*1024*1024;
constexpr std::uint64_t kMemory=640ULL*1024*1024; // 512 MiB reserve + 64 MiB cache + 64 MiB staging allowance.
void memory_headroom() {
#ifdef _WIN32
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  if(!GlobalMemoryStatusEx(&m) || m.ullAvailPhys<kMemory || m.ullAvailPageFile<kMemory)
    fail("unavailable physical/commit memory headroom");
#elif defined(__linux__)
  std::ifstream input("/proc/meminfo",std::ios::binary);
  std::array<char,16384> raw{};
  input.read(raw.data(),static_cast<std::streamsize>(raw.size()));
  if(!input.eof() || input.bad()) fail("cannot bound memory telemetry");
  std::istringstream lines(std::string(raw.data(),static_cast<std::size_t>(input.gcount())));
  std::string line;std::uint64_t available=0,limit=0,committed=0;
  bool have_available=false,have_limit=false,have_committed=false;
  while(std::getline(lines,line)) {
    std::istringstream row(line);std::string key,unit;std::uint64_t value=0;
    if(!(row>>key>>value>>unit)) continue;
    if(unit!="kB" || value>(std::numeric_limits<std::uint64_t>::max)()/1024) continue;
    if(key=="MemAvailable:") {available=value*1024;have_available=true;}
    if(key=="CommitLimit:") {limit=value*1024;have_limit=true;}
    if(key=="Committed_AS:") {committed=value*1024;have_committed=true;}
  }
  if(!have_available || !have_limit || !have_committed || available<kMemory ||
      committed>limit || limit-committed<kMemory) fail("unavailable physical/commit memory headroom");
#else
  fail("file backend resource telemetry not qualified on this platform");
#endif
}
void path_policy(const std::filesystem::path& p) {
  if(p.empty() || p.filename().empty() || p==p.root_path()) fail("specific ordinary file required");
#ifdef _WIN32
  if(!ordinary_windows_file_path(p.native())) fail("unsafe Windows output path");
#else
  if(p.native().find(char{0})!=std::string::npos) fail("embedded NUL");
  const auto normal=p.lexically_normal().string();
  for(const auto* prefix:{"/dev","/proc","/sys"}) {
    const std::string base(prefix);
    if(normal==base || normal.rfind(base+"/",0)==0) fail("device/system output forbidden");
  }
#endif
}
#ifdef _WIN32
constexpr auto maximum_windows_file_bytes=static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)());
struct PinnedDirectory {
  HANDLE handle=INVALID_HANDLE_VALUE;
  std::wstring volume_path,volume_root;
  PinnedDirectory()=default;
  PinnedDirectory(const PinnedDirectory&)=delete;
  PinnedDirectory& operator=(const PinnedDirectory&)=delete;
  ~PinnedDirectory(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
std::wstring opened_path(HANDLE handle,DWORD volume_name) {
  const auto flags=FILE_NAME_NORMALIZED|volume_name;
  const auto required=GetFinalPathNameByHandleW(handle,nullptr,0,flags);
  if(!required || required>32768)fail("cannot resolve the opened volume-file path");
  std::wstring actual(required,L'\0');
  const auto length=GetFinalPathNameByHandleW(handle,actual.data(),required,flags);
  if(!length || length>=required)fail("cannot resolve the opened volume-file path");
  actual.resize(length);
  return actual;
}
bool same_windows_path(std::wstring_view left,std::wstring_view right) noexcept {
  return CompareStringOrdinal(left.data(),static_cast<int>(left.size()),
      right.data(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
void require_opened_path(HANDLE handle,std::filesystem::path path) {
  path.make_preferred();
  if(!same_windows_path(opened_path(handle,VOLUME_NAME_DOS),L"\\\\?\\"+path.native()))
    fail("the opened volume-file path is redirected");
}
std::wstring volume_root_from_path(std::wstring_view actual) {
  const std::wstring_view prefix=L"\\\\?\\Volume{";
  const auto end=actual.find(L'\\',prefix.size());
  if(!actual.starts_with(prefix) || end==std::wstring::npos || end<=prefix.size() || actual[end-1]!=L'}')
    fail("cannot identify the opened local volume");
  return std::wstring(actual.substr(0,end+1));
}
bool local_volume_drive(UINT type) noexcept {
  return type==DRIVE_FIXED || type==DRIVE_REMOVABLE;
}
std::uint64_t available_volume_growth(const std::wstring& volume,const VolumeFileLifetime* lifetime=nullptr) {
  if(lifetime)lifetime->check();
  ULARGE_INTEGER available{},capacity{};
  const auto queried=GetDiskFreeSpaceExW(volume.c_str(),&available,&capacity,nullptr);
  if(lifetime)lifetime->check(queried?ERROR_SUCCESS:GetLastError());
  if(!queried || !capacity.QuadPart || available.QuadPart>capacity.QuadPart)
    fail("cannot verify space on the opened volume");
  // Small removable media need a reserve related to their capacity. Protect
  // larger volumes with the same reserve used by the file adapter.
  const auto reserve=(std::min)(kReserve,capacity.QuadPart/16+(capacity.QuadPart%16!=0));
  if(available.QuadPart<reserve)fail("insufficient verified volume reserve");
  return available.QuadPart-reserve;
}
void admit_volume_space(const std::wstring& volume,std::uint64_t growth,const VolumeFileLifetime& lifetime) {
  if(growth>available_volume_growth(volume,&lifetime))fail("insufficient verified volume reserve");
}
void pin_volume_directory(PinnedDirectory& pinned,std::filesystem::path file) {
  file.make_preferred();
  if(!file.is_absolute() || file!=file.lexically_normal() || !local_volume_drive(GetDriveTypeW(file.root_path().c_str())))
    fail("volume file requires an absolute path on a local fixed or removable drive");
  const auto path=file.parent_path();
  pinned.handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  FILE_ATTRIBUTE_TAG_INFO info{};
  if(pinned.handle==INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(pinned.handle,FileAttributeTagInfo,&info,sizeof(info)) ||
      !(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
    fail("cannot pin the volume-file directory");
  require_opened_path(pinned.handle,path);
  pinned.volume_path=opened_path(pinned.handle,VOLUME_NAME_GUID);
  pinned.volume_root=volume_root_from_path(pinned.volume_path);
  if(!local_volume_drive(GetDriveTypeW(pinned.volume_root.c_str())))
    fail("the opened parent has no supported local volume");
}
void validate_volume_storage(HANDLE file,StoreIoContract qualified) {
  FILE_STORAGE_INFO disk{};
  if(!GetFileInformationByHandleEx(file,FileStorageInfo,&disk,sizeof(disk)) ||
      disk.ByteOffsetForSectorAlignment || disk.ByteOffsetForPartitionAlignment)
    fail("volume storage alignment is unavailable or incompatible");
  for(auto unit:{disk.LogicalBytesPerSector,disk.PhysicalBytesPerSectorForAtomicity,
      disk.FileSystemEffectivePhysicalBytesPerSectorForAtomicity}) {
    if(!unit || (unit&(unit-1)) || unit>qualified.write_failure_unit_bytes)
      fail("live storage geometry exceeds the qualified failure unit");
  }
}
VolumeFileGeometry query_volume_file_geometry(HANDLE file,const std::wstring& volume) {
  wchar_t filesystem[32]{};
  DWORD flags=0;
  if(!GetVolumeInformationByHandleW(file,nullptr,0,nullptr,nullptr,&flags,filesystem,static_cast<DWORD>(std::size(filesystem))))
    return volume_file_geometry({},false,0);
  std::uint64_t cluster_bytes=0;
  if(std::wstring_view(filesystem)==L"NTFS") {
    DWORD sectors=0,sector_bytes=0,free_clusters=0,total_clusters=0;
    if(GetDiskFreeSpaceW(volume.c_str(),&sectors,&sector_bytes,&free_clusters,&total_clusters))
      cluster_bytes=static_cast<std::uint64_t>(sectors)*sector_bytes;
  }
  return volume_file_geometry(filesystem,(flags&FILE_SUPPORTS_SPARSE_FILES)!=0,cluster_bytes);
}
#endif
class Input final:public StoreByteIo {
  mutable std::mutex mutex_;
  ReadOnlyFile file_;
 public:
  Input(const std::filesystem::path& p,std::uint64_t limit):file_(ReadOnlyFile::open(p)) {
    file_.lock_shared_nonblocking();
    if(!limit || file_.size()>limit)fail("physical input cap");
  }
  std::uint64_t extent() const override { return file_.size(); }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    std::lock_guard lock(mutex_);file_.read_exact_at(at,out.data(),out.size());
  }
  void write_exact(std::uint64_t,std::span<const std::byte>) override { fail("read-only backing"); }
  void flush() override { fail("read-only backing"); }
  void admit_write(std::uint64_t) override { fail("read-only backing"); }
  StoreIoContract contract() const noexcept override { return {}; }
};
class Output final:public NativeVolumeRangeIo {
  mutable std::mutex mutex_;
  StoreIoContract contract_;
  mutable std::uint64_t extent_=0;
  std::filesystem::path parent_path_;
  std::uint64_t capacity_; // Clamped by the opened backing before the provider is shared.
#ifdef _WIN32
  const bool volume_file_;
  bool sparse_=false;
  std::uint64_t admitted_extent_=0;
  PinnedDirectory parent_pin_;
  HANDLE handle_=INVALID_HANDLE_VALUE;
  VolumeFileLifetime volume_lifetime_;
  std::optional<PositionedFileIo> demand_io_;
  mutable std::mutex capture_mutex_;
  mutable std::optional<PositionedFileIo> capture_io_;
  mutable std::atomic<std::uint64_t> published_extent_{0};
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
  std::shared_ptr<VolumeFileObserver> volume_observer_;
#endif
  bool has_allocations(std::uint64_t begin,std::uint64_t end) const {
    volume_lifetime_.check();
    MaintenanceBackingCall observed(MaintenanceBackingOperation::control);
    FILE_ALLOCATED_RANGE_BUFFER query{},range{};
    query.FileOffset.QuadPart=static_cast<LONGLONG>(begin);
    query.Length.QuadPart=static_cast<LONGLONG>(end-begin);
    const auto control=demand_io_->control(handle_,FSCTL_QUERY_ALLOCATED_RANGES,
        &query,sizeof(query),&range,sizeof(range));
    volume_lifetime_.check(control.error);
    if(control.error!=ERROR_SUCCESS && (control.error!=ERROR_MORE_DATA || control.bytes!=sizeof(range)))
      fail("cannot independently query sparse allocations");
    if(!control.bytes){observed.complete();return false;}
    if(control.bytes!=sizeof(range) || range.FileOffset.QuadPart<0 || range.Length.QuadPart<=0)
      fail("invalid sparse allocation range");
    const auto at=static_cast<std::uint64_t>(range.FileOffset.QuadPart);
    const auto length=static_cast<std::uint64_t>(range.Length.QuadPart);
    if(at>=end || (at<begin && length<=begin-at))fail("sparse allocation range does not intersect query");
    observed.complete();
    return true;
  }
  void read_positioned(std::uint64_t at,std::span<std::byte> out,std::uint64_t extent,
      const PositionedFileIo& request,const StoreCaptureReadScope* capture=nullptr) const {
    if(at>extent || out.size()>extent-at)fail("read outside backing extent");
    volume_lifetime_.check();
    MaintenanceBackingCall observed(MaintenanceBackingOperation::read,out.size(),!out.empty());
    while(!out.empty()) {
      const auto bytes=static_cast<DWORD>((std::min)(out.size(),std::size_t{0x7ffff000}));
      const auto result=request.read(handle_,at,out.data(),bytes,capture);
      volume_lifetime_.check(result.error);
      if(result.error!=ERROR_SUCCESS || !result.bytes || result.bytes>bytes)fail("short/failed backing read");
      at+=result.bytes;
      out=out.subspan(result.bytes);
    }
    observed.complete();
  }
  void constrain_volume_extent(VolumeFileGeometry geometry,bool reopen) {
    if(contract_.isolated_range_reclamation && !geometry.isolated_sparse_ranges)
      fail("isolated file ranges need qualified NTFS sparse storage and allocation geometry");
    if(!reopen && capacity_>geometry.maximum_extent)
      fail("the requested volume exceeds the backing filesystem's maximum file size");
    capacity_=(std::min)(capacity_,geometry.maximum_extent);
  }
  void admit_windows_growth(std::uint64_t projected,std::uint64_t growth) {
    if(projected<extent_ || projected>capacity_ || growth>capacity_)fail("projected extent outside budget");
    memory_headroom();
    if(volume_file_) {
      MaintenanceBackingCall observed(MaintenanceBackingOperation::control);
      admit_volume_space(parent_pin_.volume_root,growth,volume_lifetime_);
      observed.complete();
    }else {
      std::error_code ec;const auto free=std::filesystem::space(parent_path_,ec);
      if(ec || free.available==static_cast<std::uintmax_t>(-1) || free.available<kReserve || growth>free.available-kReserve)
        fail("insufficient verified disk reserve");
    }
    admitted_extent_=projected;
  }
#else
  int fd_=-1,parent_=-1;
  bool parent_synced_=false;
#endif
  void refresh_extent() const {
#ifdef _WIN32
    MaintenanceBackingCall observed(MaintenanceBackingOperation::control,0,volume_file_);
    if(volume_file_)volume_lifetime_.check();
    LARGE_INTEGER size{};
    const auto sized=GetFileSizeEx(handle_,&size);
    if(volume_file_)volume_lifetime_.check(sized?ERROR_SUCCESS:GetLastError());
    if(!sized || size.QuadPart<0 ||
        static_cast<std::uint64_t>(size.QuadPart)>capacity_) fail("cannot qualify current file extent");
    extent_=static_cast<std::uint64_t>(size.QuadPart);
    if(volume_file_)published_extent_.store(extent_,std::memory_order_release);
    observed.complete();
#else
    struct stat st{};
    if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 ||
        static_cast<std::uint64_t>(st.st_size)>capacity_) fail("cannot qualify current file extent");
    extent_=static_cast<std::uint64_t>(st.st_size);
#endif
  }
 public:
  Output(const std::filesystem::path& p,StoreIoContract c,bool reopen=false,
      std::optional<std::uint64_t> volume_limit=std::nullopt,
      [[maybe_unused]] std::wstring_view expected_volume={})
      :contract_(c),capacity_(volume_limit.value_or(kStoreMaxPhysicalBytes))
#ifdef _WIN32
       ,volume_file_(volume_limit.has_value())
#endif
  {
    path_policy(p);memory_headroom();
    const auto parent=p.parent_path().empty() ? std::filesystem::path(".") : p.parent_path();
    parent_path_=parent;
#ifdef _WIN32
    if(volume_file_) {
      if(!capacity_ || capacity_>maximum_windows_file_bytes || !valid_store_io_contract(c))
        fail("invalid persistent-file qualification");
      pin_volume_directory(parent_pin_,p);
      if(!expected_volume.empty() && !same_windows_path(parent_pin_.volume_root,expected_volume))
        fail("the selected backing volume has changed");
      admit_volume_space(parent_pin_.volume_root,0,volume_lifetime_);
      constrain_volume_extent(query_volume_file_geometry(parent_pin_.handle,parent_pin_.volume_root),reopen);
      demand_io_.emplace();
    }else {
      std::error_code ec;const auto space=std::filesystem::space(parent,ec);
      if(ec || space.available==static_cast<std::uintmax_t>(-1) || space.available<kReserve+kStoreMaxPhysicalBytes)
        fail("insufficient verified disk reserve");
    }
    auto target=p.native();
    if(volume_file_) {
      target=parent_pin_.volume_path;
      if(target.back()!=L'\\')target+=L'\\';
      target+=p.filename().native();
    }
    const auto flags=FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|(volume_file_?FILE_FLAG_OVERLAPPED:0);
    handle_=CreateFileW(target.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
        reopen?OPEN_EXISTING:CREATE_NEW,flags,nullptr);
    if(handle_==INVALID_HANDLE_VALUE) fail("exclusive creation failed");
    try {
      FILE_ATTRIBUTE_TAG_INFO attr{};FILE_STANDARD_INFO info{};LARGE_INTEGER size{};
      if(GetFileType(handle_)!=FILE_TYPE_DISK ||
          !GetFileInformationByHandleEx(handle_,FileAttributeTagInfo,&attr,sizeof(attr)) ||
          (attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
          !GetFileInformationByHandleEx(handle_,FileStandardInfo,&info,sizeof(info)) || info.NumberOfLinks!=1 ||
          !GetFileSizeEx(handle_,&size) || size.QuadPart<0 ||
          static_cast<std::uint64_t>(size.QuadPart)>capacity_ || (!reopen && size.QuadPart!=0))
        fail("backing is not an admissible single-link ordinary file");
      if(volume_file_) {
        if(attr.FileAttributes&(FILE_ATTRIBUTE_COMPRESSED|FILE_ATTRIBUTE_ENCRYPTED))
          fail("volume-file storage requires ordinary caching semantics");
        if(!same_windows_path(opened_path(handle_,VOLUME_NAME_GUID),target))
          fail("the opened file is redirected from the selected backing path");
        validate_volume_storage(handle_,c);
        constrain_volume_extent(query_volume_file_geometry(handle_,parent_pin_.volume_root),reopen);
        if(static_cast<std::uint64_t>(size.QuadPart)>capacity_)fail("the backing file exceeds the live extent limit");
        sparse_=(attr.FileAttributes&FILE_ATTRIBUTE_SPARSE_FILE)!=0;
        if(sparse_ && !c.isolated_range_reclamation)
          fail("sparse volume file needs an isolated range contract");
        if(c.isolated_range_reclamation) {
          if(!reopen && !sparse_) {
            if(demand_io_->control(handle_,FSCTL_SET_SPARSE,nullptr,0).error!=ERROR_SUCCESS ||
                !GetFileInformationByHandleEx(handle_,FileAttributeTagInfo,&attr,sizeof(attr)) ||
                !(attr.FileAttributes&FILE_ATTRIBUTE_SPARSE_FILE))
              fail("cannot create the qualified sparse volume backing");
            sparse_=true;
          }
        }
      }
      extent_=admitted_extent_=static_cast<std::uint64_t>(size.QuadPart);
      if(volume_file_)published_extent_.store(extent_,std::memory_order_release);
    }catch(...) {CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;throw;}
#else
    parent_=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(parent_<0) fail("cannot pin ordinary parent directory");
    struct statvfs space{};
    if(::fstatvfs(parent_,&space)!=0 || !space.f_frsize ||
        space.f_bavail<(kStoreMaxPhysicalBytes+kReserve+space.f_frsize-1)/space.f_frsize) {
      ::close(parent_);parent_=-1;fail("insufficient verified disk reserve");
    }
    const int flags=O_RDWR|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW|(reopen?0:(O_CREAT|O_EXCL));
    fd_=::openat(parent_,p.filename().c_str(),flags,0600);
    if(fd_<0) {::close(parent_);parent_=-1;fail("exclusive creation failed");}
    struct stat st{};
    if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size<0 ||
        static_cast<std::uint64_t>(st.st_size)>kStoreMaxPhysicalBytes || (!reopen && st.st_size!=0) ||
        ::flock(fd_,LOCK_EX|LOCK_NB)!=0) {
      ::close(fd_);::close(parent_);fd_=parent_=-1;fail("backing is not an exclusively locked single-link regular file");
    }
    extent_=static_cast<std::uint64_t>(st.st_size);
#endif
  }
  ~Output() override {
#ifdef _WIN32
    if(handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if(fd_>=0) ::close(fd_);
    if(parent_>=0) ::close(parent_);
#endif
  }
#if defined(_WIN32) && defined(FLITZFS_OBSERVE_VOLUME_PUBLICATION)
  void observe_volume(std::shared_ptr<VolumeFileObserver> observer) {
    if(!observer || volume_observer_)fail("invalid volume observer");
    volume_observer_=std::move(observer);
  }
#endif
  StoreIoContract contract() const noexcept override { return contract_; }
#ifdef _WIN32
 protected:
  bool allows_writer() const noexcept override {return volume_lifetime_.current();}
 public:
#endif
  bool concurrent_reads_supported() const noexcept override {
#ifdef _WIN32
    return volume_file_;
#else
    return false;
#endif
  }
  bool supports_reclaimable_ranges() const noexcept override {
#ifdef _WIN32
    return sparse_;
#else
    return false;
#endif
  }
  std::uint64_t allocated_extent(std::uint64_t offset,std::uint64_t bytes) const override {
#ifdef _WIN32
    std::lock_guard lock(mutex_);
    if(!sparse_ || offset>capacity_ || bytes>capacity_-offset || offset%kStoreCommitUnit || bytes%kStoreCommitUnit)
      fail("invalid sparse allocation query");
    refresh_extent();
    if(offset>=extent_ || !bytes)return 0;
    const auto end=(std::min)(offset+bytes,extent_);
    // A one-entry suffix query answers presence. Binary search bounds work
    // independently of the number of fragments in the selected range.
    std::uint64_t first=0,last=(end-offset+kStoreCommitUnit-1)/kStoreCommitUnit;
    while(first<last) {
      const auto middle=first+(last-first)/2;
      if(has_allocations(offset+middle*kStoreCommitUnit,end))first=middle+1;
      else last=middle;
    }
    return first*kStoreCommitUnit;
#else
    (void)offset;(void)bytes;fail("range allocation query requires Windows");
#endif
  }
  void admit_range_write(std::uint64_t projected,std::uint64_t growth) override {
#ifdef _WIN32
    std::lock_guard lock(mutex_);refresh_extent();
    if(!sparse_ || !has_writer())fail("range growth needs an exclusive sparse volume owner");
    admit_windows_growth(projected,growth);
#else
    (void)projected;(void)growth;fail("range admission requires Windows");
#endif
  }
  void reclaim_range(std::uint64_t offset,std::uint64_t bytes) override {
#ifdef _WIN32
    std::lock_guard lock(mutex_);
    if(!sparse_ || !has_writer() || offset<kNativeVolumeFileImageOffset || offset>capacity_ ||
        !bytes || bytes>capacity_-offset || offset%kStoreCommitUnit || bytes%kStoreCommitUnit)
      fail("invalid sparse range retirement");
    refresh_extent();
    if(offset>=extent_)return;
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
    if(volume_observer_)volume_observer_->before_reclaim(offset,bytes);
#endif
    FILE_ZERO_DATA_INFORMATION request{};
    request.FileOffset.QuadPart=static_cast<LONGLONG>(offset);
    request.BeyondFinalZero.QuadPart=static_cast<LONGLONG>((std::min)(offset+bytes,extent_));
    volume_lifetime_.check();
    MaintenanceBackingCall observed(MaintenanceBackingOperation::control);
    const auto result=demand_io_->control(handle_,FSCTL_SET_ZERO_DATA,&request,sizeof(request));
    volume_lifetime_.check(result.error);
    if(result.error!=ERROR_SUCCESS)
      fail("sparse range retirement failed");
    observed.complete();
#else
    (void)offset;(void)bytes;fail("range reclamation requires Windows");
#endif
  }
  std::uint64_t extent() const override {
#ifdef _WIN32
    if(volume_file_ && StoreCaptureReadScope::current()) {
      volume_lifetime_.check();
      return published_extent_.load(std::memory_order_acquire);
    }
#endif
    std::lock_guard lock(mutex_);
    refresh_extent();
    return extent_;
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
#ifdef _WIN32
    const auto* capture=StoreCaptureReadScope::current();
    if(volume_file_ && capture) {
      volume_lifetime_.check();
      std::unique_lock access(capture_mutex_,std::try_to_lock);
      if(!access)fail("another capture owns the positioned read event");
      if(!capture_io_)capture_io_.emplace();
      read_positioned(at,out,published_extent_.load(std::memory_order_acquire),*capture_io_,capture);
      return;
    }
#endif
    std::lock_guard lock(mutex_);
    refresh_extent();
#ifdef _WIN32
    if(volume_file_) {
      read_positioned(at,out,extent_,*demand_io_);
      return;
    }
#endif
    if(at>extent_ || out.size()>extent_-at) fail("read outside backing extent");
    std::size_t done=0;
    while(done<out.size()) {
#ifdef _WIN32
      LARGE_INTEGER pos{};pos.QuadPart=static_cast<LONGLONG>(at+done);
      if(!SetFilePointerEx(handle_,pos,nullptr,FILE_BEGIN)) fail("seek failed");
      DWORD n=0;
      const auto request=static_cast<DWORD>((std::min)(out.size()-done,std::size_t{0x7ffff000}));
      if(!ReadFile(handle_,out.data()+done,request,&n,nullptr) || !n) fail("short/failed backing read");
#else
      const auto n=::pread(fd_,out.data()+done,out.size()-done,static_cast<off_t>(at+done));
      if(n<0 && errno==EINTR) continue;
      if(n<=0) fail("short/failed backing read");
#endif
      done+=static_cast<std::size_t>(n);
    }
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override {
    std::lock_guard lock(mutex_);
    if(at>capacity_ || bytes.size()>capacity_-at)fail("physical output budget");
#ifdef _WIN32
    if(volume_file_ && !bytes.empty() && at+bytes.size()>admitted_extent_)
      fail("volume-file growth was not admitted");
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
    if(volume_observer_ && !bytes.empty())volume_observer_->before_write(at,bytes.size());
#endif
    MaintenanceBackingCall observed(MaintenanceBackingOperation::write,bytes.size(),volume_file_ && !bytes.empty());
#endif
    std::size_t done=0;
    while(done<bytes.size()) {
#ifdef _WIN32
      DWORD n=0;
      const auto request=static_cast<DWORD>((std::min)(bytes.size()-done,std::size_t{0x7ffff000}));
      if(volume_file_) {
        volume_lifetime_.check();
        const auto result=demand_io_->write(handle_,at+done,bytes.data()+done,request);
        volume_lifetime_.check(result.error);
        if(result.error!=ERROR_SUCCESS || !result.bytes || result.bytes>request)fail("short/failed backing write");
        n=result.bytes;
      } else {
        LARGE_INTEGER pos{};pos.QuadPart=static_cast<LONGLONG>(at+done);
        if(!SetFilePointerEx(handle_,pos,nullptr,FILE_BEGIN)) fail("seek failed");
        if(!WriteFile(handle_,bytes.data()+done,request,&n,nullptr) || !n)fail("short/failed backing write");
      }
#else
      const auto n=::pwrite(fd_,bytes.data()+done,bytes.size()-done,static_cast<off_t>(at+done));
      if(n<0 && errno==EINTR) continue;
      if(n<=0) fail("short/failed backing write");
#endif
      done+=static_cast<std::size_t>(n);
      extent_=(std::max)(extent_,at+done);
    }
#ifdef _WIN32
    if(volume_file_)published_extent_.store(extent_,std::memory_order_release);
    observed.complete();
#endif
  }
  void admit_write(std::uint64_t projected) override {
    std::lock_guard lock(mutex_);
    refresh_extent();
    if(projected<extent_ || projected>capacity_)fail("projected extent outside budget");
#ifdef _WIN32
    if(sparse_)fail("sparse volume writes require range-based admission");
    admit_windows_growth(projected,projected-extent_);
#else
    memory_headroom();
    const auto needed=projected-extent_+kReserve;
    struct statvfs free{};
    if(::fstatvfs(parent_,&free)!=0 || !free.f_frsize ||
        free.f_bavail<(needed+free.f_frsize-1)/free.f_frsize) fail("insufficient verified disk reserve");
#endif
  }
  void flush() override {
    std::lock_guard lock(mutex_);
#ifdef _WIN32
    {
      if(volume_file_)volume_lifetime_.check();
      MaintenanceBackingCall observed(MaintenanceBackingOperation::flush,0,volume_file_);
      const auto flushed=FlushFileBuffers(handle_);
      if(volume_file_)volume_lifetime_.check(flushed?ERROR_SUCCESS:GetLastError());
      if(!flushed)fail("file flush failed");
      observed.complete();
    }
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
    if(volume_observer_)volume_observer_->after_flush();
#endif
#else
    int result;
    do {result=::fsync(fd_);} while(result!=0 && errno==EINTR);
    if(result!=0) fail("file flush failed");
    if(!parent_synced_) {
      do {result=::fsync(parent_);} while(result!=0 && errno==EINTR);
      if(result!=0) fail("directory creation flush failed");
      parent_synced_=true;
    }
#endif
  }
};
}
std::shared_ptr<StoreByteIo> new_store_file(const std::filesystem::path& path,StoreIoContract c) {
  return std::make_shared<Output>(path,c);
}
std::shared_ptr<StoreByteIo> reopen_store_file_for_hosted_test(const std::filesystem::path& path,StoreIoContract c) {
  return std::make_shared<Output>(path,c,true);
}
std::shared_ptr<StoreByteIo> read_store_file(const std::filesystem::path& path,std::uint64_t limit) {
  return std::make_shared<Input>(path,limit);
}
#ifdef _WIN32
VolumeFileGeometry volume_file_geometry(std::wstring_view filesystem,bool supports_sparse_files,
    std::uint64_t cluster_bytes) noexcept {
  if(filesystem==L"FAT32")return {(std::numeric_limits<std::uint32_t>::max)(),false};
  constexpr std::uint64_t ntfs_cluster_limit=1ULL<<32;
  constexpr std::uint64_t ntfs_reserved_tail=65536;
  if(filesystem!=L"NTFS" || !std::has_single_bit(cluster_bytes) ||
      cluster_bytes>maximum_windows_file_bytes/ntfs_cluster_limit)
    return {maximum_windows_file_bytes,false};
  // The current Windows NTFS maximum is (2^32 * cluster size) - 64 KiB.
  return {ntfs_cluster_limit*cluster_bytes-ntfs_reserved_tail,
      supports_sparse_files && kStoreCommitUnit%cluster_bytes==0};
}
VolumeFileDestination volume_file_destination(const std::filesystem::path& path) {
  path_policy(path);
  PinnedDirectory pinned;
  pin_volume_directory(pinned,path);
  const auto available=available_volume_growth(pinned.volume_root);
  const auto geometry=query_volume_file_geometry(pinned.handle,pinned.volume_root);
  return {std::move(pinned.volume_root),available,geometry};
}
std::shared_ptr<StoreByteIo> new_volume_file(const std::filesystem::path& path,StoreIoContract c,std::uint64_t limit,
    std::wstring_view expected_volume) {
  return std::make_shared<Output>(path,c,false,limit,expected_volume);
}
#ifdef FLITZFS_OBSERVE_VOLUME_PUBLICATION
std::shared_ptr<StoreByteIo> new_observed_volume_file(const std::filesystem::path& path,StoreIoContract c,
    std::uint64_t limit,std::shared_ptr<VolumeFileObserver> observer) {
  auto output=std::make_shared<Output>(path,c,false,limit);
  output->observe_volume(std::move(observer));
  return output;
}
#endif
std::shared_ptr<StoreByteIo> reopen_volume_file(const std::filesystem::path& path,StoreIoContract c,std::uint64_t limit,
    std::wstring_view expected_volume) {
  return std::make_shared<Output>(path,c,true,limit,expected_volume);
}
#endif
} // namespace flitzfs::detail
