#pragma once
#include "flitzfs/gpu_recovery.hpp"
#include <filesystem>
#include <memory>
#include <stop_token>
#include <utility>

namespace flitzfs {
struct HipRouteDiscoveryRequest;
struct HipRouteDiscoveryResult;

// Only successful route acquisition can seal the helper, runtime and device
// identity together. The opaque resources retain the verified file handles.
class HipRoutePins final {
 public:
  bool uniform_module_pinned() const noexcept {return uniform_module_pinned_;}
  bool anchor_module_pinned() const noexcept {return anchor_module_pinned_;}
  bool matches(const std::filesystem::path& helper,const GpuConfigurationId& configuration,
      const std::array<std::byte,32>& runtime_sha256,int ordinal,
      const GpuMemorySnapshot& memory) const noexcept {
    return resources_ && helper.native()==helper_.native() && configuration==configuration_ &&
        runtime_sha256==runtime_sha256_ && ordinal>=0 && static_cast<std::uint32_t>(ordinal)==ordinal_ &&
        memory.luid==luid_ && memory.node_mask==node_mask_;
  }
 private:
  friend HipRouteDiscoveryResult discover_system_hip_route(const HipRouteDiscoveryRequest&,std::stop_token);
  HipRoutePins(std::filesystem::path helper,GpuConfigurationId configuration,
      std::array<std::byte,32> runtime_sha256,std::uint32_t ordinal,const GpuMemorySnapshot& memory,
      bool uniform_module_pinned,bool anchor_module_pinned,std::shared_ptr<const void> resources)
      :helper_(std::move(helper)),configuration_(configuration),runtime_sha256_(runtime_sha256),
       ordinal_(ordinal),luid_(memory.luid),node_mask_(memory.node_mask),uniform_module_pinned_(uniform_module_pinned),
       anchor_module_pinned_(anchor_module_pinned),resources_(std::move(resources)){}
  const std::filesystem::path helper_;
  const GpuConfigurationId configuration_;
  const std::array<std::byte,32> runtime_sha256_;
  const std::uint32_t ordinal_;
  const std::array<std::byte,8> luid_;
  const std::uint32_t node_mask_;
  const bool uniform_module_pinned_;
  const bool anchor_module_pinned_;
  const std::shared_ptr<const void> resources_;
};
}
