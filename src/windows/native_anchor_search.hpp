#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include "hip_route_discovery.hpp"
#include <stop_token>
#include <utility>

namespace flitzfs {
enum class WorkerExchangeCode;

// Supplied by an independently qualified package and workload profile.
// The memory field is the measured fixed child allowance. Each launch also
// funds its exact request, reply, device slab and transport copies.
struct NativeAnchorSearchOptions {
  HipDiscoveredRoute route;
  ProcessBudget worker;
  std::uint32_t minimum_members=0,maximum_members=0;
  std::uint32_t duty_ppm=10000;
  anchor_match::BatchOutput output=anchor_match::BatchOutput::anchors;
  std::uint32_t maximum_dispatch_members=0;
  // Maximum queue depth for a funded slab and profile trailer.
  std::uint32_t dispatches_per_wait=1;
  // Price alternate references only when the full batch can support them.
  bool prefer_reference=false;
  // Fixed sizing is reserved for explicit hardware comparisons.
  bool adaptive_batch_sizing=true;
};

struct NativeAnchorSearchStats {
  std::uint64_t submitted=0,verified=0,declined=0,yielded=0;
  bool retirement_confirmed=true;
  std::uint64_t submitted_groups=0,verified_groups=0,integrity_failures=0,empty_xor_batches=0;
  std::uint64_t reply_bytes=0;
  std::uint64_t reference_batches=0,reselected_groups=0;
  std::uint64_t submitted_queued_batches=0,verified_queued_batches=0,queued_worker_wall_ns=0,queued_kernel_wait_ns=0;
  std::uint64_t submitted_kernel_launches=0;
  std::uint32_t queued_dispatches_per_wait=0,largest_dispatch_members=0;
  // Work for the most recent independently verified copy batch.
  std::uint64_t copy_suggested_ranges=0,copy_compared_ranges=0,copy_suggested_bytes=0,copy_compared_bytes=0;
  std::optional<WorkerExchangeCode> last_exchange;
  std::uint32_t last_worker_exit_code=0;
  std::uint64_t last_retirement_elapsed_ms=0;
  std::uint64_t last_worker_elapsed_ms=0,verified_timing_samples=0;
  std::optional<std::uint32_t> learned_launch_limit;
  bool last_publication_observed=false;
};

enum class NativeAnchorOpportunity {poll,foreground,idle};

struct NativeAnchorRecoveryTarget {
  GpuConfigurationId identity{};
  std::uint64_t incident=0;
  bool operator==(const NativeAnchorRecoveryTarget&) const = default;
};
struct NativeAnchorRecoveryCheck;
// A CPU-verified computation on the newly pinned route for a specific
// retired fault. Discovery or another route cannot manufacture this proof.
class NativeAnchorRecoveryProof final {
 public:
  NativeAnchorRecoveryProof(NativeAnchorRecoveryProof&&) noexcept=default;
  NativeAnchorRecoveryProof& operator=(NativeAnchorRecoveryProof&&) noexcept=default;
  NativeAnchorRecoveryProof(const NativeAnchorRecoveryProof&)=delete;
  NativeAnchorRecoveryProof& operator=(const NativeAnchorRecoveryProof&)=delete;
 private:
  friend class NativeAnchorSearchController;
  friend NativeAnchorRecoveryCheck qualify_native_anchor_recovery(NativeAnchorRecoveryTarget,
      const HipDiscoveredRoute&,ProcessBudget,std::stop_token) noexcept;
  NativeAnchorRecoveryProof(NativeAnchorRecoveryTarget target,std::shared_ptr<const HipRoutePins> pins)
      :target_(target),pins_(std::move(pins)) {}
  NativeAnchorRecoveryTarget target_;
  std::shared_ptr<const HipRoutePins> pins_;
};
struct NativeAnchorRecoveryCheck {
  HipDiscoveryStatus status=HipDiscoveryStatus::configuration_quarantined;
  std::optional<NativeAnchorRecoveryProof> proof;
};
// Run outside the filesystem and controller locks after fresh discovery.
NativeAnchorRecoveryCheck qualify_native_anchor_recovery(NativeAnchorRecoveryTarget,
    const HipDiscoveredRoute&,ProcessBudget,std::stop_token={}) noexcept;

// One serialized owner, outside the filesystem control lock. The transport
// retains its own route pins and observes exact worker and supervisor exit.
class NativeAnchorSearchController final {
 public:
  explicit NativeAnchorSearchController(NativeAnchorSearchOptions);
  ~NativeAnchorSearchController();
  NativeAnchorSearchController(const NativeAnchorSearchController&)=delete;
  NativeAnchorSearchController& operator=(const NativeAnchorSearchController&)=delete;
  std::size_t stage_grant(std::uint64_t now,std::size_t funded_extents) const noexcept;
  bool fits_group_budget(std::size_t admitted_members) const noexcept;
  void start(std::span<const NativeSharedBaseSearchWindow>,std::uint64_t activity) noexcept;
  // Validate reply bytes only on an admitted idle opportunity.
  void observe(std::uint64_t activity,std::uint64_t now,
      NativeAnchorOpportunity opportunity=NativeAnchorOpportunity::poll) noexcept;
  bool waiting() const noexcept;
  // Reserve both executing work and retired replies awaiting validation.
  std::span<const NativeSharedBaseSearchWindow> pending_prefix() const noexcept;
  std::uint64_t deadline() const noexcept;
  void* wake_handle() const noexcept;
  std::span<const NativeSharedBaseSearchReply> replies() const noexcept;
  // Called after the serialized writer advances one admitted step.
  void settle_reply_checks() noexcept;
  bool can_reacquire() const noexcept;
  bool take_device_interruption() noexcept;
  std::optional<NativeAnchorRecoveryTarget> recovery_target() const noexcept;
  bool take_deadline_recovery() noexcept;
  bool rebind(HipDiscoveredRoute,const NativeAnchorRecoveryProof* =nullptr) noexcept;
  void discard_reply() noexcept;
  void cancel() noexcept;
  bool stop() noexcept;
  NativeAnchorSearchStats stats() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
