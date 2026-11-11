#include "flitzfs/compression_probe.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace flitzfs {
static_assert(StoreCompressionProbe::encoder_memory_limit==detail::kStoreEncoderMemoryCap);
static_assert(StoreCompressionProbe::input_limit==detail::kStoreMaxMatchInputBytes);
struct StoreCompressionProbe::Impl {
  CompressionProbeSource source;
  std::vector<std::byte> input;
  std::array<CompressionProbeMeasurement,levels.size()> observations{};
  std::size_t count=0,cap=0;
  CompressionProbeState state=CompressionProbeState::profiling;
};
StoreCompressionProbe::StoreCompressionProbe(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
StoreCompressionProbe::StoreCompressionProbe(StoreCompressionProbe&&) noexcept=default;
StoreCompressionProbe& StoreCompressionProbe::operator=(StoreCompressionProbe&&) noexcept=default;
StoreCompressionProbe::~StoreCompressionProbe()=default;
std::optional<StoreCompressionProbe> StoreCompressionProbe::create(CompressionProbeSource source,
    std::span<const std::byte> sample,std::size_t cap) {
  if(!source.generation || !source.object_id ||
      std::all_of(source.image.begin(),source.image.end(),[](auto b){return b==std::byte{0};}) ||
      !source.object_bytes || source.object_bytes>kStoreMaxObjectBytes ||
      source.sample_offset>source.object_bytes || sample.empty() ||
      sample.size()>input_limit || sample.size()>source.object_bytes-source.sample_offset ||
      cap>encoder_memory_limit)
    throw std::invalid_argument("invalid bounded compression probe source/range/cap");
  if(!store_zstd_available())return std::nullopt;
  auto p=std::make_unique<Impl>();p->source=source;p->cap=cap;
  p->input.assign(sample.begin(),sample.end());
  return StoreCompressionProbe(std::move(p));
}
CompressionProbeState StoreCompressionProbe::state() const noexcept {
  return impl_?impl_->state:CompressionProbeState::inactive;
}
CompressionProbeSource StoreCompressionProbe::source() const noexcept {
  return impl_?impl_->source:CompressionProbeSource{};
}
std::span<const std::byte> StoreCompressionProbe::input() const noexcept {
  return impl_?std::span<const std::byte>(impl_->input):std::span<const std::byte>{};
}
std::span<const CompressionProbeMeasurement> StoreCompressionProbe::measurements() const noexcept {
  return impl_?std::span<const CompressionProbeMeasurement>(impl_->observations).first(impl_->count):
      std::span<const CompressionProbeMeasurement>{};
}
CompressionProbeState StoreCompressionProbe::advance(std::span<const StoreMatchProposal> matches) noexcept {
  if(!impl_ || impl_->state!=CompressionProbeState::profiling)return state();
  auto& self=*impl_;
  try {
    if(self.count>=levels.size())throw std::logic_error("compression probe profile budget");
    const auto level=levels[self.count];
    const auto start=std::chrono::steady_clock::now();
    detail::StoreCodecEncoder encoder({StoreCompressionMode::automatic,level,true},self.cap);
    // The encoder retains its existing CPU round-trip guard. Optional suggestions
    // cannot bypass byte checks or prevent the ordinary CPU result.
    auto encoded=matches.empty()?encoder.encode(self.input):encoder.encode_with_matches(self.input,matches);
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
    if(encoded.payload.size()>self.input.size() || encoder.peak_bytes()>self.cap || elapsed<0)
      throw std::runtime_error("compression probe result outside bounds");
    self.observations[self.count++]={level,encoded.codec,static_cast<std::uint32_t>(self.input.size()),
        static_cast<std::uint32_t>(encoded.payload.size()),static_cast<std::uint64_t>(elapsed),
        encoder.peak_bytes(),encoded.used_match_proposals};
    // No admitted codec can beat zero bytes or a one-byte exact motif.
    if(self.count==levels.size() || encoded.codec==detail::kStoreZero ||
        (encoded.codec==detail::kStorePattern && encoded.payload.size()==1))
      self.state=CompressionProbeState::complete;
  }catch(...) {
    self.state=CompressionProbeState::failed; // No retry, I/O or accepted advice.
  }
  return self.state;
}
std::optional<StoreCompressionOptions> StoreCompressionProbe::recommendation(
    CompressionProbeSource current,std::span<const std::byte> current_sample) const noexcept {
  if(!impl_ || impl_->state!=CompressionProbeState::complete || !impl_->count ||
      current!=impl_->source || current_sample.size()!=impl_->input.size() ||
      !std::equal(current_sample.begin(),current_sample.end(),impl_->input.begin()))
    return std::nullopt;
  const auto rows=measurements();
  const auto best=std::min_element(rows.begin(),rows.end(),[](const auto& a,const auto& b) {
    if(a.stored_payload_bytes!=b.stored_payload_bytes)return a.stored_payload_bytes<b.stored_payload_bytes;
    return a.requested_level<b.requested_level;
  });
  return StoreCompressionOptions{StoreCompressionMode::automatic,best->requested_level,true};
}
}
