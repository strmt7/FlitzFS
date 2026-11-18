#include "hip_route_discovery.hpp"
#include "hip_module_verification.hpp"
#include <iostream>
#include <iterator>
#include <stdexcept>
namespace {
unsigned digit(wchar_t c) {
  if(c>=L'0'&&c<=L'9')return unsigned(c-L'0');
  if(c>=L'a'&&c<=L'f')return 10U+unsigned(c-L'a');
  throw std::invalid_argument("Invalid digest digit");
}
template<std::size_t N> std::string hex(const std::array<std::byte,N>& data) {
  constexpr char digits[]="0123456789abcdef";
  std::string text;text.reserve(data.size()*2);
  for(const auto b:data) {
    const auto n=std::to_integer<unsigned>(b);text.push_back(digits[n>>4]);text.push_back(digits[n&15]);
  }
  return text;
}
std::array<std::byte,32> digest_field(std::wstring_view digest) {
  if(digest.size()!=64)throw std::invalid_argument("Expected verified artifact SHA256");
  std::array<std::byte,32> result;
  for(std::size_t i=0;i<result.size();++i)
    result[i]=static_cast<std::byte>(16*digit(digest[2*i])+digit(digest[2*i+1]));
  return result;
}
int report(const flitzfs::HipRouteDiscoveryResult& result) {
  std::cout<<"{\"status\":"<<static_cast<unsigned>(result.status)<<",\"identity_queries\":"<<result.identity_queries
      <<",\"worker_exit\":"<<result.worker_exit<<",\"gpu_compute_requested\":false";
  if(const auto& binding=result.route) {
    std::cout<<",\"configuration\":\""<<hex(binding->identity)<<"\",\"luid\":\""<<hex(binding->memory.luid)
        <<"\",\"node\":"<<binding->memory.node_mask<<",\"ordinal\":"<<binding->ordinal
        <<",\"runtime_sha256\":\""<<hex(binding->runtime_sha256)<<"\",\"local_budget\":"<<binding->memory.local_budget
        <<",\"vendor_id\":"<<binding->hardware.vendor_id<<",\"device_id\":"<<binding->hardware.device_id
        <<",\"subsystem_id\":"<<binding->hardware.subsystem_id<<",\"revision\":"<<binding->hardware.revision;
  }
  std::cout<<"}\n";return result.route?0:2;
}
int verify_request_refusals() {
  using flitzfs::HipDiscoveryStatus;
  using flitzfs::HipRouteDiscoveryRequest;
  HipRouteDiscoveryRequest valid;
  valid.helper = std::filesystem::current_path() / L"flitzfs_hip_lz4_worker.exe";
  valid.helper_sha256.fill(std::byte{1});
  valid.runtime_basename = L"amdhip64_7.dll";
  valid.worker = {128, 2000, 5000};
  valid.maximum_identity_queries = 4;

  const auto reject = [](const HipRouteDiscoveryRequest& request) {
    const auto result = flitzfs::discover_system_hip_route(request);
    if (result.status != HipDiscoveryStatus::invalid_request || result.route ||
        result.identity_queries || result.worker_exit)
      throw std::runtime_error("Invalid discovery request escaped preflight");
  };
  const auto relative = [](auto& request) { request.helper = request.helper.filename(); };
  const auto name = [](auto& request) { request.helper.replace_filename(L"untrusted.exe"); };
  const auto hash = [](auto& request) { request.helper_sha256 = {}; };
  const auto runtime = [](auto& request) { request.runtime_basename = L"..\\amdhip64_7.dll"; };
  const auto abi = [](auto& request) { request.runtime_basename = L"amdhip64_07.dll"; };
  const auto timeout = [](auto& request) { request.worker.timeout_ms = 0; };
  const auto empty = [](auto& request) { request.maximum_identity_queries = 0; };
  const auto oversized = [](auto& request) { request.maximum_identity_queries = 65; };
  const auto wrong_qualifier = [](auto& request) {request.anchor_device.emplace();};
  const auto conflicting_modules = [](auto& request) {
    request.helper.replace_filename(L"flitzfs_hip_worker.exe");
    request.uniform_module_sha256.fill(std::byte{1});request.anchor_module_sha256.fill(std::byte{1});
  };
  using MutateRequest = void (*)(HipRouteDiscoveryRequest&);
  const MutateRequest changes[] = {relative,name,hash,runtime,abi,timeout,empty,oversized,wrong_qualifier,conflicting_modules};
  for (const auto change : changes) {
    auto request = valid;
    change(request);
    reject(request);
  }
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled=flitzfs::discover_system_hip_route(valid,stop.get_token());
  if(cancelled.status!=HipDiscoveryStatus::cancelled || cancelled.route || cancelled.identity_queries)
    throw std::runtime_error("Cancelled discovery opened a route");
  std::cout << "HIP route request refusals: " << std::size(changes) << "\n";
  return 0;
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--verify-request-refusals")
      return verify_request_refusals();
    if(argc==3 && std::wstring_view(argv[1])==L"--system-fingerprint") {
      const flitzfs::hip_detail::VerifiedSystemModule observed(argv[2]);
      const flitzfs::hip_detail::VerifiedSystemModule verified(argv[2],std::string_view(hex(observed.digest())));
      if(verified.digest()!=observed.digest() || verified.size()!=observed.size())
        throw std::runtime_error("Runtime fingerprint changed across verified opens");
      std::cout<<"{\"sha256\":\""<<hex(observed.digest())<<"\",\"bytes\":"<<observed.size()<<",\"hip_loaded\":false}\n";
      return 0;
    }
    const auto mode=argc>1?std::wstring_view(argv[1]):std::wstring_view{};
    const bool wrong_runtime=argc==6 && mode==L"--reject-unqualified-anchor-runtime";
    const bool wrong_device=argc==7 && mode==L"--reject-unqualified-anchor-device";
    const bool anchor=(argc==6 && mode==L"--bind-verified-anchor-fixture") || wrong_runtime || wrong_device;
    const bool installed=argc==2 && mode==L"--inspect-installed-anchor";
    if(!anchor && !installed && (argc!=5 || mode!=L"--bind-verified-fixture"))
      throw std::invalid_argument("Expected explicit fingerprint or verified fixture mode");
    if(GetEnvironmentVariableW(L"FLITZFS_ALLOW_HIP_METADATA",nullptr,0)!=2)
      throw std::invalid_argument("Explicit HIP metadata admission required");
    wchar_t consent[2]{};
    if(GetEnvironmentVariableW(L"FLITZFS_ALLOW_HIP_METADATA",consent,2)!=1 || consent[0]!=L'1')
      throw std::invalid_argument("Explicit HIP metadata admission required");
    if(installed)return report(flitzfs::discover_installed_hip_anchor_route());
    flitzfs::HipRouteDiscoveryRequest request;
    request.helper=argv[2];request.runtime_basename=argv[anchor?5:4];
    request.helper_sha256=digest_field(argv[3]);
    if(anchor)request.anchor_module_sha256=digest_field(argv[4]);
    if(wrong_runtime || wrong_device) {
      flitzfs::HipAnchorDeviceBinding unqualified;
      unqualified.hardware={0x1002,0xffff,0,0};
      if(wrong_runtime)unqualified.runtime_sha256.fill(std::byte{0xff});
      else unqualified.runtime_sha256=digest_field(argv[6]);
      request.anchor_device=unqualified;
    }
    request.worker={128,2000,5000};request.maximum_identity_queries=4;
    const auto result=flitzfs::discover_system_hip_route(request);
    if(wrong_runtime || wrong_device) {
      (void)report(result);
      return result.status==flitzfs::HipDiscoveryStatus::profile_mismatch && !result.identity_queries && !result.route?0:1;
    }
    return report(result);
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
  }
}
