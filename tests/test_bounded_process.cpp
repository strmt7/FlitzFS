#include "bounded_process.hpp"
#include "supervised_source_fixture.hpp"
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
namespace source=source_fixture;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::filesystem::path self() {
  wchar_t path[512]{};
  const auto n = GetModuleFileNameW(nullptr, path, 512);
  require(n > 0 && n < 512, "test executable path unavailable");
  return std::filesystem::path(std::wstring(path, n));
}
int child(int argc, wchar_t** argv) {
  const std::wstring_view mode(argv[1]);
  if (mode == L"--child-source-protocol" || mode == L"--child-source-hold-ack") {
    if(argc!=3)return 1;
    source_fixture::Events protocol(false,argv[2]);
    protocol.signal(source_fixture::Phase::ready);
    protocol.wait_written();
    protocol.signal(source_fixture::Phase::ack);
    if(mode == L"--child-source-hold-ack") Sleep(10000);
    return 73;
  }
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
    const source::ControllerBudget controller_budget;
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
    auto protocol_round=[&] {
      source::Events events(true);
      const auto token=events.token();
      source::Child child(executable,{L"--child-source-protocol",token},{128,2000,5000});
      child.await(events,source::Phase::ready);
      require(child.process_id()!=0&&child.process_id()!=GetCurrentProcessId()&&!child.retirement_confirmed(),
          "source actor identity/lifetime");
      bool second_refused=false;
      try{source::Child second(executable,{L"--child-exit"},{128,2000,5000});}
      catch(const std::runtime_error&){second_refused=true;}
      require(second_refused,"parallel source admission accepted");
      events.signal(source::Phase::written);child.await(events,source::Phase::ack);
      const auto result=child.result();
      require(result.process_id==child.process_id()&&result.exit_confirmed&&!result.timed_out&&
          !result.cancelled&&result.exit_code==73&&child.retirement_confirmed(),"source result/retirement");
      require(child.result().exit_code==73,"retired source result changed");
    };
    // Prime the real successful protocol and real failed-launch exception path
    // before strict repeated accounting; no tolerance or fixed handle subtraction.
    protocol_round();
    const auto missing=executable.parent_path()/(L"missing-source-"+source::random_token()+L".exe");
    require(GetFileAttributesW(missing.c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,
        "new missing-helper fixture already exists");
    auto failed_source=[&] {
      bool failed=false;source::Child child(missing,{}, {128,2000,5000});
      try{(void)child.result();}catch(const std::system_error&){failed=true;}
      require(failed&&child.retirement_confirmed()&&!source::source_busy.load(),"source launch error lost retirement");
    };
    failed_source();
    {
      source::Events events(true);
      bool collision=false;
      try{source::Events duplicate(true,events.token());}catch(const std::runtime_error&){collision=true;}
      require(collision&&WaitForSingleObject(events.get(source::Phase::ready),0)==WAIT_TIMEOUT,
          "source event collision reset/signal");
      source::Events recipient(false,events.token());
      bool role_refused=false;
      try{recipient.signal(source::Phase::written);}catch(const std::system_error&){role_refused=true;}
      require(role_refused&&WaitForSingleObject(events.get(source::Phase::written),0)==WAIT_TIMEOUT,
          "source child acquired parent signal rights");
    }
    for(const auto& token:std::vector<std::wstring>{L"",std::wstring(31,L'a'),
        std::wstring(32,L'A'),L"..\\foreign-source",std::wstring(32,L'g')}) {
      bool invalid=false;try{source::Events opened(false,token);}catch(const std::runtime_error&){invalid=true;}
      require(invalid,"unbounded/foreign source event token accepted");
    }
    std::wstring retired_token;
    {
      source::Events events(true);retired_token=events.token();
      source::Child abandoned(executable,{L"--child-source-protocol",events.token()},{128,2000,5000});
      abandoned.await(events,source::Phase::ready);
    }
    require(!source::source_busy.load(),"abandoned CPU source did not retire");
    for(auto phase:{source::Phase::ready,source::Phase::written,source::Phase::ack}) {
      const auto name=source::Events::event_name(retired_token,phase);
      const auto opened=OpenEventW(SYNCHRONIZE,FALSE,name.c_str());const auto error=GetLastError();
      source::Handle gone(opened);
      require(!gone.value&&error==ERROR_FILE_NOT_FOUND,"source event retained after actor retirement");
    }
    {
      source::Child timed_source(executable,{L"--child-sleep"},{128,2000,50});
      const auto result=timed_source.result();
      require(result.timed_out&&!result.cancelled&&result.exit_confirmed&&result.exit_code==ERROR_TIMEOUT&&
          timed_source.retirement_confirmed(),"async source deadline/retirement");
    }
    {
      source::Events events(true);
      source::Child cancelled_source(executable,{L"--child-source-protocol",events.token()},{128,2000,5000});
      cancelled_source.await(events,source::Phase::ready);
      require(cancelled_source.request_cancel(),"async source cancellation signal");
      const auto result=cancelled_source.result();
      require(result.cancelled&&!result.timed_out&&result.exit_confirmed&&result.exit_code==ERROR_CANCELLED&&
          cancelled_source.retirement_confirmed(),"async source cancellation/retirement");
    }
    auto acknowledged_cancel=[&](bool abandon) {
      {
        source::Events events(true);
        source::Child child(executable,{L"--child-source-hold-ack",events.token()},{128,2000,5000});
        child.await(events,source::Phase::ready);
        events.signal(source::Phase::written);
        child.await(events,source::Phase::ack);
        require(child.process_id()!=0&&!child.retirement_confirmed(),
            "acknowledged child lost live ownership");
        if(!abandon) {
          require(child.request_cancel(),"acknowledged source cancellation signal");
          const auto result=child.result();
          require(result.cancelled&&!result.timed_out&&result.exit_confirmed&&
              result.exit_code==ERROR_CANCELLED&&child.retirement_confirmed(),
              "acknowledged source cancellation/retirement");
        }
      }
      require(!source::source_busy.load(),"acknowledged source admission retained after retirement");
    };
    auto early_exit=[&] {
      source::Events events(true);
      source::Child child(executable,{L"--child-exit"},{128,2000,5000});
      bool refused=false;
      try { child.await(events,source::Phase::ready); }
      catch(const std::runtime_error& error) {
        const std::string_view message(error.what());
        refused=message.find("exit=37 confirmed=1")!=std::string_view::npos;
      }
      const auto result=child.result();
      require(refused&&result.exit_code==37&&result.exit_confirmed&&
          !result.cancelled&&!result.timed_out&&child.retirement_confirmed(),
          "source early exit was not reconciled before phase failure");
      require(WaitForSingleObject(events.get(source::Phase::ready),0)==WAIT_TIMEOUT&&
          WaitForSingleObject(events.get(source::Phase::ack),0)==WAIT_TIMEOUT,
          "early source exit invented a protocol phase");
    };
    // Prime actual cancellation and early-exit paths before strict accounting.
    acknowledged_cancel(false);acknowledged_cancel(true);early_exit();
    DWORD before=0,after=0;
    require(GetProcessHandleCount(GetCurrentProcess(),&before),"source handle baseline");
    for(unsigned repeat=0;repeat<8;++repeat){
      protocol_round();failed_source();acknowledged_cancel(false);acknowledged_cancel(true);early_exit();
    }
    require(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&source::cleanup_ok.load()&&
        !source::source_busy.load(),"source fixture handles/cleanup grew");
    std::cout<<"Protected source phases, actor identity, cancellation, timeout, abandonment, failed launch, post-ack cancellation/abandonment, early exit and repeated exact retirement passed; CPU only.\n";
    std::cout << "Windows Job Object admission, quoting, limits and timeout tests passed; CPU only.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
