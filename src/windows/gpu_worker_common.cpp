#include "gpu_worker_common.hpp"
#include "hip_worker_exit.hpp"
#include <windows.h>
#include <new>
#include <system_error>
namespace flitzfs {
namespace {
GpuRecoveryOutcome supervisor_failure(const WorkerExchangeSession& session) noexcept {
  try {
    if(const auto error=session.failure())std::rethrow_exception(error);
  }catch(const std::bad_alloc&){return GpuRecoveryOutcome::temporary_pressure;}
  catch(const std::system_error& error) {
    if(error.code().category()==std::system_category()) {
      const auto code=error.code().value();
      if(code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND ||
          code==ERROR_MOD_NOT_FOUND || code==ERROR_BAD_EXE_FORMAT)
        return GpuRecoveryOutcome::runtime_unavailable;
      if(code==ERROR_NOT_ENOUGH_MEMORY || code==ERROR_OUTOFMEMORY ||
          code==ERROR_NO_SYSTEM_RESOURCES)
        return GpuRecoveryOutcome::temporary_pressure;
    }
  }catch(...){}
  return GpuRecoveryOutcome::device_fault; // Unknown failure never proves repair.
}

}
GpuRecoveryOutcome worker_transport_failure(const WorkerExchangeSession& session,
    const WorkerExchangeResult& result,WorkerExecution expected) noexcept {
  if (expected == WorkerExecution::hip && result.code == WorkerExchangeCode::worker_failed &&
      result.process.exit_confirmed) {
    switch (static_cast<HipWorkerExit>(hip_worker_exit_category(result.process.exit_code))) {
      case HipWorkerExit::runtime_unavailable: return GpuRecoveryOutcome::runtime_unavailable;
      case HipWorkerExit::unsupported_configuration: return GpuRecoveryOutcome::unsupported_configuration;
      case HipWorkerExit::temporary_pressure: return GpuRecoveryOutcome::temporary_pressure;
      case HipWorkerExit::invalid_request:
      case HipWorkerExit::integrity_failure: return GpuRecoveryOutcome::integrity_failure;
      case HipWorkerExit::device_fault: return GpuRecoveryOutcome::device_fault;
    }
  }
  switch(result.code) {
    case WorkerExchangeCode::timed_out:
    case WorkerExchangeCode::cancelled:
      // Exact process retirement says nothing about a HIP context/driver.
      // CPU fixtures can use pressure hysteresis; HIP needs recovery evidence.
      return expected==WorkerExecution::hip?GpuRecoveryOutcome::device_fault:
          GpuRecoveryOutcome::temporary_pressure;
    case WorkerExchangeCode::invalid_response:return GpuRecoveryOutcome::integrity_failure;
    case WorkerExchangeCode::supervisor_failed:return supervisor_failure(session);
    default:return GpuRecoveryOutcome::device_fault;
  }
}
GpuRecoveryOutcome worker_gate_failure(GpuGateCode code) noexcept {
  switch(code) {
    case GpuGateCode::invalid_output:
    case GpuGateCode::task_failed:
    case GpuGateCode::mismatch:
    case GpuGateCode::stale_result:return GpuRecoveryOutcome::integrity_failure;
    default:return GpuRecoveryOutcome::temporary_pressure;
  }
}
}
