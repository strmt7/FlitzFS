#include "gpu_topology.hpp"
#include "gpu_budget.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <bit>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>

namespace flitzfs {
namespace {
using Phase = WindowsGpuTopologyPhase;
using Status = WindowsGpuTopologyStatus;

std::int32_t last_native_error() noexcept {
  const auto error = GetLastError();
  return static_cast<std::int32_t>(error ? HRESULT_FROM_WIN32(error) : E_FAIL);
}

bool record(WindowsGpuTopologyResult& result, Phase phase, HRESULT status) noexcept {
  result.phase = phase;
  result.native_status = static_cast<std::int32_t>(status);
  return SUCCEEDED(status);
}

struct Module {
  HMODULE handle = nullptr;
  ~Module() { if (handle) FreeLibrary(handle); }
  bool close() noexcept {
    const auto owned = std::exchange(handle, nullptr);
    return !owned || FreeLibrary(owned) != 0;
  }
};

struct ErrorMode {
  DWORD old = 0;
  bool active = SetThreadErrorMode(
      SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, &old) != 0;
  ~ErrorMode() { if (active) SetThreadErrorMode(old, nullptr); }
  bool close() noexcept {
    if (!active || !SetThreadErrorMode(old, nullptr)) return false;
    active = false;
    return true;
  }
};

WindowsGpuTopologyResult sample_and_retire(WindowsGpuTopologyProbe& probe) noexcept {
  const auto observed = probe.sample();
  if (const auto error = probe.retire()) return {Status::unavailable, Phase::retire, error};
  return observed;
}
}

struct WindowsGpuTopologyProbe::Native {
  Module module;
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  Microsoft::WRL::ComPtr<ID3D12Device> device;

  bool bind_adapter(std::array<std::byte,8> luid, bool software, WindowsGpuTopologyResult& result) noexcept {
    LUID native_luid{};
    static_assert(sizeof(native_luid) == 8);
    std::memcpy(&native_luid, luid.data(), luid.size());
    if (!record(result, Phase::factory, CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory)
      return false;
    if (!record(result, Phase::adapter,
        factory->EnumAdapterByLuid(native_luid, IID_PPV_ARGS(adapter.GetAddressOf()))) || !adapter)
      return false;
    DXGI_ADAPTER_DESC1 description{};
    if (!record(result, Phase::description, adapter->GetDesc1(&description))) return false;
    const bool is_software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    const bool eligible = software ? is_software :
        !is_software && description.VendorId == 0x1002 && description.DedicatedVideoMemory != 0;
    if (!eligible || std::memcmp(&description.AdapterLuid, luid.data(), luid.size())) {
      result.status = Status::wrong_adapter;
      return false;
    }
    return true;
  }

  bool open_device(WindowsGpuTopologyResult& result) noexcept {
    result.phase = Phase::loader;
    std::array<wchar_t,32768> path{}, actual{};
    const auto n = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    constexpr std::wstring_view suffix = L"\\d3d12.dll";
    if (!n || n >= path.size() - suffix.size()) {
      result.native_status = n ? static_cast<std::int32_t>(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) : last_native_error();
      return false;
    }
    std::copy(suffix.begin(), suffix.end(), path.begin() + n);
    path[n + suffix.size()] = L'\0';
    module.handle = LoadLibraryExW(path.data(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module.handle) {
      result.native_status = last_native_error();
      return false;
    }
    const auto length = GetModuleFileNameW(module.handle, actual.data(), static_cast<DWORD>(actual.size()));
    if (!length || length >= actual.size() || CompareStringOrdinal(path.data(), static_cast<int>(n + suffix.size()),
        actual.data(), static_cast<int>(length), TRUE) != CSTR_EQUAL) {
      result.status = Status::wrong_adapter;
      return false;
    }
    const auto entry = GetProcAddress(module.handle, "D3D12CreateDevice");
    if (!entry) {
      result.native_status = last_native_error();
      return false;
    }
    const auto create = std::bit_cast<PFN_D3D12_CREATE_DEVICE>(entry);
    const auto created = create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()));
    if (!record(result, Phase::device, created) || created != S_OK || !device) {
      result.status = Status::device_failed;
      return false;
    }
    return true;
  }
};

WindowsGpuTopologyProbe::WindowsGpuTopologyProbe(std::array<std::byte,8> luid) noexcept
    : WindowsGpuTopologyProbe(luid, false) {}
WindowsGpuTopologyProbe::WindowsGpuTopologyProbe(std::array<std::byte,8> luid, bool software_fixture) noexcept
    : luid_(luid), software_fixture_(software_fixture) {}
WindowsGpuTopologyProbe::~WindowsGpuTopologyProbe() { (void)retire(); }

std::int32_t WindowsGpuTopologyProbe::retire() noexcept {
  if (sealed_) return sealed_->phase == Phase::retire ? sealed_->native_status : 0;
  WindowsGpuTopologyResult result{Status::unavailable, Phase::retire};
  if (native_ && native_->module.handle) {
    ErrorMode errors;
    if (!errors.active) result.native_status = last_native_error();
    native_->device.Reset();
    if (!native_->module.close()) result.native_status = last_native_error();
    if (errors.active && !errors.close()) result.native_status = last_native_error();
  }
  native_.reset();
  sealed_ = result;
  return result.native_status;
}

WindowsGpuTopologyResult WindowsGpuTopologyProbe::sample() noexcept {
  if (sealed_) return *sealed_;
  const auto current = collect();
  if (current.status == Status::success) return current;
  if (retire() == 0) sealed_ = current;
  return *sealed_;
}

WindowsGpuTopologyResult WindowsGpuTopologyProbe::collect() noexcept {
  WindowsGpuTopologyResult result;
  if (!native_) {
    if (std::all_of(luid_.begin(), luid_.end(), [](auto byte) { return byte == std::byte{}; })) {
      result.status = Status::invalid_identity;
      return result;
    }
    native_.reset(new (std::nothrow) Native);
    if (!native_) {
      record(result, Phase::factory, E_OUTOFMEMORY);
      return result;
    }
    if (!native_->bind_adapter(luid_, software_fixture_, result)) return result;
  }
  auto& native = *native_;
  result.amd_hardware = !software_fixture_;
  if (!native.factory->IsCurrent()) {
    result.phase = Phase::topology_changed;
    return result;
  }
  if (!software_fixture_) {
    const auto budget = query_windows_gpu_budget(luid_, 1);
    result.phase = Phase::budget;
    result.native_status = budget.native_status;
    if (budget.status != WindowsGpuBudgetStatus::success ||
        check_gpu_memory_budget(budget.snapshot, luid_, 1, GetTickCount64(), 0) != GpuGateCode::admitted) {
      result.status = Status::budget_unavailable;
      return result;
    }
  }
  ErrorMode errors;
  if (!errors.active) {
    record(result, Phase::loader, last_native_error());
    return result;
  }
  if (!native.device && !native.open_device(result)) return result;
  const auto device_luid = native.device->GetAdapterLuid();
  std::memcpy(result.luid.data(), &device_luid, luid_.size());
  result.phase = Phase::device_identity;
  if (result.luid != luid_) {
    result.status = Status::wrong_adapter;
    return result;
  }
  result.node_count = native.device->GetNodeCount();
  if (!result.node_count || result.node_count > 32) {
    result.status = Status::invalid_topology;
    return result;
  }
  const auto health = native.device->GetDeviceRemovedReason();
  if (!record(result, Phase::device_status, health) || health != S_OK) {
    result.status = Status::device_failed;
    return result;
  }
  if (!native.factory->IsCurrent()) {
    result.phase = Phase::topology_changed;
    return result;
  }
  if (!errors.close()) {
    record(result, Phase::retire, last_native_error());
    return result;
  }
  result.status = Status::success;
  result.phase = Phase::complete;
  return result;
}

WindowsGpuTopologyResult query_windows_gpu_topology(std::array<std::byte,8> luid) noexcept {
  WindowsGpuTopologyProbe probe(luid);
  return sample_and_retire(probe);
}
WindowsGpuTopologyResult query_windows_software_topology_fixture(std::array<std::byte,8> luid) noexcept {
  WindowsGpuTopologyProbe probe(luid, true);
  return sample_and_retire(probe);
}
}
