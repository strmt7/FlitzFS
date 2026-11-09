#include "store_file.hpp"
#include "flitzfs/read_only_file.hpp"
#include "flitzfs/windows_path.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace flitzfs::detail {
namespace {
[[noreturn]] void fail(const char* message) { throw std::runtime_error(std::string("FlitzFS store file: ")+message); }
constexpr std::uint64_t kReserve=1024ULL*1024*1024;
constexpr std::uint64_t kMemory=608ULL*1024*1024;
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
  if(p.empty() || p.filename().empty() || p==p.root_path()) fail("specific new ordinary file required");
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
class Input final:public StoreByteIo {
  mutable std::mutex mutex_;
  ReadOnlyFile file_;
 public:
  explicit Input(const std::filesystem::path& p):file_(ReadOnlyFile::open(p)) {
    if(file_.size()>kStoreMaxPhysicalBytes) fail("physical input cap");
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
  std::uint64_t extent_=0;
  std::filesystem::path parent_path_;
#ifdef _WIN32
  HANDLE handle_=INVALID_HANDLE_VALUE;
#else
  int fd_=-1,parent_=-1;
  bool parent_synced_=false;
#endif
 public:
  Output(const std::filesystem::path& p,StoreIoContract c):contract_(c) {
    path_policy(p);memory_headroom();
    const auto parent=p.parent_path().empty() ? std::filesystem::path(".") : p.parent_path();
    parent_path_=parent;
#ifdef _WIN32
    std::error_code ec;const auto space=std::filesystem::space(parent,ec);
    if(ec || space.available==static_cast<std::uintmax_t>(-1) ||
        space.available<kStoreMaxPhysicalBytes+kReserve) fail("insufficient verified disk reserve");
    handle_=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle_==INVALID_HANDLE_VALUE) fail("exclusive creation failed");
    FILE_ATTRIBUTE_TAG_INFO attr{};
    if(GetFileType(handle_)!=FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle_,FileAttributeTagInfo,&attr,sizeof(attr)) ||
        (attr.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) {
      CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;fail("output is not an ordinary file");
    }
#else
    parent_=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(parent_<0) fail("cannot pin ordinary parent directory");
    struct statvfs space{};
    if(::fstatvfs(parent_,&space)!=0 || !space.f_frsize ||
        space.f_bavail<(kStoreMaxPhysicalBytes+kReserve+space.f_frsize-1)/space.f_frsize) {
      ::close(parent_);parent_=-1;fail("insufficient verified disk reserve");
    }
    fd_=::openat(parent_,p.filename().c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd_<0) {::close(parent_);parent_=-1;fail("exclusive creation failed");}
    struct stat st{};
    if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size!=0) {
      ::close(fd_);::close(parent_);fd_=parent_=-1;fail("output is not an empty regular file");
    }
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
  std::uint64_t extent() const override { std::lock_guard lock(mutex_);return extent_; }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    std::lock_guard lock(mutex_);
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
    if(at>kStoreMaxPhysicalBytes || bytes.size()>kStoreMaxPhysicalBytes-at) fail("physical output budget");
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
    if(projected<extent_ || projected>kStoreMaxPhysicalBytes) fail("projected extent outside budget");
    memory_headroom();
    const auto needed=projected-extent_+kReserve;
#ifdef _WIN32
    std::error_code ec;const auto free=std::filesystem::space(parent_path_,ec);
    if(ec || free.available==static_cast<std::uintmax_t>(-1) || free.available<needed)
      fail("insufficient verified disk reserve");
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
std::shared_ptr<StoreByteIo> read_store_file(const std::filesystem::path& path) {
  return std::make_shared<Input>(path);
}
} // namespace flitzfs::detail
