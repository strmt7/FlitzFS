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
  using MutateRequest = void (*)(HipRouteDiscoveryRequest&);
  const MutateRequest changes[] = {relative, name, hash, runtime, abi, timeout, empty, oversized};
  for (const auto change : changes) {
    auto request = valid;
    change(request);
    reject(request);
  }
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
    if(argc!=5 || std::wstring_view(argv[1])!=L"--bind-verified-fixture")
      throw std::invalid_argument("Expected explicit fingerprint or verified fixture mode");
    if(GetEnvironmentVariableW(L"FLITZFS_ALLOW_HIP_METADATA",nullptr,0)!=2)
      throw std::invalid_argument("Explicit HIP metadata admission required");
    wchar_t consent[2]{};
    if(GetEnvironmentVariableW(L"FLITZFS_ALLOW_HIP_METADATA",consent,2)!=1 || consent[0]!=L'1')
      throw std::invalid_argument("Explicit HIP metadata admission required");
    const std::wstring_view digest(argv[3]);
    if(digest.size()!=64)throw std::invalid_argument("Expected verified helper SHA256");
    flitzfs::HipRouteDiscoveryRequest request;
    request.helper=argv[2];request.runtime_basename=argv[4];
    for(std::size_t i=0;i<32;++i)
      request.helper_sha256[i]=static_cast<std::byte>(16*digit(digest[2*i])+digit(digest[2*i+1]));
    request.worker={128,2000,5000};request.maximum_identity_queries=4;
    const auto result=flitzfs::discover_system_hip_route(request);
    std::cout<<"{\"status\":"<<static_cast<unsigned>(result.status)<<",\"identity_queries\":"<<result.identity_queries
        <<",\"worker_exit\":"<<result.worker_exit<<",\"gpu_compute_requested\":false";
    if(const auto& binding=result.route) {
      std::cout<<",\"configuration\":\""<<hex(binding->identity)<<"\",\"luid\":\""<<hex(binding->memory.luid)
          <<"\",\"node\":"<<binding->memory.node_mask<<",\"ordinal\":"<<binding->ordinal
          <<",\"runtime_sha256\":\""<<hex(binding->runtime_sha256)<<"\",\"local_budget\":"<<binding->memory.local_budget;
    }
    std::cout<<"}\n";return result.route?0:2;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';return 1;
  }
}
