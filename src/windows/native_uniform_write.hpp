#pragma once
#include "native_file_info.hpp"
#include "uniform_lz4_worker_controller.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#include <array>
#include <optional>
namespace flitzfs {
// Admission and immutable copying shared with the deferred callback owner.
std::vector<std::byte> own_native_uniform_source(std::span<const std::byte>,NativeFileInfoAllocation);
using NativeUniformSweepGrant=GpuLz4SweepGrant;
struct NativeUniformWorkerBinding {
  std::filesystem::path helper;
  GpuConfigurationId configuration{};
  int ordinal=-1;
  std::array<std::byte,32> runtime_sha256{};
  // Optional qualified per-quantum work grant, not a limit on the full write.
  // Absent preserves the original single-window worker route.
  std::optional<NativeUniformSweepGrant> sweep;
  // Used ONLY with the explicitly CPU-semantic test backend.
  std::vector<std::wstring> cpu_fixture_arguments;
  // Optional diagnostic evidence; default writes perform no extra payload hash.
  bool capture_advice_receipt=false;
};
struct NativeUniformDispatch {
  std::vector<std::wstring> arguments;
  std::uint32_t quantum_chunks=0,windows=0;
  std::uint64_t device_peak_bytes=0;
  bool reused=false;
};
// Pure command/geometry preparation; no runtime, process, backing or GPU calls.
// Validates optional sweep grants for BOTH real and CPU-semantic callers.
NativeUniformDispatch prepare_native_uniform_dispatch(const NativeUniformWorkerBinding&,
    const GpuHealthEvidence&,const PreparedUniformLz4&,WorkerExecution,
    const GpuLz4SweepGrant* admitted_sweep=nullptr);
struct NativeUniformWriteStart {
  NativeWindowsIoOutcome admission;
  UniformLz4WorkerStart worker;
  std::uint32_t eligible_bytes=0,quantum_chunks=0,planned_windows=0;
  std::uint64_t device_peak_bytes=0;
  bool reused=false;
};
// Integrity receipts describe verified advice before native publication. They
// are diagnostic facts, not authentication or a new stored representation.
struct NativeUniformAdviceReceipt {
  std::uint32_t payload_bytes=0;
  std::array<std::byte,32> payload_digest{};
  bool captured=false,uniform_payloads=false;
};
struct NativeUniformWriteResult {
  NativeWindowsIoOutcome outcome;
  ULONG transferred=0;
  FSP_FSCTL_FILE_INFO info{};
  std::uint32_t candidate_count=0,candidate_bytes=0;
  NativeUniformAdviceReceipt advice{};
  UniformLz4WorkerOutcome worker{};
};
// Explicit operation, not installed in ordinary callbacks. One serialized
// control thread; the session must outlive this coordinator without moving,
// and the external file context must stay owned until completion.
class ControlledNativeUniformWrite final {
 public:
  explicit ControlledNativeUniformWrite(NativeFileSession&,WorkerExecution expected=WorkerExecution::hip);
  ControlledNativeUniformWrite(const ControlledNativeUniformWrite&)=delete;
  ControlledNativeUniformWrite& operator=(const ControlledNativeUniformWrite&)=delete;
  ControlledNativeUniformWrite(ControlledNativeUniformWrite&&)=delete;
  ControlledNativeUniformWrite& operator=(ControlledNativeUniformWrite&&)=delete;
  // A transferable source must be the exact supplied span and already have
  // an owning resource admission. Success transfers it with no second copy.
  NativeUniformWriteStart begin(const NativeFileHandle&,std::span<const std::byte>,
      UINT64 offset,bool append,bool constrained,const NativeAutomaticMetadata&,
      NativeFileInfoAllocation,const NativeUniformWorkerBinding&,bool consent,bool worthwhile,
      const GpuHealthEvidence&,std::uint64_t now_ms,const GpuLz4ResidentCost&,
      GpuLz4WindowPolicy,ProcessBudget,std::uint32_t deadline_ms,
      std::vector<std::byte>* transferable_source=nullptr) noexcept;
  GpuWorkerState poll(const GpuMemorySnapshot&,std::uint64_t now_ms) noexcept;
  std::span<const std::byte> use_cpu_fallback() noexcept;
  std::optional<NativeUniformWriteResult> finish() noexcept;
  bool awaiting_fresh_observation() const noexcept{return pending_ && submitted_ && controller_.awaiting_fresh_observation();}
  bool request_cancel() const noexcept{return controller_.request_cancel();}
  bool try_retire() noexcept;
  std::uint32_t process_id() const noexcept{return controller_.process_id();}
 private:
  NativeFileSession& session_;
  WorkerExecution expected_;
  UniformLz4WorkerController controller_;
  std::vector<std::byte> source_;
  NativeFileHandle handle_;
  NativeAutomaticMetadata effect_{};
  NativeFileInfoAllocation mode_=NativeFileInfoAllocation::unavailable;
  UINT64 offset_=0,first_extent_=0;
  bool append_=false,constrained_=false,submitted_=false,pending_=false,completed_=false;
  bool capture_advice_=false;
  NativeUniformWriteResult result_{};
};
}
