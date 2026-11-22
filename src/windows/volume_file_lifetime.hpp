#pragma once

#include <windows.h>
#include <atomic>
#include <system_error>

namespace flitzfs::detail {

// Demand I/O and independent captures share the lifetime of one opened file.
// Observations from this handle cannot authorize a replacement handle.
class VolumeFileLifetime final {
 public:
  bool current() const noexcept {return status_.load(std::memory_order_acquire)==ERROR_SUCCESS;}

  void check(DWORD result=ERROR_SUCCESS) const {
    switch(result) {
      case ERROR_DEV_NOT_EXIST:
      case ERROR_DEVICE_NOT_CONNECTED:
      case ERROR_FILE_INVALID:
      case ERROR_MEDIA_CHANGED:
      case ERROR_NO_MEDIA_IN_DRIVE: {
        DWORD expected=ERROR_SUCCESS;
        status_.compare_exchange_strong(expected,result,std::memory_order_acq_rel);
        break;
      }
      default:break;
    }
    const auto status=status_.load(std::memory_order_acquire);
    if(status!=ERROR_SUCCESS)
      throw std::system_error(static_cast<int>(status),std::system_category(),
          "FlitzFS backing volume disconnected or changed");
  }

 private:
  mutable std::atomic<DWORD> status_{ERROR_SUCCESS};
};

} // namespace flitzfs::detail
