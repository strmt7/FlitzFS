#pragma once
#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"
#include <compare>

namespace flitzfs {class PagedObjectStoreSnapshot;}

namespace flitzfs::detail {
class PagedReadMetadata;
struct NativeLz4ReadKey {
  StoreIdentity image{};
  std::uint64_t object=0,generation=0;
  std::uint32_t decoded_bytes=0;
  Digest decoded_digest{};
  auto operator<=>(const NativeLz4ReadKey&) const=default;
};

// The paged reader alone binds the stored digest to these captured bytes.
class NativeLz4CapturedFrame final {
 public:
  const NativeLz4ReadKey& key() const noexcept {return key_;}
  std::span<const std::byte> encoded() const noexcept {return encoded_;}
 private:
  friend class ::flitzfs::PagedObjectStoreSnapshot;
  friend class NativeLz4ReadExchange;
  friend class NativeLz4ReadyReads;
  NativeLz4ReadKey key_;
  Digest stored_digest_{};
  std::vector<std::byte> encoded_;
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
