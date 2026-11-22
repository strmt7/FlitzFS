#pragma once
#include "native_lz4_decode.hpp"
#include "flitzfs/worker_wire.hpp"

namespace flitzfs::detail {
struct NativeLz4WireRequest {
  std::uint64_t id=0;
  std::size_t reply_bytes=0;
  Digest request_digest{};
  std::vector<NativeLz4ReadKey> keys;
  std::vector<NativeLz4DecodeBatch> batches;
};

class NativeLz4ReadyReads final:public NativeLz4ReadRoute {
 public:
  std::optional<std::vector<std::byte>> try_ready(const NativeLz4ReadKey&,
      std::span<const std::byte>) noexcept override;
  void reject(const NativeLz4ReadKey&) noexcept override;
  std::size_t consumed() const noexcept{return rejected_?0:consumed_;}
  std::size_t indexed_consumed() const noexcept{return rejected_?0:indexed_consumed_;}
 private:
  friend class NativeLz4ReadExchange;
  friend class ::flitzfs::PagedObjectStoreSnapshot;
  struct Frame {
    NativeLz4ReadKey key;
    std::vector<std::byte> bytes;
    std::optional<Digest> stored_digest;
  };
  std::optional<std::vector<std::byte>> try_indexed(const StoreIdentity&,std::uint64_t object,
      std::uint64_t generation,std::uint32_t size,const Digest& stored_digest) noexcept;
  std::vector<std::byte> consume(Frame&) noexcept;
  std::vector<Frame> frames_;
  std::size_t consumed_=0,indexed_consumed_=0;
  bool rejected_=false;
};

class NativeLz4ReadExchange final {
 public:
  static NativeLz4ReadExchange prepare(std::span<const NativeLz4DecodeSource>,
      std::uint64_t id,NativeLz4DecodeGrant);
  static NativeLz4ReadExchange prepare_captured(std::span<const NativeLz4CapturedFrame>,
      std::uint64_t id,NativeLz4DecodeGrant);
  SharedWorkerRequest request() const noexcept {return request_;}
  std::size_t reply_capacity() const noexcept {return reply_bytes_;}
  std::unique_ptr<NativeLz4ReadyReads> accept(std::span<const std::byte>,
      WorkerExecution expected) const;
 private:
  SharedWorkerRequest request_;
  std::size_t reply_bytes_=0;
  std::uint64_t id_=0;
  Digest request_digest_{};
  std::vector<NativeLz4ReadKey> keys_;
  std::vector<Digest> stored_digests_;
};

NativeLz4WireRequest decode_native_lz4_request(std::span<const std::byte>,NativeLz4DecodeGrant,
    std::uint32_t maximum_frames_per_batch);
std::vector<std::byte> encode_native_lz4_reply(const NativeLz4WireRequest&,
    std::span<const NativeLz4DecodedFrame>,WorkerExecution);
}
