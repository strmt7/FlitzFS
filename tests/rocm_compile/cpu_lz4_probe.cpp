#include "bounded_process.hpp"
#include "hip_worker_exit.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
int wmain(int argc,wchar_t** argv) {
  try {
    if(argc!=3 || (std::wstring_view(argv[2])!=L"--available" &&
        std::wstring_view(argv[2])!=L"--unavailable"))
      throw std::invalid_argument("Explicit worker and expected CPU decoder capability required");
    const std::filesystem::path worker(argv[1]);
    if(!worker.is_absolute())throw std::invalid_argument("Absolute trusted worker required");
    const bool available=std::wstring_view(argv[2])==L"--available";
    const auto expected=available?0U:static_cast<std::uint32_t>(
        flitzfs::HipWorkerExit::unsupported_configuration);
    const auto result=flitzfs::run_bounded_process(worker,{L"--verify-cpu-lz4"},
        flitzfs::ProcessBudget{128,2000,15000});
    if(!result.process_id || !result.exit_confirmed || result.timed_out ||
        result.cancelled || result.exit_code!=expected ||
        result.peak_committed_bytes>128ULL*1024*1024)
      throw std::runtime_error("CPU decoder worker retirement or capability mismatch");
    std::cout<<"HIP worker CPU decoder: {\"available\":"<<(available?"true":"false")
        <<",\"exit_confirmed\":true,\"exit_code\":"<<result.exit_code
        <<",\"peak_commit_bytes\":"<<result.peak_committed_bytes
        <<",\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
