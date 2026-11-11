#include "gpu_budget.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstring>
#include <wrl/client.h>

namespace flitzfs {
WindowsGpuBudgetResult query_windows_gpu_budget(std::array<std::byte,8> luid,
    std::uint32_t node_mask) noexcept {
  WindowsGpuBudgetResult result;
  if(!node_mask || (node_mask&(node_mask-1)) ||
      std::all_of(luid.begin(),luid.end(),[](auto b){return b==std::byte{0};})) {
    result.status=WindowsGpuBudgetStatus::invalid_identity;return result;
  }
  const auto captured=GetTickCount64(); // age includes time spent in OS queries
  LUID adapter_luid{};
  static_assert(sizeof(adapter_luid)==8);
  std::memcpy(&adapter_luid,luid.data(),8);
  const auto observed = [&](WindowsGpuBudgetPhase phase, HRESULT status) noexcept {
    result.phase=phase;result.native_status=static_cast<std::int32_t>(status);
    return SUCCEEDED(status);
  };
  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  if(!observed(WindowsGpuBudgetPhase::factory,CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return result;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
  if(!observed(WindowsGpuBudgetPhase::adapter,factory->EnumAdapterByLuid(adapter_luid,IID_PPV_ARGS(adapter.GetAddressOf())))) return result;
  DXGI_ADAPTER_DESC1 desc{};
  if(!observed(WindowsGpuBudgetPhase::description,adapter->GetDesc1(&desc))) {result.status=WindowsGpuBudgetStatus::query_failed;return result;}
  if(std::memcmp(&desc.AdapterLuid,luid.data(),8) || desc.VendorId!=0x1002 ||
      !desc.DedicatedVideoMemory || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
    result.status=WindowsGpuBudgetStatus::wrong_adapter;return result;
  }
  UINT index=0;
  while((node_mask>>index)!=1)++index; // mask has exactly one bit; index <=31
  DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};
  if(!observed(WindowsGpuBudgetPhase::local,adapter->QueryVideoMemoryInfo(index,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)) ||
      !observed(WindowsGpuBudgetPhase::nonlocal,adapter->QueryVideoMemoryInfo(index,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal))) {
    result.status=WindowsGpuBudgetStatus::query_failed;return result;
  }
  auto& s=result.snapshot;
  s.luid=luid;s.node_mask=node_mask;s.captured_ms=captured;s.dedicated_bytes=desc.DedicatedVideoMemory;
  s.local_budget=local.Budget;s.local_usage=local.CurrentUsage;s.nonlocal_usage=nonlocal.CurrentUsage;
  s.amd_hardware=true;s.valid=true;result.status=WindowsGpuBudgetStatus::success;
  result.phase=WindowsGpuBudgetPhase::complete;return result;
}
} // namespace flitzfs
