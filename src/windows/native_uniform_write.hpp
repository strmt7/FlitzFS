#pragma once
#include "native_file_info.hpp"
#include "uniform_lz4_worker_controller.hpp"
#include <optional>
namespace flitzfs {
struct NativeUniformWorkerBinding {
  std::filesystem::path helper;
  GpuConfigurationId configuration{};
  int ordinal=-1;
  std::array<std::byte,32> runtime_sha256{};
  // Used ONLY with the explicitly CPU-semantic test backend.
  std::vector<std::wstring> cpu_fixture_arguments;
};
struct NativeUniformWriteStart {
  NativeWindowsIoOutcome admission;
  UniformLz4WorkerStart worker;
  std::uint32_t eligible_bytes=0;
};
struct NativeUniformWriteResult {
  NativeWindowsIoOutcome outcome;
  ULONG transferred=0;
  FSP_FSCTL_FILE_INFO info{};
  std::uint32_t candidate_count=0,candidate_bytes=0;
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
  NativeUniformWriteStart begin(const NativeFileHandle&,std::span<const std::byte>,
      UINT64 offset,bool append,bool constrained,const NativeAutomaticMetadata&,
      NativeFileInfoAllocation,const NativeUniformWorkerBinding&,bool consent,bool worthwhile,
      const GpuHealthEvidence&,std::uint64_t now_ms,const GpuLz4ResidentCost&,
      GpuLz4WindowPolicy,ProcessBudget,std::uint32_t deadline_ms) noexcept;
  GpuWorkerState poll(const GpuMemorySnapshot&,std::uint64_t now_ms) noexcept;
  std::span<const std::byte> use_cpu_fallback() noexcept;
  std::optional<NativeUniformWriteResult> finish() noexcept;
  bool awaiting_fresh_observation() const noexcept{return pending_ && submitted_ && controller_.awaiting_fresh_observation();}
  bool request_cancel() const noexcept{return controller_.request_cancel();}
  bool try_retire() noexcept{return controller_.try_retire();}
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
  NativeUniformWriteResult result_{};
};
}
