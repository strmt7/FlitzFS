#include "hip_decode_worker.hpp"
#include "gpu_budget.hpp"
#include "hip_worker_exit.hpp"
#include "hip_api.hpp"
#include "hip_identity_wire.hpp"
#include "hip_worker_bundle.hpp"
#include "hip_module_verification.hpp"
#include "worker_exchange.hpp"
#include "flitzfs/read_only_file.hpp"
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <new>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <optional>
#include <cstdlib>

namespace flitzfs {
namespace {
namespace fs = std::filesystem;
using Exit = HipWorkerExit;

void require(bool condition, Exit reason, const char* message,
    HipWorkerStage stage = HipWorkerStage::none) {
  if (!condition) throw HipWorkerFailure(reason, message, stage);
}

using hip_detail::DirectoryPin;
using hip_detail::matches_sha256;

fs::path module_path(HMODULE handle) {
  std::array<wchar_t,32768> buffer{};
  const auto length = GetModuleFileNameW(handle, buffer.data(), static_cast<DWORD>(buffer.size()));
  require(length && length < buffer.size(), Exit::runtime_unavailable, "module path unavailable");
  return fs::path(std::wstring_view(buffer.data(), length));
}
bool same_path(const fs::path& left, const fs::path& right) {
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

class Bundle {
 public:
  explicit Bundle(bool kernel_only = false) : root_(module_path(nullptr).parent_path() / L"hip"),
      kernel_only_(kernel_only) {
    const auto drive = root_.root_name().native();
    require(root_.is_absolute() && drive.size() == 2 && drive[1] == L':',
        Exit::runtime_unavailable, "HIP bundle must be on a local drive path");
    auto parent = root_.root_path();
    directories_.emplace_back(parent);
    for (const auto& component : root_.relative_path()) {
      require(component != L"." && component != L".." && directories_.size() < 32,
          Exit::runtime_unavailable, "HIP bundle path depth");
      parent /= component;
      directories_.emplace_back(parent);
    }
    const auto expected_count = kernel_only_ ? std::size_t{1} : std::size(hip_bundle::files);
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root_)) {
      ++count;
      const auto known = std::any_of(std::begin(hip_bundle::files), std::end(hip_bundle::files),
          [&](const auto& file) { return included(file.name) && same_path(entry.path().filename(), file.name); });
      require(count <= expected_count && known,
          Exit::integrity_failure, "unexpected HIP bundle entry");
    }
    require(count == expected_count, Exit::runtime_unavailable, "HIP bundle incomplete");
    files_.reserve(count);
    for (const auto& entry : hip_bundle::files) {
      if (!included(entry.name)) continue;
      auto file = ReadOnlyFile::open(root_ / entry.name);
      require(file.size() == entry.size && matches_sha256(file, entry.sha256),
          Exit::integrity_failure, "HIP bundle file identity");
      files_.push_back(std::move(file));
    }
  }
  fs::path path(const wchar_t* name) const { return root_ / name; }
  void check_loaded_paths() const {
    for (const auto& entry : hip_bundle::files) {
      if (!included(entry.name)) continue;
      if (const auto loaded = GetModuleHandleW(entry.name)) {
        require(same_path(path(entry.name), module_path(loaded)),
            Exit::integrity_failure, "different HIP dependency already loaded");
      }
    }
  }
 private:
  bool included(const wchar_t* name) const {
    return !kernel_only_ || same_path(name, hip_bundle::kernel);
  }
  fs::path root_;
  bool kernel_only_ = false;
  std::vector<DirectoryPin> directories_;
  std::vector<ReadOnlyFile> files_;
};

Bundle verify_bundle(bool kernel_only = false) {
  try { return Bundle(kernel_only); }
  catch (const HipWorkerFailure&) { throw; }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) {
    throw HipWorkerFailure(Exit::runtime_unavailable, "HIP bundle cannot be opened");
  }
}

class Module {
 public:
  explicit Module(const fs::path& path, HipWorkerStage stage) {
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(handle_ != nullptr, Exit::runtime_unavailable, "HIP module load failed", stage);
    try {
      require(same_path(path, module_path(handle_)), Exit::integrity_failure, "wrong loaded HIP module", stage);
    } catch (...) {
      FreeLibrary(handle_);
      throw;
    }
  }
  ~Module() { if (handle_) FreeLibrary(handle_); }
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  HMODULE get() const noexcept { return handle_; }
  void close() {
    if (auto handle = std::exchange(handle_, nullptr))
      require(FreeLibrary(handle) != 0, Exit::device_fault, "HIP module release failed", HipWorkerStage::module_retire);
  }
 private:
  HMODULE handle_ = nullptr;
};

unsigned decimal(std::wstring_view input, unsigned limit) {
  require(!input.empty() && input.size() <= 10, Exit::invalid_request, "adapter decimal field");
  unsigned value = 0;
  for (const auto digit : input) {
    require(digit >= L'0' && digit <= L'9', Exit::invalid_request, "adapter decimal digit");
    const auto next = static_cast<unsigned>(digit - L'0');
    require(next <= limit && value <= (limit - next) / 10, Exit::invalid_request, "adapter decimal range");
    value = 10 * value + next;
  }
  return value;
}

unsigned hex_value(wchar_t digit) {
  if (digit >= L'0' && digit <= L'9') return static_cast<unsigned>(digit - L'0');
  if (digit >= L'a' && digit <= L'f') return static_cast<unsigned>(digit - L'a' + 10);
  throw HipWorkerFailure(Exit::invalid_request, "adapter LUID digit");
}

std::array<std::byte,8> parse_luid(std::wstring_view text) {
  require(text.size() == 16, Exit::invalid_request, "adapter LUID length");
  std::array<std::byte,8> luid{};
  for (std::size_t i = 0; i < luid.size(); ++i)
    luid[i] = static_cast<std::byte>(16 * hex_value(text[2*i]) + hex_value(text[2*i+1]));
  require(std::any_of(luid.begin(), luid.end(), [](auto byte) { return byte != std::byte{}; }),
      Exit::invalid_request, "zero adapter LUID");
  return luid;
}

WorkerBatchRequest read_request(std::span<const std::byte> bytes) {
  try { return decode_worker_request(bytes); }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) {
    throw HipWorkerFailure(Exit::invalid_request, "invalid HIP worker request");
  }
}

MatchWorkerRequest read_match_request(std::span<const std::byte> bytes) {
  try{return decode_match_worker_request(bytes);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid HIP match request");}
}
std::string runtime_digest(std::wstring_view wide) {
  require(wide.size() == 64, Exit::invalid_request, "system runtime hash length");
  std::string expected;
  expected.reserve(64);
  for (const auto character : wide) {
    require((character >= L'0' && character <= L'9') ||
        (character >= L'a' && character <= L'f'), Exit::invalid_request, "system runtime hash digit");
    expected.push_back(static_cast<char>(character));
  }
  return expected;
}

int worker(int argc, wchar_t** argv) {
  require_hip_worker_job();
  // Compiler discovery must not leak into an application runtime. This changes
  // only this disposable child; the launching process remains untouched.
  require(_wputenv_s(L"LLVM_PATH", L"") == 0,
      Exit::runtime_unavailable, "cannot isolate HIP compiler environment");
  SetLastError(ERROR_SUCCESS);
  require(GetEnvironmentVariableW(L"LLVM_PATH", nullptr, 0) == 0 &&
      (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS),
      Exit::runtime_unavailable, "HIP compiler environment remains visible");
  if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-system-runtime") {
    const hip_detail::VerifiedSystemModule verified(hip_bundle::runtime, runtime_digest(argv[2]));
    return 0;
  }
  if (argc == 7 && std::wstring_view(argv[1]) == L"--inspect-system-adapter") {
    require(std::wstring_view(argv[5]) == L"--worker-channel",
        Exit::invalid_request,"identity metadata channel required");
    const auto ordinal=decimal(argv[2],63);
    const auto luid=parse_luid(argv[3]);
    const auto expected_runtime=runtime_digest(argv[4]);
    auto channel=WorkerChannel::open(argv[6]);
    HipIdentityRequest request;
    try{request=decode_hip_identity_request(channel.request());}
    catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid identity metadata request");}
    require(request.ordinal==ordinal && request.luid==luid,
        Exit::invalid_request,"identity metadata argument/frame mismatch");
    const hip_detail::VerifiedSystemModule installed(hip_bundle::runtime,expected_runtime);
    const auto budget=query_windows_gpu_budget(luid,request.node_mask);
    require(budget.status==WindowsGpuBudgetStatus::success,
        Exit::unsupported_configuration,"identity metadata adapter budget unavailable",HipWorkerStage::adapter_budget);
    require(check_gpu_memory_budget(budget.snapshot,luid,request.node_mask,GetTickCount64(),0)==GpuGateCode::admitted,
        Exit::temporary_pressure,"identity metadata memory budget",HipWorkerStage::adapter_budget);
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=0,
        Exit::runtime_unavailable,"restrict metadata DLL lookup");
    require(GetModuleHandleW(hip_bundle::kernel)==nullptr,
        Exit::integrity_failure,"identity metadata unexpectedly contains a kernel module");
    Module runtime(installed.path(),HipWorkerStage::runtime_load);
    const auto observed=hip_detail::query_device_identity(hip_detail::bind_api(runtime.get()),
        static_cast<int>(ordinal));
    require(GetModuleHandleW(hip_bundle::kernel)==nullptr,
        Exit::integrity_failure,"identity metadata loaded a kernel module");
    const auto frame=encode_hip_identity_reply(
        {request.request_id,request.ordinal,observed.node_mask,observed.luid});
    runtime.close(); // Metadata cannot be published before confirmed module retirement.
    channel.publish(frame);
    return 0;
  }
  if (argc == 2 && (std::wstring_view(argv[1]) == L"--verify-bundle" ||
      std::wstring_view(argv[1]) == L"--verify-kernel-bundle")) {
    const auto verified = verify_bundle(std::wstring_view(argv[1]) == L"--verify-kernel-bundle");
    return 0;
  }
  const std::wstring_view mode = argc > 1 ? argv[1] : L"";
  const bool memory_observation=mode==L"--driver-memory-adapter";
  const bool driver = mode == L"--driver-adapter" || mode == L"--driver-match-adapter" || memory_observation;
  const bool matching = mode == L"--match-adapter" || mode == L"--driver-match-adapter";
  require((driver ? argc == 8 : argc == 7) &&
      (driver || mode == L"--adapter" || mode == L"--match-adapter") &&
      std::wstring_view(argv[driver ? 6 : 5]) == L"--worker-channel",
      Exit::invalid_request, "explicit HIP adapter and channel required");
  const auto expected_runtime = driver ? runtime_digest(argv[5]) : std::string{};
  const auto ordinal = decimal(argv[2], 63);
  const auto luid = parse_luid(argv[3]);
  const auto node_mask = decimal(argv[4], (std::numeric_limits<unsigned>::max)());
  require(node_mask && !(node_mask & (node_mask - 1)), Exit::invalid_request, "adapter node mask");
  auto channel = WorkerChannel::open(argv[driver ? 7 : 6]);
  std::optional<WorkerBatchRequest> decode;
  std::optional<MatchWorkerRequest> match;
  if(matching)match.emplace(read_match_request(channel.request()));
  else decode.emplace(read_request(channel.request()));
  const auto allocation=matching?match->search.device_buffer_bytes():decode->batch.device_buffer_bytes();
  std::optional<HipMemoryBinding> diagnostic_binding;
  if(memory_observation){
    diagnostic_binding=HipMemoryBinding{decode->request_id,ordinal,node_mask,luid,
        allocation,decode->batch.cpu_reference().size()};
    try{hip_memory_detail::binding_valid(*diagnostic_binding);}
    catch(const std::invalid_argument&){
      throw HipWorkerFailure(Exit::invalid_request,"memory diagnostic binding",HipWorkerStage::worker_request);
    } // Before loading a runtime/kernel.
  }
  const auto bundle = verify_bundle(driver);
  std::optional<hip_detail::VerifiedSystemModule> installed;
  if (driver) installed.emplace(hip_bundle::runtime, expected_runtime);
  const auto observed = query_windows_gpu_budget(luid, node_mask);
  require(observed.status == WindowsGpuBudgetStatus::success,
      Exit::unsupported_configuration, "AMD adapter unavailable", HipWorkerStage::adapter_budget);
  require(check_gpu_memory_budget(observed.snapshot, luid, node_mask, GetTickCount64(),
      allocation) == GpuGateCode::admitted,
      Exit::temporary_pressure, "AMD worker memory budget", HipWorkerStage::adapter_budget);

  // Per-process search settings. Imported private DLLs are pinned in hip/.
  require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) != 0,
      Exit::runtime_unavailable, "restrict worker DLL lookup");
  bundle.check_loaded_paths();
  const auto runtime_path = driver ? installed->path() : bundle.path(hip_bundle::runtime);
  Module runtime(runtime_path, HipWorkerStage::runtime_load);
  Module kernels(bundle.path(hip_bundle::kernel), HipWorkerStage::kernel_load);
  bundle.check_loaded_paths();
  const auto loaded_runtime = GetModuleHandleW(hip_bundle::runtime);
  require(loaded_runtime && same_path(runtime_path, module_path(loaded_runtime)),
      Exit::integrity_failure, "different installed HIP runtime loaded");
  std::vector<std::byte> frame;
  if(matching) {
    const auto address=GetProcAddress(kernels.get(),"flitzfs_rocm_match_api_v1");
    require(address!=nullptr,Exit::unsupported_configuration,"HIP match entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<match_detail::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && match_detail::compatible_kernel_api(*api),
        Exit::unsupported_configuration,"incompatible HIP match table",HipWorkerStage::kernel_table);
    const auto reply=run_hip_match_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*match);
    frame=encode_match_worker_reply(match->search,match->request_id,reply,WorkerExecution::hip);
  } else {
    const auto address=GetProcAddress(kernels.get(),"flitzfs_rocm_kernel_api_v1");
    require(address!=nullptr,Exit::unsupported_configuration,"HIP kernel entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<gpu_detail::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && gpu_detail::compatible_kernel_api(*api),
        Exit::unsupported_configuration,"incompatible HIP kernel table",HipWorkerStage::kernel_table);
    if(memory_observation){
      HipMemoryObservation observation;
      observation.binding=*diagnostic_binding;
      try{(void)run_hip_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*decode,&observation);}
      catch(const HipWorkerFailure& failure){
        observation.failure_category=static_cast<std::uint32_t>(failure.code);
        observation.failure_stage=static_cast<std::uint32_t>(failure.stage);
      }
      // A diagnostic may describe a rejected computation, but never publish
      // its data bytes. Unconfirmed module retirement still publishes nothing.
      kernels.close();runtime.close();observation.flags|=hip_memory_modules_retired;
      const auto metadata=encode_hip_memory_observation(observation);
      channel.publish(metadata);return 0;
    }
    const auto reply=run_hip_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*decode);
    frame=encode_worker_reply(reply,WorkerExecution::hip);
  }
  kernels.close();
  runtime.close();
  channel.publish(frame);
  return 0;
}
}
}

int wmain(int argc, wchar_t** argv) {
  try { return flitzfs::worker(argc, argv); }
  catch (const flitzfs::HipWorkerFailure& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(flitzfs::hip_worker_diagnostic_exit(error.code, error.stage));
  } catch (const std::bad_alloc&) {
    return static_cast<int>(flitzfs::HipWorkerExit::temporary_pressure);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(flitzfs::HipWorkerExit::device_fault);
  }
}
