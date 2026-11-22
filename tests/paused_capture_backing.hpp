#pragma once

#include "flitzfs/object_store.hpp"
#include "../src/store_read_scope.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace flitzfs::test {

// Hold one capture after copying its backing bytes. An ordinary operation
// can still take the underlying provider lock while the capture is held.
class PausedCaptureBacking final : public StoreByteIo {
 public:
  explicit PausedCaptureBacking(std::shared_ptr<StoreByteIo> bytes,
      std::optional<std::uint64_t> offset={}) : bytes_(std::move(bytes)),offset_(offset) {}

  std::uint64_t extent() const override {
    std::lock_guard lock(io_);
    return bytes_->extent();
  }
  StoreIoContract contract() const noexcept override {return bytes_->contract();}
  bool concurrent_reads_supported() const noexcept override {return true;}
  void admit_write(std::uint64_t bytes) override {
    std::lock_guard lock(io_);
    bytes_->admit_write(bytes);
  }
  void write_exact(std::uint64_t offset,std::span<const std::byte> bytes) override {
    std::lock_guard lock(io_);
    bytes_->write_exact(offset,bytes);
  }
  void flush() override {
    std::lock_guard lock(io_);
    bytes_->flush();
  }
  void read_exact(std::uint64_t offset,std::span<std::byte> bytes) const override {
    const bool capture=detail::StoreCaptureReadScope::current()!=nullptr;
    {
      std::lock_guard lock(io_);
      if(capture)++capture_reads_;
      ++reads_;
      bytes_->read_exact(offset,bytes);
    }
    if(capture && (!offset_ || offset==*offset_) && !held_.exchange(true)) {
      entered_.set_value();
      if(proceed_.wait_for(deadline)!=std::future_status::ready)
        throw std::runtime_error("Fixture capture was not released");
    }
  }
  bool wait_until_held() const {return arrival_.wait_for(deadline)==std::future_status::ready;}
  void release() {resume_.set_value();}
  std::uint64_t reads() const noexcept {return reads_.load();}
  std::uint64_t capture_reads() const noexcept {return capture_reads_.load();}

 private:
  static constexpr auto deadline=std::chrono::seconds(5);
  std::shared_ptr<StoreByteIo> bytes_;
  const std::optional<std::uint64_t> offset_;
  mutable std::mutex io_;
  mutable std::promise<void> entered_;
  std::future<void> arrival_=entered_.get_future();
  std::promise<void> resume_;
  std::shared_future<void> proceed_=resume_.get_future().share();
  mutable std::atomic<bool> held_{false};
  mutable std::atomic<std::uint64_t> reads_{0},capture_reads_{0};
};

} // namespace flitzfs::test
