#pragma once
#include "flitzfs/object_store.hpp"
#include <array>
#include <memory>

namespace flitzfs {
// Experimental advisory measurement only. It owns a bounded immutable sample;
// it performs no I/O, scheduling, worker launch, device lookup or publication.
// One control thread; do not race calls, views, moves or destruction.
struct CompressionProbeSource {
  StoreIdentity image{};
  std::uint64_t generation=0,object_id=0;
  std::uint32_t object_bytes=0,sample_offset=0;
  bool operator==(const CompressionProbeSource&) const = default;
};
enum class CompressionProbeState { profiling,complete,failed,inactive };
struct CompressionProbeMeasurement {
  int requested_level=0;
  std::uint32_t codec=0,logical_bytes=0,stored_payload_bytes=0;
  std::uint64_t elapsed_nanoseconds=0;
  std::size_t encoder_peak_bytes=0; // Codec allocator only, not whole-process memory.
  bool used_match_proposals=false;
};
class StoreCompressionProbe final {
 public:
  static constexpr std::size_t input_limit=64U*1024U;
  static constexpr std::size_t encoder_memory_limit=16U*1024U*1024U;
  static constexpr std::array<int,3> levels{3,6,9};
  // Invalid identity/range/cap throws before copying. No Zstd returns no probe.
  static std::optional<StoreCompressionProbe> create(CompressionProbeSource,
      std::span<const std::byte> sample,std::size_t encoder_cap=encoder_memory_limit);
  StoreCompressionProbe(StoreCompressionProbe&&) noexcept;
  StoreCompressionProbe& operator=(StoreCompressionProbe&&) noexcept;
  StoreCompressionProbe(const StoreCompressionProbe&)=delete;
  StoreCompressionProbe& operator=(const StoreCompressionProbe&)=delete;
  ~StoreCompressionProbe();
  // One profile per call, at most two encodes if optional match hints are used.
  // Hints use existing CPU validation/fallback. A failed probe is terminal;
  // its source remains available and no partial recommendation is returned.
  CompressionProbeState advance(std::span<const StoreMatchProposal> matches={}) noexcept;
  CompressionProbeState state() const noexcept;
  std::span<const std::byte> input() const noexcept;
  CompressionProbeSource source() const noexcept;
  std::span<const CompressionProbeMeasurement> measurements() const noexcept;
  // Require the exact source tuple AND sample bytes again. The smallest measured
  // payload wins; ties prefer the lower requested level. This is sample advice,
  // not a prediction or authorization to publish a whole-volume rewrite.
  std::optional<StoreCompressionOptions> recommendation(CompressionProbeSource current,
      std::span<const std::byte> current_sample) const noexcept;
 private:
  struct Impl;
  explicit StoreCompressionProbe(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};
}
