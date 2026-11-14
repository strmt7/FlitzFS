#include "flitzfs/process_budget.hpp"
#include "flitzfs/windows_path.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace flitzfs {
bool valid_process_budget(const ProcessBudget& b) noexcept {
  return b.memory_mib >= 64 &&
      b.memory_mib <= (std::numeric_limits<std::size_t>::max)()/(1024U*1024U) &&
      (b.memory_share_divisor ? b.memory_share_divisor>=16 : b.memory_mib<=256) && b.cpu_rate >= 100 &&
      b.cpu_rate <= 2000 && b.timeout_ms >= 1 && b.timeout_ms <= 40000;
}
ProcessBudget plan_resource_process_budget(std::uint64_t required,std::uint64_t physical,
    std::uint64_t commit,std::uint32_t divisor,std::uint32_t cpu,std::uint32_t timeout) noexcept {
  constexpr std::uint64_t mib=1024*1024,reserve=512*mib;
  ProcessBudget refused;refused.memory_mib=0;
  if(!required || divisor<16 || physical<=reserve || commit<=reserve)return refused;
  const auto allowance=(std::min)(physical-reserve,commit-reserve)/divisor;
  const auto rounded=required/mib+(required%mib!=0);
  const auto count=(std::max)(rounded,std::uint64_t{64});
  if(count>allowance/mib || count>(std::numeric_limits<std::size_t>::max)()/mib)return refused;
  ProcessBudget budget{static_cast<std::size_t>(count),cpu,timeout,divisor};
  return valid_process_budget(budget)?budget:refused;
}
std::wstring windows_child_command(std::wstring_view executable,
    const std::vector<std::wstring>& arguments) {
  if (executable.size() < 4 || executable.size() > 240 ||
      !ordinary_windows_file_path(executable) || executable[1] != L':' ||
      executable[2] != L'\\' || arguments.size() > 16)
    throw std::invalid_argument("invalid bounded executable path or argument count");
  // argv[0] uses different CRT rules; quotes cannot occur in a valid filename.
  std::wstring result = L"\"" + std::wstring(executable) + L"\"";
  for (const auto& arg : arguments) {
    if (arg.size() > 512 || arg.find(L'\0') != arg.npos)
      throw std::invalid_argument("invalid bounded child argument");
    result += L" \"";
    std::size_t slashes = 0;
    for (const auto c : arg) {
      if (c == L'\\') { ++slashes; continue; }
      result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      result += c;
      slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    if (result.size() > 4096) throw std::invalid_argument("child command exceeds limit");
  }
  return result;
}
} // namespace flitzfs
