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
  // Zero preserves the legacy fixture profile. An adaptive profile instead
  // scales committed memory to fresh available physical AND commit headroom.
  // At least 16; the supervisor rechecks before launch and still enforces the
  // resulting exact byte limit in its creation-time Job Object.
  std::uint32_t memory_share_divisor = 0;
};
bool valid_process_budget(const ProcessBudget& budget) noexcept;
// Required bytes must cover the configuration-qualified complete child peak,
// not merely input or VRAM. Zero/unknown/overflow/insufficient headroom returns
// an invalid zero-memory budget without allocation or OS activity.
ProcessBudget plan_resource_process_budget(std::uint64_t required_child_bytes,
    std::uint64_t physical_available,std::uint64_t commit_available,
    std::uint32_t memory_share_divisor,std::uint32_t cpu_rate,
    std::uint32_t timeout_ms) noexcept;
// Construct a bounded Microsoft CRT command line with an explicit absolute
// executable; never invoke a shell or search PATH. Reject invalid inputs.
std::wstring windows_child_command(std::wstring_view executable,
    const std::vector<std::wstring>& arguments);
} // namespace flitzfs
