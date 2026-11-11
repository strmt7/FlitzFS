#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace flitzfs {
// Conservative process-committed-memory and CPU scheduling limits. Neither
// value is a VRAM, kernel-memory, filesystem-access or physical-RAM sandbox.
struct ProcessBudget {
  std::size_t memory_mib = 128;
  std::uint32_t cpu_rate = 2000; // per 10,000 cycles, relative to enclosing jobs
  std::uint32_t timeout_ms = 15000;
};
bool valid_process_budget(const ProcessBudget& budget) noexcept;
// Construct a bounded Microsoft CRT command line with an explicit absolute
// executable; never invoke a shell or search PATH. Reject invalid inputs.
std::wstring windows_child_command(std::wstring_view executable,
    const std::vector<std::wstring>& arguments);
} // namespace flitzfs
