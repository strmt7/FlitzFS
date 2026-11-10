#include "hip_decode_worker.hpp"
#include "gpu_budget.hpp"
#include "hip_worker_exit.hpp"
#include "hip_worker_bundle.hpp"
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

namespace flitzfs {
namespace {
namespace fs = std::filesystem;
using Exit = HipWorkerExit;

void require(bool condition, Exit reason, const char* message) {
  if (!condition) throw HipWorkerFailure(reason, message);
}

struct DirectoryPin {
  explicit DirectoryPin(const fs::path& path)
      : handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr)) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
        !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
      throw HipWorkerFailure(Exit::runtime_unavailable, "unsafe HIP bundle directory");
    }
  }
  ~DirectoryPin() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
  DirectoryPin(DirectoryPin&& other) noexcept
      : handle(std::exchange(other.handle, INVALID_HANDLE_VALUE)) {}
  DirectoryPin(const DirectoryPin&) = delete;
  DirectoryPin& operator=(const DirectoryPin&) = delete;
  HANDLE handle;
};

struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Hash {
  BCRYPT_HASH_HANDLE handle = nullptr;
  ~Hash() { if (handle) BCryptDestroyHash(handle); }
};

bool matches_sha256(const ReadOnlyFile& file, std::string_view expected) {
  Algorithm algorithm;
  require(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
      Exit::runtime_unavailable, "SHA256 provider unavailable");
  ULONG length = 0, returned = 0;
  require(BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH,
              reinterpret_cast<PUCHAR>(&length), sizeof(length), &returned, 0) >= 0 &&
          returned == sizeof(length) && length && length <= 65536,
      Exit::runtime_unavailable, "SHA256 object size");
  std::vector<UCHAR> object(length);
  Hash hash;
  require(BCryptCreateHash(algorithm.handle, &hash.handle, object.data(), length, nullptr, 0, 0) >= 0,
      Exit::runtime_unavailable, "SHA256 context");
  std::array<UCHAR,65536> bytes{};
  for (std::uint64_t offset = 0; offset < file.size();) {
    const auto count = static_cast<ULONG>((std::min)(file.size() - offset,
        static_cast<std::uint64_t>(bytes.size())));
    file.read_exact_at(offset, bytes.data(), count);
    require(BCryptHashData(hash.handle, bytes.data(), count, 0) >= 0,
        Exit::runtime_unavailable, "SHA256 input");
    offset += count;
  }
  std::array<UCHAR,32> digest{};
  require(BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
      Exit::runtime_unavailable, "SHA256 final");
  constexpr std::string_view hex = "0123456789abcdef";
  if (expected.size() != 2 * digest.size()) return false;
  for (std::size_t i = 0; i < digest.size(); ++i) {
    if (expected[2*i] != hex[digest[i] >> 4] || expected[2*i+1] != hex[digest[i] & 15])
      return false;
  }
  return true;
}

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
  Bundle() : root_(module_path(nullptr).parent_path() / L"hip") {
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
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root_)) {
      ++count;
      const auto known = std::any_of(std::begin(hip_bundle::files), std::end(hip_bundle::files),
          [&](const auto& file) { return same_path(entry.path().filename(), file.name); });
      require(count <= std::size(hip_bundle::files) && known,
          Exit::integrity_failure, "unexpected HIP bundle entry");
    }
    require(count == std::size(hip_bundle::files), Exit::runtime_unavailable, "HIP bundle incomplete");
    files_.reserve(count);
    for (const auto& entry : hip_bundle::files) {
      auto file = ReadOnlyFile::open(root_ / entry.name);
      require(file.size() == entry.size && matches_sha256(file, entry.sha256),
          Exit::integrity_failure, "HIP bundle file identity");
      files_.push_back(std::move(file));
    }
  }
  fs::path path(const wchar_t* name) const { return root_ / name; }
  void check_loaded_paths() const {
    for (const auto& entry : hip_bundle::files) {
      if (const auto loaded = GetModuleHandleW(entry.name)) {
        require(same_path(path(entry.name), module_path(loaded)),
            Exit::integrity_failure, "different HIP dependency already loaded");
      }
    }
  }
 private:
  fs::path root_;
  std::vector<DirectoryPin> directories_;
  std::vector<ReadOnlyFile> files_;
};

Bundle verify_bundle() {
  try { return Bundle(); }
  catch (const HipWorkerFailure&) { throw; }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) {
    throw HipWorkerFailure(Exit::runtime_unavailable, "HIP bundle cannot be opened");
  }
}

class Module {
 public:
  explicit Module(const fs::path& path) {
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(handle_ != nullptr, Exit::runtime_unavailable, "HIP module load failed");
    try {
      require(same_path(path, module_path(handle_)), Exit::integrity_failure, "wrong loaded HIP module");
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
      require(FreeLibrary(handle) != 0, Exit::device_fault, "HIP module release failed");
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

int worker(int argc, wchar_t** argv) {
  require_hip_worker_job();
  if (argc == 2 && std::wstring_view(argv[1]) == L"--verify-bundle") {
    const auto verified = verify_bundle();
    return 0;
  }
  require(argc == 7 && std::wstring_view(argv[1]) == L"--adapter" &&
      std::wstring_view(argv[5]) == L"--worker-channel",
      Exit::invalid_request, "explicit HIP adapter and channel required");
  const auto ordinal = decimal(argv[2], 63);
  const auto luid = parse_luid(argv[3]);
  const auto node_mask = decimal(argv[4], (std::numeric_limits<unsigned>::max)());
  require(node_mask && !(node_mask & (node_mask - 1)), Exit::invalid_request, "adapter node mask");
  auto channel = WorkerChannel::open(argv[6]);
  auto request = read_request(channel.request());
  const auto bundle = verify_bundle();
  const auto observed = query_windows_gpu_budget(luid, node_mask);
  require(observed.status == WindowsGpuBudgetStatus::success,
      Exit::unsupported_configuration, "AMD adapter unavailable");
  require(check_gpu_memory_budget(observed.snapshot, luid, node_mask, GetTickCount64(),
      request.batch.device_buffer_bytes()) == GpuGateCode::admitted,
      Exit::temporary_pressure, "AMD worker memory budget");

  // Per-process search settings. Imported private DLLs are pinned in hip/.
  require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) != 0,
      Exit::runtime_unavailable, "restrict worker DLL lookup");
  bundle.check_loaded_paths();
  Module runtime(bundle.path(hip_bundle::runtime));
  Module kernels(bundle.path(hip_bundle::kernel));
  bundle.check_loaded_paths();
  const auto export_address = GetProcAddress(kernels.get(), "flitzfs_rocm_kernel_api_v1");
  require(export_address != nullptr, Exit::unsupported_configuration, "HIP kernel entry unavailable");
  const auto entry = std::bit_cast<gpu_detail::KernelApiEntry>(export_address);
  const auto* api = entry();
  require(api && gpu_detail::compatible_kernel_api(*api),
      Exit::unsupported_configuration, "incompatible HIP kernel table");
  auto reply = run_hip_decode_in_worker(runtime.get(), *api, static_cast<int>(ordinal), luid, node_mask, request);
  kernels.close();
  runtime.close();
  const auto frame = encode_worker_reply(reply, WorkerExecution::hip);
  channel.publish(frame);
  return 0;
}
}
}

int wmain(int argc, wchar_t** argv) {
  try { return flitzfs::worker(argc, argv); }
  catch (const flitzfs::HipWorkerFailure& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(error.code);
  } catch (const std::bad_alloc&) {
    return static_cast<int>(flitzfs::HipWorkerExit::temporary_pressure);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(flitzfs::HipWorkerExit::device_fault);
  }
}
