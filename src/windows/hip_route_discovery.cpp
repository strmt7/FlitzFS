#include "hip_route_discovery.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
#include "hip_identity_wire.hpp"
#include "hip_module_verification.hpp"
#include "worker_exchange.hpp"
#include "flitzfs/windows_path.hpp"
#include "record_digest.hpp"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <new>
#include <vector>
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
#include "hip_installed_package_config.hpp"
#endif
namespace flitzfs {
namespace {
using Status=HipDiscoveryStatus;
template<std::size_t N>
bool nonzero(const std::array<std::byte,N>& id) noexcept {
  return std::any_of(id.begin(),id.end(),[](std::byte b){return b!=std::byte{};});
}
template<std::size_t N>
std::wstring hex(const std::array<std::byte,N>& bytes) {
  constexpr wchar_t digits[]=L"0123456789abcdef";
  std::wstring result;result.reserve(bytes.size()*2);
  for(const auto b:bytes) {
    const auto v=std::to_integer<unsigned>(b);
    result.push_back(digits[v>>4]);result.push_back(digits[v&15]);
  }
  return result;
}
bool hip_runtime_name(std::wstring_view name) noexcept {
  if(name==L"amdhip64.dll")return true;
  constexpr std::wstring_view prefix=L"amdhip64_",suffix=L".dll";
  if(!name.starts_with(prefix) || !name.ends_with(suffix) || name.size()<=prefix.size()+suffix.size())return false;
  const auto major=name.substr(prefix.size(),name.size()-prefix.size()-suffix.size());
  return major.size()<=3 && major.front()!=L'0' &&
      std::all_of(major.begin(),major.end(),[](wchar_t v){return v>=L'0'&&v<=L'9';});
}
std::vector<hip_detail::DirectoryPin> pin_worker_parent(const std::filesystem::path& helper) {
  auto parent=helper.root_path();
  std::vector<hip_detail::DirectoryPin> pins;pins.emplace_back(parent);
  for(const auto& part:helper.parent_path().relative_path()) {
    if(part==L"." || part==L".." || pins.size()>=32)throw std::invalid_argument("Unsafe HIP worker ancestry");
    parent/=part;pins.emplace_back(parent);
  }
  return pins;
}
struct DxgiAmdAdapter {
  std::array<std::byte,8> luid;
  std::uint64_t dedicated_bytes;
};

Status enumerate_amd_adapters(std::vector<DxgiAmdAdapter>& adapters,std::uint64_t deadline) {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory)
    return Status::topology_unavailable;
  for(UINT ordinal=0;GetTickCount64()<deadline;++ordinal) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto status=factory->EnumAdapters1(ordinal,adapter.GetAddressOf());
    if(status==DXGI_ERROR_NOT_FOUND)return adapters.empty()?Status::no_amd_adapter:Status::ready;
    if(FAILED(status) || !adapter)return Status::topology_unavailable;
    DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(adapter->GetDesc1(&desc)))return Status::topology_unavailable;
    if(desc.VendorId!=0x1002 || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
    const auto luid=std::bit_cast<std::array<std::byte,8>>(desc.AdapterLuid);
    if(nonzero(luid))adapters.push_back({luid,desc.DedicatedVideoMemory});
  }
  return Status::resource_deferred;
}
GpuConfigurationId route_key(const std::array<std::byte,32>& helper,const std::array<std::byte,32>& runtime,
    const std::array<std::byte,32>& module,const HipIdentityObservation& device,std::uint32_t node) {
  std::array<std::byte,120> key{};
  constexpr std::string_view domain="FLHIPR02";
  for(std::size_t i=0;i<domain.size();++i)key[i]=static_cast<std::byte>(domain[i]);
  std::copy(helper.begin(),helper.end(),key.begin()+8);
  std::copy(runtime.begin(),runtime.end(),key.begin()+40);
  std::copy(device.luid.begin(),device.luid.end(),key.begin()+72);
  for(unsigned i=0;i<4;++i) {
    key[80+i]=static_cast<std::byte>(device.ordinal>>(8*i));
    key[84+i]=static_cast<std::byte>(node>>(8*i));
  }
  std::copy(module.begin(),module.end(),key.begin()+88);
  return detail::record_digest(key);
}
struct HipRouteResources {
  std::vector<hip_detail::DirectoryPin> directories;
  ReadOnlyFile worker;
  std::optional<ReadOnlyFile> module;
  hip_detail::VerifiedSystemModule runtime;

  HipRouteResources(std::vector<hip_detail::DirectoryPin> parents,ReadOnlyFile verified_worker,
      std::optional<ReadOnlyFile> verified_module,std::wstring_view runtime_basename)
      :directories(std::move(parents)),worker(std::move(verified_worker)),
       module(std::move(verified_module)),runtime(runtime_basename){}
};
} // namespace

HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest& request) {
  HipRouteDiscoveryResult result;
  const bool anchor=nonzero(request.anchor_module_sha256);
  const auto* helper_name=anchor?L"flitzfs_hip_worker.exe":L"flitzfs_hip_lz4_worker.exe";
  const auto& module_digest=anchor?request.anchor_module_sha256:request.uniform_module_sha256;
  if(!request.helper.is_absolute() || !ordinary_windows_file_path(request.helper.native()) ||
      request.helper.filename()!=helper_name || (anchor && nonzero(request.uniform_module_sha256)) ||
      !nonzero(request.helper_sha256) ||
      !hip_runtime_name(request.runtime_basename) || !valid_process_budget(request.worker) ||
      !request.maximum_identity_queries || request.maximum_identity_queries>64)
    return result;
  if(!WorkerExchangeSession::available()){result.status=Status::transport_busy;return result;}
  std::shared_ptr<HipRouteResources> pinned;
  try {
    auto parents = pin_worker_parent(request.helper);
    auto worker = ReadOnlyFile::open(request.helper);
    if (!worker.size() || worker.size() > 8ULL * 1024 * 1024 ||
        hip_detail::sha256_file(worker) != request.helper_sha256) {
      result.status = Status::worker_untrusted;
      return result;
    }
    std::optional<ReadOnlyFile> module;
    if(nonzero(module_digest)) {
      const auto directory=request.helper.parent_path()/(anchor?L"hip":L"hip-uniform-lz4");
      parents.emplace_back(directory);
      module.emplace(ReadOnlyFile::open(directory/(anchor?L"flitzfs_rocm_kernels.dll":L"flitzfs_rocm_uniform_lz4_candidate.dll")));
      if(!module->size() || module->size()>128ULL*1024*1024 ||
          hip_detail::sha256_file(*module)!=module_digest) {
        result.status=Status::worker_untrusted;
        return result;
      }
    }
    pinned=std::make_shared<HipRouteResources>(std::move(parents),std::move(worker),
        std::move(module),request.runtime_basename);
  } catch(const HipWorkerFailure&) {result.status=Status::runtime_unavailable;return result;}
    catch(const std::bad_alloc&) {result.status=Status::resource_deferred;return result;}
    catch(const std::exception&) {result.status=Status::worker_untrusted;return result;}

  const auto deadline=GetTickCount64()+request.worker.timeout_ms;
  std::vector<DxgiAmdAdapter> adapters;
  try {
    result.status=enumerate_amd_adapters(adapters,deadline);
    if(result.status!=Status::ready)return result;
    std::stable_sort(adapters.begin(),adapters.end(),[](const auto& a,const auto& b){
      return a.dedicated_bytes>b.dedicated_bytes;
    });
  }catch(const std::bad_alloc&){result.status=Status::resource_deferred;return result;}
  // A budget-eligible DXGI adapter gives the metadata worker a budget to
  // check before runtime initialization. The HIP ordinal is matched separately.
  auto bootstrap=adapters.begin();
  for(;bootstrap!=adapters.end() && GetTickCount64()<deadline;++bootstrap) {
    if(verified_single_gpu_node(query_windows_gpu_topology(bootstrap->luid),bootstrap->luid))break;
  }
  if(GetTickCount64()>=deadline){result.status=Status::resource_deferred;return result;}
  if(bootstrap==adapters.end()){result.status=Status::topology_unavailable;return result;}
  constexpr std::uint32_t node=1; // Verified single D3D node.
  for(std::uint32_t ordinal=0;ordinal<request.maximum_identity_queries;++ordinal) {
    const auto& luid=bootstrap->luid;
    const auto observation=query_windows_gpu_budget(luid,node);
    const auto now=GetTickCount64();
    if(now>=deadline || observation.status!=WindowsGpuBudgetStatus::success ||
        check_gpu_memory_budget(observation.snapshot,luid,node,now,0)!=GpuGateCode::admitted) {
      result.status=Status::resource_deferred;return result;
    }
    auto remaining=request.worker;remaining.timeout_ms=static_cast<std::uint32_t>(deadline-now);
    const HipIdentityRequest identity{std::uint64_t(ordinal)+1,ordinal,node,luid};
    try {
      ++result.identity_queries;
      const auto exchange=run_worker_exchange(request.helper,
          {L"--inspect-system-adapter",std::to_wstring(ordinal),hex(luid),hex(pinned->runtime.digest())},
          encode_hip_identity_request(identity),remaining);
      result.worker_exit=exchange.process.exit_code;
      if(exchange.code==WorkerExchangeCode::exit_unconfirmed ||
          exchange.code==WorkerExchangeCode::supervisor_exit_unconfirmed) {
        result.status=Status::retirement_unconfirmed;return result;
      }
      if(exchange.code!=WorkerExchangeCode::completed || !exchange.process.exit_confirmed ||
          exchange.process.timed_out || exchange.process.cancelled || exchange.process.exit_code) {
        result.status=Status::identity_unavailable;return result;
      }
      const auto match=decode_hip_identity_reply(exchange.response,identity);
      if(match.node_mask && match.node_mask!=node)continue;
      if(match.luid!=luid) {
        const auto matching=std::find_if(adapters.begin(),adapters.end(),[&](const auto& candidate){
          return candidate.luid==match.luid;
        });
        if(matching==adapters.end())continue;
        if(GetTickCount64()>=deadline){result.status=Status::resource_deferred;return result;}
        if(!verified_single_gpu_node(query_windows_gpu_topology(match.luid),match.luid))continue;
      }
      const auto fresh=query_windows_gpu_budget(match.luid,node);
      if(GetTickCount64()>=deadline || fresh.status!=WindowsGpuBudgetStatus::success ||
          check_gpu_memory_budget(fresh.snapshot,match.luid,node,GetTickCount64(),0)!=GpuGateCode::admitted) {
        result.status=Status::resource_deferred;return result;
      }
      const auto runtime=pinned->runtime.digest();
      const auto configuration=route_key(request.helper_sha256,runtime,module_digest,match,node);
      const auto module_pinned=pinned->module.has_value();
      auto binding=std::shared_ptr<const HipRoutePins>(new HipRoutePins(request.helper,configuration,
          runtime,ordinal,fresh.snapshot,module_pinned && !anchor,module_pinned && anchor,std::move(pinned)));
      result.route.emplace(HipDiscoveredRoute{configuration,runtime,fresh.snapshot,ordinal,
          std::move(binding),request.helper,request.runtime_basename});
      result.status=Status::ready;return result;
    } catch(const std::bad_alloc&) {
      result.status=Status::resource_deferred;return result;
    } catch(const std::exception&) {
      result.status=Status::identity_unavailable;return result;
    }
  }
  result.status=Status::identity_mismatch;return result;
}

HipRouteDiscoveryResult discover_installed_hip_route(ProcessBudget budget,std::uint32_t maximum_identity_queries) {
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  if(!valid_process_budget(budget) || !maximum_identity_queries || maximum_identity_queries>64)return {};
  try {
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!length || length>=path.size())return {Status::package_unavailable};
    const auto directory=std::filesystem::path(std::wstring_view(path.data(),length)).parent_path()/L"amd";
    HipRouteDiscoveryRequest request;
    request.helper=directory/L"flitzfs_hip_lz4_worker.exe";
    if(GetFileAttributesW(request.helper.c_str())==INVALID_FILE_ATTRIBUTES) {
      const auto error=GetLastError();
      return {error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?
          Status::package_unavailable:Status::worker_untrusted};
    }
    request.helper_sha256=installed_amd_package::worker_sha256;
    request.uniform_module_sha256=installed_amd_package::uniform_module_sha256;
    request.runtime_basename=installed_amd_package::runtime_basename;
    request.worker=budget;
    request.maximum_identity_queries=maximum_identity_queries;
    return discover_system_hip_route(request);
  }catch(const std::bad_alloc&){return {Status::resource_deferred};}
  catch(const std::exception&){return {Status::worker_untrusted};}
#else
  (void)budget;
  (void)maximum_identity_queries;
  return {Status::package_unavailable};
#endif
}
}
