#pragma once
#include "flitzfs/optimization_state.hpp"

namespace flitzfs::detail {
inline constexpr std::size_t kOptimizationGroupFeatureBytes=168;

bool valid_optimization_group_feature(const OptimizationGroupFeature&) noexcept;
void encode_optimization_group_feature(
    std::span<std::byte,kOptimizationGroupFeatureBytes>,const OptimizationGroupFeature&);
std::optional<OptimizationGroupFeature> decode_optimization_group_feature(
    std::span<const std::byte,kOptimizationGroupFeatureBytes>);
}
