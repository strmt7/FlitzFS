#pragma once
#include "flitzfs/gpu_anchor_match.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/worker_wire.hpp"
#include <array>
#include <optional>

namespace flitzfs::detail {

struct AnchorMatchLayout {
  std::uint32_t members,request_bytes,reply_bytes;
  bool operator==(const AnchorMatchLayout&) const = default;
};

struct AnchorMatchSweepPlan {
  AnchorMatchLayout whole;
  std::uint32_t quantum_members,windows,device_bytes;
  bool operator==(const AnchorMatchSweepPlan&) const = default;
};

struct AnchorMatchSource {
  std::uint64_t id;
  std::span<const std::byte> source;
  std::array<std::byte,32> source_digest{};
};

struct AnchorMatchRequest {
  std::uint64_t id;
  std::vector<std::byte> source;
  std::array<std::byte,32> source_digest{};
  AnchorMatchSource view() const & noexcept {return {id,source,source_digest};}
  AnchorMatchSource view() const && = delete;
};
using AnchorMatchAdvice=std::vector<std::vector<NativeSharedBaseCopyRange>>;
using AnchorXorAdvice=std::vector<std::vector<std::byte>>;

struct AnchorMatchBatchPlan {
  std::vector<AnchorMatchSweepPlan> groups;
  std::uint32_t request_bytes,reply_bytes,quantum_members,device_bytes,launches;
  bool operator==(const AnchorMatchBatchPlan&) const = default;
};

class AnchorMatchBatchRequest final {
 public:
  AnchorMatchBatchRequest(AnchorMatchBatchRequest&&) noexcept = default;
  AnchorMatchBatchRequest& operator=(AnchorMatchBatchRequest&&) noexcept = default;
  std::uint64_t id() const noexcept {return id_;}
  const std::array<std::byte,32>& digest() const & noexcept {return digest_;}
  const AnchorMatchBatchPlan& plan() const & noexcept {return plan_;}
  std::span<const AnchorMatchSource> groups() const & noexcept {
    if(!snapshot_)return {};
    return groups_;
  }
  const std::array<std::byte,32>& digest() const && = delete;
  const AnchorMatchBatchPlan& plan() const && = delete;
  std::span<const AnchorMatchSource> groups() const && = delete;
  SharedWorkerRequest snapshot() const noexcept {return snapshot_;}
 private:
  AnchorMatchBatchRequest()=default;
  friend class AnchorMatchBatchBuilder;
  friend AnchorMatchBatchRequest decode_anchor_match_batch_request(SharedWorkerRequest,
      std::uint32_t,std::uint32_t,std::uint32_t,std::uint64_t,std::uint32_t);
  // Every group view is validated against this one private allocation.
  SharedWorkerRequest snapshot_;
  std::uint64_t id_=0;
  std::array<std::byte,32> digest_{};
  AnchorMatchBatchPlan plan_{};
  std::vector<AnchorMatchSource> groups_;
};

struct AnchorMatchTransferPlan {
  std::uint32_t members_per_transfer,device_bytes,transfers;
  bool operator==(const AnchorMatchTransferPlan&) const = default;
};

std::uint32_t anchor_match_members(std::span<const std::byte> source);
AnchorMatchLayout anchor_match_layout(std::uint32_t members);
// The caller independently admits the whole host request and its transport.
// This plan admits one device slab and bounds the number of search dispatches.
AnchorMatchSweepPlan plan_anchor_match_sweep(std::uint32_t members,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_windows);
std::vector<std::byte> encode_anchor_match_request(std::span<const std::byte> base,
    std::span<const std::byte> members,std::uint64_t id,std::optional<AnchorMatchLayout> admitted={});
AnchorMatchRequest decode_anchor_match_request(std::span<const std::byte> frame,
    std::optional<AnchorMatchLayout> admitted={});
std::vector<anchor_match::u32> cpu_anchor_match_records(const AnchorMatchSource&);
AnchorMatchAdvice verify_anchor_match_records(const AnchorMatchSource&,std::span<const anchor_match::u32>);
std::vector<std::byte> encode_anchor_match_reply(const AnchorMatchSource&,
    std::span<const anchor_match::u32>,std::size_t reserve_tail_bytes=0);
AnchorMatchAdvice accept_anchor_match_reply(std::span<const std::byte> frame,const AnchorMatchSource&);

// The grant counts the index build and all search launches for each group.
AnchorMatchBatchPlan plan_anchor_match_batch(std::span<const std::uint32_t> members,
    std::uint32_t maximum_quantum,std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches);
// Transfers can span several launches. The last transfer of each group may
// be partial, while every earlier transfer ends at a dispatch boundary.
AnchorMatchTransferPlan plan_anchor_match_transfers(const AnchorMatchBatchPlan&,std::uint64_t maximum_device_bytes);

// After admitting the complete plan, append one owned group at a time. The
// caller can release each source immediately after it is copied and bound.
class AnchorMatchBatchBuilder final {
 public:
  AnchorMatchBatchBuilder(std::uint64_t id,const AnchorMatchBatchPlan&);
  AnchorMatchBatchBuilder(const AnchorMatchBatchBuilder&)=delete;
  AnchorMatchBatchBuilder& operator=(const AnchorMatchBatchBuilder&)=delete;
  void append(std::uint64_t id,std::span<const std::byte> base,std::span<const std::byte> members);
  void append(std::uint64_t id,const NativeSharedBaseSearchWindow&);
  AnchorMatchBatchRequest finish();
 private:
  std::size_t begin_group(std::uint64_t id,std::span<const std::byte> base,std::size_t count);
  void complete_group(std::uint64_t id,std::size_t source_offset);
  AnchorMatchBatchRequest request_;
  std::vector<std::byte> frame_;
};

AnchorMatchBatchRequest decode_anchor_match_batch_request(SharedWorkerRequest snapshot,
    std::uint32_t request_capacity,std::uint32_t reply_capacity,std::uint32_t maximum_quantum,
    std::uint64_t maximum_device_bytes,std::uint32_t maximum_launches);
std::vector<std::byte> encode_anchor_match_batch_reply(const AnchorMatchBatchRequest&,
    std::span<const std::vector<anchor_match::u32>> records,std::size_t reserve_tail_bytes=0,bool packed_xor=false);
std::vector<AnchorMatchAdvice> accept_anchor_match_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest&);
// These bodies are owned, bound to the request and structurally checked.
// The native indexed writer independently proves each candidate's decoded bytes.
std::vector<AnchorXorAdvice> decode_anchor_xor_batch_reply(
    std::span<const std::byte> frame,const AnchorMatchBatchRequest&);

} // namespace flitzfs::detail
