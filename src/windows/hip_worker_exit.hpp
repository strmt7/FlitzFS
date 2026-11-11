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
  worker_job, worker_request,
  pre_init_invalid_budget, pre_init_stale_budget, pre_init_wrong_adapter, pre_init_headroom,
  initialized_invalid_budget, initialized_stale_budget, initialized_wrong_adapter,
  initialized_headroom, initialized_nonlocal_growth,
  completed_invalid_budget, completed_stale_budget, completed_wrong_adapter,
  completed_headroom, completed_nonlocal_growth,
};
constexpr bool hip_worker_category_valid(std::uint32_t value) noexcept {
  return value >= 0x46530001U && value <= 0x46530006U;
}
constexpr std::uint32_t hip_worker_exit_category(std::uint32_t value) noexcept {
  const auto category = value & 0xffff00ffU;
  const auto stage = (value >> 8) & 255U;
  return hip_worker_category_valid(category) && stage >= 1 && stage <= 31 ? category : value;
}
constexpr HipWorkerStage hip_worker_exit_stage(std::uint32_t value) noexcept {
  return hip_worker_exit_category(value) != value ?
      static_cast<HipWorkerStage>((value >> 8) & 255U) : HipWorkerStage::none;
}
constexpr std::uint32_t hip_worker_diagnostic_exit(HipWorkerExit category, HipWorkerStage stage) noexcept {
  const auto code = static_cast<std::uint32_t>(category);
  const auto detail = static_cast<std::uint32_t>(stage);
  return hip_worker_category_valid(code) && detail >= 1 && detail <= 31 ? code | (detail << 8) : code;
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
    case HipWorkerStage::worker_job: return "worker_job";
    case HipWorkerStage::worker_request: return "worker_request";
    case HipWorkerStage::pre_init_invalid_budget: return "pre_init_invalid_budget";
    case HipWorkerStage::pre_init_stale_budget: return "pre_init_stale_budget";
    case HipWorkerStage::pre_init_wrong_adapter: return "pre_init_wrong_adapter";
    case HipWorkerStage::pre_init_headroom: return "pre_init_headroom";
    case HipWorkerStage::initialized_invalid_budget: return "initialized_invalid_budget";
    case HipWorkerStage::initialized_stale_budget: return "initialized_stale_budget";
    case HipWorkerStage::initialized_wrong_adapter: return "initialized_wrong_adapter";
    case HipWorkerStage::initialized_headroom: return "initialized_headroom";
    case HipWorkerStage::initialized_nonlocal_growth: return "initialized_nonlocal_growth";
    case HipWorkerStage::completed_invalid_budget: return "completed_invalid_budget";
    case HipWorkerStage::completed_stale_budget: return "completed_stale_budget";
    case HipWorkerStage::completed_wrong_adapter: return "completed_wrong_adapter";
    case HipWorkerStage::completed_headroom: return "completed_headroom";
    case HipWorkerStage::completed_nonlocal_growth: return "completed_nonlocal_growth";
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
