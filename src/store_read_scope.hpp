#pragma once
#include <exception>
#include <functional>
#include <utility>

namespace flitzfs::detail {

class StoreCaptureInterrupted final : public std::exception {
 public:
  const char* what() const noexcept override {return "Store capture interrupted";}
};

// A capture owns this scope for one query on its private snapshot.
// Ordinary demand and publication stay on their existing I/O path.
class StoreCaptureReadScope final {
 public:
  explicit StoreCaptureReadScope(const std::function<bool()>& interrupted) noexcept
      :interrupted_(interrupted),previous_(std::exchange(active_,this)) {}
  ~StoreCaptureReadScope(){active_=previous_;}
  StoreCaptureReadScope(const StoreCaptureReadScope&)=delete;
  StoreCaptureReadScope& operator=(const StoreCaptureReadScope&)=delete;

  static const StoreCaptureReadScope* current() noexcept {return active_;}
  static void require_active() {
    if(active_ && active_->interrupted())throw StoreCaptureInterrupted{};
  }
  bool interrupted() const noexcept {
    try{return interrupted_ && interrupted_();}
    catch(...){return true;}
  }

 private:
  inline static thread_local const StoreCaptureReadScope* active_=nullptr;
  const std::function<bool()>& interrupted_;
  const StoreCaptureReadScope* previous_;
};

}
