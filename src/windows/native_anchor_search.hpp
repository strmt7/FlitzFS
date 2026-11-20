#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include "hip_route_discovery.hpp"

namespace flitzfs {

// Supplied by an independently qualified package and workload profile.
// The memory field is the measured fixed child allowance. Each launch also
// funds its exact request, reply, device slab and transport copies.
struct NativeAnchorSearchOptions {
  HipDiscoveredRoute route;
  ProcessBudget worker;
  std::uint32_t minimum_members=0,maximum_members=0;
  std::uint32_t duty_ppm=10000;
  anchor_match::BatchOutput output=anchor_match::BatchOutput::anchors;
  std::uint32_t maximum_dispatch_members=0;
  // Use a queued profile when the admitted slab contains its wait group.
  std::uint32_t dispatches_per_wait=1;
};

struct NativeAnchorSearchStats {
  std::uint64_t submitted=0,verified=0,declined=0,yielded=0;
  bool retirement_confirmed=true;
  std::uint64_t submitted_groups=0,verified_groups=0,integrity_failures=0,empty_xor_batches=0;
  std::uint64_t reply_bytes=0;
  std::uint64_t reference_batches=0,reselected_groups=0;
  std::uint64_t submitted_queued_batches=0,verified_queued_batches=0,queued_worker_wall_ns=0,queued_kernel_wait_ns=0;
  std::uint64_t submitted_kernel_launches=0;
  std::uint32_t queued_dispatches_per_wait=0,largest_dispatch_members=0;
  // Work for the most recent independently verified copy batch.
  std::uint64_t copy_suggested_ranges=0,copy_compared_ranges=0,copy_suggested_bytes=0,copy_compared_bytes=0;
};

// One serialized owner, outside the filesystem control lock. The transport
// retains its own route pins and observes exact worker and supervisor exit.
class NativeAnchorSearchController final {
 public:
  explicit NativeAnchorSearchController(NativeAnchorSearchOptions);
  ~NativeAnchorSearchController();
  NativeAnchorSearchController(const NativeAnchorSearchController&)=delete;
  NativeAnchorSearchController& operator=(const NativeAnchorSearchController&)=delete;
  std::size_t stage_grant(std::uint64_t now,std::size_t funded_extents) const noexcept;
  bool fits_group_budget(std::size_t admitted_members) const noexcept;
  void start(std::span<const NativeSharedBaseSearchWindow>,std::uint64_t activity) noexcept;
  // Input validation only runs on an admitted idle opportunity.
  void observe(std::uint64_t activity,std::uint64_t now,bool collect=false) noexcept;
  bool waiting() const noexcept;
  std::span<const NativeSharedBaseSearchWindow> pending_prefix() const noexcept;
  std::uint64_t deadline() const noexcept;
  void* wake_handle() const noexcept;
  std::span<const NativeSharedBaseSearchReply> replies() const noexcept;
  // Called after the serialized writer advances one admitted step.
  void settle_reply_checks() noexcept;
  bool can_reacquire() const noexcept;
  bool take_device_interruption() noexcept;
  bool rebind(HipDiscoveredRoute) noexcept;
  void discard_reply() noexcept;
  // Cancel unresolved work and keep fully verified, detached advice.
  void yield_to_foreground() noexcept;
  void cancel() noexcept;
  bool stop() noexcept;
  NativeAnchorSearchStats stats() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
