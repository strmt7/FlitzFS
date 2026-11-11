#include "hip_api.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace flitzfs;
unsigned checks = 0;
void require(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
struct State {
  unsigned initialized = 0, queried = 0, selected = 0, priorities = 0;
  int ordinal = -1;
  hipError_t init_status = hipSuccess, properties_status = hipSuccess;
  hipError_t select_status = hipSuccess, priority_status = hipSuccess;
  std::array<std::byte,8> luid{std::byte{7}};
  unsigned mask = 2;
  int least = 2, greatest = -1;
} state;

hipError_t fake_init(unsigned flags) {
  require(flags == 0, "unexpected initialization flags");
  ++state.initialized; return state.init_status;
}
hipError_t fake_properties(hipDeviceProp_t* output, int ordinal) {
  ++state.queried; state.ordinal = ordinal;
  *output = {};
  std::copy(state.luid.begin(), state.luid.end(), reinterpret_cast<std::byte*>(output->luid));
  output->luidDeviceNodeMask = state.mask;
  return state.properties_status;
}
hipError_t fake_select(int ordinal) {
  require(ordinal == state.ordinal, "selected another device");
  ++state.selected; return state.select_status;
}
hipError_t fake_priorities(int* least, int* greatest) {
  ++state.priorities; *least = state.least; *greatest = state.greatest; return state.priority_status;
}
void never_called() { throw std::runtime_error("unexpected fake runtime function call"); }
FARPROC lookup(std::string_view name) {
  if (name == "hipInit") return std::bit_cast<FARPROC>(&fake_init);
  if (name == "hipGetDevicePropertiesR0600") return std::bit_cast<FARPROC>(&fake_properties);
  if (name == "hipSetDevice") return std::bit_cast<FARPROC>(&fake_select);
  if (name == "hipDeviceGetStreamPriorityRange") return std::bit_cast<FARPROC>(&fake_priorities);
  require(std::find(hip_detail::runtime_symbols.begin(), hip_detail::runtime_symbols.end(), name) !=
      hip_detail::runtime_symbols.end(), "unexpected runtime lookup");
  return std::bit_cast<FARPROC>(&never_called);
}
template<class Function>
void failure(Function&& call, HipWorkerExit expected, HipWorkerStage stage) {
  try { call(); throw std::runtime_error("expected refusal"); }
  catch (const HipWorkerFailure& error) { require(error.code == expected, "wrong typed failure");
    require(error.stage == stage, "wrong diagnostic stage"); }
}
}
int main() {
  try {
    require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME), "HIP runtime already loaded");
    const std::array<std::byte,8> expected{std::byte{7}};
    require(hip_detail::runtime_symbols.size() == 11, "binding inventory changed without qualification");
    require(hip_detail::runtime_symbols[1] == "hipGetDevicePropertiesR0600",
        "selected SDK properties ABI changed without qualification");
    for (const auto missing : hip_detail::runtime_symbols) {
      failure([&] { (void)hip_detail::bind_api([&](const char* name) {
        return std::string_view(name) == missing ? FARPROC{} : lookup(name);
      }); }, HipWorkerExit::runtime_unavailable, HipWorkerStage::runtime_api);
    }
    require(state.initialized == 0, "binding called the runtime");
    const auto api = hip_detail::bind_api([](const char* name) { return lookup(name); });
    require(hip_detail::initialize_device(api, 7, expected, 2) == 2, "wrong low-priority result");
    require(state.initialized == 1 && state.queried == 1 && state.selected == 1 &&
        state.priorities == 1 && state.ordinal == 7, "qualified device sequence differs");
    for(const unsigned mask:{0U,2U,0x80000000U,0xffffffffU}) {
      state={};state.mask=mask;
      const auto identity=hip_detail::query_device_identity(api,7);
      require(identity.luid==expected && identity.node_mask==mask,"metadata query changed raw identity");
      require(state.initialized==1 && state.queried==1 && state.selected==0 && state.priorities==0,
          "metadata query performed device selection or stream-priority work");
    }
    state = {}; state.luid[1] = std::byte{1};
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::unsupported_configuration, HipWorkerStage::adapter_luid);
    require(state.selected == 0 && state.priorities == 0, "mismatched LUID reached selection");
    state = {}; state.mask = 1;
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::unsupported_configuration, HipWorkerStage::adapter_node);
    require(state.selected == 0 && state.priorities == 0, "mismatched node reached selection");
    for (const auto error : {hipErrorNoDevice, hipErrorInvalidDevice, hipErrorNotSupported,
                            hipErrorInsufficientDriver, hipErrorOutOfMemory, hipErrorUnknown}) {
      state = {}; state.properties_status = error;
      const auto expected_exit = error == hipErrorInsufficientDriver ? HipWorkerExit::runtime_unavailable :
          error == hipErrorOutOfMemory ? HipWorkerExit::temporary_pressure :
          error == hipErrorUnknown ? HipWorkerExit::device_fault : HipWorkerExit::unsupported_configuration;
      failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); }, expected_exit, HipWorkerStage::device_properties);
      require(state.selected == 0 && state.priorities == 0, "failed properties reached selection");
    }
    state = {}; state.init_status = hipErrorInsufficientDriver;
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::runtime_unavailable, HipWorkerStage::runtime_initialize);
    require(state.queried == 0 && state.selected == 0, "failed initialization continued");
    state = {}; state.select_status = hipErrorInvalidDevice;
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::unsupported_configuration, HipWorkerStage::device_select);
    require(state.priorities == 0, "failed selection queried stream priorities");
    state = {}; state.priority_status = hipErrorNotSupported;
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::unsupported_configuration, HipWorkerStage::stream_priorities);
    state = {}; state.least = -2; state.greatest = 1;
    failure([&] { (void)hip_detail::initialize_device(api, 7, expected, 2); },
        HipWorkerExit::device_fault, HipWorkerStage::stream_priorities);
    require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME), "fake test loaded HIP");
    std::cout << "HIP API CPU qualification: {\"checks\":" << checks <<
        ",\"identity_before_selection\":true,\"missing_exports_refused\":11,\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "HIP API CPU qualification failed: " << error.what() << '\n'; return 1;
  }
}
