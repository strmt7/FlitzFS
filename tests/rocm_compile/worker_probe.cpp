#include "bounded_process.hpp"
#include "hip_worker_exit.hpp"
#include "worker_exchange.hpp"
#include <windows.h>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void retired(const flitzfs::ProcessResult& process, std::uint32_t expected) {
  require(process.process_id && process.exit_confirmed && !process.timed_out &&
      !process.cancelled && process.exit_code == expected, "unexpected worker retirement or exit status");
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 4, "explicit worker, tampered copy and fresh probe directory required");
    const std::filesystem::path worker(argv[1]), fixture(argv[2]), tampered(argv[3]);
    require(worker.is_absolute() && fixture.is_absolute() && tampered.is_absolute(), "absolute qualification paths required");
    require(std::filesystem::create_directory(fixture), "new worker probe fixture required");
    const auto copied = fixture / worker.filename();
    require(CopyFileW(worker.c_str(), copied.c_str(), TRUE) != 0, "worker relocation copy");
    const flitzfs::ProcessBudget budget{128,2000,15000};
    const auto verified = flitzfs::run_bounded_process(worker, {L"--verify-bundle"}, budget);
    retired(verified, 0);
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
    std::cout<<"HIP worker admission: {\"bundle_verified\":true,\"missing_bundle_refused\":true,"
        "\"tampered_bundle_refused\":true,\"invalid_wire_refused\":true,\"exits_confirmed\":4,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr<<"HIP worker admission failed: "<<error.what()<<'\n';
    return 1;
  }
}
