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
Status sole_amd_adapter(std::array<std::byte,8>& luid) noexcept {
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory)
    return Status::topology_unavailable;
  for(UINT ordinal=0;ordinal<=64;++ordinal) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto status=factory->EnumAdapters1(ordinal,adapter.GetAddressOf());
    if(status==DXGI_ERROR_NOT_FOUND)return nonzero(luid)?Status::ready:Status::no_amd_adapter;
    if(ordinal==64 || FAILED(status) || !adapter)return Status::topology_unavailable;
    DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(adapter->GetDesc1(&desc)))return Status::topology_unavailable;
    if(desc.VendorId!=0x1002 || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
    if(nonzero(luid))return Status::ambiguous_adapter;
    luid=std::bit_cast<std::array<std::byte,8>>(desc.AdapterLuid);
    if(!nonzero(luid))return Status::topology_unavailable;
  }
  return Status::topology_unavailable;
}
GpuConfigurationId route_key(const std::array<std::byte,32>& helper,const std::array<std::byte,32>& runtime,
    const HipIdentityObservation& device,std::uint32_t node) {
  std::array<std::byte,88> key{};
  constexpr std::string_view domain="FLHIPR01";
  for(std::size_t i=0;i<domain.size();++i)key[i]=static_cast<std::byte>(domain[i]);
  std::copy(helper.begin(),helper.end(),key.begin()+8);
  std::copy(runtime.begin(),runtime.end(),key.begin()+40);
  std::copy(device.luid.begin(),device.luid.end(),key.begin()+72);
  for(unsigned i=0;i<4;++i) {
    key[80+i]=static_cast<std::byte>(device.ordinal>>(8*i));
    key[84+i]=static_cast<std::byte>(node>>(8*i));
  }
  return detail::record_digest(key);
}
}
class HipRoutePins final {
 public:
  HipRoutePins(std::vector<hip_detail::DirectoryPin> parents, ReadOnlyFile verified_worker,
               std::wstring_view runtime_basename)
      : worker_parents(std::move(parents)), worker(std::move(verified_worker)),
        runtime(runtime_basename) {}
  std::vector<hip_detail::DirectoryPin> worker_parents;
  ReadOnlyFile worker;
  hip_detail::VerifiedSystemModule runtime;
};

HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest& request) {
  HipRouteDiscoveryResult result;
  if(!request.helper.is_absolute() || !ordinary_windows_file_path(request.helper.native()) ||
      request.helper.filename()!=L"flitzfs_hip_lz4_worker.exe" || !nonzero(request.helper_sha256) ||
      !hip_runtime_name(request.runtime_basename) || !valid_process_budget(request.worker) ||
      !request.maximum_identity_queries || request.maximum_identity_queries>64)
    return result;
  if(!WorkerExchangeSession::available()){result.status=Status::transport_busy;return result;}
  std::shared_ptr<HipRoutePins> pinned;
  try {
    auto parents = pin_worker_parent(request.helper);
    auto worker = ReadOnlyFile::open(request.helper);
    if (!worker.size() || worker.size() > 8ULL * 1024 * 1024 ||
        hip_detail::sha256_file(worker) != request.helper_sha256) {
      result.status = Status::worker_untrusted;
      return result;
    }
    pinned = std::make_shared<HipRoutePins>(std::move(parents), std::move(worker),
                                           request.runtime_basename);
  } catch(const HipWorkerFailure&) {result.status=Status::runtime_unavailable;return result;}
    catch(const std::bad_alloc&) {result.status=Status::resource_deferred;return result;}
    catch(const std::exception&) {result.status=Status::worker_untrusted;return result;}

  std::array<std::byte,8> luid{};
  result.status=sole_amd_adapter(luid);
  if(result.status!=Status::ready)return result;
  const auto topology=query_windows_gpu_topology(luid);
  if(!verified_single_gpu_node(topology,luid)){result.status=Status::topology_unavailable;return result;}
  constexpr std::uint32_t node=1; // Verified single D3D node.
  const auto deadline=GetTickCount64()+request.worker.timeout_ms;
  for(std::uint32_t ordinal=0;ordinal<request.maximum_identity_queries;++ordinal) {
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
      if(match.luid!=luid)continue; // DXGI's enumeration order is not a HIP ordinal.
      if(match.node_mask && match.node_mask!=node) {
        result.status=Status::identity_mismatch;return result;
      }
      const auto fresh=query_windows_gpu_budget(luid,node);
      if(GetTickCount64()>=deadline || fresh.status!=WindowsGpuBudgetStatus::success ||
          check_gpu_memory_budget(fresh.snapshot,luid,node,GetTickCount64(),0)!=GpuGateCode::admitted) {
        result.status=Status::resource_deferred;return result;
      }
      result.route.emplace(HipDiscoveredRoute{route_key(request.helper_sha256,pinned->runtime.digest(),match,node),
          pinned->runtime.digest(),fresh.snapshot,ordinal,std::move(pinned)});
      result.status=Status::ready;return result;
    } catch(const std::bad_alloc&) {
      result.status=Status::resource_deferred;return result;
    } catch(const std::exception&) {
      result.status=Status::identity_unavailable;return result;
    }
  }
  result.status=Status::identity_mismatch;return result;
}
}
