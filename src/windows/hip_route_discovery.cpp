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
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL
constexpr HipAdapterHardware installed_write_trial_hardware{
    installed_amd_package::write_trial_vendor_id,installed_amd_package::write_trial_device_id,
    installed_amd_package::write_trial_subsystem_id,installed_amd_package::write_trial_revision};
#endif
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_ANCHOR
constexpr HipAdapterHardware installed_anchor_hardware{
    installed_amd_package::anchor_vendor_id,installed_amd_package::anchor_device_id,
    installed_amd_package::anchor_subsystem_id,installed_amd_package::anchor_revision};
#endif
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ
constexpr HipAdapterHardware installed_read_hardware{
    installed_amd_package::read_vendor_id,installed_amd_package::read_device_id,
    installed_amd_package::read_subsystem_id,installed_amd_package::read_revision};
#endif
struct RouteFiles {
  const wchar_t* worker_directory;
  const wchar_t* worker;
  const wchar_t* module_directory;
  const wchar_t* module;
};
constexpr RouteFiles route_files(HipRouteRole role) noexcept {
  switch(role) {
    case HipRouteRole::anchor:
      return {nullptr,L"flitzfs_hip_worker.exe",L"hip",L"flitzfs_rocm_kernels.dll"};
    case HipRouteRole::native_lz4_read:
      return {L"native-lz4-read",L"flitzfs_native_lz4_read_worker.exe",nullptr,L"flitzfs_rocm_native_lz4_decode.dll"};
    default:
      return {nullptr,L"flitzfs_hip_lz4_worker.exe",L"hip-uniform-lz4",L"flitzfs_rocm_uniform_lz4_candidate.dll"};
  }
}
std::filesystem::path route_module(const std::filesystem::path& worker,RouteFiles files) {
  auto parent=worker.parent_path();
  if(files.module_directory)parent/=files.module_directory;
  return parent/files.module;
}
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
  HipAdapterHardware hardware;
  std::vector<std::wstring> device_instances;
};

Status check_display_readiness(const WindowsAmdDisplaySnapshot& display,const HipAdapterHardware* expected) noexcept {
  using State=WindowsAmdDisplayStatus;
  switch(display.status(expected)) {
    case State::ready:return Status::ready;
    case State::not_ready:return Status::device_not_ready;
    case State::absent:return expected && !display.devices.empty()?Status::profile_mismatch:Status::no_amd_adapter;
    case State::unavailable:return Status::device_not_ready;
  }
  return Status::topology_unavailable;
}

Status enumerate_amd_adapters(std::vector<DxgiAmdAdapter>& adapters,const WindowsAmdDisplaySnapshot& display,
    const HipAdapterHardware* expected,std::uint64_t deadline) {
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
    const HipAdapterHardware hardware{desc.VendorId,desc.DeviceId,desc.SubSysId,desc.Revision};
    if(expected && hardware!=*expected)continue;
    const auto luid=std::bit_cast<std::array<std::byte,8>>(desc.AdapterLuid);
    if(!nonzero(luid))continue;
    auto devices=display.bind(hardware);
    if(!devices.empty())
      adapters.push_back({luid,desc.DedicatedVideoMemory,hardware,std::move(devices)});
  }
  return Status::resource_deferred;
}
bool admit_discovery_budget(HipRouteDiscoveryResult& result, const WindowsGpuBudgetResult& observation,
    std::array<std::byte,8> luid, std::uint32_t node, std::uint64_t deadline) noexcept {
  const auto now = GetTickCount64();
  if (now < deadline && observation.status == WindowsGpuBudgetStatus::success) {
    const auto code = check_gpu_memory_budget(observation.snapshot, luid, node, now, 0);
    if (code == GpuGateCode::admitted) return true;
    if (code == GpuGateCode::insufficient_headroom) result.memory_pressure = observation.snapshot;
  }
  result.status = Status::resource_deferred;
  return false;
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

HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest& request,std::stop_token stop) {
  HipRouteDiscoveryResult result;
  const bool anchor=nonzero(request.anchor_module_sha256);
  const bool read=nonzero(request.native_read_module_sha256);
  const auto role=read?HipRouteRole::native_lz4_read:anchor?HipRouteRole::anchor:HipRouteRole::uniform;
  const auto files=route_files(role);
  const auto& module_digest=read?request.native_read_module_sha256:
      anchor?request.anchor_module_sha256:request.uniform_module_sha256;
  if(!request.helper.is_absolute() || !ordinary_windows_file_path(request.helper.native()) ||
      request.helper.filename()!=files.worker || (anchor && read) ||
      ((anchor || read) && nonzero(request.uniform_module_sha256)) ||
      !nonzero(request.helper_sha256) || (read && !request.qualified_device) ||
      (request.qualified_device && (!nonzero(module_digest) ||
          request.qualified_device->hardware.vendor_id!=0x1002 ||
          !request.qualified_device->hardware.device_id || !nonzero(request.qualified_device->runtime_sha256))) ||
      !hip_runtime_name(request.runtime_basename) || !valid_process_budget(request.worker) ||
      !request.maximum_identity_queries || request.maximum_identity_queries>64)
    return result;
  if(stop.stop_requested())return {Status::cancelled};
  if(!WorkerExchangeSession::available()){result.status=Status::transport_busy;return result;}
  const auto* expected=request.qualified_device?&request.qualified_device->hardware:nullptr;
  WindowsAmdDisplaySnapshot display;
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
      const auto path=route_module(request.helper,files);
      if(files.module_directory)parents.emplace_back(path.parent_path());
      module.emplace(ReadOnlyFile::open(path));
      if(!module->size() || module->size()>128ULL*1024*1024 ||
          hip_detail::sha256_file(*module)!=module_digest) {
        result.status=Status::worker_untrusted;
        return result;
      }
    }
    if(stop.stop_requested())return {Status::cancelled};
    display=observe_windows_amd_displays();
    result.status=check_display_readiness(display,expected);
    if(result.status!=Status::ready)return result;
    pinned=std::make_shared<HipRouteResources>(std::move(parents),std::move(worker),
        std::move(module),request.runtime_basename);
  } catch(const HipWorkerFailure&) {result.status=Status::runtime_unavailable;return result;}
    catch(const std::bad_alloc&) {result.status=Status::resource_deferred;return result;}
    catch(const std::exception&) {result.status=Status::worker_untrusted;return result;}
  if(stop.stop_requested())return {Status::cancelled};
  if(request.qualified_device && pinned->runtime.digest()!=request.qualified_device->runtime_sha256)
    return {Status::profile_mismatch};

  const auto deadline=GetTickCount64()+request.worker.timeout_ms;
  std::vector<DxgiAmdAdapter> adapters;
  try {
    display=observe_windows_amd_displays();
    result.status=check_display_readiness(display,expected);
    if(result.status!=Status::ready)return result;
    result.status=enumerate_amd_adapters(adapters,display,expected,deadline);
    if(result.status!=Status::ready)return result;
    std::stable_sort(adapters.begin(),adapters.end(),[](const auto& a,const auto& b){
      return a.dedicated_bytes>b.dedicated_bytes;
    });
  }catch(const std::bad_alloc&){result.status=Status::resource_deferred;return result;}
  // A budget-eligible DXGI adapter gives the metadata worker a budget to
  // check before runtime initialization. The HIP ordinal is matched separately.
  auto bootstrap=adapters.begin();
  bool ready_device_seen=false;
  for(;bootstrap!=adapters.end() && GetTickCount64()<deadline;++bootstrap) {
    if(!bound_windows_amd_displays_ready(bootstrap->device_instances))continue;
    ready_device_seen=true;
    if(verified_single_gpu_node(query_windows_gpu_topology(bootstrap->luid),bootstrap->luid))break;
  }
  if(GetTickCount64()>=deadline){result.status=Status::resource_deferred;return result;}
  if(bootstrap==adapters.end())
    return {ready_device_seen?Status::topology_unavailable:Status::device_not_ready};
  constexpr std::uint32_t node=1; // Verified single D3D node.
  for(std::uint32_t ordinal=0;ordinal<request.maximum_identity_queries;++ordinal) {
    if(stop.stop_requested())return {Status::cancelled,0,result.identity_queries};
    if(!bound_windows_amd_displays_ready(bootstrap->device_instances))
      return {Status::device_not_ready,0,result.identity_queries};
    const auto& luid=bootstrap->luid;
    const auto observation=query_windows_gpu_budget(luid,node);
    if (!admit_discovery_budget(result, observation, luid, node, deadline)) return result;
    if(!bound_windows_amd_displays_ready(bootstrap->device_instances))
      return {Status::device_not_ready,0,result.identity_queries};
    const auto dispatch_time=GetTickCount64();
    if(dispatch_time>=deadline)return {Status::resource_deferred,0,result.identity_queries};
    auto remaining=request.worker;remaining.timeout_ms=static_cast<std::uint32_t>(deadline-dispatch_time);
    const HipIdentityRequest identity{std::uint64_t(ordinal)+1,ordinal,node,luid};
    try {
      ++result.identity_queries;
      const auto command=read?L"--inspect-system-adapter-sized":L"--inspect-system-adapter";
      const auto exchange=run_worker_exchange(request.helper,
          {command,std::to_wstring(ordinal),hex(luid),hex(pinned->runtime.digest())},
          encode_hip_identity_request(identity),remaining,stop,pinned);
      result.worker_exit=exchange.process.exit_code;
      if(exchange.code==WorkerExchangeCode::exit_unconfirmed ||
          exchange.code==WorkerExchangeCode::supervisor_exit_unconfirmed) {
        result.status=Status::retirement_unconfirmed;return result;
      }
      if(stop.stop_requested() || exchange.code==WorkerExchangeCode::cancelled) {
        result.status=Status::cancelled;return result;
      }
      if(exchange.code!=WorkerExchangeCode::completed || !exchange.process.exit_confirmed ||
          exchange.process.timed_out || exchange.process.cancelled || exchange.process.exit_code) {
        result.status=Status::identity_unavailable;return result;
      }
      const auto match=decode_hip_identity_reply(exchange.response,identity);
      if(match.node_mask && match.node_mask!=node)continue;
      const auto matching=std::find_if(adapters.begin(),adapters.end(),[&](const auto& candidate){
        return candidate.luid==match.luid;
      });
      if(matching==adapters.end())continue;
      if(!bound_windows_amd_displays_ready(matching->device_instances))
        return {Status::device_not_ready,result.worker_exit,result.identity_queries};
      if(match.luid!=luid) {
        if(GetTickCount64()>=deadline){result.status=Status::resource_deferred;return result;}
        if(!verified_single_gpu_node(query_windows_gpu_topology(match.luid),match.luid))continue;
      }
      const auto fresh=query_windows_gpu_budget(match.luid,node);
      if (!admit_discovery_budget(result, fresh, match.luid, node, deadline)) return result;
      if(stop.stop_requested())return {Status::cancelled,result.worker_exit,result.identity_queries};
      const auto runtime=pinned->runtime.digest();
      const auto configuration=route_key(request.helper_sha256,runtime,module_digest,match,node);
      const auto module_pinned=pinned->module.has_value();
      auto binding=std::shared_ptr<const HipRoutePins>(new HipRoutePins(request.helper,configuration,
          runtime,ordinal,fresh.snapshot,role,module_pinned,
          std::move(matching->device_instances),std::move(pinned)));
      result.route.emplace(HipDiscoveredRoute{configuration,runtime,fresh.snapshot,ordinal,
          std::move(binding),request.helper,request.runtime_basename,matching->hardware});
      result.status=Status::ready;return result;
    } catch(const std::bad_alloc&) {
      result.status=Status::resource_deferred;return result;
    } catch(const std::exception&) {
      result.status=Status::identity_unavailable;return result;
    }
  }
  result.status=Status::identity_mismatch;return result;
}

#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
namespace {
std::filesystem::path application_amd_directory() {
  std::array<wchar_t,32768> path{};
  const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
  if(!length || length>=path.size())return {};
  return std::filesystem::path(std::wstring_view(path.data(),length)).parent_path()/L"amd";
}

std::filesystem::path package_worker(std::filesystem::path directory,HipRouteRole role) {
  const auto files=route_files(role);
  if(files.worker_directory)directory/=files.worker_directory;
  return directory/files.worker;
}

HipRouteDiscoveryResult discover_application_package(HipRouteDiscoveryRequest request,HipRouteRole role,
    std::stop_token stop) {
  if(stop.stop_requested())return {Status::cancelled};
  try {
    const auto directory=application_amd_directory();
    if(directory.empty())return {Status::package_unavailable};
    request.helper=package_worker(directory,role);
    const auto module=route_module(request.helper,route_files(role));
    const std::filesystem::path* files[]{&request.helper,&module};
    for(const auto* path:files) {
      if(GetFileAttributesW(path->c_str())==INVALID_FILE_ATTRIBUTES) {
        const auto error=GetLastError();
        return {error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND?
            Status::package_unavailable:Status::worker_untrusted};
      }
    }
    request.runtime_basename=installed_amd_package::runtime_basename;
    return discover_system_hip_route(request,stop);
  }catch(const std::bad_alloc&){return {Status::resource_deferred};}
  catch(const std::exception&){return {Status::worker_untrusted};}
}
}
#endif

namespace {
struct InstalledInputObservation {
  bool present=false;
  std::uint64_t volume=0;
  std::array<unsigned char,16> file_id{};
  std::int64_t bytes=0,write_time=0,change_time=0;
  bool operator==(const InstalledInputObservation&) const noexcept=default;
};

#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
std::optional<HipAdapterHardware> installed_hardware(HipRouteRole role) noexcept {
#if FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL
  if(role==HipRouteRole::uniform)return installed_write_trial_hardware;
#endif
#if FLITZFS_INSTALLED_AMD_HAS_ANCHOR
  if(role==HipRouteRole::anchor)return installed_anchor_hardware;
#endif
#if FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ
  if(role==HipRouteRole::native_lz4_read)return installed_read_hardware;
#endif
  (void)role;
  return {};
}
std::optional<InstalledInputObservation> observe_installed_input(const std::filesystem::path& path) noexcept {
  if(path.empty() || !ordinary_windows_file_path(path.native()))return {};
  const auto file=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
      nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(file==INVALID_HANDLE_VALUE) {
    const auto error=GetLastError();
    if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return InstalledInputObservation{};
    return {};
  }
  FILE_ID_INFO id{};
  FILE_BASIC_INFO basic{};
  FILE_STANDARD_INFO standard{};
  const bool valid=GetFileType(file)==FILE_TYPE_DISK &&
      GetFileInformationByHandleEx(file,FileBasicInfo,&basic,sizeof(basic)) &&
      !(basic.FileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) &&
      GetFileInformationByHandleEx(file,FileIdInfo,&id,sizeof(id)) &&
      GetFileInformationByHandleEx(file,FileStandardInfo,&standard,sizeof(standard));
  CloseHandle(file);
  if(!valid)return {};
  return InstalledInputObservation{true,id.VolumeSerialNumber,std::bit_cast<std::array<unsigned char,16>>(id.FileId),
      standard.EndOfFile.QuadPart,basic.LastWriteTime.QuadPart,basic.ChangeTime.QuadPart};
}
#endif
} // namespace

bool recovered_hip_discovery_headroom(const GpuMemorySnapshot& denied,
    const GpuMemorySnapshot& observed, std::uint64_t now_ms) noexcept {
  return observed.captured_ms > denied.captured_ms && observed.dedicated_bytes == denied.dedicated_bytes &&
      check_gpu_memory_budget(denied, denied.luid, denied.node_mask, denied.captured_ms, 0) ==
          GpuGateCode::insufficient_headroom &&
      check_gpu_memory_budget(observed, denied.luid, denied.node_mask, now_ms, 0) == GpuGateCode::admitted;
}

struct InstalledHipInputWitness::Impl {
  struct MemoryPressure {
    GpuMemorySnapshot denied;
    WindowsGpuBudgetSampler sampler;
    explicit MemoryPressure(const GpuMemorySnapshot& snapshot) noexcept
        :denied(snapshot), sampler(snapshot.luid, snapshot.node_mask) {}
  };
  std::optional<MemoryPressure> pressure;
  std::array<std::filesystem::path,3> paths;
  std::array<std::optional<InstalledInputObservation>,3> before;
  std::optional<HipAdapterHardware> hardware;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  bool device_unavailable=false;
};
InstalledHipInputWitness::InstalledHipInputWitness(HipRouteRole role):impl_(std::make_unique<Impl>()) {
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  impl_->hardware=installed_hardware(role);
  if(!impl_->hardware)return;
  const auto directory=application_amd_directory();
  if(!directory.empty()) {
    impl_->paths[0]=package_worker(directory,role);
    impl_->paths[1]=route_module(impl_->paths[0],route_files(role));
  }
  std::array<wchar_t,32768> system{};
  const auto length=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));
  if(length && length<system.size())
    impl_->paths[2]=std::filesystem::path(std::wstring_view(system.data(),length))/installed_amd_package::runtime_basename;
  for(std::size_t i=0;i<impl_->paths.size();++i)impl_->before[i]=observe_installed_input(impl_->paths[i]);
  impl_->device_unavailable=check_display_readiness(observe_windows_amd_displays(),&*impl_->hardware)!=Status::ready;
  const bool have_package=impl_->before[0] && impl_->before[0]->present && impl_->before[1] && impl_->before[1]->present;
  if(!impl_->device_unavailable && have_package &&
      FAILED(CreateDXGIFactory1(IID_PPV_ARGS(impl_->factory.GetAddressOf()))))impl_->factory.Reset();
#else
  (void)role;
#endif
}
InstalledHipInputWitness::~InstalledHipInputWitness()=default;
void InstalledHipInputWitness::note_device_unavailable() noexcept {impl_->device_unavailable=true;}
void InstalledHipInputWitness::note_memory_pressure(const GpuMemorySnapshot& denied) noexcept {
  impl_->pressure.emplace(denied);
}
bool InstalledHipInputWitness::changed() noexcept {
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  if(!impl_->hardware)return false;
  if(check_display_readiness(observe_windows_amd_displays(),&*impl_->hardware)!=Status::ready) {
    note_device_unavailable();
    return false;
  }
  if(impl_->device_unavailable)return true;
  for(std::size_t i=0;i<impl_->paths.size();++i) {
    const auto current=observe_installed_input(impl_->paths[i]);
    if(current && impl_->before[i] && *current!=*impl_->before[i])return true;
  }
  if (impl_->factory && !impl_->factory->IsCurrent()) return true;
  if (!impl_->pressure) return false;
  auto& pressure = *impl_->pressure;
  const auto current = pressure.sampler.sample();
  return current.status == WindowsGpuBudgetStatus::success &&
      recovered_hip_discovery_headroom(pressure.denied, current.snapshot, GetTickCount64());
#else
  return false;
#endif
}

bool has_configured_uniform_write_package() noexcept {
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  return installed_amd_package::has_uniform;
#else
  return false;
#endif
}

bool installed_uniform_write_files_present() noexcept {
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  if(!has_configured_uniform_write_package())return false;
  try {
    const auto directory=application_amd_directory();
    if(directory.empty())return false;
    const auto worker=package_worker(directory,HipRouteRole::uniform);
    const auto module=route_module(worker,route_files(HipRouteRole::uniform));
    const auto worker_state=observe_installed_input(worker);
    const auto module_state=observe_installed_input(module);
    return worker_state && worker_state->present && module_state && module_state->present;
  }catch(...) {}
#endif
  return false;
}

HipRouteDiscoveryResult discover_installed_hip_route(ProcessBudget budget,std::uint32_t maximum_identity_queries,
    std::stop_token stop) {
  if(stop.stop_requested())return {Status::cancelled};
#ifdef FLITZFS_BIND_INSTALLED_AMD_PACKAGE
  if(!valid_process_budget(budget) || !maximum_identity_queries || maximum_identity_queries>64)return {};
  if(!has_configured_uniform_write_package())return {Status::package_unavailable};
  HipRouteDiscoveryRequest request;
  request.helper_sha256=installed_amd_package::worker_sha256;
  request.uniform_module_sha256=installed_amd_package::uniform_module_sha256;
  request.worker=budget;
  request.maximum_identity_queries=maximum_identity_queries;
  return discover_application_package(std::move(request),HipRouteRole::uniform,stop);
#else
  (void)budget;
  (void)maximum_identity_queries;
  return {Status::package_unavailable};
#endif
}

std::optional<InstalledUniformWriteTrialProfile> installed_uniform_write_trial_profile() noexcept {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL
  using namespace installed_amd_package;
  return InstalledUniformWriteTrialProfile{{write_trial_worker_memory_mib,write_trial_worker_cpu_rate,
      write_trial_worker_timeout_ms,write_trial_memory_share_divisor},write_trial_maximum_batch_chunks,
      write_trial_maximum_launch_chunks,write_trial_separate_requests,write_trial_await_later_writes};
#else
  return {};
#endif
}

HipRouteDiscoveryResult discover_installed_hip_write_trial_route(std::stop_token stop) {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL
  using namespace installed_amd_package;
  HipRouteDiscoveryRequest request;
  request.worker=installed_uniform_write_trial_profile()->worker;
  request.maximum_identity_queries=write_trial_maximum_identity_queries;
  request.helper_sha256=worker_sha256;
  request.uniform_module_sha256=uniform_module_sha256;
  request.qualified_device=HipQualifiedDeviceBinding{installed_write_trial_hardware,write_trial_runtime_sha256};
  return discover_application_package(std::move(request),HipRouteRole::uniform,stop);
#else
  (void)stop;
  return {Status::package_unavailable};
#endif
}

std::optional<InstalledAnchorProfile> installed_anchor_profile() noexcept {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_ANCHOR
  using namespace installed_amd_package;
  using Output=anchor_match::BatchOutput;
  const auto output=anchor_maximum_dispatch_members>anchor_match::max_members?Output::wide_anchors:Output::anchors;
  return InstalledAnchorProfile{{anchor_worker_memory_mib,anchor_worker_cpu_rate,anchor_worker_timeout_ms,
      anchor_memory_share_divisor},anchor_minimum_members,anchor_maximum_members,anchor_duty_ppm,
      anchor_dispatches_per_wait,anchor_maximum_dispatch_members,output,anchor_reference_xor};
#else
  return {};
#endif
}

HipRouteDiscoveryResult discover_installed_hip_anchor_route(std::stop_token stop) {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_ANCHOR
  using namespace installed_amd_package;
  HipRouteDiscoveryRequest request;
  request.worker=installed_anchor_profile()->worker;
  request.maximum_identity_queries=anchor_maximum_identity_queries;
  request.helper_sha256=anchor_worker_sha256;
  request.anchor_module_sha256=anchor_module_sha256;
  request.qualified_device=HipQualifiedDeviceBinding{installed_anchor_hardware,anchor_runtime_sha256};
  return discover_application_package(std::move(request),HipRouteRole::anchor,stop);
#else
  (void)stop;
  return {Status::package_unavailable};
#endif
}

std::optional<InstalledNativeReadProfile> installed_native_read_profile() noexcept {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ
  using namespace installed_amd_package;
  return InstalledNativeReadProfile{{read_worker_memory_mib,read_worker_cpu_rate,read_worker_timeout_ms,
      read_memory_share_divisor},read_minimum_frames,read_maximum_frames,read_minimum_lead_ms,read_maximum_slab_bytes};
#else
  return {};
#endif
}

HipRouteDiscoveryResult discover_installed_hip_native_read_route(std::stop_token stop) {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ
  using namespace installed_amd_package;
  HipRouteDiscoveryRequest request;
  request.worker=installed_native_read_profile()->worker;
  request.maximum_identity_queries=read_maximum_identity_queries;
  request.helper_sha256=read_worker_sha256;
  request.native_read_module_sha256=read_module_sha256;
  request.qualified_device=HipQualifiedDeviceBinding{installed_read_hardware,read_runtime_sha256};
  return discover_application_package(std::move(request),HipRouteRole::native_lz4_read,stop);
#else
  (void)stop;
  return {Status::package_unavailable};
#endif
}

std::optional<NativeLz4ReadWorkerProfile> bind_installed_native_read(const HipDiscoveredRoute& route) {
#if defined(FLITZFS_BIND_INSTALLED_AMD_PACKAGE) && FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ
  using namespace installed_amd_package;
  if(!route.pins || !route.pins->native_read_module_pinned() ||
      route.hardware!=installed_read_hardware || route.runtime_sha256!=read_runtime_sha256 ||
      route.runtime_basename!=runtime_basename ||
      !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory))
    return {};
  const auto ascii_hex=[](const auto& hash) {
    const auto value=hex(hash);
    return std::string(value.begin(),value.end());
  };
  const auto p=*installed_native_read_profile();
  return NativeLz4ReadWorkerProfile{route.helper,route.runtime_basename,read_architecture,
      ascii_hex(read_worker_sha256),ascii_hex(read_runtime_sha256),ascii_hex(read_module_sha256),
      route.memory.luid,route.memory.node_mask,route.ordinal,read_wave_mask,p.worker,
      p.minimum_frames,p.maximum_frames,p.maximum_slab_bytes,p.minimum_lead_ms};
#else
  (void)route;
  return {};
#endif
}
}
