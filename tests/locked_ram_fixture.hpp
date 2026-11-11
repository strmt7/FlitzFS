#pragma once
#include "flitzfs/object_store.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace locked_ram_fixture {
inline std::atomic<std::size_t> locked_bytes{0},peak_locked_bytes{0};
inline std::atomic<bool> cleanup_ok{true};
inline constexpr std::size_t aggregate_cap=40U*1024*1024;
inline void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
inline void wincheck(BOOL value,const char* message){
  if(!value){const auto code=GetLastError();throw std::runtime_error(std::string(message)+" Win32="+std::to_string(code));}
}
inline void headroom() {
  MEMORYSTATUSEX m{};m.dwLength=sizeof(m);
  wincheck(GlobalMemoryStatusEx(&m),"unknown RAM/commit headroom");
  constexpr auto reserve=2ULL*1024*1024*1024;
  check(m.ullAvailPhys>=reserve+aggregate_cap&&m.ullAvailPageFile>=reserve+aggregate_cap,
      "insufficient RAM/commit reserve");
}
class WorkingSet final {
 public:
  WorkingSet() {
    headroom();
    wincheck(GetProcessWorkingSetSizeEx(GetCurrentProcess(),&minimum_,&maximum_,&flags_),"working-set query");
    wincheck(SetProcessWorkingSetSizeEx(GetCurrentProcess(),48U*1024*1024,96U*1024*1024,0),
        "working-set admission denied; no elevation or fallback");
    active_=true;
  }
  void restore() {
    if(active_){wincheck(SetProcessWorkingSetSizeEx(GetCurrentProcess(),minimum_,maximum_,flags_),
        "working-set restore");active_=false;}
  }
  ~WorkingSet(){if(active_&&!SetProcessWorkingSetSizeEx(GetCurrentProcess(),minimum_,maximum_,flags_))cleanup_ok=false;}
  WorkingSet(const WorkingSet&)=delete;WorkingSet& operator=(const WorkingSet&)=delete;
 private:
  SIZE_T minimum_=0,maximum_=0;DWORD flags_=0;bool active_=false;
};
class Block final {
 public:
  explicit Block(std::size_t bytes):bytes_(bytes) {
    headroom();
    SYSTEM_INFO info{};GetSystemInfo(&info);page_=info.dwPageSize;
    check(bytes&&page_&&!(page_&(page_-1))&&bytes%page_==0,"locked allocation alignment");
    const auto prior=locked_bytes.fetch_add(bytes);
    if(prior>aggregate_cap||bytes>aggregate_cap-prior){locked_bytes.fetch_sub(bytes);throw std::runtime_error("aggregate locked RAM cap");}
    auto peak=peak_locked_bytes.load();while(peak<prior+bytes&&!peak_locked_bytes.compare_exchange_weak(peak,prior+bytes)){}
    value_=VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!value_){locked_bytes.fetch_sub(bytes);throw std::runtime_error("locked RAM reservation");}
    if(!VirtualLock(value_,bytes)){
      const auto error=GetLastError();
      if(VirtualFree(value_,0,MEM_RELEASE))locked_bytes.fetch_sub(bytes);else cleanup_ok=false;
      value_=nullptr;throw std::runtime_error("VirtualLock denied; no fallback Win32="+std::to_string(error));
    }
    try{verify();}catch(...){release();throw;}
  }
  ~Block(){release();}
  Block(const Block&)=delete;Block& operator=(const Block&)=delete;
  std::byte* data() noexcept{return static_cast<std::byte*>(value_);}
  const std::byte* data() const noexcept{return static_cast<const std::byte*>(value_);}
  void verify() const {
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(bytes_/page_);
    for(std::size_t i=0;i<pages.size();++i)pages[i].VirtualAddress=static_cast<std::byte*>(value_)+i*page_;
    check(pages.size()*sizeof(pages[0])<=1024U*1024,"residency telemetry cap");
    wincheck(QueryWorkingSetEx(GetCurrentProcess(),pages.data(),static_cast<DWORD>(pages.size()*sizeof(pages[0]))),
        "locked RAM residency query");
    for(const auto& p:pages)check(p.VirtualAttributes.Valid&&p.VirtualAttributes.Locked&&!p.VirtualAttributes.Bad,
        "page residency/locking not proven");
  }
 private:
  void release() noexcept {
    if(!value_)return;
    SecureZeroMemory(value_,bytes_);
    if(!VirtualUnlock(value_,bytes_))cleanup_ok=false;
    if(VirtualFree(value_,0,MEM_RELEASE))locked_bytes.fetch_sub(bytes_);else cleanup_ok=false;
    value_=nullptr;
  }
  void* value_=nullptr;std::size_t bytes_=0,page_=0;
};
// A VOLATILE test persistence model: stable means a RAM checkpoint while the
// owning process is alive, never physical power-loss durability. All authoritative
// live/stable image bytes are locked before input is copied into either buffer.
class Store final:public flitzfs::StoreByteIo {
 public:
  static std::size_t admitted(std::size_t n){check(n&&n<=8U*1024*1024,"single RAM store cap");return n;}
  explicit Store(std::size_t capacity):live_(admitted(capacity)),stable_(capacity),capacity_(capacity) {}
  std::uint64_t extent() const override{return extent_;}
  flitzfs::StoreIoContract contract() const noexcept override{return {65536,true};}
  void admit_write(std::uint64_t projected) override{headroom();check(projected<=capacity_,"RAM image capacity");}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    check(at<=extent_&&out.size()<=extent_-at,"RAM read bounds");
    if(!out.empty())std::memcpy(out.data(),live_.data()+at,out.size());
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    check(at<=capacity_&&in.size()<=capacity_-at,"RAM write bounds");
    if(!in.empty())std::memcpy(live_.data()+at,in.data(),in.size());
    extent_=(std::max)(extent_,at+in.size());
  }
  void flush() override {
    if(extent_)std::memcpy(stable_.data(),live_.data(),static_cast<std::size_t>(extent_));
    stable_extent_=extent_;
  }
  void verify() const{live_.verify();stable_.verify();}
  void crash_to_checkpoint() {
    SecureZeroMemory(live_.data(),capacity_);
    if(stable_extent_)std::memcpy(live_.data(),stable_.data(),static_cast<std::size_t>(stable_extent_));
    extent_=stable_extent_;
  }
 private:
  Block live_,stable_;std::size_t capacity_=0;std::uint64_t extent_=0,stable_extent_=0;
};
}
