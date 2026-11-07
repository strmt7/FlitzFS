#pragma once
#include "flitzfs/process_budget.hpp"
#include <filesystem>

namespace flitzfs {
struct ProcessResult {
  std::uint32_t process_id = 0; // identity of the launched helper, not a live process handle
  std::uint32_t exit_code = 0;
  bool timed_out = false;
  bool exit_confirmed = false;
  std::size_t peak_committed_bytes = 0;
};
// Run one trusted CPU helper inside a Windows Job Object. Creation-time job
// assignment closes the create-then-assign orphan window. No handle inheritance,
// shell, elevation, driver activity or fallback that removes a failed limit.
// Failures throw; timeout requests termination and waits at most 2s for exit.
// This is NOT a security sandbox or protection against kernel/driver hangs.
ProcessResult run_bounded_process(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, ProcessBudget budget = {});
} // namespace flitzfs
