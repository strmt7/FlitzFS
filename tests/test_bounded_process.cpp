#include "bounded_process.hpp"
#include <windows.h>
#include <iostream>
#include <cstddef>
#include <stdexcept>
#include <string_view>

// Bind Python qualification telemetry to the current compiled AMD64 SDK ABI.
static_assert(sizeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION)==48);
static_assert(offsetof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION,ActiveProcesses)==40);
static_assert(sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)==144);

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::filesystem::path self() {
  wchar_t path[512]{};
  const auto n = GetModuleFileNameW(nullptr, path, 512);
  require(n > 0 && n < 512, "test executable path unavailable");
  return std::filesystem::path(std::wstring(path, n));
}
int child(int argc, wchar_t** argv) {
  const std::wstring_view mode(argv[1]);
  if (mode == L"--child-exit") return 37;
  if (mode == L"--child-sleep") { Sleep(10000); return 1; }
  if (mode == L"--child-args") {
    const std::vector<std::wstring> expected{L"", L"a b", L"tab\tvalue", L"quote\"value",
        L"trailing\\", L"before\\\"quote", L"two\\\\", L"plain"};
    if (argc != static_cast<int>(expected.size()) + 2) return 1;
    for (std::size_t i = 0; i < expected.size(); ++i)
      if (argv[i + 2] != expected[i]) return 2;
    return 73;
  }
  if (mode == L"--child-probe") {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) ||
        !QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu), nullptr)) return 1;
    constexpr DWORD required = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY | JOB_OBJECT_LIMIT_PRIORITY_CLASS;
    if ((limits.BasicLimitInformation.LimitFlags & required) != required ||
        (limits.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) ||
        limits.BasicLimitInformation.ActiveProcessLimit != 1 ||
        limits.BasicLimitInformation.PriorityClass != BELOW_NORMAL_PRIORITY_CLASS ||
        limits.ProcessMemoryLimit != 128ULL * 1024 * 1024 || limits.JobMemoryLimit != limits.ProcessMemoryLimit ||
        cpu.ControlFlags != (JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) ||
        cpu.CpuRate != 2000) return 2;
    return 73;
  }
  if (mode == L"--child-allocation") {
    // One virtual-commit request above a 64 MiB job cap, without touching pages.
    void* allocation = VirtualAlloc(nullptr, 128ULL * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (allocation) { VirtualFree(allocation, 0, MEM_RELEASE); return 1; }
    return 73;
  }
  if (mode == L"--child-spawn") {
    const auto executable = self();
    auto command = flitzfs::windows_child_command(executable.native(), {L"--child-exit"});
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
          CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 73;
    // Unexpected permission must not leave a suspended descendant behind.
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 1000);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return 1;
  }
  return 99;
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc > 1) return child(argc, argv);
    const auto executable = self();
    auto run = [&](std::vector<std::wstring> args, DWORD expected, flitzfs::ProcessBudget budget = {}) {
      const auto result = flitzfs::run_bounded_process(executable, args, budget);
      require(result.process_id != 0 && result.process_id != GetCurrentProcessId() && result.exit_confirmed && !result.timed_out && !result.cancelled && result.exit_code == expected, "child result mismatch");
      require(result.peak_committed_bytes <= budget.memory_mib * 1024ULL * 1024, "job memory accounting exceeded cap");
    };
    run({L"--child-probe"}, 73);
    run({L"--child-exit"}, 37);
    run({L"--child-args", L"", L"a b", L"tab\tvalue", L"quote\"value", L"trailing\\",
        L"before\\\"quote", L"two\\\\", L"plain"}, 73);
    run({L"--child-spawn"}, 73);
    flitzfs::ProcessBudget memory_limited; memory_limited.memory_mib = 64;
    run({L"--child-allocation"}, 73, memory_limited);
    flitzfs::ProcessBudget quick; quick.timeout_ms = 50;
    const auto timed = flitzfs::run_bounded_process(executable, {L"--child-sleep"}, quick);
    require(timed.process_id != 0 && timed.timed_out && timed.exit_confirmed && timed.exit_code == ERROR_TIMEOUT, "timeout not terminated/confirmed");
    auto observation = std::make_shared<flitzfs::ProcessObserver>();
    require(observation->process_id() == 0 &&
        observation->wait_for_exit() == flitzfs::ProcessObservation::not_started,
        "unlaunched observer invented a process");
    require(observation->request_stop(), "prelaunch cancellation signal failed");
    const auto cancelled = flitzfs::run_bounded_process(executable, {L"--child-sleep"}, {}, observation);
    require(cancelled.cancelled && !cancelled.timed_out && cancelled.exit_confirmed &&
        cancelled.exit_code == ERROR_CANCELLED && observation->process_id() == cancelled.process_id &&
        observation->wait_for_exit() == flitzfs::ProcessObservation::exited,
        "signalled cancellation did not retain/retire the exact child");
    bool reused = false;
    try { (void)flitzfs::run_bounded_process(executable, {L"--child-exit"}, {}, observation); }
    catch (const std::invalid_argument&) { reused = true; }
    require(reused && observation->process_id() == cancelled.process_id,
        "observer reuse replaced a process identity");
    observation.reset();
    bool rejected = false;
    try { flitzfs::ProcessBudget invalid; invalid.memory_mib = 0;
      (void)flitzfs::run_bounded_process(executable, {}, invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "invalid budget accepted");
    std::cout << "Windows Job Object admission, quoting, limits and timeout tests passed; CPU only.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
