#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace flitzfs {
struct WindowsGpuEngineCounter {
  std::wstring_view instance;
  double busy_percent;
  bool valid;
};
struct WindowsGpuEngineActivity {
  struct Adapter {
    std::array<std::byte,8> luid;
    std::optional<std::uint32_t> busy_ppm;
  };
  std::uint64_t captured_ms=0;
  std::vector<Adapter> adapters;
  bool fresh(std::uint64_t now_ms) const noexcept;
  bool admits(const std::array<std::byte,8>& luid,std::uint64_t now_ms,
      std::uint32_t required_idle_ppm) const noexcept;
};
inline constexpr std::uint64_t gpu_engine_sample_ms=1000;
inline constexpr std::uint32_t minimum_background_gpu_idle_ppm=125000;
constexpr std::uint32_t background_gpu_idle_ppm(std::uint64_t duty_ppm) noexcept {
  if(duty_ppm>=500000)return 1000000;
  const auto twice_duty=static_cast<std::uint32_t>(2*duty_ppm);
  return twice_duty<minimum_background_gpu_idle_ppm?minimum_background_gpu_idle_ppm:twice_duty;
}
std::optional<WindowsGpuEngineActivity> aggregate_windows_gpu_engines(
    std::span<const WindowsGpuEngineCounter>,std::uint64_t captured_ms);

// A local read-only query owned by the ordinary maintenance thread. The
// caller supplies memory from current host observations. Readers can check
// the independently published snapshot without touching the PDH query.
class WindowsGpuEngineSampler final {
 public:
  WindowsGpuEngineSampler() noexcept;
  ~WindowsGpuEngineSampler();
  WindowsGpuEngineSampler(const WindowsGpuEngineSampler&)=delete;
  WindowsGpuEngineSampler& operator=(const WindowsGpuEngineSampler&)=delete;
  void observe(std::uint64_t memory_allowance) noexcept;
  std::shared_ptr<const WindowsGpuEngineActivity> current(std::uint64_t now_ms) const noexcept;
  std::optional<bool> available(const std::array<std::byte,8>& luid,std::uint64_t now_ms,
      std::uint32_t required_idle_ppm) const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::atomic<std::shared_ptr<const WindowsGpuEngineActivity>> latest_;
};
}
