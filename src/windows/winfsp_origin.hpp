#pragma once
#include <winfsp/winfsp.h>
#include <atomic>
namespace flitzfs {
// Only valid inside actual SDK Create/Open callbacks. Direct CPU fixtures
// explicitly inject a synthetic origin; a live provider supplies the SDK query.
using WinFspOriginQuery=decltype(&FspFileSystemOperationProcessId);
class WinFspOriginGuard final {
 public:
  // Configure only before callbacks can start; never rebind a live adapter.
  void bind(WinFspOriginQuery query) noexcept {
    query_=query;self_rejected_.store(false,std::memory_order_relaxed);
  }
  bool allows_open() noexcept {
    if(!query_||!self_)return false;
    try {
      const auto origin=query_();
      if(origin==self_){self_rejected_.store(true,std::memory_order_release);return false;}
      // Preserve kernel-origin zero. This flag is lifecycle admission, not
      // replacement authorization, caller authentication or driver recovery.
      return true;
    }catch(...){return false;}
  }
  bool self_rejected() const noexcept{return self_rejected_.load(std::memory_order_acquire);}
 private:
  const UINT32 self_=GetCurrentProcessId();
  WinFspOriginQuery query_=nullptr;
  std::atomic<bool> self_rejected_{false};
};
} // namespace flitzfs
