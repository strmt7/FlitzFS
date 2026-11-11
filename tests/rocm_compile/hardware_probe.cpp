#include "flitzfs/worker_wire.hpp"
#include "flitzfs/match_worker_wire.hpp"
#include "worker_exchange.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
#include "../gpu_probe_budget.hpp"
#include <cstdlib>
#include "hip_worker_exit.hpp"
#include "hip_identity_wire.hpp"
#include "hip_memory_wire.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <bit>
#include <cstdint>
#include <string>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

using ProbeBudget=gpu_probe_fixture::Budget;

struct AdapterIdentity {
  UINT dxgi_ordinal = 0;
  std::array<std::byte,8> luid{};
  UINT vendor = 0, device = 0;
  std::uint64_t dedicated = 0;
};
void describe_adapters(const ProbeBudget& budget) {
  std::vector<AdapterIdentity> selected;
  selected.reserve(4);
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) && factory.Get(),
      "DXGI metadata factory unavailable");
  bool exhausted = false;
  UINT seen = 0;
  for (UINT index = 0; index < 64; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto status = factory->EnumAdapters1(index, adapter.GetAddressOf());
    if (status == DXGI_ERROR_NOT_FOUND) { exhausted = true; break; }
    require(SUCCEEDED(status) && adapter.Get(), "DXGI metadata enumeration failed");
    ++seen;
    DXGI_ADAPTER_DESC1 description{};
    require(SUCCEEDED(adapter->GetDesc1(&description)), "DXGI adapter description unavailable");
    if (description.VendorId != 0x1002 || !description.DedicatedVideoMemory ||
        (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
    static_assert(sizeof(LUID) == 8);
    const auto luid = std::bit_cast<std::array<std::byte,8>>(description.AdapterLuid);
    require(std::ranges::any_of(luid, [](std::byte value) { return value != std::byte{}; }),
        "zero hardware adapter identity");
    require(std::ranges::none_of(selected, [&](const auto& old) { return old.luid == luid; }),
        "duplicate hardware adapter identity");
    selected.push_back({index,luid,description.VendorId,description.DeviceId,description.DedicatedVideoMemory});
  }
  require(exhausted, "DXGI metadata adapter count cap");
  factory.Reset();
  std::cout << "HIP adapter metadata: {\"complete\":true,\"gpu_executed\":false,"
      "\"hip_initialized\":false,\"adapters_seen\":" << seen << ",\"adapters\":[";
  bool separator = false;
  constexpr char hex[] = "0123456789abcdef";
  for (const auto& adapter : selected) {
    if (separator) std::cout << ',';
    separator = true;
    std::cout << "{\"dxgi_ordinal\":" << adapter.dxgi_ordinal << ",\"luid\":\"";
    for (const auto byte : adapter.luid) {
      const auto value = std::to_integer<unsigned>(byte);
      std::cout << hex[value >> 4] << hex[value & 15];
    }
    std::cout << "\",\"vendor\":" << adapter.vendor << ",\"device\":" << adapter.device
        << ",\"dedicated_bytes\":" << adapter.dedicated << ",\"probe_node_mask\":1}";
  }
  std::cout << "],\"controller_job_peak_commit_bytes\":" << budget.peak_commit() << "}\n";
}

void inspect_budget(std::wstring_view text, const ProbeBudget& budget) {
  require(text.size()==16 && text!=L"0000000000000000", "invalid budget LUID");
  std::array<std::byte,8> luid{};
  const auto digit = [](wchar_t value) -> unsigned {
    require((value>=L'0'&&value<=L'9')||(value>=L'a'&&value<=L'f'),"invalid budget LUID digit");
    return value<=L'9'?static_cast<unsigned>(value-L'0'):static_cast<unsigned>(value-L'a'+10);
  };
  for(std::size_t i=0;i<8;++i)luid[i]=static_cast<std::byte>(16*digit(text[2*i])+digit(text[2*i+1]));
  const auto result=flitzfs::query_windows_gpu_budget(luid,1);
  const auto& memory=result.snapshot;
  const auto gate=flitzfs::check_gpu_memory_budget(memory,luid,1,GetTickCount64(),16492);
  std::cout << std::boolalpha << "HIP budget metadata: {\"gpu_executed\":false,\"hip_initialized\":false,"
      "\"status\":" << static_cast<unsigned>(result.status)
      << ",\"phase\":" << static_cast<unsigned>(result.phase)
      << ",\"native_status\":" << result.native_status << ",\"snapshot_valid\":" << memory.valid
      << ",\"dedicated_bytes\":" << memory.dedicated_bytes << ",\"local_budget\":" << memory.local_budget
      << ",\"local_usage\":" << memory.local_usage << ",\"nonlocal_budget\":" << memory.nonlocal_budget << ",\"nonlocal_usage\":" << memory.nonlocal_usage
      << ",\"physical_total\":" << memory.physical_total << ",\"physical_available\":" << memory.physical_available
      << ",\"commit_limit\":" << memory.commit_limit << ",\"commit_available\":" << memory.commit_available
      << ",\"nonlocal_growth_envelope\":" << flitzfs::gpu_memory_envelope(memory,16492).nonlocal_growth
      << ",\"gate_code\":" << static_cast<unsigned>(gate)
      << ",\"controller_job_peak_commit_bytes\":" << budget.peak_commit() << "}\n";
}

bool valid_digest(std::wstring_view value) {
  return value.size() == 64 && std::ranges::all_of(value, [](wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
  });
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

std::array<std::byte,8> probe_luid(std::wstring_view text) {
  require(valid_luid(text),"invalid identity metadata LUID");
  const auto digit=[](wchar_t c){return static_cast<unsigned>(c<=L'9'?c-L'0':c-L'a'+10);};
  std::array<std::byte,8> value{};
  for(std::size_t i=0;i<8;++i)value[i]=static_cast<std::byte>(16*digit(text[2*i])+digit(text[2*i+1]));
  return value;
}
void inspect_topology(std::array<std::byte,8> luid,const ProbeBudget& budget,bool software) {
  const auto result=software?flitzfs::query_windows_software_topology_fixture(luid):
      flitzfs::query_windows_gpu_topology(luid);
  std::cout<<std::boolalpha<<"GPU topology metadata: {\"gpu_executed\":false,\"hip_initialized\":false,"
      "\"queues_requested\":false,\"batch_allocations_requested\":false,\"kernel_launch_requested\":false,"
      "\"status\":"<<static_cast<unsigned>(result.status)
      <<",\"phase\":"<<static_cast<unsigned>(result.phase)
      <<",\"native_status\":"<<result.native_status
      <<",\"node_count\":"<<result.node_count<<",\"amd_hardware\":"<<result.amd_hardware
      <<",\"luid_matches\":"<<(result.luid==luid)
      <<",\"single_node_verified\":"<<flitzfs::verified_single_gpu_node(result,luid)
      <<",\"software_fixture\":"<<software
      <<",\"controller_job_peak_commit_bytes\":"<<budget.peak_commit()<<"}\n";
}
void inspect_warp_topology(const ProbeBudget& budget) {
  const auto* actions=std::getenv("GITHUB_ACTIONS");
  const auto* environment=std::getenv("RUNNER_ENVIRONMENT");
  require(actions && std::string_view(actions)=="true" && environment &&
      std::string_view(environment)=="github-hosted","WARP metadata is disposable-hosted only");
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) && factory,
      "WARP metadata factory unavailable");
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf()))) && adapter,
      "explicit WARP adapter unavailable");
  DXGI_ADAPTER_DESC1 description{};
  require(SUCCEEDED(adapter->GetDesc1(&description)) &&
      (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"WARP description unavailable");
  const auto luid=std::bit_cast<std::array<std::byte,8>>(description.AdapterLuid);
  adapter.Reset();factory.Reset();inspect_topology(luid,budget,true);
}
int inspect_driver_identity(const std::filesystem::path& worker,const wchar_t* ordinal,
    const wchar_t* luid,const wchar_t* digest,const ProbeBudget& budget) {
  const flitzfs::HipIdentityRequest expected{1,static_cast<std::uint32_t>(std::stoul(ordinal)),1,probe_luid(luid)};
  const auto request=flitzfs::encode_hip_identity_request(expected);
  const auto result=flitzfs::run_worker_exchange(worker,
      {L"--inspect-system-adapter",ordinal,luid,digest},request,{128,2000,5000});
  require(result.process.process_id && result.process.exit_confirmed,"identity metadata child exit unconfirmed");
  const bool completed=result.code==flitzfs::WorkerExchangeCode::completed &&
      !result.process.timed_out && !result.process.cancelled && result.process.exit_code==0;
  std::optional<flitzfs::HipIdentityObservation> observed;
  if(completed)observed=flitzfs::decode_hip_identity_reply(result.response,expected);
  else require(result.response.empty(),"failed identity metadata returned bytes");
  std::cout << std::boolalpha << "HIP identity metadata: {\"properties_verified\":" << completed
      << ",\"hip_initialization_confirmed\":" << completed
      << ",\"flitzfs_kernel_load_requested\":false,\"batch_allocations_requested\":false,\"kernel_launch_requested\":false"
      << ",\"ordinal\":" << expected.ordinal << ",\"node_mask\":";
  if(observed)std::cout << observed->node_mask;else std::cout << "null";
  std::cout << ",\"luid\":";
  if(observed) {
    constexpr char hex[]="0123456789abcdef";std::string actual(16,'0');
    for(std::size_t i=0;i<8;++i){const auto b=std::to_integer<unsigned>(observed->luid[i]);actual[2*i]=hex[b>>4];actual[2*i+1]=hex[b&15];}
    std::cout << '"' << actual << '"';
  }else std::cout << "null";
  std::cout << ",\"luid_matches\":";
  if(observed)std::cout << (observed->luid==expected.luid);else std::cout << "null";
  std::cout << ",\"node_mask_matches\":";
  if(observed)std::cout << (observed->node_mask==expected.node_mask);else std::cout << "null";
  std::cout << ",\"worker_exit\":" << result.process.exit_code
      << ",\"worker_category\":" << flitzfs::hip_worker_exit_category(result.process.exit_code)
      << ",\"worker_stage\":\"" << flitzfs::hip_worker_stage_name(flitzfs::hip_worker_exit_stage(result.process.exit_code)) << '"'
      << ",\"timed_out\":" << result.process.timed_out << ",\"exit_confirmed\":true"
      << ",\"peak_commit_bytes\":" << result.process.peak_committed_bytes
      << ",\"controller_job_peak_commit_bytes\":" << budget.peak_commit() << "}\n";
  return completed?0:2;
}

}

int wmain(int argc, wchar_t** argv) {
  try {
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    const bool discovery = mode == L"--describe-amd-adapters";
    const bool budget_metadata = mode == L"--inspect-adapter-budget";
    const bool topology = mode == L"--inspect-adapter-topology";
    const bool warp_topology = mode == L"--inspect-warp-topology";
    const bool metadata = mode == L"--verify-driver-runtime";
    const bool identity = mode == L"--inspect-driver-adapter";
    const bool memory_observation = mode == L"--run-amd-driver-memory-once";
    const bool matching = mode == L"--run-amd-driver-match-once";
    const bool driver = mode == L"--run-amd-driver-once" || matching || memory_observation;
    require((topology && argc==3) || (warp_topology && argc==2) || (budget_metadata && argc == 3) || (discovery && argc == 2) || (metadata && argc == 4) || ((driver || identity) && argc == 6) ||
        (mode == L"--run-amd-once" && argc == 5), "explicit qualified worker and probe mode required");
    if (discovery) { const ProbeBudget budget; describe_adapters(budget); return 0; }
    if (budget_metadata) { const ProbeBudget budget; inspect_budget(argv[2],budget); return 0; }
    if (topology) { const auto luid=probe_luid(argv[2]);const ProbeBudget budget;inspect_topology(luid,budget,false);return 0; }
    if (warp_topology) { const ProbeBudget budget;inspect_warp_topology(budget);return 0; }
    const std::filesystem::path worker(argv[2]);
    require(worker.is_absolute() && worker.filename() == L"flitzfs_hip_worker.exe",
        "expected absolute qualified worker path");
    if (metadata || driver || identity) require(valid_digest(argv[metadata ? 3 : 5]), "invalid runtime digest");
    if (!metadata) require(valid_ordinal(argv[3]) && valid_luid(argv[4]), "invalid explicit adapter selector");
    const ProbeBudget budget;
    if (metadata) {
      const auto result = flitzfs::run_bounded_process(worker,
          {L"--verify-system-runtime", argv[3]}, {128,2000,5000});
      require(result.process_id && result.exit_confirmed, "metadata child exit unconfirmed");
      const bool verified = !result.timed_out && !result.cancelled && result.exit_code == 0;
      std::cout << std::boolalpha << "HIP runtime metadata probe: {\"verified\":" << verified
          << ",\"worker_exit\":" << result.exit_code << ",\"timed_out\":" << result.timed_out
          << ",\"exit_confirmed\":true,\"peak_commit_bytes\":" << result.peak_committed_bytes
          << ",\"controller_job_peak_commit_bytes\":" << budget.peak_commit()
          << ",\"gpu_executed\":false}\n";
      return verified ? 0 : 2;
    }

    if (identity) return inspect_driver_identity(worker,argv[3],argv[4],argv[5],budget);

    if (matching) {
      std::array<std::byte,4096> source{};
      for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<std::byte>((i % 257U) & 255U);
      const auto search = flitzfs::PreparedGpuMatchSearch::prepare(source);
      constexpr std::uint64_t request_id = 1;
      const auto request = flitzfs::encode_match_worker_request(search, request_id);
      const auto result = flitzfs::run_worker_exchange(worker,
          {L"--driver-match-adapter", argv[3], argv[4], L"1", argv[5]}, request, {128,2000,5000});
      require(result.process.process_id && result.process.exit_confirmed, "match child exit unconfirmed");
      const bool completed = result.code == flitzfs::WorkerExchangeCode::completed &&
          !result.process.timed_out && !result.process.cancelled && result.process.exit_code == 0;
      require(std::ranges::equal(search.input(), source), "owned match source changed");
      std::size_t proposal_count = 0;
      if (completed) {
        const auto proposals = flitzfs::accept_match_worker_reply(result.response, search,
            request_id, flitzfs::WorkerExecution::hip);
        require(!proposals.empty(), "qualified repeated source produced no match");
        proposal_count = proposals.size();
      } else require(result.response.empty(), "failed match returned candidate bytes");
      std::cout << std::boolalpha << "HIP match hardware probe: {\"gpu_result_verified\":" << completed
          << ",\"source_bytes_verified\":" << source.size() << ",\"proposals\":" << proposal_count
          << ",\"explicit_device_bytes\":" << search.device_buffer_bytes()
          << ",\"worker_exit\":" << result.process.exit_code
          << ",\"worker_category\":" << flitzfs::hip_worker_exit_category(result.process.exit_code)
          << ",\"worker_stage\":\"" << flitzfs::hip_worker_stage_name(flitzfs::hip_worker_exit_stage(result.process.exit_code)) << "\"" << ",\"timed_out\":" << result.process.timed_out
          << ",\"publication_observed\":" << result.publication_observed
          << ",\"peak_commit_bytes\":" << result.process.peak_committed_bytes
          << ",\"controller_job_peak_commit_bytes\":" << budget.peak_commit()
          << ",\"exit_confirmed\":true}\n";
      return completed ? 0 : 2;
    }
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
    std::vector<std::wstring> arguments{
        memory_observation ? L"--driver-memory-adapter" : driver ? L"--driver-adapter" : L"--adapter", argv[3], argv[4], L"1"};
    if (driver) arguments.emplace_back(argv[5]);
    const auto result = flitzfs::run_worker_exchange(worker, arguments, request, {128,2000,5000});
    require(result.process.process_id && result.process.exit_confirmed,
        "worker exit could not be confirmed");

    const bool completed = result.code == flitzfs::WorkerExchangeCode::completed &&
        !result.process.timed_out && !result.process.cancelled && result.process.exit_code == 0;
    const auto cpu = batch.cpu_reference();
    require(cpu.size() == raw.size() + 8192 &&
        std::ranges::equal(cpu.first(raw.size()), raw) &&
        std::ranges::all_of(cpu.subspan(raw.size()), [](std::byte b) { return b == std::byte{0x53}; }),
        "CPU fixture byte mismatch");
    if(memory_observation){
      std::optional<flitzfs::HipMemoryObservation> observed;
      if(completed)observed=flitzfs::decode_hip_memory_observation(result.response,
          {generation,static_cast<std::uint32_t>(std::stoul(argv[3])),1,probe_luid(argv[4]),
              batch.device_buffer_bytes(),cpu.size()});
      else require(result.response.empty(),"failed memory diagnostic returned metadata");
      std::cout<<std::boolalpha<<"HIP memory observation: {\"metadata_verified\":"<<completed
          <<",\"gpu_candidate_accepted\":false,\"cpu_reference_bytes\":"<<cpu.size()
          <<",\"worker_exit\":"<<result.process.exit_code
          <<",\"timed_out\":"<<result.process.timed_out<<",\"exit_confirmed\":true"
          <<",\"peak_commit_bytes\":"<<result.process.peak_committed_bytes
          <<",\"controller_job_peak_commit_bytes\":"<<budget.peak_commit()<<",\"observation\":";
      if(observed){
        const auto& value=*observed;
        std::cout<<"{\"failure_category\":"<<value.failure_category
            <<",\"failure_stage\":\""<<flitzfs::hip_worker_stage_name(static_cast<flitzfs::HipWorkerStage>(value.failure_stage))
            <<"\",\"captured_mask\":"<<value.captured<<",\"flags\":"<<value.flags
            <<",\"cpu_verified_gpu_bytes\":"<<value.cpu_verified_bytes
            <<",\"explicit_device_bytes\":"<<value.binding.device_bytes
            <<",\"admitted\":"<<((value.flags&flitzfs::hip_memory_admitted)!=0)
            <<",\"resources_released\":"<<((value.flags&flitzfs::hip_memory_resources_released)!=0)
            <<",\"modules_retired\":"<<((value.flags&flitzfs::hip_memory_modules_retired)!=0)
            <<",\"launch_submitted\":"<<((value.flags&flitzfs::hip_memory_launch_submitted)!=0)
            <<",\"stream_completed\":"<<((value.flags&flitzfs::hip_memory_stream_completed)!=0)
            <<",\"statuses_verified\":"<<((value.flags&flitzfs::hip_memory_statuses_verified)!=0)
            <<",\"snapshots\":[";
        for(unsigned i=0;i<3;++i){
          if(i)std::cout<<',';const auto& s=value.snapshots[i];
          std::cout<<"{\"point\":"<<i<<",\"captured\":"<<((value.captured&(1U<<i))!=0)
              <<",\"valid\":"<<s.valid<<",\"amd_hardware\":"<<s.amd_hardware
              <<",\"luid_matches\":"<<(s.luid==value.binding.luid)<<",\"node_mask\":"<<s.node_mask
              <<",\"captured_ms\":"<<s.captured_ms<<",\"dedicated_bytes\":"<<s.dedicated_bytes
              <<",\"local_budget\":"<<s.local_budget<<",\"local_usage\":"<<s.local_usage
              <<",\"nonlocal_budget\":"<<s.nonlocal_budget<<",\"nonlocal_usage\":"<<s.nonlocal_usage<<"}";
        }
        std::cout<<"]}";
      }else std::cout<<"null";
      std::cout<<"}\n";return completed?0:2;
    }
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
        << ",\"worker_category\":" << flitzfs::hip_worker_exit_category(result.process.exit_code)
        << ",\"worker_stage\":\"" << flitzfs::hip_worker_stage_name(flitzfs::hip_worker_exit_stage(result.process.exit_code)) << "\""
        << ",\"timed_out\":" << result.process.timed_out
        << ",\"publication_observed\":" << result.publication_observed
        << ",\"peak_commit_bytes\":" << result.process.peak_committed_bytes
        << ",\"controller_job_peak_commit_bytes\":" << budget.peak_commit()
        << ",\"installed_driver_route\":" << driver
        << ",\"exit_confirmed\":true}\n";
    return completed ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "HIP hardware probe failed: " << error.what() << '\n';
    return 1;
  }
}
