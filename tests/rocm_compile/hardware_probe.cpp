#include "flitzfs/worker_wire.hpp"
#include "worker_exchange.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

bool valid_ordinal(std::wstring_view value) {
  return (value.size() == 1 && value.front() >= L'0' && value.front() <= L'9') ||
      (value.size() == 2 && value >= L"10" && value <= L"63" &&
       value.back() >= L'0' && value.back() <= L'9');
}

bool valid_luid(std::wstring_view value) {
  return value.size() == 16 && value != L"0000000000000000" &&
      std::ranges::all_of(value, [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
      });
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 5 && std::wstring_view(argv[1]) == L"--run-amd-once",
        "explicit worker path, HIP ordinal and AMD adapter LUID required");
    const std::filesystem::path worker(argv[2]);
    require(worker.is_absolute() && worker.filename() == L"flitzfs_hip_worker.exe",
        "expected absolute qualified worker path");
    require(valid_ordinal(argv[3]) && valid_luid(argv[4]), "invalid explicit adapter selector");

    std::array<std::byte,4096> raw{};
    for (std::size_t i = 0; i < raw.size(); ++i)
      raw[i] = static_cast<std::byte>((i * 53U + i / 17U) & 255U);
    const std::array<flitzfs::GpuBlockView,2> blocks{{
      {0,0,static_cast<std::uint32_t>(raw.size()),raw},
      {1,0x53,8192,{}},
    }};
    const auto batch = flitzfs::PreparedGpuBatch::prepare(blocks);
    constexpr std::uint64_t generation = 1;
    const auto request = flitzfs::encode_worker_request(batch, generation);
    const auto start = std::chrono::steady_clock::now();
    // Adapter budget, exact HIP identity and cleanup are checked inside the
    // supervised process. The probe never loads the runtime or enumerates devices.
    const auto result = flitzfs::run_worker_exchange(worker,
        {L"--adapter",argv[3],argv[4],L"1"}, request,{128,2000,5000});
    require(result.process.process_id && result.process.exit_confirmed,
        "worker exit could not be confirmed");

    const bool completed = result.code == flitzfs::WorkerExchangeCode::completed &&
        !result.process.timed_out && !result.process.cancelled && result.process.exit_code == 0;
    const auto cpu = batch.cpu_reference();
    require(cpu.size() == raw.size() + 8192 &&
        std::ranges::equal(cpu.first(raw.size()), raw) &&
        std::ranges::all_of(cpu.subspan(raw.size()), [](std::byte b) { return b == std::byte{0x53}; }),
        "CPU fixture byte mismatch");
    if (completed) {
      const auto reply = flitzfs::accept_worker_reply(result.response,batch,generation,
          flitzfs::WorkerExecution::hip);
      require(std::ranges::equal(reply.bytes,cpu), "worker output byte mismatch");
    } else {
      require(result.response.empty(), "failed worker returned candidate bytes");
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << std::boolalpha << "HIP hardware probe: {\"gpu_result_verified\":" << completed
        << ",\"cpu_bytes_verified\":" << cpu.size()
        << ",\"explicit_device_bytes\":" << batch.device_buffer_bytes()
        << ",\"elapsed_ms\":" << elapsed
        << ",\"exchange_code\":" << static_cast<unsigned>(result.code)
        << ",\"worker_exit\":" << result.process.exit_code
        << ",\"timed_out\":" << result.process.timed_out
        << ",\"publication_observed\":" << result.publication_observed
        << ",\"peak_commit_bytes\":" << result.process.peak_committed_bytes
        << ",\"exit_confirmed\":true}\n";
    return completed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "HIP hardware probe failed: " << error.what() << '\n';
    return 1;
  }
}
