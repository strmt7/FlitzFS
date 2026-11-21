#pragma once
#include "native_anchor_search.hpp"
#include "worker_exchange.hpp"
#include "../native_group_anchor.hpp"

namespace flitzfs {
// The planned batch contains only complete groups in their existing order.
struct NativeAnchorLaunchPlan {
  detail::AnchorMatchBatchPlan batch;
  detail::AnchorMatchTransferPlan transfer;
  WorkerChannelCapacity capacity;
  ProcessBudget worker;
  std::uint32_t dispatches_per_wait = 1;
};

// A conservative estimate from verified jobs on the current route. An empty
// limit calls for a small initial batch. A zero limit leaves work on the CPU.
class NativeAnchorBatchTiming final {
 public:
  void record_verified(std::uint64_t elapsed_ms,std::uint32_t launches) noexcept;
  std::optional<std::uint32_t> launch_limit(std::uint32_t hard_timeout_ms) const noexcept;
 private:
  std::uint64_t microseconds_per_launch_=0;
};

std::optional<NativeAnchorLaunchPlan> plan_native_anchor_launch(
    std::span<const std::uint32_t> members, const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed, std::uint64_t now, bool permits_reference_change,
    std::optional<std::uint32_t> launch_limit={});
}
