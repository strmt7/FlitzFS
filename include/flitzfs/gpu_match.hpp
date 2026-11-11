#pragma once
#include "flitzfs/gpu_match_protocol.hpp"
#include "flitzfs/object_store.hpp"
#include <span>
#include <vector>
namespace flitzfs {
// CPU preparation and result validation only. No device discovery or runtime.
class PreparedGpuMatchSearch final {
 public:
  static PreparedGpuMatchSearch prepare(std::span<const std::byte> input);
  std::span<const std::byte> input() const noexcept {return input_;}
  std::span<const match_detail::u16> predecessors() const noexcept {return previous_;}
  std::uint32_t lanes() const noexcept;
  std::uint64_t device_buffer_bytes() const noexcept;
  // Validate ALL output lanes before retaining nonoverlapping proposals.
  // Exact input-byte comparisons independently check the suggested matches.
  std::vector<StoreMatchProposal> verified_proposals(std::span<const match_detail::Match>) const;
 private:
  std::vector<std::byte> input_;
  std::vector<match_detail::u16> previous_;
};
}
