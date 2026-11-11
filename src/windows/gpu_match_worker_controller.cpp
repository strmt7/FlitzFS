#include "gpu_match_worker_controller.hpp"
#include "../gpu_request_id.hpp"
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
namespace flitzfs {
namespace {
constexpr auto owned_bytes=2*match_detail::input_limit*(1+sizeof(match_detail::u16));
static_assert(owned_bytes+2*match_detail::lane_limit*sizeof(StoreMatchProposal)+
    kMatchWorkerRequestLimit+kMatchWorkerReplyLimit<=2U*1024*1024);
}
GpuMatchWorkerController::GpuMatchWorkerController(WorkerExecution expected):expected_(expected) {
  if(expected!=WorkerExecution::hip&&expected!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("unknown trusted match worker backend");
}
void GpuMatchWorkerController::report(GpuRecoveryOutcome outcome) noexcept {
  if(!ticket_||reported_)return;
  if(!policy_.finish(ticket_,outcome,false))disabled_=true;
  reported_=true;
}
void GpuMatchWorkerController::select_cpu(GpuRecoveryOutcome outcome,GpuGateCode code,bool finalize) noexcept {
  selected_.clear();state_=GpuWorkerState::cpu_selected;last_gate_=code;
  if(finalize)report(outcome);
}
bool GpuMatchWorkerController::try_retire() noexcept {
  if(!ticket_)return !session_&&WorkerExchangeSession::available();
  if(!reported_) {
    if(state_==GpuWorkerState::waiting||!session_)return false;
    const auto* late=session_->try_result();
    if(!late)return false;
    auto outcome=worker_transport_failure(*session_,*late,expected_);
    if(late->code==WorkerExchangeCode::completed) {
      outcome=GpuRecoveryOutcome::temporary_pressure;
      try{(void)accept_match_worker_reply(late->response,*pending_,generation_,expected_);}
      catch(const std::bad_alloc&){}
      catch(...){outcome=GpuRecoveryOutcome::integrity_failure;}
    }
    report(outcome);
  }
  const bool confirmed=session_?session_->retirement_confirmed():WorkerExchangeSession::available();
  if(!confirmed)return false;
  if(!policy_.confirm_quiescent(ticket_)){disabled_=true;return false;}
  session_.reset();ticket_=0;reported_=false;generation_=0;
  if(current_)pending_.reset();
  return WorkerExchangeSession::available();
}
GpuWorkerStart GpuMatchWorkerController::begin(PreparedGpuMatchSearch&& input,
    const std::filesystem::path& helper,const std::vector<std::wstring>& arguments,
    bool consent,bool worthwhile,const GpuHealthEvidence& evidence,std::uint64_t now,
    ProcessBudget budget,std::uint32_t deadline) {
  if(state_==GpuWorkerState::waiting)
    return {{GpuRouteReason::worker_busy},GpuGateCode::wrong_state,false};
  (void)try_retire();
  if(!ticket_){pending_.reset();generation_=0;}
  current_.emplace(std::move(input));selected_.clear();state_=GpuWorkerState::cpu_selected;
  last_gate_=GpuGateCode::wrong_state;
  if(disabled_)return {{GpuRouteReason::exhausted},last_gate_,false};
  if(!deadline||deadline>100||now>(std::numeric_limits<std::uint64_t>::max)()-deadline) {
    last_gate_=GpuGateCode::invalid_budget;
    return {{GpuRouteReason::bad_evidence},last_gate_,false};
  }
  auto checked=evidence;
  checked.supervisor_quiescent=evidence.supervisor_quiescent&&!ticket_&&!session_&&WorkerExchangeSession::available();
  const auto route=policy_.consider(consent,worthwhile,checked,*current_,now);
  if(!route.try_gpu())return {route,last_gate_,false};
  ticket_=route.ticket;reported_=false;
  try {
    pending_.emplace(std::move(*current_));current_.reset();
    generation_=detail::next_gpu_request_id();
    if(!generation_) {
      disabled_=true;
      select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::invalid_budget);
      return {route,last_gate_,false};
    }
    luid_=checked.memory.luid;node_mask_=checked.memory.node_mask;
    started_=now;deadline_=now+deadline;nonlocal_=checked.memory.nonlocal_usage;
    const auto request=encode_match_worker_request(*pending_,generation_);
    session_.emplace(WorkerExchangeSession::start(helper,arguments,request,budget));
    last_gate_=GpuGateCode::admitted;state_=GpuWorkerState::waiting;
    return {route,last_gate_,true};
  }catch(...) {
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
    return {route,last_gate_,false};
  }
}
GpuWorkerState GpuMatchWorkerController::poll(const GpuMemorySnapshot& after,std::uint64_t now) noexcept {
  if(state_!=GpuWorkerState::waiting)return state_;
  if(now<started_||now>=deadline_) {
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::expired,false);
    (void)request_cancel();
    return state_;
  }
  const auto* result=session_->try_result();
  if(!result)return state_;
  if(result->code!=WorkerExchangeCode::completed) {
    select_cpu(worker_transport_failure(*session_,*result,expected_),GpuGateCode::worker_failed);
    return state_;
  }
  try {
    auto proposals=accept_match_worker_reply(result->response,*pending_,generation_,expected_);
    auto code=after.captured_ms<started_?GpuGateCode::stale_budget:
        check_gpu_memory_budget(after,luid_,node_mask_,now,0);
    if(code==GpuGateCode::admitted&&after.nonlocal_usage>nonlocal_)code=GpuGateCode::nonlocal_growth;
    if(code!=GpuGateCode::admitted) {
      select_cpu(worker_gate_failure(code),code);
    } else {
      selected_=std::move(proposals);last_gate_=GpuGateCode::accepted;
      state_=GpuWorkerState::worker_selected;report(GpuRecoveryOutcome::accepted);
    }
  }catch(const std::bad_alloc&) {
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
  }catch(...) {
    select_cpu(GpuRecoveryOutcome::integrity_failure,GpuGateCode::invalid_output);
  }
  return state_;
}
std::span<const std::byte> GpuMatchWorkerController::use_cpu_fallback() noexcept {
  if(state_==GpuWorkerState::waiting)
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed,false);
  return source();
}
bool GpuMatchWorkerController::request_cancel() const noexcept {
  return session_&&session_->request_cancel();
}
std::uint32_t GpuMatchWorkerController::process_id() const noexcept {
  return session_?session_->process_id():0;
}
std::span<const std::byte> GpuMatchWorkerController::source() const noexcept {
  if(current_)return current_->input();
  if(pending_)return pending_->input();
  return {};
}
std::span<const StoreMatchProposal> GpuMatchWorkerController::selected_proposals() const noexcept {
  return state_==GpuWorkerState::worker_selected?std::span<const StoreMatchProposal>(selected_):
      std::span<const StoreMatchProposal>{};
}
}
