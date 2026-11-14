#include "uniform_lz4_worker_controller.hpp"
#include "../gpu_request_id.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
namespace flitzfs {
namespace {
bool complete_host_accounting(const PreparedUniformLz4& batch,ProcessBudget budget,
    std::uint64_t admitted_peak) noexcept{
  if(!valid_process_budget(budget))return false;
  const auto& limits=batch.limits();std::uint64_t required=0,part=0;
  const auto charge=[&](std::uint64_t bytes,std::uint64_t copies){
    return uniform_lz4::multiply(bytes,copies,part) && uniform_lz4::add(required,part,required);
  };
  // Child is enforced independently. Parent covers its immutable/serialized/
  // transport copies and one no-larger replacement source, reply mapping,
  // supervisor result and accepted payload, size/offset directories, scratch
  // and supervisor overhead. No presumed RAM size or absolute input ceiling.
  return charge(budget.memory_mib,1024*1024) && charge(limits.request_bytes,6) &&
      charge(limits.reply_bytes,3) && charge(limits.chunks,12) &&
      charge(4ULL*1024*1024+65536+sizeof(std::uint64_t),1) && required<=admitted_peak;
}
}
UniformLz4WorkerController::UniformLz4WorkerController(WorkerExecution e):expected_(e){
  if(e!=WorkerExecution::hip && e!=WorkerExecution::cpu_semantics)
    throw std::invalid_argument("unknown uniform worker backend");
}
void UniformLz4WorkerController::report(GpuRecoveryOutcome outcome) noexcept{
  if(!ticket_ || reported_)return;
  if(!policy_.finish(ticket_,outcome,false))disabled_=true;
  reported_=true;
}
void UniformLz4WorkerController::select_cpu(GpuRecoveryOutcome outcome,GpuGateCode code,bool finalize) noexcept{
  // Deliberately retain large owned vectors. Releasing them here could enter
  // the allocator/OS and turn CPU selection into cleanup of a failed worker.
  state_=GpuWorkerState::cpu_selected;last_gate_=code;
  if(finalize)report(outcome);
}
UniformLz4WorkerStart UniformLz4WorkerController::begin(PreparedUniformLz4&& input,
    const std::filesystem::path& helper,const std::vector<std::wstring>& arguments,
    bool consent,bool worthwhile,const GpuHealthEvidence& evidence,std::uint64_t now,
    const GpuLz4ResidentCost& cost,GpuLz4WindowPolicy window,ProcessBudget budget,std::uint32_t deadline){
  if(state_==GpuWorkerState::waiting || (ticket_ && pending_ &&
      input.plaintext().size()>pending_->plaintext().size()))
    return {{{GpuRouteReason::worker_busy},GpuGateCode::wrong_state,false},false};
  // No late decode/retirement work on this admission path.
  if(!ticket_){pending_.reset();generation_=0;}
  current_.emplace(std::move(input));selected_=UniformLz4Candidates{};offsets_.clear();verified_=false;
  state_=GpuWorkerState::cpu_selected;last_gate_=GpuGateCode::wrong_state;
  if(ticket_)return {{{GpuRouteReason::worker_busy},last_gate_,false},true};
  if(!consent)return {{{GpuRouteReason::disabled},last_gate_,false},true};
  if(!worthwhile)return {{{GpuRouteReason::unqualified},last_gate_,false},true};
  if(disabled_)return {{{GpuRouteReason::exhausted},last_gate_,false},true};
  if(!gpu_lz4_cpu_verification_available())return {{{GpuRouteReason::unqualified},last_gate_,false},true};
  if(!current_->ready() || !valid_process_budget(budget) || !deadline || deadline>budget.timeout_ms ||
      now>(std::numeric_limits<std::uint64_t>::max)()-deadline){
    last_gate_=GpuGateCode::invalid_budget;return {{{GpuRouteReason::bad_evidence},last_gate_,false},true};
  }
  const auto plan=plan_gpu_lz4_window(current_->plaintext().size(),current_->layout().chunk_bytes,
      evidence,now,cost,window);
  if(plan.code!=GpuGateCode::admitted || plan.layout.input_bytes!=current_->plaintext().size() ||
      plan.layout.total_bytes!=current_->layout().total_bytes ||
      !complete_host_accounting(*current_,budget,plan.host_peak_bytes)){
    last_gate_=plan.code==GpuGateCode::admitted?GpuGateCode::invalid_budget:plan.code;
    return {{{GpuRouteReason::unqualified},last_gate_,false},true};
  }
  auto checked=evidence;
  checked.supervisor_quiescent=evidence.supervisor_quiescent && !session_ && WorkerExchangeSession::available();
  const auto route=policy_.consider(consent,worthwhile,checked,*current_,now);
  if(!route.try_gpu())return {{route,last_gate_,false},true};
  ticket_=route.ticket;reported_=false;
  try{
    pending_.emplace(std::move(*current_));current_.reset();
    generation_=detail::next_gpu_request_id();
    if(!generation_){
      disabled_=true;select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::invalid_budget);
      return {{route,last_gate_,false},true};
    }
    started_=now;deadline_=now+deadline;baseline_=checked.memory;
    const auto frame=encode_uniform_lz4_request(*pending_,generation_);
    const auto& l=pending_->limits();
    session_.emplace(WorkerExchangeSession::start_sized(helper,arguments,frame,
        {static_cast<std::size_t>(l.request_bytes),static_cast<std::size_t>(l.reply_bytes)},budget));
    state_=GpuWorkerState::waiting;last_gate_=GpuGateCode::admitted;
    return {{route,last_gate_,true},true};
  }catch(...){
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
    return {{route,last_gate_,false},true};
  }
}
GpuWorkerState UniformLz4WorkerController::poll(const GpuMemorySnapshot& after,std::uint64_t now) noexcept{
  if(state_!=GpuWorkerState::waiting)return state_;
  if(now<started_ || now>=deadline_){
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::expired,false);
    (void)request_cancel();return state_;
  }
  if(verified_){
    if(now<=verified_poll_)return state_;
    if(after.captured_ms<=verified_poll_){
      select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::stale_budget);return state_;
    }
    const auto code=check_gpu_memory_transition(baseline_,after,baseline_.luid,baseline_.node_mask,
        now,pending_->layout().total_bytes);
    if(code!=GpuGateCode::admitted)select_cpu(worker_gate_failure(code),code);
    else{state_=GpuWorkerState::worker_selected;last_gate_=GpuGateCode::accepted;report(GpuRecoveryOutcome::accepted);}
    return state_;
  }
  const auto* result=session_->try_result();if(!result)return state_;
  if(result->code!=WorkerExchangeCode::completed){
    select_cpu(worker_transport_failure(*session_,*result,expected_),GpuGateCode::worker_failed);return state_;
  }
  try{
    auto candidate=accept_uniform_lz4_reply(result->response,*pending_,generation_,expected_);
    std::vector<std::uint64_t> offsets;offsets.reserve(candidate.sizes.size()+1);offsets.push_back(0);
    for(const auto n:candidate.sizes)offsets.push_back(offsets.back()+n);
    selected_=std::move(candidate);offsets_=std::move(offsets);verified_=true;verified_poll_=now;
    // The NEXT observation, obtained after this CPU validation returns, decides
    // acceptance. CPU may still select its own source before that observation.
  }catch(const std::bad_alloc&){
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed);
  }catch(...){select_cpu(GpuRecoveryOutcome::integrity_failure,GpuGateCode::invalid_output);}
  return state_;
}
std::span<const std::byte> UniformLz4WorkerController::use_cpu_fallback() noexcept{
  if(state_==GpuWorkerState::waiting)
    select_cpu(GpuRecoveryOutcome::temporary_pressure,GpuGateCode::worker_failed,false);
  return source();
}
bool UniformLz4WorkerController::try_retire() noexcept{
  if(!ticket_)return !session_ && WorkerExchangeSession::available();
  if(!reported_){
    if(state_==GpuWorkerState::waiting || !session_)return false;
    const auto* late=session_->try_result();if(!late)return false;
    auto outcome=worker_transport_failure(*session_,*late,expected_);
    if(late->code==WorkerExchangeCode::completed){
      outcome=GpuRecoveryOutcome::temporary_pressure;
      try{verify_uniform_lz4_reply(late->response,*pending_,generation_,expected_);}
      catch(const std::bad_alloc&){}
      catch(...){outcome=GpuRecoveryOutcome::integrity_failure;}
    }
    report(outcome);
  }
  const auto confirmed=session_?session_->retirement_confirmed():WorkerExchangeSession::available();
  if(!confirmed)return false;
  if(!policy_.confirm_quiescent(ticket_)){disabled_=true;return false;}
  session_.reset();ticket_=0;reported_=false;generation_=0;
  if(current_)pending_.reset();
  return WorkerExchangeSession::available();
}
bool UniformLz4WorkerController::request_cancel() const noexcept{return session_ && session_->request_cancel();}
std::uint32_t UniformLz4WorkerController::process_id() const noexcept{return session_?session_->process_id():0;}
std::span<const std::byte> UniformLz4WorkerController::source() const noexcept{
  if(current_)return current_->plaintext();if(pending_)return pending_->plaintext();return {};
}
std::uint32_t UniformLz4WorkerController::count() const noexcept{
  return current_?current_->layout().chunks:pending_?pending_->layout().chunks:0;
}
std::span<const std::byte> UniformLz4WorkerController::candidate(std::uint32_t index) const noexcept{
  if(state_!=GpuWorkerState::worker_selected || index>=selected_.sizes.size() ||
      offsets_.size()!=selected_.sizes.size()+1)return {};
  return std::span(selected_.payload).subspan(static_cast<std::size_t>(offsets_[index]),selected_.sizes[index]);
}
}
