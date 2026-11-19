#include "store_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "flitzfs/windows_path.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
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
  PinnedDirectory()=default;
  PinnedDirectory(const PinnedDirectory&)=delete;
  PinnedDirectory& operator=(const PinnedDirectory&)=delete;
  ~PinnedDirectory(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
void require_opened_path(HANDLE handle,std::filesystem::path path) {
  path.make_preferred();
  const auto expected=L"\\\\?\\"+path.native();
  const auto flags=FILE_NAME_NORMALIZED|VOLUME_NAME_DOS;
  const auto required=GetFinalPathNameByHandleW(handle,nullptr,0,flags);
  if(!required || required>32768)fail("cannot resolve the opened volume-file path");
  std::wstring actual(required,L'\0');
  const auto length=GetFinalPathNameByHandleW(handle,actual.data(),required,flags);
  if(!length || length>=required || CompareStringOrdinal(actual.data(),static_cast<int>(length),
      expected.data(),static_cast<int>(expected.size()),TRUE)!=CSTR_EQUAL)
    fail("the opened volume-file path is redirected");
}
void pin_volume_directory(PinnedDirectory& pinned,std::filesystem::path file) {
  file.make_preferred();
  if(!file.is_absolute() || file!=file.lexically_normal() || GetDriveTypeW(file.root_path().c_str())!=DRIVE_FIXED)
    fail("volume file requires a normalized absolute local path");
  const auto path=file.parent_path();
  pinned.handle=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  FILE_ATTRIBUTE_TAG_INFO info{};
  if(pinned.handle==INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(pinned.handle,FileAttributeTagInfo,&info,sizeof(info)) ||
      !(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
    fail("cannot pin the volume-file directory");
  if(path!=path.root_path())require_opened_path(pinned.handle,path);
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
class Output final:public StoreByteIo {
  mutable std::mutex mutex_;
  StoreIoContract contract_;
  mutable std::uint64_t extent_=0;
  std::filesystem::path parent_path_;
  const std::uint64_t capacity_;
#ifdef _WIN32
  const bool volume_file_;
  std::uint64_t admitted_extent_=0;
  PinnedDirectory parent_pin_;
  HANDLE handle_=INVALID_HANDLE_VALUE;
#else
  int fd_=-1,parent_=-1;
  bool parent_synced_=false;
#endif
  void refresh_extent() const {
#ifdef _WIN32
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(handle_,&size) || size.QuadPart<0 ||
        static_cast<std::uint64_t>(size.QuadPart)>capacity_) fail("cannot qualify current file extent");
    extent_=static_cast<std::uint64_t>(size.QuadPart);
#else
    struct stat st{};
    if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0 ||
        static_cast<std::uint64_t>(st.st_size)>capacity_) fail("cannot qualify current file extent");
    extent_=static_cast<std::uint64_t>(st.st_size);
#endif
  }
 public:
  Output(const std::filesystem::path& p,StoreIoContract c,bool reopen=false,
      std::optional<std::uint64_t> volume_limit=std::nullopt)
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
    }
    std::error_code ec;const auto space=std::filesystem::space(parent,ec);
    const auto required=kReserve+(volume_file_?0:kStoreMaxPhysicalBytes);
    if(ec || space.available==static_cast<std::uintmax_t>(-1) || space.available<required)
      fail("insufficient verified disk reserve");
    handle_=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,reopen?OPEN_EXISTING:CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
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
        if(attr.FileAttributes&(FILE_ATTRIBUTE_SPARSE_FILE|FILE_ATTRIBUTE_COMPRESSED|FILE_ATTRIBUTE_ENCRYPTED))
          fail("volume-file storage requires ordinary allocation and caching semantics");
        require_opened_path(handle_,p);
        validate_volume_storage(handle_,c);
      }
      extent_=admitted_extent_=static_cast<std::uint64_t>(size.QuadPart);
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
  StoreIoContract contract() const noexcept override { return contract_; }
  std::uint64_t extent() const override { std::lock_guard lock(mutex_);refresh_extent();return extent_; }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    std::lock_guard lock(mutex_);
    refresh_extent();
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
#endif
    std::size_t done=0;
    while(done<bytes.size()) {
#ifdef _WIN32
      LARGE_INTEGER pos{};pos.QuadPart=static_cast<LONGLONG>(at+done);
      if(!SetFilePointerEx(handle_,pos,nullptr,FILE_BEGIN)) fail("seek failed");
      DWORD n=0;
      const auto request=static_cast<DWORD>((std::min)(bytes.size()-done,std::size_t{0x7ffff000}));
      if(!WriteFile(handle_,bytes.data()+done,request,&n,nullptr) || !n) fail("short/failed backing write");
#else
      const auto n=::pwrite(fd_,bytes.data()+done,bytes.size()-done,static_cast<off_t>(at+done));
      if(n<0 && errno==EINTR) continue;
      if(n<=0) fail("short/failed backing write");
#endif
      done+=static_cast<std::size_t>(n);
      extent_=(std::max)(extent_,at+done);
    }
  }
  void admit_write(std::uint64_t projected) override {
    std::lock_guard lock(mutex_);
    refresh_extent();
    if(projected<extent_ || projected>capacity_)fail("projected extent outside budget");
    memory_headroom();
    const auto needed=projected-extent_+kReserve;
#ifdef _WIN32
    std::error_code ec;const auto free=std::filesystem::space(parent_path_,ec);
    if(ec || free.available==static_cast<std::uintmax_t>(-1) || free.available<needed)
      fail("insufficient verified disk reserve");
    admitted_extent_=projected;
#else
    struct statvfs free{};
    if(::fstatvfs(parent_,&free)!=0 || !free.f_frsize ||
        free.f_bavail<(needed+free.f_frsize-1)/free.f_frsize) fail("insufficient verified disk reserve");
#endif
  }
  void flush() override {
    std::lock_guard lock(mutex_);
#ifdef _WIN32
    if(!FlushFileBuffers(handle_)) fail("file flush failed");
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
std::shared_ptr<StoreByteIo> new_volume_file(const std::filesystem::path& path,StoreIoContract c,std::uint64_t limit) {
  return std::make_shared<Output>(path,c,false,limit);
}
std::shared_ptr<StoreByteIo> reopen_volume_file(const std::filesystem::path& path,StoreIoContract c,std::uint64_t limit) {
  return std::make_shared<Output>(path,c,true,limit);
}
#endif
} // namespace flitzfs::detail
