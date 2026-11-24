#pragma once
#include "flitzfs/worker_wire.hpp"
#include "gpu/native_group_decode.hpp"
#include "native_group_read.hpp"

namespace flitzfs::detail {
struct NativeGroupWireRequest {
  std::uint64_t id=0;
  Digest digest{};
  group_decode::Layout layout{};
  std::vector<std::byte> upload;
};
NativeGroupWireRequest decode_native_group_request(std::span<const std::byte>,
    std::uint32_t maximum_members,std::uint64_t maximum_device_bytes);
std::size_t native_group_reply_bytes(group_decode::Layout) noexcept;
std::vector<std::byte> encode_native_group_reply(const NativeGroupWireRequest&,
    std::span<const std::byte> output_and_status,WorkerExecution);

class NativeGroupReadExchange final {
 public:
  static NativeGroupReadExchange prepare(const NativeGroupReadBatch&,std::uint64_t id,
      std::uint32_t maximum_members,std::uint64_t maximum_device_bytes);
  SharedWorkerRequest request() const noexcept{return request_;}
  std::size_t reply_capacity() const noexcept{return native_group_reply_bytes(layout_);}
  std::uint64_t device_bytes() const noexcept{return group_decode::slab_bytes(layout_);}
  std::unique_ptr<NativeGroupReadyReads> accept(NativeGroupReadBatch&,std::span<const std::byte>,
      WorkerExecution,const std::function<bool()>& interrupted={}) const;
 private:
  NativeGroupReadExchange()=default;
  SharedWorkerRequest request_;
  std::weak_ptr<PagedReadMetadata> capture_;
  Digest digest_{};
  group_decode::Layout layout_{};
  std::uint64_t id_=0;
};
}
