#include "gpu_budget.hpp"
#include <windows.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <wrl/client.h>

namespace flitzfs {
WindowsGpuBudgetSampler::WindowsGpuBudgetSampler(std::array<std::byte,8> luid,std::uint32_t node_mask) noexcept
    :luid_(luid),node_mask_(node_mask) {}
WindowsGpuBudgetSampler::~WindowsGpuBudgetSampler()=default;

WindowsGpuBudgetResult WindowsGpuBudgetSampler::sample() noexcept {
  if(failure_)return *failure_;
  const auto result=collect();
  if(result.status!=WindowsGpuBudgetStatus::success) {
    failure_=result;
    adapter_.Reset();
    factory_.Reset();
  }
  return result;
}
WindowsGpuBudgetResult WindowsGpuBudgetSampler::collect() noexcept {
  WindowsGpuBudgetResult result;
  const auto captured=GetTickCount64(); // age includes time spent in OS queries
  const auto observed = [&](WindowsGpuBudgetPhase phase, HRESULT status) noexcept {
    result.phase=phase;result.native_status=static_cast<std::int32_t>(status);
    return SUCCEEDED(status);
  };
  if(!adapter_) {
    if(!std::has_single_bit(node_mask_) ||
        std::all_of(luid_.begin(),luid_.end(),[](auto byte){return byte==std::byte{};})) {
      result.status=WindowsGpuBudgetStatus::invalid_identity;
      return result;
    }
    LUID adapter_luid{};
    static_assert(sizeof(adapter_luid)==8);
    std::memcpy(&adapter_luid,luid_.data(),luid_.size());
    if(!observed(WindowsGpuBudgetPhase::factory,CreateDXGIFactory1(IID_PPV_ARGS(factory_.GetAddressOf()))) || !factory_)
      return result;
    if(!observed(WindowsGpuBudgetPhase::adapter,
        factory_->EnumAdapterByLuid(adapter_luid,IID_PPV_ARGS(adapter_.GetAddressOf()))) || !adapter_)
      return result;
    DXGI_ADAPTER_DESC1 description{};
    if(!observed(WindowsGpuBudgetPhase::description,adapter_->GetDesc1(&description))) {
      result.status=WindowsGpuBudgetStatus::query_failed;
      return result;
    }
    if(std::memcmp(&description.AdapterLuid,luid_.data(),luid_.size()) || description.VendorId!=0x1002 ||
        !description.DedicatedVideoMemory || (description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
      result.status=WindowsGpuBudgetStatus::wrong_adapter;
      return result;
    }
    dedicated_bytes_=description.DedicatedVideoMemory;
  }
  if(!factory_->IsCurrent()) {
    result.phase=WindowsGpuBudgetPhase::topology;
    return result;
  }
  const auto index=static_cast<UINT>(std::countr_zero(node_mask_));
  DXGI_QUERY_VIDEO_MEMORY_INFO local{},nonlocal{};
  if(!observed(WindowsGpuBudgetPhase::local,adapter_->QueryVideoMemoryInfo(index,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&local)) ||
      !observed(WindowsGpuBudgetPhase::nonlocal,adapter_->QueryVideoMemoryInfo(index,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&nonlocal))) {
    result.status=WindowsGpuBudgetStatus::query_failed;return result;
  }
  MEMORYSTATUSEX host{};host.dwLength=sizeof(host);
  if(!GlobalMemoryStatusEx(&host)) {
    result.status=WindowsGpuBudgetStatus::query_failed;
    result.phase=WindowsGpuBudgetPhase::host_memory;
    result.native_status=static_cast<std::int32_t>(GetLastError());return result;
  }
  if(!factory_->IsCurrent()) {
    result.phase=WindowsGpuBudgetPhase::topology;
    return result;
  }
  auto& s=result.snapshot;
  s.luid=luid_;s.node_mask=node_mask_;s.captured_ms=captured;s.dedicated_bytes=dedicated_bytes_;
  s.local_budget=local.Budget;s.local_usage=local.CurrentUsage;s.nonlocal_usage=nonlocal.CurrentUsage;s.nonlocal_budget=nonlocal.Budget;
  s.physical_total=host.ullTotalPhys;s.physical_available=host.ullAvailPhys;
  s.commit_limit=host.ullTotalPageFile;s.commit_available=host.ullAvailPageFile;
  s.amd_hardware=true;s.valid=true;result.status=WindowsGpuBudgetStatus::success;
  result.phase=WindowsGpuBudgetPhase::complete;return result;
}
WindowsGpuBudgetResult query_windows_gpu_budget(std::array<std::byte,8> luid,
    std::uint32_t node_mask) noexcept {
  WindowsGpuBudgetSampler sampler(luid,node_mask);
  return sampler.sample();
}
} // namespace flitzfs
