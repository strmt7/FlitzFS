#include "native_anchor_search.hpp"
#include "gpu_budget.hpp"
#include "gpu_worker_common.hpp"
#include "../native_group_anchor.hpp"
#include <algorithm>
#include <limits>

namespace flitzfs {
namespace {
template<std::size_t N>std::wstring hex(const std::array<std::byte,N>& bytes) {
  constexpr wchar_t digits[]=L"0123456789abcdef";
  std::wstring result;
  result.reserve(2*N);
  for(const auto byte:bytes) {
    const auto value=std::to_integer<unsigned>(byte);
    result.push_back(digits[value>>4]);
    result.push_back(digits[value&15]);
  }
  return result;
}
constexpr std::uint64_t mib=1024*1024,million=1000000;
constexpr DWORD retirement_grace_ms=2000;
}
struct NativeAnchorSearchController::Impl {
  enum class Disposition {pending,yielded,expired};
  struct Flight {
    WorkerExchangeSession session;
    detail::AnchorMatchBatchRequest request;
    std::uint64_t ticket,started,deadline;
    Disposition disposition=Disposition::pending;
    bool supervisor_exited=false;
  };
  const NativeAnchorSearchOptions options;
  GpuRecoveryPolicy recovery;
  std::optional<Flight> flight;
  NativeSharedBaseSearchWindow window;
  detail::AnchorMatchAdvice advice;
  NativeAnchorSearchStats stats;
  std::uint64_t activity=0,next_launch=0,sequence=0,unobserved_ticket=0;
  bool enabled=false;
  explicit Impl(NativeAnchorSearchOptions configured):options(std::move(configured)) {
    const auto& route=options.route;
    enabled=route.ordinal<64 && route.pins && route.pins->anchor_module_pinned() &&
        route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) &&
        options.minimum_members && options.minimum_members<=options.maximum_members &&
        options.maximum_members<kNativeMaxExtents && options.duty_ppm && options.duty_ppm<=100000 &&
        options.worker.memory_share_divisor && valid_process_budget(options.worker);
  }
  void discard() noexcept {
    window={};
    detail::AnchorMatchAdvice{}.swap(advice);
  }
  void yield(Disposition disposition) noexcept {
    discard();
    if(!flight || flight->disposition!=Disposition::pending)return;
    flight->disposition=disposition;
    (void)flight->session.request_cancel();
    if(disposition==Disposition::yielded)++stats.yielded;
    else ++stats.declined;
  }
  void pace(std::uint64_t now,std::uint64_t started) noexcept {
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    const auto used=(std::max)(std::uint64_t{1},now>=started?now-started:std::uint64_t{0});
    const auto delay=(std::min)(used,maximum/million)*million/options.duty_ppm;
    next_launch=delay<=maximum-now?now+delay:maximum;
  }
  void fail(std::uint64_t ticket,GpuRecoveryOutcome outcome,bool retired) noexcept {
    (void)recovery.finish(ticket,outcome,retired);
    if(outcome!=GpuRecoveryOutcome::temporary_pressure)enabled=false;
  }
};
NativeAnchorSearchController::NativeAnchorSearchController(NativeAnchorSearchOptions options)
    :impl_(std::make_unique<Impl>(std::move(options))) {}
NativeAnchorSearchController::~NativeAnchorSearchController(){impl_->yield(Impl::Disposition::yielded);}
std::size_t NativeAnchorSearchController::stage_grant(std::uint64_t now) const noexcept {
  const auto& self=*impl_;
  return self.enabled && !self.flight && !self.window && !self.unobserved_ticket && now>=self.next_launch?
      self.options.maximum_members:0;
}
bool NativeAnchorSearchController::fits_group_budget(std::size_t admitted_members) const noexcept {
  return admitted_members>=2 && impl_->window.members().size()<admitted_members;
}
void NativeAnchorSearchController::start(NativeSharedBaseSearchWindow source,std::uint64_t activity) noexcept {
  auto& self=*impl_;
  const auto& options=self.options;
  const auto count=source.members().size();
  if(!stage_grant(GetTickCount64()) || count<options.minimum_members || count>options.maximum_members ||
      !WorkerExchangeSession::available())return;
  std::uint64_t ticket=0;
  try {
    const auto& route=options.route;
    const auto observed=query_windows_gpu_budget(route.memory.luid,route.memory.node_mask);
    const auto now=GetTickCount64();
    if(observed.status!=WindowsGpuBudgetStatus::success){++self.stats.declined;return;}
    const auto members=static_cast<std::uint32_t>(count);
    std::uint32_t fit=0,limit=members;
    while(fit<limit) {
      const auto test=fit+(limit-fit+1)/2;
      if(check_gpu_memory_budget(observed.snapshot,route.memory.luid,route.memory.node_mask,now,
          anchor_match::device_bytes(test))==GpuGateCode::admitted)fit=test;
      else limit=test-1;
    }
    if(!fit){++self.stats.declined;return;}
    const auto quantum=(std::min)(fit,anchor_match::max_members);
    const std::array counts{members};
    const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),members+1);
    const auto transfer=detail::plan_anchor_match_transfers(plan,anchor_match::device_bytes(fit));
    const WorkerChannelCapacity capacity{plan.request_bytes,plan.reply_bytes};
    const auto variable=std::uint64_t(transfer.device_bytes)+2ULL*plan.request_bytes+2ULL*plan.reply_bytes;
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    if(options.worker.memory_mib>(maximum-variable)/mib || self.sequence==maximum){self.enabled=false;return;}
    const auto child=(std::max)(options.worker.memory_mib*mib+variable,
        6ULL*plan.request_bytes+4ULL*plan.reply_bytes+mib);
    const auto budget=plan_resource_process_budget(child,observed.snapshot.physical_available,
        observed.snapshot.commit_available,options.worker.memory_share_divisor,options.worker.cpu_rate,options.worker.timeout_ms);
    const auto required=required_sized_worker_headroom(capacity,budget);
    if(!required || required>(std::min)(observed.snapshot.physical_available,observed.snapshot.commit_available)) {
      ++self.stats.declined;return;
    }
    const GpuHealthEvidence health{route.identity,++self.sequence,0,true,true,observed.snapshot};
    const auto route_decision=self.recovery.consider_anchor(true,health,transfer.device_bytes,now);
    if(!route_decision.try_gpu()){++self.stats.declined;return;}
    ticket=route_decision.ticket;
    detail::AnchorMatchBatchBuilder builder(ticket,plan);
    builder.append(ticket,source);
    auto request=builder.finish();
    const auto begun=GetTickCount64();
    auto session=WorkerExchangeSession::start_sized(route.helper,
        {L"--driver-anchor-transfer-batch",std::to_wstring(route.ordinal),hex(route.memory.luid),
            std::to_wstring(route.memory.node_mask),hex(route.runtime_sha256),std::to_wstring(plan.request_bytes),
            std::to_wstring(plan.reply_bytes),std::to_wstring(quantum),std::to_wstring(plan.device_bytes),
            std::to_wstring(plan.launches),std::to_wstring(transfer.device_bytes)},
        request.snapshot(),capacity,budget,route.pins);
    self.flight.emplace(Impl::Flight{std::move(session),std::move(request),ticket,begun,begun+budget.timeout_ms});
    self.activity=activity;
    self.window=std::move(source);
    ++self.stats.submitted;
  }catch(...) {
    ++self.stats.declined;
    if(ticket) {
      const auto idle=WorkerExchangeSession::available();
      self.fail(ticket,GpuRecoveryOutcome::temporary_pressure,idle);
      if(!idle)self.unobserved_ticket=ticket;
    }
  }
}
void NativeAnchorSearchController::observe(std::uint64_t activity,std::uint64_t now,bool collect) noexcept {
  auto& self=*impl_;
  if(self.unobserved_ticket && WorkerExchangeSession::available()) {
    (void)self.recovery.confirm_quiescent(self.unobserved_ticket);
    self.unobserved_ticket=0;
  }
  if(activity!=self.activity)self.yield(Impl::Disposition::yielded);
  if(!self.flight)return;
  auto& flight=*self.flight;
  if(!flight.supervisor_exited) {
    const auto state=WaitForSingleObject(flight.session.supervisor_wait_handle(),0);
    if(state==WAIT_TIMEOUT) {
      if(now>=flight.deadline)self.yield(Impl::Disposition::expired);
      return;
    }
    if(state!=WAIT_OBJECT_0 || !flight.session.retirement_confirmed()) {
      self.yield(Impl::Disposition::expired);
      self.fail(flight.ticket,GpuRecoveryOutcome::device_fault,false);
      self.stats.retirement_confirmed=false;
      self.flight.reset();
      return;
    }
    self.pace(now,flight.started);
    flight.supervisor_exited=true;
  }
  if(flight.disposition==Impl::Disposition::yielded) {
    (void)self.recovery.abandon(flight.ticket,true);
  } else if(flight.disposition==Impl::Disposition::expired) {
    self.fail(flight.ticket,GpuRecoveryOutcome::device_fault,true);
  } else {
    const auto* result=flight.session.try_result();
    if(!result || result->code!=WorkerExchangeCode::completed) {
      self.fail(flight.ticket,result?worker_transport_failure(flight.session,*result,WorkerExecution::hip):
          GpuRecoveryOutcome::device_fault,true);
      self.discard();
      ++self.stats.declined;
    } else {
      if(!collect)return;
      try {
        auto verified=detail::accept_anchor_match_batch_reply(result->response,flight.request);
        self.advice=std::move(verified.front());
        (void)self.recovery.finish(flight.ticket,GpuRecoveryOutcome::accepted,true);
        ++self.stats.verified;
      }catch(const std::bad_alloc&) {
        self.fail(flight.ticket,GpuRecoveryOutcome::temporary_pressure,true);
        self.discard();++self.stats.declined;
      }catch(...) {
        self.fail(flight.ticket,GpuRecoveryOutcome::integrity_failure,true);
        self.discard();++self.stats.declined;
      }
    }
  }
  self.flight.reset();
}
bool NativeAnchorSearchController::waiting() const noexcept {
  return impl_->flight && !impl_->flight->supervisor_exited && impl_->flight->disposition==Impl::Disposition::pending;
}
std::uint64_t NativeAnchorSearchController::deadline() const noexcept {
  return waiting()?impl_->flight->deadline:0;
}
void* NativeAnchorSearchController::wake_handle() const noexcept {
  return impl_->flight && !impl_->flight->supervisor_exited?impl_->flight->session.supervisor_wait_handle():nullptr;
}
NativeSharedBaseSearchReply NativeAnchorSearchController::reply() const noexcept {
  return impl_->advice.empty()?NativeSharedBaseSearchReply{}:
      NativeSharedBaseSearchReply{impl_->window,impl_->advice};
}
void NativeAnchorSearchController::discard_reply() noexcept {impl_->discard();}
void NativeAnchorSearchController::cancel() noexcept {impl_->yield(Impl::Disposition::yielded);}
bool NativeAnchorSearchController::stop() noexcept {
  auto& self=*impl_;
  self.yield(Impl::Disposition::yielded);
  if(self.flight) {
    const auto wait=WaitForSingleObject(self.flight->session.supervisor_wait_handle(),retirement_grace_ms);
    if(wait==WAIT_OBJECT_0)observe(self.activity,GetTickCount64());
    else {
      self.stats.retirement_confirmed=false;
      self.flight.reset();
    }
  }
  return self.stats.retirement_confirmed && !self.flight && !self.unobserved_ticket;
}
NativeAnchorSearchStats NativeAnchorSearchController::stats() const noexcept {
  auto state=impl_->stats;
  state.retirement_confirmed=state.retirement_confirmed && !impl_->flight && !impl_->unobserved_ticket;
  return state;
}
}
