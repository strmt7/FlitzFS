#pragma once
#include <cstdint>
#include <stdexcept>

namespace flitzfs {
// The original six category values remain unchanged.
enum class HipWorkerExit : std::uint32_t {
  invalid_request = 0x46530001,
  runtime_unavailable,
  unsupported_configuration,
  temporary_pressure,
  device_fault,
  integrity_failure,
};
enum class HipWorkerStage : std::uint32_t {
  none, adapter_budget, kernel_entry, kernel_table, runtime_initialize,
  device_properties, adapter_luid, adapter_node, device_select,
  stream_priorities, stream_create, execution, runtime_api,
  runtime_load, kernel_load, module_retire,
};
constexpr bool hip_worker_category_valid(std::uint32_t value) noexcept {
  return value >= 0x46530001U && value <= 0x46530006U;
}
constexpr std::uint32_t hip_worker_exit_category(std::uint32_t value) noexcept {
  const auto category = value & 0xffff00ffU;
  const auto stage = (value >> 8) & 255U;
  return hip_worker_category_valid(category) && stage >= 1 && stage <= 15 ? category : value;
}
constexpr HipWorkerStage hip_worker_exit_stage(std::uint32_t value) noexcept {
  return hip_worker_exit_category(value) != value ?
      static_cast<HipWorkerStage>((value >> 8) & 255U) : HipWorkerStage::none;
}
constexpr std::uint32_t hip_worker_diagnostic_exit(HipWorkerExit category, HipWorkerStage stage) noexcept {
  const auto code = static_cast<std::uint32_t>(category);
  const auto detail = static_cast<std::uint32_t>(stage);
  return hip_worker_category_valid(code) && detail >= 1 && detail <= 15 ? code | (detail << 8) : code;
}
constexpr const char* hip_worker_stage_name(HipWorkerStage stage) noexcept {
  switch (stage) {
    case HipWorkerStage::none: return "none";
    case HipWorkerStage::adapter_budget: return "adapter_budget";
    case HipWorkerStage::kernel_entry: return "kernel_entry";
    case HipWorkerStage::kernel_table: return "kernel_table";
    case HipWorkerStage::runtime_initialize: return "runtime_initialize";
    case HipWorkerStage::device_properties: return "device_properties";
    case HipWorkerStage::adapter_luid: return "adapter_luid";
    case HipWorkerStage::adapter_node: return "adapter_node";
    case HipWorkerStage::device_select: return "device_select";
    case HipWorkerStage::stream_priorities: return "stream_priorities";
    case HipWorkerStage::stream_create: return "stream_create";
    case HipWorkerStage::execution: return "execution";
    case HipWorkerStage::runtime_api: return "runtime_api";
    case HipWorkerStage::runtime_load: return "runtime_load";
    case HipWorkerStage::kernel_load: return "kernel_load";
    case HipWorkerStage::module_retire: return "module_retire";
  }
  return "unknown";
}
class HipWorkerFailure final : public std::runtime_error {
 public:
  HipWorkerFailure(HipWorkerExit result, const char* operation,
      HipWorkerStage location = HipWorkerStage::none)
      : std::runtime_error(operation), code(result), stage(location) {}
  const HipWorkerExit code;
  const HipWorkerStage stage;
};
}
