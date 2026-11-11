#include "gpu_topology.hpp"
#include "gpu_budget.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>
namespace flitzfs {
namespace {
struct Module {
  HMODULE handle=nullptr;
  ~Module(){if(handle)FreeLibrary(handle);}
  bool close() noexcept {const auto h=std::exchange(handle,nullptr);return !h || FreeLibrary(h)!=0;}
};
struct ErrorMode {
  DWORD old=0;bool active=false;
  ErrorMode() noexcept {active=SetThreadErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX,&old)!=0;}
  ~ErrorMode(){if(active)SetThreadErrorMode(old,nullptr);}
  bool close() noexcept {if(!active)return false;active=false;return SetThreadErrorMode(old,nullptr)!=0;}
};
WindowsGpuTopologyResult query(std::array<std::byte,8> luid,bool software) noexcept {
  WindowsGpuTopologyResult result;
  if(std::all_of(luid.begin(),luid.end(),[](auto b){return b==std::byte{0};})) {
    result.status=WindowsGpuTopologyStatus::invalid_identity;return result;
  }
  const auto observed=[&](WindowsGpuTopologyPhase phase,HRESULT status) noexcept {
    result.phase=phase;result.native_status=static_cast<std::int32_t>(status);return SUCCEEDED(status);
  };
  LUID native{};static_assert(sizeof(native)==8);std::memcpy(&native,luid.data(),8);
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if(!observed(WindowsGpuTopologyPhase::factory,CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory)return result;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  if(!observed(WindowsGpuTopologyPhase::adapter,factory->EnumAdapterByLuid(native,IID_PPV_ARGS(adapter.GetAddressOf()))) || !adapter)return result;
  DXGI_ADAPTER_DESC1 desc{};
  if(!observed(WindowsGpuTopologyPhase::description,adapter->GetDesc1(&desc)))return result;
  const bool is_software=(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)!=0;
  result.amd_hardware=!is_software && desc.VendorId==0x1002 && desc.DedicatedVideoMemory!=0;
  if(std::memcmp(&desc.AdapterLuid,luid.data(),8) ||
      (software?!is_software:!result.amd_hardware)) {
    result.status=WindowsGpuTopologyStatus::wrong_adapter;return result;
  }
  if(!software) {
    const auto budget=query_windows_gpu_budget(luid,1);
    result.phase=WindowsGpuTopologyPhase::budget;result.native_status=budget.native_status;
    if(budget.status!=WindowsGpuBudgetStatus::success ||
        check_gpu_memory_budget(budget.snapshot,luid,1,GetTickCount64(),0)!=GpuGateCode::admitted) {
      result.status=WindowsGpuTopologyStatus::budget_unavailable;return result;
    }
  }
  ErrorMode errors;
  result.phase=WindowsGpuTopologyPhase::loader;
  if(!errors.active){result.native_status=static_cast<std::int32_t>(HRESULT_FROM_WIN32(GetLastError()));return result;}
  std::array<wchar_t,32768> path{},actual{};
  const auto n=GetSystemDirectoryW(path.data(),static_cast<UINT>(path.size()));
  constexpr std::wstring_view suffix=L"\\d3d12.dll";
  if(!n || n>=path.size()-suffix.size()-1){result.native_status=static_cast<std::int32_t>(HRESULT_FROM_WIN32(GetLastError()));return result;}
  std::copy(suffix.begin(),suffix.end(),path.begin()+n);path[n+suffix.size()]=L'\0';
  Module module;
  module.handle=LoadLibraryExW(path.data(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!module.handle){result.native_status=static_cast<std::int32_t>(HRESULT_FROM_WIN32(GetLastError()));return result;}
  const auto length=GetModuleFileNameW(module.handle,actual.data(),static_cast<DWORD>(actual.size()));
  if(!length || length>=actual.size() || CompareStringOrdinal(path.data(),static_cast<int>(n+suffix.size()),
      actual.data(),static_cast<int>(length),TRUE)!=CSTR_EQUAL) {
    result.status=WindowsGpuTopologyStatus::wrong_adapter;return result;
  }
  const auto entry=GetProcAddress(module.handle,"D3D12CreateDevice");
  if(!entry){result.native_status=static_cast<std::int32_t>(HRESULT_FROM_WIN32(GetLastError()));return result;}
  const auto create=std::bit_cast<PFN_D3D12_CREATE_DEVICE>(entry);
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  const auto created=create(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device.GetAddressOf()));
  if(!observed(WindowsGpuTopologyPhase::device,created) || created!=S_OK || !device) {
    result.status=WindowsGpuTopologyStatus::device_failed;return result;
  }
  const auto device_luid=device->GetAdapterLuid();
  std::memcpy(result.luid.data(),&device_luid,8);result.phase=WindowsGpuTopologyPhase::device_identity;
  if(result.luid!=luid){result.status=WindowsGpuTopologyStatus::wrong_adapter;return result;}
  result.node_count=device->GetNodeCount();
  if(!result.node_count || result.node_count>32){result.status=WindowsGpuTopologyStatus::invalid_topology;return result;}
  const auto health=device->GetDeviceRemovedReason();
  if(!observed(WindowsGpuTopologyPhase::device_status,health) || health!=S_OK) {
    result.status=WindowsGpuTopologyStatus::device_failed;return result;
  }
  device.Reset(); // Retire our COM reference before retiring the loader handle.
  result.phase=WindowsGpuTopologyPhase::retire;
  if(!module.close() || !errors.close()) {
    result.native_status=static_cast<std::int32_t>(HRESULT_FROM_WIN32(GetLastError()));return result;
  }
  result.status=WindowsGpuTopologyStatus::success;result.native_status=0;
  result.phase=WindowsGpuTopologyPhase::complete;return result;
}
}
WindowsGpuTopologyResult query_windows_gpu_topology(std::array<std::byte,8> luid) noexcept {
  return query(luid,false);
}
WindowsGpuTopologyResult query_windows_software_topology_fixture(std::array<std::byte,8> luid) noexcept {
  const auto* actions=std::getenv("GITHUB_ACTIONS");const auto* environment=std::getenv("RUNNER_ENVIRONMENT");
  if(!actions || std::string_view(actions)!="true" || !environment ||
      std::string_view(environment)!="github-hosted") {
    WindowsGpuTopologyResult result;result.status=WindowsGpuTopologyStatus::fixture_forbidden;return result;
  }
  return query(luid,true);
}
}
