#include "worker_exchange.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace flitzfs {
namespace {
constexpr ULONG request_magic=0x51575846, reply_magic=0x52575846;
constexpr LONG pending=-1, writing=-2;
constexpr std::size_t guard_bytes=16;
struct Control { ULONG magic, capacity; LONG published; ULONG reserved; };
static_assert(sizeof(Control)==16 && offsetof(Control,published)==8);
struct Handle {
  HANDLE value=nullptr;
  explicit Handle(HANDLE h=nullptr):value(h) {}
  ~Handle() {if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
  Handle(const Handle&)=delete;
  Handle& operator=(const Handle&)=delete;
  Handle(Handle&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
};
struct View {
  void* value=nullptr;
  explicit View(void* p=nullptr):value(p) {}
  ~View() {if(value) UnmapViewOfFile(value);}
  View(const View&)=delete;
  View& operator=(const View&)=delete;
  View(View&& other) noexcept:value(std::exchange(other.value,nullptr)) {}
};
struct Local { void* value=nullptr;~Local(){if(value)LocalFree(value);} };
[[noreturn]] void failed(const char* operation,DWORD error=GetLastError()) {
  throw std::system_error(static_cast<int>(error),std::system_category(),operation);
}
void require(bool ok,const char* why) {if(!ok)throw std::invalid_argument(why);}
bool valid_token(std::wstring_view token) noexcept {
  return token.size()==32 && std::all_of(token.begin(),token.end(),[](wchar_t c){
    return (c>=L'0' && c<=L'9') || (c>=L'a' && c<=L'f');});
}
std::wstring object_name(std::wstring_view token,bool response) {
  require(valid_token(token),"invalid worker channel token");
  return L"Local\\FlitzFS.worker."+std::wstring(token)+(response?L".reply":L".request");
}
std::wstring random_token() {
  std::array<UCHAR,16> random{};
  if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
    throw std::runtime_error("worker channel randomness unavailable");
  constexpr wchar_t hex[]=L"0123456789abcdef";
  std::wstring token;token.reserve(32);
  for(auto b:random){token+=hex[b>>4];token+=hex[b&15];}
  return token;
}
void owner_security(Local& descriptor) {
  Handle token;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))failed("worker identity query");
  DWORD length=0;
  GetTokenInformation(token.value,TokenUser,nullptr,0,&length);
  if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || !length || length>16384)
    throw std::runtime_error("invalid worker identity size");
  std::vector<std::byte> storage(length);
  if(!GetTokenInformation(token.value,TokenUser,storage.data(),length,&length))failed("worker identity read");
  LPWSTR sid_text=nullptr;
  if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid_text))failed("worker SID encoding");
  Local sid{sid_text};
  const std::wstring identity(sid_text);
  const auto sddl=L"O:"+identity+L"D:P(A;;GA;;;"+identity+L")";
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor.value,&length) ||
      length>4096 || !IsValidSecurityDescriptor(descriptor.value))failed("worker mapping ACL");
}
struct Mapping {
  Handle object;
  View view;
  std::size_t capacity;
  Mapping(Handle h,DWORD access,std::size_t count):object(std::move(h)),
      view(MapViewOfFile(object.value,access,0,0,sizeof(Control)+count+guard_bytes)),capacity(count) {
    if(!view.value)failed("worker mapping view");
  }
  Control* control() const noexcept {return static_cast<Control*>(view.value);}
  std::byte* bytes() const noexcept {return static_cast<std::byte*>(view.value)+sizeof(Control);}
  bool intact(ULONG magic) const noexcept {
    const auto* c=control();
    return c->magic==magic && c->capacity==capacity && !c->reserved &&
        std::all_of(bytes()+capacity,bytes()+capacity+guard_bytes,[](auto b){return b==std::byte{0xa7};});
  }
  static Mapping create(const std::wstring& name,std::size_t count,ULONG magic,PSECURITY_DESCRIPTOR acl) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),acl,FALSE};
    SetLastError(ERROR_SUCCESS);
    Handle h(CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,
        static_cast<DWORD>(sizeof(Control)+count+guard_bytes),name.c_str()));
    const auto error=GetLastError();
    if(!h.value)failed("worker mapping creation",error);
    require(error!=ERROR_ALREADY_EXISTS,"worker mapping name collision");
    Mapping mapping(std::move(h),FILE_MAP_WRITE,count);
    *mapping.control()={magic,static_cast<ULONG>(count),pending,0};
    std::fill_n(mapping.bytes()+count,guard_bytes,std::byte{0xa7});return mapping;
  }
  static Mapping open(const std::wstring& name,std::size_t count,DWORD access) {
    Handle h(OpenFileMappingW(access,FALSE,name.c_str()));
    if(!h.value)failed("worker mapping open");
    return Mapping(std::move(h),access,count);
  }
};
void headroom(ProcessBudget budget) {
  require(valid_process_budget(budget),"invalid worker process budget");
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))failed("worker memory headroom");
  const auto needed=static_cast<ULONGLONG>(budget.memory_mib)*1024*1024+
      512ULL*1024*1024+kWorkerRequestLimit+kWorkerReplyLimit+2*(sizeof(Control)+guard_bytes);
  if(memory.ullAvailPhys<needed || memory.ullAvailPageFile<needed)
    throw std::runtime_error("insufficient worker channel memory headroom");
}
}
WorkerExchangeResult run_worker_exchange(const std::filesystem::path& helper,
    const std::vector<std::wstring>& arguments,std::span<const std::byte> request,ProcessBudget budget) {
  require(request.size()>=kWorkerHeaderBytes && request.size()<=kWorkerRequestLimit &&
      arguments.size()<=14,"invalid worker request or argument bound");
  static std::mutex in_flight;
  std::unique_lock active(in_flight,std::try_to_lock);
  if(!active.owns_lock())throw std::runtime_error("a worker exchange is already active");
  headroom(budget);
  const std::vector<std::byte> owned_request(request.begin(),request.end());
  request=owned_request;
  const auto token=random_token();auto command=arguments;
  command.push_back(L"--worker-channel");command.push_back(token);
  (void)windows_child_command(helper.native(),command); // Validate before mappings or launch.
  Local acl;owner_security(acl);
  auto input=Mapping::create(object_name(token,false),kWorkerRequestLimit,request_magic,acl.value);
  auto output=Mapping::create(object_name(token,true),kWorkerReplyLimit,reply_magic,acl.value);
  std::memcpy(input.bytes(),request.data(),request.size());
  input.control()->published=static_cast<LONG>(request.size());
  MemoryBarrier();
  WorkerExchangeResult result;
  result.process=run_bounded_process(helper,command,budget);
  if(!result.process.exit_confirmed) {result.code=WorkerExchangeCode::exit_unconfirmed;return result;}
  if(result.process.timed_out) {result.code=WorkerExchangeCode::timed_out;return result;}
  if(result.process.exit_code) {result.code=WorkerExchangeCode::worker_failed;return result;}
  // Wait on child exit is the synchronization boundary; there is no live helper
  // able to mutate this reply. No success is inferred from shared flags alone.
  MemoryBarrier();
  const LONG length=output.control()->published;
  if(!input.intact(request_magic) || !output.intact(reply_magic) ||
      input.control()->published!=static_cast<LONG>(request.size()) ||
      std::memcmp(input.bytes(),request.data(),request.size()) ||
      length<static_cast<LONG>(kWorkerHeaderBytes) || length>static_cast<LONG>(kWorkerReplyLimit))return result;
  result.response.assign(output.bytes(),output.bytes()+length);
  result.code=WorkerExchangeCode::completed;return result;
}
struct WorkerChannel::Impl {
  Mapping input,output;
  std::vector<std::byte> owned;
  explicit Impl(std::wstring_view token):
      input(Mapping::open(object_name(token,false),kWorkerRequestLimit,FILE_MAP_READ)),
      output(Mapping::open(object_name(token,true),kWorkerReplyLimit,FILE_MAP_WRITE)) {
    const LONG length=input.control()->published;
    require(input.intact(request_magic) && output.intact(reply_magic) && output.control()->published==pending &&
        length>=static_cast<LONG>(kWorkerHeaderBytes) && length<=static_cast<LONG>(kWorkerRequestLimit),
        "invalid worker channel framing");
    owned.assign(input.bytes(),input.bytes()+length);
  }
};
WorkerChannel::WorkerChannel(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)) {}
WorkerChannel::~WorkerChannel()=default;
WorkerChannel::WorkerChannel(WorkerChannel&&) noexcept=default;
WorkerChannel& WorkerChannel::operator=(WorkerChannel&&) noexcept=default;
WorkerChannel WorkerChannel::open(std::wstring_view token) {
  require(valid_token(token),"invalid worker channel token");
  return WorkerChannel(std::make_unique<Impl>(token));
}
std::span<const std::byte> WorkerChannel::request() const noexcept {
  return impl_?std::span<const std::byte>(impl_->owned):std::span<const std::byte>{};
}
void WorkerChannel::publish(std::span<const std::byte> response) {
  require(impl_ && response.size()>=kWorkerHeaderBytes && response.size()<=kWorkerReplyLimit,
      "invalid worker response bound");
  auto& output=impl_->output;
  require(output.intact(reply_magic),"worker response mapping changed");
  require(InterlockedCompareExchange(&output.control()->published,writing,pending)==pending,
      "worker response already published");
  std::memcpy(output.bytes(),response.data(),response.size());
  InterlockedExchange(&output.control()->published,static_cast<LONG>(response.size()));
}
} // namespace flitzfs
