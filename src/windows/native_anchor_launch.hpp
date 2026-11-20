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

std::optional<NativeAnchorLaunchPlan> plan_native_anchor_launch(
    std::span<const std::uint32_t> members, const NativeAnchorSearchOptions& options,
    const GpuMemorySnapshot& observed, std::uint64_t now, bool permits_reference_change);
}
