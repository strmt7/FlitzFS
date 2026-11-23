#include "memory_volume.hpp"
#include "flitzfs/native_hybrid_backing.hpp"
#include "maintenance_backing.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace flitzfs {
namespace {
std::atomic<bool> domain_claimed{false},cleanup_ok{true};
constexpr std::uint64_t mib=1024ULL*1024;
constexpr std::uint64_t lock_overhead=8*mib, working_set_slack=48*mib;
void require(bool value,const char* message) {
  if(!value)throw std::runtime_error(message);
}
void check_headroom(std::uint64_t requested) {
  require(requested <= usable_memory_volume_headroom(observe_memory_volume_headroom()),
      "Insufficient physical/commit headroom");
}
}
MemoryVolumeObservation observe_memory_volume_headroom() {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  require(GlobalMemoryStatusEx(&m) != FALSE, "Memory headroom unavailable");
  return {m.ullTotalPhys,m.ullAvailPhys,m.ullTotalPageFile,m.ullAvailPageFile};
}
std::uint64_t usable_memory_volume_headroom(const MemoryVolumeObservation& m) {
  require(m.physical_total && m.physical_available <= m.physical_total && m.commit_total &&
      m.commit_available <= m.commit_total, "Inconsistent memory headroom");
  const auto reserve = (std::max)(768*mib, m.physical_total/8);
  const auto available = (std::min)(m.physical_available, m.commit_available);
  require(available >= reserve, "Insufficient physical/commit headroom");
  return available - reserve;
}
struct MemoryVolumeDomain::State {
  std::uint64_t capacity=0,reserved=0,resident=0,stores=0,lock_budget=0;
  std::array<Store*,64> registry{};
  SIZE_T old_min=0,old_max=0;DWORD old_flags=0;std::size_t page=0;
  std::atomic<SIZE_T> minimum{0}; // The WinFsp controller checks this outside the callback lock.
  bool working_set_changed=false,read_only=false;
  explicit State(std::uint64_t cap):capacity(cap) {
    check_headroom(0);
    SYSTEM_INFO info{};GetSystemInfo(&info);page=info.dwPageSize;
    require(valid_store_io_contract({static_cast<std::uint32_t>(page),true}) && cap%page==0,
        "Unsupported memory page size or capacity alignment");
    require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&old_min,&old_max,&old_flags)!=FALSE,
        "Working-set query failed");
    minimum.store(old_min, std::memory_order_relaxed);
  }
  void admit_lock_growth(std::uint64_t added) {
    const auto available=usable_memory_volume_headroom(observe_memory_volume_headroom());
    require(added<=available,"Insufficient physical/commit headroom");
    const auto required=resident+added;
    if(required<=lock_budget)return;
    // Quota grows in steps within current headroom. Reserve pages only on use.
    const auto doubled=lock_budget+(std::min)(lock_budget,capacity-lock_budget);
    const auto maximum=resident+(std::min)(available,capacity-resident);
    const auto budget=(std::max)(required,(std::min)(doubled,maximum));
    const auto next_min = static_cast<SIZE_T>(budget + lock_overhead);
    if (next_min > minimum.load(std::memory_order_relaxed)) {
      const auto next_max = (std::max)(old_max, next_min + static_cast<SIZE_T>(working_set_slack));
      require(SetProcessWorkingSetSizeEx(GetCurrentProcess(), next_min, next_max, old_flags) != FALSE,
          "Memory locking admission denied");
      working_set_changed = true;
      minimum.store(next_min, std::memory_order_release);
    }
    lock_budget = budget;
  }
  ~State() {
    if(reserved||resident||stores)cleanup_ok=false;
    if(working_set_changed &&
        !SetProcessWorkingSetSizeEx(GetCurrentProcess(),old_min,old_max,old_flags))cleanup_ok=false;
    // A failed cleanup permanently refuses a new provider in this process.
    domain_claimed.store(false,std::memory_order_release);
  }
};
class MemoryVolumeDomain::Store final:public StoreByteIo {
 public:
  Store(std::shared_ptr<State> state,std::uint64_t capacity,std::uint32_t minimum_unit):
      state_(std::move(state)),capacity_(capacity),
      unit_((std::max)(minimum_unit,static_cast<std::uint32_t>(state_->page))) {
    require(!minimum_unit || valid_store_io_contract({minimum_unit,true}),"Invalid memory isolation unit");
    require(state_->stores<state_->registry.size(),"Memory store count cap");
    require(capacity && capacity%state_->page==0 && capacity<=state_->capacity &&
        state_->reserved<=state_->capacity-capacity,"Memory allocation capacity exceeded");
    data_=static_cast<std::byte*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(capacity),MEM_RESERVE,PAGE_NOACCESS));
    if(!data_)throw std::runtime_error("Memory reservation failed; Windows status "+std::to_string(GetLastError()));
    state_->reserved+=capacity;
    ++state_->stores;
    for(auto& entry:state_->registry)if(!entry){entry=this;break;}
  }
  ~Store() override {release();}
  std::uint64_t extent() const override {return extent_.load(std::memory_order_acquire);}
  bool concurrent_reads_supported() const noexcept override {return true;}
  // Admission is checked before copying, and each copy targets only its
  // supplied byte span. Size the isolation units from the actual Windows
  // pages. A flush preserves bytes for this live, serialized memory provider.
  StoreIoContract contract() const noexcept override {
    return {unit_,true};
  }
  void admit_write(std::uint64_t projected) override {
    require(allows_writer() && projected<=capacity_,"Memory write admission denied");
    detail::MaintenanceBackingCall observed(detail::MaintenanceBackingOperation::control);
    if(projected<=committed_)check_headroom(0);
    else grow(projected);
    observed.complete();
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    const auto end=extent();
    require(at<=end && out.size()<=end-at,"Memory read bounds");
    if(out.empty())return;
    detail::MaintenanceBackingCall observed(detail::MaintenanceBackingOperation::read,out.size());
    std::lock_guard lock(bytes_mutex_);
    std::memcpy(out.data(),data_+at,out.size());
    observed.complete();
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    require(allows_writer(),"Memory store is not writable");
    require(at<=capacity_ && in.size()<=capacity_-at,"Memory write bounds");
    if(in.empty())return; // A zero-byte operation must never extend the image.
    detail::MaintenanceBackingCall observed(detail::MaintenanceBackingOperation::write,in.size());
    const auto end = at + in.size();
    if (end > committed_) grow(end);
    // Page admission above only extends the reservation. Serialize the byte
    // copy itself with capture readers of the previously published prefix.
    std::lock_guard lock(bytes_mutex_);
    std::memcpy(data_+at,in.data(),in.size());
    extent_.store((std::max)(extent_.load(std::memory_order_relaxed),end),std::memory_order_release);
    observed.complete();
  }
  void flush() override {
    require(allows_writer(),"Memory store is not writable");
    // No stable mirror and no O(image-size) copy on every transaction.
    detail::MaintenanceBackingCall observed(detail::MaintenanceBackingOperation::flush);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    observed.complete();
  }
  bool writer_in_use() const noexcept {return has_writer();}
  bool retire_step() {
    const auto bytes=(std::min)(committed_,std::uint64_t{kStoreMaxObjectBytes});
    if(!bytes)return true;
    detail::MaintenanceBackingCall observed(detail::MaintenanceBackingOperation::control);
    auto* const start=data_+committed_-bytes;
    SecureZeroMemory(start,static_cast<SIZE_T>(bytes));
    if(!VirtualUnlock(start,static_cast<SIZE_T>(bytes)) || !VirtualFree(start,static_cast<SIZE_T>(bytes),MEM_DECOMMIT)) {
      cleanup_ok=false;
      throw std::runtime_error("Memory image decommit failed");
    }
    committed_-=bytes;
    extent_.store((std::min)(extent(),committed_),std::memory_order_release);
    state_->resident-=bytes;
    observed.complete();
    return !committed_;
  }
  std::uint64_t verify() const {
    std::array<PSAPI_WORKING_SET_EX_INFORMATION,256> pages{};
    const auto count = static_cast<std::size_t>(committed_/state_->page);
    for (std::size_t first=0; first<count;) {
      const auto batch = (std::min)(pages.size(), count-first);
      for (std::size_t i=0; i<batch; ++i) pages[i].VirtualAddress=data_+(first+i)*state_->page;
      require(QueryWorkingSetEx(GetCurrentProcess(),pages.data(),
          static_cast<DWORD>(batch*sizeof(pages[0])))!=FALSE,"Residency query failed");
      for (const auto& p : std::span(pages).first(batch))
        require(p.VirtualAttributes.Valid && p.VirtualAttributes.Locked &&
            !p.VirtualAttributes.Bad,"Locked residency not established");
      first += batch;
    }
    return committed_;
  }
 private:
  bool allows_writer() const noexcept override {return !state_->read_only && cleanup_ok.load();}
  void grow(std::uint64_t end) {
    const auto page = state_->page;
    const auto next = ((end-1)/page+1)*page;
    const auto added = static_cast<SIZE_T>(next-committed_);
    auto* const start = data_+committed_;
    state_->admit_lock_growth(added);
    if (!VirtualAlloc(start, added, MEM_COMMIT, PAGE_READWRITE))
      throw std::runtime_error("Memory commit failed; Windows status "+std::to_string(GetLastError()));
    if (!VirtualLock(start, added)) {
      const auto error = GetLastError();
      if (!VirtualFree(start, added, MEM_DECOMMIT)) cleanup_ok=false;
      throw std::runtime_error("Memory locking failed; Windows status "+std::to_string(error));
    }
    committed_ = next;
    state_->resident += added;
  }
  void release() noexcept {
    if(!data_)return;
    if(const auto end=extent())SecureZeroMemory(data_,static_cast<SIZE_T>(end));
    if(committed_ && !VirtualUnlock(data_,static_cast<SIZE_T>(committed_)))cleanup_ok=false;
    if(VirtualFree(data_,0,MEM_RELEASE)) {
      state_->reserved-=capacity_;
      state_->resident-=committed_;
    } else cleanup_ok=false; // Retain accounting until release is confirmed.
    for(auto& entry:state_->registry)if(entry==this){entry=nullptr;break;}
    --state_->stores;data_=nullptr;
  }
  std::shared_ptr<State> state_;std::byte* data_=nullptr;
  std::uint64_t capacity_=0,committed_=0;
  const std::uint32_t unit_;
  std::atomic<std::uint64_t> extent_{0};
  mutable std::mutex bytes_mutex_;
};
MemoryVolumeDomain::MemoryVolumeDomain(std::shared_ptr<State> state) noexcept:state_(std::move(state)) {}
MemoryVolumeDomain::~MemoryVolumeDomain()=default;
MemoryVolumeDomain::MemoryVolumeDomain(MemoryVolumeDomain&&) noexcept=default;
MemoryVolumeDomain MemoryVolumeDomain::create(std::uint64_t cap) {
  require(cap && cap <= (std::numeric_limits<SIZE_T>::max)()-lock_overhead-working_set_slack,
      "Invalid memory domain capacity");
  require(cleanup_ok.load(),"Prior memory cleanup failed");
  bool expected=false;
  require(domain_claimed.compare_exchange_strong(expected,true),"Memory domain already owned");
  try{return MemoryVolumeDomain(std::make_shared<State>(cap));}
  catch(...){domain_claimed=false;throw;}
}
std::shared_ptr<StoreByteIo> MemoryVolumeDomain::create_store(std::uint64_t cap,std::uint32_t minimum_unit) {
  require(state_ && !state_->read_only && cleanup_ok.load(),"Memory domain unavailable for allocation");
  return std::make_shared<Store>(state_,cap,minimum_unit);
}
MemoryStoreRetirement MemoryVolumeDomain::retire_store(std::shared_ptr<StoreByteIo>& backing) {
  require(backing && owns(*backing) && !state_->read_only && cleanup_ok.load(),
      "Memory retirement requires a writable domain and its own store");
  auto& store=static_cast<Store&>(*backing);
  if(backing.use_count()!=1 || store.writer_in_use())return MemoryStoreRetirement::pinned;
  if(!store.retire_step())return MemoryStoreRetirement::advanced;
  backing.reset();
  return MemoryStoreRetirement::released;
}
MemoryVolumeUsage MemoryVolumeDomain::usage() const noexcept {
  if(!state_)return {};
  return {state_->capacity,state_->reserved,state_->resident,state_->stores};
}
void MemoryVolumeDomain::verify_working_set() const {
  require(state_ && cleanup_ok.load(),"Memory domain unavailable");
  const auto expected = state_->minimum.load(std::memory_order_acquire);
  SIZE_T minimum=0,maximum=0;DWORD flags=0;
  require(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&minimum,&maximum,&flags)!=FALSE &&
      minimum>=expected,"Working-set reservation changed");
}
void MemoryVolumeDomain::verify_resident() const {
  verify_working_set();
  std::uint64_t observed=0,resident=0;
  for (const auto* store : state_->registry) if (store) {
    resident += store->verify();
    ++observed;
  }
  require(observed==state_->stores && resident==state_->resident && resident<=state_->reserved,
      "Memory store ownership accounting differs");
}
bool MemoryVolumeDomain::owns(const StoreByteIo& candidate) const noexcept {
  return state_ && std::find(state_->registry.begin(),state_->registry.end(),&candidate)!=state_->registry.end();
}
bool MemoryVolumeDomain::owns_image(const StoreByteIo& candidate) const noexcept {
  if(owns(candidate))return true;
  const auto* hybrid=dynamic_cast<const NativeHybridBacking*>(&candidate);
  if(!hybrid)return false;
  const auto parts=hybrid->components();
  return owns(*parts[0]) && owns(*parts[1]);
}
void MemoryVolumeDomain::seal_read_only() {
  seal_read_only(nullptr,nullptr);
}
void MemoryVolumeDomain::seal_read_only(const NativeHybridBacking* active,const NativeHybridBacking* retained) {
  verify_resident();
  std::array<const StoreByteIo*,4> delegated{};
  std::size_t next=0;
  for(const auto* image:{active,retained})if(image) {
    require(owns_image(*image) && !image->has_writer(),"Memory image still has a writer or foreign backing");
    for(const auto& part:image->components())delegated[next++]=part.get();
  }
  for(const auto* store:state_->registry)
    require(!store || !store->writer_in_use() || std::ranges::find(delegated,store)!=delegated.end(),
        "Memory domain still has a writer");
  state_->read_only=true;
}
bool MemoryVolumeDomain::cleanup_healthy() noexcept {return cleanup_ok.load();}
} // namespace flitzfs
