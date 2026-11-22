#pragma once
#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include <compare>

namespace flitzfs::detail {
struct NativeLz4ReadKey {
  StoreIdentity image{};
  std::uint64_t object=0,generation=0;
  std::uint32_t decoded_bytes=0;
  Digest decoded_digest{};
  auto operator<=>(const NativeLz4ReadKey&) const=default;
};

// Stored bytes verified against their checkpoint. A deferred consumer must
// still bind its result to the complete key and verify the decoded digest.
struct NativeLz4CapturedFrame {
  NativeLz4ReadKey key;
  std::vector<std::byte> encoded;
};

// An advisory route for an owned result from an exact native LZ4 frame.
// Calls run on the serialized reader. Return immediately when no result is
// ready. Any deferred work must own a copy of its input before this call
// ends. The store independently verifies the decoded record before use.
class NativeLz4ReadRoute {
 public:
  virtual ~NativeLz4ReadRoute()=default;
  virtual std::optional<std::vector<std::byte>> try_ready(const NativeLz4ReadKey&,
      std::span<const std::byte> encoded) noexcept=0;
  virtual void reject(const NativeLz4ReadKey&) noexcept=0;
};
}
