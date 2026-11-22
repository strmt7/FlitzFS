#pragma once

#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"

namespace flitzfs::detail {

inline constexpr std::size_t kProtectedStripeDataUnits=8;

// A complete immutable native byte stream. Only the outer image is durable.
// Stripe writes and decoded records are controlled by the CPU.
struct ProtectedStripeLayout {
  StoreIdentity identity{};
  std::uint64_t logical_bytes=0,physical_bytes=0,unit=0;
  bool operator==(const ProtectedStripeLayout&) const=default;
};

class ProtectedStripeImage final:public StoreByteIo {
 public:
  static std::uint64_t projected_occupied(StoreIdentity,std::uint64_t logical_bytes,
      std::uint32_t failure_unit,std::uint64_t physical_limit);
  static std::shared_ptr<ProtectedStripeImage> open(std::shared_ptr<StoreByteIo>,std::uint64_t physical_limit);
  static std::shared_ptr<ProtectedStripeImage> seal(std::shared_ptr<StoreByteIo> closed_source,
      std::shared_ptr<StoreByteIo> empty_output,StoreIdentity,std::uint64_t physical_limit);
  std::uint64_t extent() const override {return layout_.logical_bytes;}
  std::uint64_t occupied() const noexcept {return layout_.physical_bytes;}
  StoreIdentity identity() const noexcept {return layout_.identity;}
  bool degraded() const noexcept {return degraded_;}
  void read_exact(std::uint64_t,std::span<std::byte>) const override;
  void verify_all() const;
  StoreIoContract contract() const noexcept override {return {};}
  void write_exact(std::uint64_t,std::span<const std::byte>) override;
  void admit_write(std::uint64_t) override;
  void flush() override;

 private:
  struct Lease;
  ProtectedStripeImage(std::shared_ptr<StoreByteIo>,ProtectedStripeLayout,std::uint64_t,bool);
  void verify_stripes(const StoreByteIo* clean_source=nullptr) const;
  using Checksums=std::array<Digest,kProtectedStripeDataUnits>;
  const Checksums& stripe_checksums(std::uint64_t stripe) const;
  const std::vector<std::byte>& checked_block(std::uint64_t block) const;
  bool allows_writer() const noexcept override {return false;}
  std::shared_ptr<StoreByteIo> backing_;
  ProtectedStripeLayout layout_;
  std::uint64_t physical_limit_;
  struct CheckedStripe {
    std::uint64_t stripe=UINT64_MAX;
    Checksums checksums{};
  };
  // Immutable descriptor digests avoid rereading their full allocation unit.
  // A scrub always starts with a fresh view of the stored descriptors.
  mutable std::array<CheckedStripe,4> checked_{};
  struct CheckedBlock {
    std::uint64_t ordinal=UINT64_MAX;
    std::vector<std::byte> bytes;
  };
  mutable std::array<CheckedBlock,4> blocks_{};
  mutable bool degraded_=false;
};

}
