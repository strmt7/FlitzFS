#include "flitzfs/gpu_lz4_verify.hpp"
#include "flitzfs/gpu_lz4_layout.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#ifdef FLITZFS_HAS_LZ4
#include <lz4.h>
static_assert(LZ4_VERSION_NUMBER>=10904,"LZ4 1.9.4 or newer is required");
#endif
namespace flitzfs {
namespace {
bool overlaps(const void* a,std::size_t an,const void* b,std::size_t bn) noexcept {
  if(!a || !b)return true;
  const auto first=reinterpret_cast<std::uintptr_t>(a),second=reinterpret_cast<std::uintptr_t>(b);
  const auto maximum=(std::numeric_limits<std::uintptr_t>::max)();
  if(an>maximum-first || bn>maximum-second)return true;
  return first<second+bn && second<first+an;
}
}
bool gpu_lz4_cpu_verification_available() noexcept {
#ifdef FLITZFS_HAS_LZ4
  return true;
#else
  return false;
#endif
}
GpuLz4Verification verify_gpu_lz4_candidate(std::span<const std::byte> source,
    std::span<const std::byte> payload,std::span<std::byte> scratch) noexcept {
  if(source.empty() || source.size()>gpu_detail::max_block_bytes ||
      scratch.size()!=source.size() || payload.empty())
    return GpuLz4Verification::invalid_extent;
  const auto capacity=gpu_lz4_detail::output_capacity(static_cast<std::uint32_t>(source.size()));
  if(payload.size()>capacity)return GpuLz4Verification::invalid_extent;
  if(overlaps(scratch.data(),scratch.size(),source.data(),source.size()) ||
      overlaps(scratch.data(),scratch.size(),payload.data(),payload.size()))
    return GpuLz4Verification::overlapping_output;
#ifdef FLITZFS_HAS_LZ4
  const auto decoded=LZ4_decompress_safe(reinterpret_cast<const char*>(payload.data()),
      reinterpret_cast<char*>(scratch.data()),static_cast<int>(payload.size()),static_cast<int>(scratch.size()));
  if(decoded!=static_cast<int>(source.size()))return GpuLz4Verification::decode_failed;
  if(!std::equal(source.begin(),source.end(),scratch.begin()))return GpuLz4Verification::mismatch;
  return GpuLz4Verification::verified;
#else
  return GpuLz4Verification::unavailable;
#endif
}
} // namespace flitzfs
