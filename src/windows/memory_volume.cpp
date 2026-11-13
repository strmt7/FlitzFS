#include "memory_volume.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace flitzfs {
namespace {
std::atomic<bool> domain_claimed{false},cleanup_ok{true};
constexpr std::uint64_t mib=1024ULL*1024;
void require(bool value,const char* message) {
  if(!value)throw std::runtime_error(message);
}
void headroom(std::uint64_t requested) {
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  require(GlobalMemoryStatusEx(&m)!=FALSE,"Memory headroom unavailable");
  // Proportional admission plus an explicit experimental reserve. These are
  // provider qualification limits, never inferred host specifications.
  const auto reserve=(std::max)(768*mib,m.ullTotalPhys/8);
  require(requested<=m.ullAvailPhys && m.ullAvailPhys-requested>=reserve &&
      requested<=m.ullAvailPageFile && m.ullAvailPageFile-requested>=reserve,
      "Insufficient physical/commit headroom");
}
}
struct MemoryVolumeDomain::State {
  std::uint64_t capacity=0,allocated=0,peak=0,stores=0;
  std::array<Store*,64> registry{};
  SIZE_T old_min=0,old_max=0;DWORD old_flags=0;std::size_t page=0;
  bool working_set_changed=false;
  explicit State(std::uint64_t cap):capacity(cap) {
    headroom(cap);
    SYSTEM_INFO info{};GetSystemInfo(&info);page=info.dwPageSize;
    require(page && !(page&(page-1)) && cap%page==0,"Memory capacity alignment");
    require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&old_min,&old_max,&old_flags)!=FALSE,
        "Working-set query failed");
    const auto minimum=(std::max)(old_min,static_cast<SIZE_T>(cap+8*mib));
    const auto maximum=(std::max)(old_max,minimum+48*mib);
    require(SetProcessWorkingSetSizeEx(GetCurrentProcess(),minimum,maximum,old_flags)!=FALSE,
        "Memory locking admission denied");
    working_set_changed=true;
  }
  ~State() {
    if(allocated||stores)cleanup_ok=false;
    if(working_set_changed &&
        !SetProcessWorkingSetSizeEx(GetCurrentProcess(),old_min,old_max,old_flags))cleanup_ok=false;
    // A failed cleanup permanently refuses a new provider in this process.
    domain_claimed.store(false,std::memory_order_release);
  }
};
class MemoryVolumeDomain::Store final:public StoreByteIo {
 public:
  Store(std::shared_ptr<State> state,std::uint64_t capacity):state_(std::move(state)),capacity_(capacity) {
    require(state_->stores<state_->registry.size(),"Memory store count cap");
    require(capacity && capacity%state_->page==0 && capacity<=state_->capacity &&
        state_->allocated<=state_->capacity-capacity,"Memory allocation capacity exceeded");
    headroom(capacity);
    data_=static_cast<std::byte*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(capacity),
        MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    require(data_!=nullptr,"Memory reservation failed");
    state_->allocated+=capacity;state_->peak=(std::max)(state_->peak,state_->allocated);
    ++state_->stores;
    for(auto& entry:state_->registry)if(!entry){entry=this;break;}
    if(!VirtualLock(data_,static_cast<SIZE_T>(capacity))) {
      release();throw std::runtime_error("Memory locking failed; no pageable fallback");
    }
    locked_=true;
    try{verify();}catch(...){release();throw;}
  }
  ~Store() override {release();}
  std::uint64_t extent() const override {return extent_;}
  // Writes are aligned-independent byte copies in one serial control thread.
  // Successful flush preserves bytes through later calls while this process
  // owns the domain. It promises NO survival of process exit or power loss.
  StoreIoContract contract() const noexcept override {return {65536,true};}
  void admit_write(std::uint64_t projected) override {
    require(cleanup_ok.load() && projected<=capacity_,"Memory write admission denied");
    headroom(0);
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    require(at<=extent_ && out.size()<=extent_-at,"Memory read bounds");
    if(!out.empty())std::memcpy(out.data(),data_+at,out.size());
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    require(at<=capacity_ && in.size()<=capacity_-at,"Memory write bounds");
    if(in.empty())return; // A zero-byte operation must never extend the image.
    std::memcpy(data_+at,in.data(),in.size());extent_=(std::max)(extent_,at+in.size());
  }
  void flush() override {
    // No stable mirror and no O(image-size) copy on every transaction.
    std::atomic_thread_fence(std::memory_order_seq_cst);
  }
  void verify() const {
    const auto count=static_cast<std::size_t>(capacity_/state_->page);
    require(count*sizeof(PSAPI_WORKING_SET_EX_INFORMATION)<=1024*1024,"Residency query cap");
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(count);
    for(std::size_t i=0;i<count;++i)pages[i].VirtualAddress=data_+i*state_->page;
    require(QueryWorkingSetEx(GetCurrentProcess(),pages.data(),
        static_cast<DWORD>(pages.size()*sizeof(pages[0])))!=FALSE,"Residency query failed");
    for(const auto& p:pages)require(p.VirtualAttributes.Valid && p.VirtualAttributes.Locked &&
        !p.VirtualAttributes.Bad,"Locked residency not established");
  }
 private:
  void release() noexcept {
    if(!data_)return;
    SecureZeroMemory(data_,static_cast<SIZE_T>(capacity_));
    if(locked_ && !VirtualUnlock(data_,static_cast<SIZE_T>(capacity_)))cleanup_ok=false;
    if(VirtualFree(data_,0,MEM_RELEASE))state_->allocated-=capacity_;
    else cleanup_ok=false; // Retain accounting; never recycle unconfirmed bytes.
    for(auto& entry:state_->registry)if(entry==this){entry=nullptr;break;}
    --state_->stores;data_=nullptr;locked_=false;
  }
  std::shared_ptr<State> state_;std::byte* data_=nullptr;
  std::uint64_t capacity_=0,extent_=0;bool locked_=false;
};
MemoryVolumeDomain::MemoryVolumeDomain(std::shared_ptr<State> state) noexcept:state_(std::move(state)) {}
MemoryVolumeDomain::~MemoryVolumeDomain()=default;
MemoryVolumeDomain::MemoryVolumeDomain(MemoryVolumeDomain&&) noexcept=default;
MemoryVolumeDomain MemoryVolumeDomain::create(std::uint64_t cap) {
  require(cap && cap<=maximum_capacity,"Experimental domain capacity exceeded");
  require(cleanup_ok.load(),"Prior memory cleanup failed");
  bool expected=false;
  require(domain_claimed.compare_exchange_strong(expected,true),"Memory domain already owned");
  try{return MemoryVolumeDomain(std::make_shared<State>(cap));}
  catch(...){domain_claimed=false;throw;}
}
std::shared_ptr<StoreByteIo> MemoryVolumeDomain::create_store(std::uint64_t cap) {
  require(state_ && cleanup_ok.load(),"Memory domain unavailable");
  return std::make_shared<Store>(state_,cap);
}
MemoryVolumeUsage MemoryVolumeDomain::usage() const noexcept {
  if(!state_)return {};
  return {state_->capacity,state_->allocated,state_->peak,state_->stores};
}
void MemoryVolumeDomain::verify_resident() const {
  require(state_ && cleanup_ok.load(),"Memory domain unavailable");
  // Retain a bounded registry, not a second image or a dangling owner. Queries
  // are explicit controller checks, never a full-image copy on every flush.
  SIZE_T minimum=0,maximum=0;DWORD flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&minimum,&maximum,&flags)!=FALSE &&
      minimum>=state_->capacity+8*mib,"Working-set reservation changed");
  std::uint64_t observed=0;
  for(const auto* store:state_->registry)if(store){store->verify();++observed;}
  require(observed==state_->stores,"Memory store ownership accounting differs");
}
bool MemoryVolumeDomain::cleanup_healthy() noexcept {return cleanup_ok.load();}
} // namespace flitzfs
