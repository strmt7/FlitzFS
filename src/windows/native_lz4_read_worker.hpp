#pragma once
#include "flitzfs/process_budget.hpp"
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

struct NativeLz4ReadWorkerStats {
  std::uint64_t submitted=0,published=0,superseded=0,failures=0,declined=0;
  std::uint64_t submitted_frames=0,verified_frames=0,budget_queries=0,last_worker_elapsed_ms=0;
  std::uint64_t last_turnaround_ms=0,lead_refusals=0;
  std::uint64_t prepared_groups_started=0,prepared_groups_submitted=0,last_prepared_submit_ms=0;
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
  bool prepare_group(const WinFspReadAheadWork&,const NativeMaintenanceObservation&,const std::atomic<bool>&) noexcept;
  bool start(WinFspReadAheadWork&,const NativeMaintenanceObservation&,const std::atomic<bool>&) noexcept;
  void abandon_unused_preparation() noexcept;
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
