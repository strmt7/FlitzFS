#include "gpu_worker_controller.hpp"
#include "hip_worker_exit.hpp"
#include <windows.h>
#include <new>
#include <system_error>
#include <utility>

namespace flitzfs {
namespace {
constexpr std::size_t prepared_buffers=2*gpu_detail::max_batch_bytes+
    gpu_detail::max_tasks*sizeof(gpu_detail::Task);
// One pending gate + one later CPU batch; either one accepted output or a
// temporary late-validation candidate/status vector. Transport is separate.
static_assert(2*prepared_buffers+gpu_detail::max_batch_bytes+
    gpu_detail::max_tasks*sizeof(std::uint32_t)<=2U*1024*1024);
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
GpuRecoveryOutcome transport_failure(const WorkerExchangeSession& session,
    const WorkerExchangeResult& result,WorkerExecution expected) noexcept {
  if (expected == WorkerExecution::hip && result.code == WorkerExchangeCode::worker_failed &&
      result.process.exit_confirmed) {
    switch (static_cast<HipWorkerExit>(result.process.exit_code)) {
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
GpuRecoveryOutcome gate_failure(GpuGateCode code) noexcept {
  switch(code) {
    case GpuGateCode::invalid_output:
    case GpuGateCode::task_failed:
    case GpuGateCode::mismatch:
    case GpuGateCode::stale_result:return GpuRecoveryOutcome::integrity_failure;
    default:return GpuRecoveryOutcome::temporary_pressure;
  }
}
}
GpuWorkerController::GpuWorkerController(WorkerExecution expected):expected_(expected) {
  if(expected!=WorkerExecution::hip && expected!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("unknown trusted worker backend");
}
void GpuWorkerController::report(GpuRecoveryOutcome outcome) noexcept {
  if(!ticket_ || reported_)return;
  // Reporting never queries/waits on a process. Confirm retirement separately.
  if(!policy_.finish(ticket_,outcome,false))disabled_=true;
  reported_=true;
}
void GpuWorkerController::select_cpu(GpuRecoveryOutcome outcome,GpuGateCode why,bool finalize) noexcept {
  if(gate_)(void)gate_->use_cpu_fallback();
  last_gate_=why;state_=GpuWorkerState::cpu_selected;
  if(finalize)report(outcome);
}
bool GpuWorkerController::try_retire() noexcept {
  if(!ticket_)return !session_ && WorkerExchangeSession::available();
  if(!reported_) {
    if(state_==GpuWorkerState::waiting || !session_)return false;
    const auto* late=session_->try_result();
    if(!late)return false;
    auto outcome=transport_failure(*session_,*late,expected_);
    if(late->code==WorkerExchangeCode::completed) {
      // Terminal CPU bytes stay untouched. Late integrity/availability evidence
      // still belongs to THIS pending configuration before its ticket retires.
      outcome=GpuRecoveryOutcome::temporary_pressure;
      try {(void)accept_worker_reply(late->response,gate_->batch(),generation_,expected_);}
      catch(const std::bad_alloc&){} // Resource failure is not corruption proof.
      catch(...){outcome=GpuRecoveryOutcome::integrity_failure;}
    }
    report(outcome);
  }
  const bool confirmed=session_?session_->retirement_confirmed():WorkerExchangeSession::available();
  if(!confirmed)return false;
  if(!policy_.confirm_quiescent(ticket_)){disabled_=true;return false;}
  session_.reset();ticket_=0;reported_=false;generation_=0;
  if(cpu_)gate_.reset(); // Old gate is no longer needed by the current CPU request.
  return WorkerExchangeSession::available();
}
GpuWorkerStart GpuWorkerController::begin(PreparedGpuBatch&& batch,
    const std::filesystem::path& helper,const std::vector<std::wstring>& arguments,
    bool consent,bool worthwhile,const GpuHealthEvidence& evidence,std::uint64_t now,
    ProcessBudget budget,std::uint32_t deadline) {
  if(state_==GpuWorkerState::waiting)
    return {{GpuRouteReason::worker_busy},GpuGateCode::wrong_state,false};
  (void)try_retire();
  // Retain the pending gate/reference through late-result classification even
  // when a later request selects DIFFERENT CPU bytes. At most one of each.
  if(!ticket_){gate_.reset();generation_=0;}
  cpu_.emplace(std::move(batch));state_=GpuWorkerState::cpu_selected;
  last_gate_=GpuGateCode::wrong_state;
  if(disabled_)return {{GpuRouteReason::exhausted},last_gate_,false};
  if(!deadline || deadline>100) {
    last_gate_=GpuGateCode::invalid_budget;
    return {{GpuRouteReason::bad_evidence},last_gate_,false};
  }
  auto checked=evidence;
  // External compatibility/health cannot override actual transport ownership.
  checked.supervisor_quiescent=evidence.supervisor_quiescent && !ticket_ &&
      !session_ && WorkerExchangeSession::available();
  const auto route=policy_.consider(consent,worthwhile,checked,*cpu_,now);
  if(!route.try_gpu())return {route,last_gate_,false};
  ticket_=route.ticket;reported_=false;
  try {
    gate_=std::make_unique<GpuResultGate>(std::move(*cpu_),checked.memory.luid,
        checked.memory.node_mask,deadline);
    cpu_.reset();
    const auto attempt=gate_->begin(true,checked.memory,now);last_gate_=attempt.code;
    if(attempt.code!=GpuGateCode::admitted) {
      select_cpu(GpuRecoveryOutcome::temporary_pressure,attempt.code);
      return {route,last_gate_,false};
    }
    generation_=attempt.generation;
    const auto request=encode_worker_request(gate_->batch(),generation_);
    session_.emplace(WorkerExchangeSession::start(helper,arguments,request,budget));
    state_=GpuWorkerState::waiting;
    return {route,last_gate_,true};
  }catch(...) {
    // Setup/resource failures do not establish a broken driver. A partially
    // launched session retains its global supervision/lease after abandonment.
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
    return {route,last_gate_,false};
  }
}
GpuWorkerState GpuWorkerController::poll(const GpuMemorySnapshot& after,std::uint64_t now) noexcept {
  if(state_!=GpuWorkerState::waiting)return state_;
  const auto expiry=gate_->expire(now);
  if(expiry!=GpuGateCode::admitted) {
    select_cpu(GpuRecoveryOutcome::temporary_pressure,expiry,false);
    (void)request_cancel();return state_;
  }
  const auto* result=session_->try_result();
  if(!result)return state_;
  if(result->code!=WorkerExchangeCode::completed) {
    select_cpu(transport_failure(*session_,*result,expected_),GpuGateCode::worker_failed);
    return state_;
  }
  try {
    auto reply=accept_worker_reply(result->response,gate_->batch(),generation_,expected_);
    last_gate_=gate_->complete(std::move(reply),after,now);
    if(last_gate_==GpuGateCode::accepted) {
      state_=GpuWorkerState::worker_selected;report(GpuRecoveryOutcome::accepted);
    }else select_cpu(gate_failure(last_gate_),last_gate_);
  }catch(const std::bad_alloc&) {
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
  }catch(...) {
    select_cpu(GpuRecoveryOutcome::integrity_failure,GpuGateCode::invalid_output);
  }
  return state_;
}
std::span<const std::byte> GpuWorkerController::use_cpu_fallback() noexcept {
  if(state_==GpuWorkerState::waiting)
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed,false);
  return selected_bytes();
}
bool GpuWorkerController::request_cancel() const noexcept {
  return session_ && session_->request_cancel();
}
std::uint32_t GpuWorkerController::process_id() const noexcept {
  return session_?session_->process_id():0;
}
std::span<const std::byte> GpuWorkerController::selected_bytes() const noexcept {
  if(state_==GpuWorkerState::cpu_selected)
    return cpu_?cpu_->cpu_reference():gate_?gate_->batch().cpu_reference():std::span<const std::byte>{};
  return state_==GpuWorkerState::worker_selected?gate_->accepted_bytes():std::span<const std::byte>{};
}
} // namespace flitzfs
