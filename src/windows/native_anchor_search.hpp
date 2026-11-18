#pragma once
#include "flitzfs/native_filesystem.hpp"
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
};

struct NativeAnchorSearchStats {
  std::uint64_t submitted=0,verified=0,declined=0,yielded=0;
  bool retirement_confirmed=true;
  std::uint64_t submitted_groups=0,verified_groups=0;
};

// One serialized owner, outside the filesystem control lock. The transport
// retains its own route pins and observes exact worker and supervisor exit.
class NativeAnchorSearchController final {
 public:
  explicit NativeAnchorSearchController(NativeAnchorSearchOptions);
  ~NativeAnchorSearchController();
  NativeAnchorSearchController(const NativeAnchorSearchController&)=delete;
  NativeAnchorSearchController& operator=(const NativeAnchorSearchController&)=delete;
  std::size_t stage_grant(std::uint64_t now) const noexcept;
  bool fits_group_budget(std::size_t admitted_members) const noexcept;
  void start(std::span<const NativeSharedBaseSearchWindow>,std::uint64_t activity) noexcept;
  // Input validation only runs on an admitted idle opportunity.
  void observe(std::uint64_t activity,std::uint64_t now,bool collect=false) noexcept;
  bool waiting() const noexcept;
  std::uint64_t deadline() const noexcept;
  void* wake_handle() const noexcept;
  std::span<const NativeSharedBaseSearchReply> replies() const noexcept;
  void discard_reply() noexcept;
  void cancel() noexcept;
  bool stop() noexcept;
  NativeAnchorSearchStats stats() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
