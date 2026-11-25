#pragma once
#include "flitzfs/process_budget.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace flitzfs {
class WinFspNativeWritable;
struct WinFspReadAheadWork;
struct WinFspGroupReadWindow;
struct NativeMaintenanceObservation;

// The installation and workload qualification owner supplies these exact
// artifacts, adapter and measured worker allowance. No PATH or device scan.
struct NativeLz4ReadWorkerProfile {
  std::filesystem::path helper;
  std::wstring runtime_name,architecture;
  std::string helper_sha256,runtime_sha256,module_sha256;
  std::array<std::byte,8> luid{};
  std::uint32_t node_mask=0,ordinal=0,wave_mask=0;
  ProcessBudget worker;
  std::uint32_t minimum_frames=0,maximum_frames=0;
  std::uint64_t maximum_slab_bytes=0;
  std::uint32_t minimum_lead_ms=0;
  // Each grouped read ABI needs qualification for this exact package.
  bool packed_groups=false;
  bool mixed_groups=false;
  std::uint32_t prepared_group_submit_lead_ms=0;
};

namespace detail {
class NativeGroupReadTiming final {
 public:
  std::uint64_t start_lead(std::uint64_t qualified_ms) const noexcept {
    return (std::max)({qualified_ms, unready_ms_, ready_ms_});
  }
  std::uint64_t submit_lead(std::uint64_t startup_ms, std::uint64_t dispatch_ms,
      std::uint64_t elapsed_ms, bool ready) const noexcept {
    const auto dispatch=(std::max)(dispatch_ms, ready_ms_);
    if(ready)return dispatch;
    const auto total=start_lead(startup_ms);
    // An overdue preparation still needs an observed readiness signal.
    const auto remaining=elapsed_ms<total?total-elapsed_ms:total;
    return (std::max)(dispatch, remaining);
  }
  void observe_unfinished(std::uint64_t elapsed_ms,bool ready_at_submit) noexcept {
    // The exchange still had work at this age. Keep a lower bound until
    // a completed exchange measures the same phase.
    auto& observed=ready_at_submit?ready_ms_:unready_ms_;
    observed=(std::max)(observed,elapsed_ms);
  }
  void observe_accepted(std::uint64_t turnaround_ms, std::uint64_t submit_ms, bool ready_at_submit) noexcept {
    // A ready worker can wait for its request. That interval carries no
    // information about the time needed to become ready.
    if(ready_at_submit)ready_ms_=submit_ms;
    else unready_ms_=turnaround_ms;
  }
 private:
  std::uint64_t unready_ms_=0,ready_ms_=0;
};
} // namespace detail

struct NativeLz4ReadWorkerStats {
  std::uint64_t submitted=0,published=0,superseded=0,failures=0,declined=0;
  std::uint64_t submitted_frames=0,verified_frames=0,budget_queries=0,last_worker_elapsed_ms=0;
  std::uint64_t last_turnaround_ms=0,lead_refusals=0,group_demand_misses=0;
  std::uint64_t prepared_groups_started=0,prepared_groups_submitted=0,last_prepared_submit_ms=0;
  std::uint64_t prepared_groups_retargeted=0;
  std::uint32_t last_worker_exit_code=0;
  bool pending=false,configuration_refused=false,retirement_confirmed=true;
};

// Serialized maintenance owner. A flight owns only immutable input and
// advisory data. Ordinary callbacks never observe or wait on its child.
class NativeLz4ReadWorker final {
 public:
  explicit NativeLz4ReadWorker(NativeLz4ReadWorkerProfile);
  ~NativeLz4ReadWorker();
  NativeLz4ReadWorker(const NativeLz4ReadWorker&)=delete;
  NativeLz4ReadWorker& operator=(const NativeLz4ReadWorker&)=delete;
  WinFspGroupReadWindow group_read_window() const noexcept;
  bool prepare_group(const WinFspReadAheadWork&,const NativeMaintenanceObservation&,const std::atomic<bool>&) noexcept;
  bool start(WinFspReadAheadWork&,const NativeMaintenanceObservation&,const std::atomic<bool>&) noexcept;
  void abandon_unused_preparation(const WinFspNativeWritable&) noexcept;
  void observe(const WinFspNativeWritable&,bool cancel) noexcept;
  bool ready_to_verify() const noexcept;
  bool verify_and_publish(WinFspNativeWritable&,const NativeMaintenanceObservation&,std::uint64_t now,
      const std::atomic<bool>&) noexcept;
  void* wake_handle() const noexcept;
  bool stop() noexcept;
  NativeLz4ReadWorkerStats stats() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
