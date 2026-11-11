#include "bounded_process.hpp"
#include "hip_worker_exit.hpp"
#include "worker_exchange.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include <windows.h>
#include <array>
#include <cstdlib>
#include <string>
#include <string_view>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
class CompilerEnvironmentFixture {
 public:
  CompilerEnvironmentFixture() {
    std::size_t count = 0;
    require(_wgetenv_s(&count, nullptr, 0, L"LLVM_PATH") == 0 && count <= 32768,
        "cannot inspect compiler environment");
    std::wstring value(count, L'\0');
    if (count) {
      require(_wgetenv_s(&count, value.data(), value.size(), L"LLVM_PATH") == 0,
          "cannot retain compiler environment");
      previous_ = value.c_str();
    }
    require(_wputenv_s(L"LLVM_PATH", L"flitzfs-cpu-qualification-poison") == 0,
        "cannot prepare compiler environment fixture");
  }
  ~CompilerEnvironmentFixture() { (void)_wputenv_s(L"LLVM_PATH", previous_.c_str()); }
  void verify_parent_unchanged() const {
    std::array<wchar_t,64> value{};
    require(GetEnvironmentVariableW(L"LLVM_PATH", value.data(), static_cast<DWORD>(value.size())) ==
        std::wstring_view(L"flitzfs-cpu-qualification-poison").size() &&
        std::wstring_view(value.data()) == L"flitzfs-cpu-qualification-poison",
        "worker changed the parent's compiler environment");
  }
 private:
  std::wstring previous_;
};
void retired(const flitzfs::ProcessResult& process, std::uint32_t expected) {
  require(process.process_id && process.exit_confirmed && !process.timed_out &&
      !process.cancelled && process.exit_code == expected, "unexpected worker retirement or exit status");
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 6, "explicit worker, fixture, tampered copy, metadata probe and digest required");
    const std::filesystem::path worker(argv[1]), fixture(argv[2]), tampered(argv[3]);
    require(worker.is_absolute() && fixture.is_absolute() && tampered.is_absolute(), "absolute qualification paths required");
    require(std::filesystem::create_directory(fixture), "new worker probe fixture required");
    const auto copied = fixture / worker.filename();
    require(CopyFileW(worker.c_str(), copied.c_str(), TRUE) != 0, "worker relocation copy");
    const flitzfs::ProcessBudget budget{128,2000,15000};
    const auto metadata = flitzfs::run_bounded_process(std::filesystem::path(argv[4]), {argv[5]}, budget);
    std::cout << "system_metadata exit=" << metadata.exit_code << " timed_out=" << metadata.timed_out
        << " exit_confirmed=" << metadata.exit_confirmed << " peak_commit_bytes="
        << metadata.peak_committed_bytes << '\n';
    retired(metadata, 0);
    const CompilerEnvironmentFixture compiler_environment;
    const auto verified = flitzfs::run_bounded_process(worker, {L"--verify-bundle"}, budget);
    retired(verified, 0);
    compiler_environment.verify_parent_unchanged();
    const auto absent = flitzfs::run_bounded_process(copied, {L"--verify-bundle"}, budget);
    retired(absent, static_cast<std::uint32_t>(flitzfs::HipWorkerExit::runtime_unavailable));
    const auto altered = flitzfs::run_bounded_process(tampered, {L"--verify-bundle"}, budget);
    retired(altered, static_cast<std::uint32_t>(flitzfs::HipWorkerExit::integrity_failure));
    // A valid transport carries a malformed wire header. Rejection must occur
    // before DXGI and before the private runtime can load.
    const std::array<std::byte,flitzfs::kWorkerHeaderBytes> malformed{};
    const auto invalid = flitzfs::run_worker_exchange(worker,
        {L"--adapter",L"0",L"0100000000000000",L"1"}, malformed, budget);
    retired(invalid.process, static_cast<std::uint32_t>(flitzfs::HipWorkerExit::invalid_request));
    require(invalid.code == flitzfs::WorkerExchangeCode::worker_failed &&
        invalid.response.empty() && !invalid.publication_observed, "invalid worker published a response");
    const auto invalid_match=flitzfs::run_worker_exchange(worker,
        {L"--match-adapter",L"0",L"0100000000000000",L"1"},malformed,budget);
    retired(invalid_match.process,static_cast<std::uint32_t>(flitzfs::HipWorkerExit::invalid_request));
    require(invalid_match.response.empty()&&!invalid_match.publication_observed,
        "invalid match worker published a response");
    const std::array<std::byte,64> match_input{};
    const auto search=flitzfs::PreparedGpuMatchSearch::prepare(match_input);
    const auto match_request=flitzfs::encode_match_worker_request(search,1);
    const auto absent_match=flitzfs::run_worker_exchange(copied,
        {L"--match-adapter",L"0",L"0100000000000000",L"1"},match_request,budget);
    retired(absent_match.process,static_cast<std::uint32_t>(flitzfs::HipWorkerExit::runtime_unavailable));
    require(absent_match.response.empty()&&!absent_match.publication_observed,
        "missing match runtime published a response");
    const auto malformed_identity = flitzfs::run_bounded_process(copied,
        {L"--verify-system-runtime", L"invalid"}, budget);
    retired(malformed_identity, static_cast<std::uint32_t>(flitzfs::HipWorkerExit::invalid_request));
    const auto wrong_identity = flitzfs::run_bounded_process(copied,
        {L"--verify-system-runtime", std::wstring(64,L'0')}, budget);
    require(wrong_identity.process_id && wrong_identity.exit_confirmed && !wrong_identity.timed_out &&
        !wrong_identity.cancelled && (wrong_identity.exit_code ==
          static_cast<std::uint32_t>(flitzfs::HipWorkerExit::runtime_unavailable) ||
        wrong_identity.exit_code == static_cast<std::uint32_t>(flitzfs::HipWorkerExit::integrity_failure)),
        "unqualified system runtime identity was accepted");
    std::cout<<"HIP worker admission: {\"bundle_verified\":true,\"missing_bundle_refused\":true,"
        "\"tampered_bundle_refused\":true,\"invalid_wire_refused\":true,\"invalid_match_refused\":true,\"missing_match_runtime_refused\":true,\"compiler_environment_isolated\":true,\"system_module_metadata_passed\":true,\"system_runtime_bad_hash_refused\":true,\"system_runtime_argument_refused\":true,\"exits_confirmed\":9,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<"HIP worker admission failed: "<<error.what()<<'\n';
    return 1;
  }
}
