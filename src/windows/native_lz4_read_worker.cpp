#include "native_lz4_read_worker.hpp"
#include "hip_route_binding.hpp"
#include "gpu_budget.hpp"
#include "gpu_engine_activity.hpp"
#include "hip_module_verification.hpp"
#include "winfsp_native.hpp"
#include "worker_exchange.hpp"
#include "../gpu_request_id.hpp"
#include "../native_lz4_decode_wire.hpp"
#include "../native_group_read_wire.hpp"
#include "../native_maintenance_source.hpp"
#include "../record_digest.hpp"
#include "flitzfs/gpu_recovery.hpp"
#include "flitzfs/windows_path.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace flitzfs {
namespace {
constexpr auto mib=1024ULL*1024;
constexpr DWORD retirement_grace_ms=2000;
constexpr wchar_t module_name[]=L"flitzfs_rocm_native_lz4_decode.dll";
using ReadExchange=std::variant<detail::NativeLz4ReadExchange,detail::NativeGroupReadExchange>;

std::wstring hex(std::span<const std::byte> data) {
  constexpr wchar_t digit[]=L"0123456789abcdef";
  std::wstring result;
  result.reserve(2*data.size());
  for(const auto value:data) {
    const auto byte=std::to_integer<unsigned>(value);
    result.push_back(digit[byte>>4]);
    result.push_back(digit[byte&15]);
  }
  return result;
}
std::wstring wide(std::string_view value) {return {value.begin(),value.end()};}

struct PinnedReadPackage final {
  std::vector<hip_detail::DirectoryPin> directories;
  std::optional<ReadOnlyFile> helper,module;
  hip_detail::VerifiedSystemModule runtime;
  explicit PinnedReadPackage(const NativeLz4ReadWorkerProfile& profile)
      :runtime(profile.runtime_name,profile.runtime_sha256) {
    const auto parent=profile.helper.parent_path();
    auto directory=parent.root_path();
    directories.emplace_back(directory);
    for(const auto& part:parent.relative_path()) {
      directory/=part;
      directories.emplace_back(directory);
    }
    helper.emplace(ReadOnlyFile::open(profile.helper));
    module.emplace(ReadOnlyFile::open(parent/module_name));
    if(!hip_detail::matches_sha256(*helper,profile.helper_sha256) ||
        !hip_detail::matches_sha256(*module,profile.module_sha256))
      throw std::invalid_argument("Native LZ4 read package identity differs");
  }
};
bool valid_profile(const NativeLz4ReadWorkerProfile& p) {
  const auto& arch=p.architecture;
  return p.helper.is_absolute() && p.helper==p.helper.lexically_normal() &&
      ordinary_windows_file_path(p.helper.native()) &&
      hip_detail::valid_sha256(p.helper_sha256) && hip_detail::valid_sha256(p.module_sha256) &&
      hip_detail::valid_sha256(p.runtime_sha256) && p.luid!=std::array<std::byte,8>{} &&
      std::has_single_bit(p.node_mask) && p.ordinal<64 && p.wave_mask && p.wave_mask<=3 &&
      arch.size()>3 && arch.size()<=32 && arch.starts_with(L"gfx") && std::ranges::all_of(arch,[](wchar_t c) {
        return (c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9');
      }) && (!p.mixed_groups || p.packed_groups) && p.minimum_frames && p.minimum_frames<=p.maximum_frames &&
      (!p.prepared_group_submit_lead_ms ||
          (p.packed_groups && p.prepared_group_submit_lead_ms<=p.minimum_lead_ms)) &&
      p.maximum_slab_bytes && p.worker.memory_share_divisor && valid_process_budget(p.worker);
}

GpuConfigurationId configuration_id(const NativeLz4ReadWorkerProfile& p) {
  const auto identity=wide(p.helper_sha256+p.module_sha256+p.runtime_sha256)+hex(p.luid)+L":"+
      std::to_wstring(p.node_mask)+L":"+std::to_wstring(p.ordinal)+L":"+p.architecture+L":"+std::to_wstring(p.wave_mask);
  return detail::record_digest(std::as_bytes(std::span(identity)));
}

GpuRecoveryOutcome refusal(const WorkerExchangeResult* result) noexcept {
  if(!result || result->code==WorkerExchangeCode::supervisor_failed)
    return GpuRecoveryOutcome::runtime_unavailable;
  if(result->code==WorkerExchangeCode::timed_out)return GpuRecoveryOutcome::temporary_pressure;
  if(result->code==WorkerExchangeCode::invalid_response)return GpuRecoveryOutcome::integrity_failure;
  switch(static_cast<HipWorkerExit>(hip_worker_exit_category(result->process.exit_code))) {
    case HipWorkerExit::unsupported_configuration:return GpuRecoveryOutcome::unsupported_configuration;
    case HipWorkerExit::runtime_unavailable:return GpuRecoveryOutcome::runtime_unavailable;
    case HipWorkerExit::temporary_pressure:return GpuRecoveryOutcome::temporary_pressure;
    case HipWorkerExit::device_fault:return GpuRecoveryOutcome::device_fault;
    default:return GpuRecoveryOutcome::integrity_failure;
  }
}
}

struct NativeLz4ReadWorker::Impl {
  struct LaunchPlan {
    const wchar_t* command;
    WorkerChannelCapacity capacity;
    std::uint64_t slab;
    std::uint32_t frames,argument;
    SharedWorkerRequest request;
  };
  struct StartedChild {
    WorkerExchangeSession child;
    std::uint64_t ticket,started_at;
  };
  struct Flight {
    std::optional<ReadExchange> request;
    StartedChild worker;
    WinFspReadAheadWork work;
    std::uint64_t submitted_at=0;
    std::uint32_t frames=0;
    detail::NativeGroupPreparedGrant preparation;
    bool superseded=false,ready_at_submit=false;
    std::optional<detail::NativeGroupReplyVerifier> group_verification;
    std::unique_ptr<detail::NativeGroupReadyReads> verified_groups;
  };
  const NativeLz4ReadWorkerProfile profile;
  const WindowsGpuEngineSampler* const engines;
  const std::shared_ptr<const PinnedReadPackage> package;
  const GpuConfigurationId configuration;
  detail::NativeLz4ReadSession& session;
  NativeLz4ReadWorkerStats& status;
  std::optional<Flight> flight;
  detail::NativeGroupReadTiming group_timing;

  Impl(NativeLz4ReadWorkerProfile config,detail::NativeLz4ReadSession& owner,const WindowsGpuEngineSampler* sampler)
      :profile(std::move(config)),engines(sampler),package(std::make_shared<PinnedReadPackage>(profile)),
       configuration(configuration_id(profile)),session(owner),status(owner.status) {}

  std::optional<bool> engine_available(std::uint64_t now) const noexcept {
    if(!engines)return true;
    return engines->available(profile.luid,now,minimum_background_gpu_idle_ppm);
  }

  bool has_lead(const WinFspReadAheadWork& work,std::uint64_t required,std::uint64_t now) noexcept {
    if(work.has_read_lead(required,now))return true;
    ++status.lead_refusals;
    ++status.declined;
    return false;
  }
  static bool current(const WinFspReadAheadWork& work,const std::atomic<bool>& stopped) noexcept {
    return work.owner && !stopped.load(std::memory_order_acquire) && !work.owner->foreground_pending() &&
        work.owner->foreground_activity()==work.foreground_epoch;
  }
  bool device_ready() noexcept {
    if(!profile.installed_route || profile.installed_route->device_ready())return true;
    status.device_unavailable=true;
    status.configuration_refused=true;
    cancel();
    return false;
  }
  struct BudgetObservation {
    WindowsGpuBudgetResult gpu;
    ProcessBudget process;
  };
  std::optional<BudgetObservation> observe_budget(const NativeMaintenanceObservation& observed,
      WorkerChannelCapacity capacity) noexcept {
    ++status.budget_queries;
    auto sample=query_windows_gpu_budget(profile.luid,profile.node_mask);
    const auto physical=(std::min)(observed.physical_available,sample.snapshot.physical_available);
    const auto commit=(std::min)(observed.commit_available,sample.snapshot.commit_available);
    const auto process=plan_resource_process_budget(profile.worker.memory_mib*mib,physical,commit,
        profile.worker.memory_share_divisor,profile.worker.cpu_rate,profile.worker.timeout_ms);
    const auto required=required_sized_worker_headroom(capacity,process);
    if(!required || physical<required || commit<required)return {};
    return BudgetObservation{std::move(sample),process};
  }
  std::uint64_t preparation_lead(const Flight& f,std::uint64_t now,bool ready) const noexcept {
    if(now<f.worker.started_at)return 0;
    const auto elapsed=now-f.worker.started_at;
    if(elapsed>=profile.worker.timeout_ms)return 0;
    const auto lead=group_timing.submit_lead(profile.minimum_lead_ms,profile.prepared_group_submit_lead_ms,elapsed,ready);
    return lead<=profile.worker.timeout_ms-elapsed?lead:0;
  }
  bool keep_preparation(const WinFspNativeWritable& adapter,const Flight& f) const noexcept {
    const auto now=GetTickCount64();
    return !f.superseded && now>=f.worker.started_at && now-f.worker.started_at<profile.worker.timeout_ms &&
        (adapter.foreground_pending() || adapter.foreground_activity()!=f.work.foreground_epoch) &&
        adapter.group_preparation_state(f.work,now)!=WinFspReadAheadState::superseded;
  }

  std::optional<StartedChild> launch(const WinFspReadAheadWork& work,const NativeMaintenanceObservation& observed,
      const std::atomic<bool>& stopped,const LaunchPlan& plan,std::uint64_t required_lead) {
    if(!engine_available(GetTickCount64()).value_or(false) || !device_ready())return {};
    const auto budget=observe_budget(observed,plan.capacity);
    if(!budget || !current(work,stopped)) {
      ++status.declined;
      return {};
    }
    const auto& sample=budget->gpu;
    const auto& memory=sample.snapshot;
    const std::vector<std::wstring> args{plan.command,std::to_wstring(profile.ordinal),
      hex(profile.luid),std::to_wstring(profile.node_mask),wide(profile.runtime_sha256),wide(profile.module_sha256),
      profile.architecture,std::to_wstring(profile.wave_mask),std::to_wstring(plan.capacity.request_bytes),
      std::to_wstring(plan.capacity.reply_bytes),std::to_wstring(plan.frames),std::to_wstring(plan.slab),
      std::to_wstring(plan.argument)};
    const auto now=GetTickCount64();
    if(!engine_available(now).value_or(false) || !has_lead(work,required_lead,now))return {};
    const auto decision=session.recovery.consider_native_lz4_read(true,
        {configuration,++session.sequence,0,sample.status==WindowsGpuBudgetStatus::success,true,memory},plan.slab,now);
    if(!decision.try_gpu()) {++status.declined;return {};}
    try {
      auto child=plan.request?WorkerExchangeSession::start_sized(profile.helper,args,plan.request,plan.capacity,budget->process,package):
          WorkerExchangeSession::start_prepared_bounded(profile.helper,args,plan.capacity,budget->process,package);
      return StartedChild{std::move(child),decision.ticket,now};
    }catch(...) {
      status.retirement_confirmed=WorkerExchangeSession::available();
      (void)session.recovery.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,status.retirement_confirmed);
      ++status.failures;
      return {};
    }
  }
  void cancel() noexcept {
    if(flight && !flight->superseded) {
      flight->superseded=true;
      (void)flight->worker.child.request_cancel();
    }
  }
  void retire(bool exact) noexcept {
    const auto* result=flight->worker.child.try_result();
    if(result) {
      status.last_worker_exit_code=result->process.exit_code;
      status.last_worker_elapsed_ms=result->process.elapsed_ms;
    }
    if(flight->superseded) {
      ++status.superseded;
      (void)session.recovery.abandon(flight->worker.ticket,exact);
    }else {
      ++status.failures;
      const auto outcome=refusal(result);
      status.configuration_refused=outcome!=GpuRecoveryOutcome::temporary_pressure;
      (void)session.recovery.finish(flight->worker.ticket,outcome,exact);
    }
    status.retirement_confirmed=exact;
    flight.reset();
  }
};

NativeLz4ReadWorker::NativeLz4ReadWorker(NativeLz4ReadWorkerProfile p,detail::NativeLz4ReadSession& session,
    const WindowsGpuEngineSampler* engines) {
  p.helper.make_preferred();
  if(!valid_profile(p))throw std::invalid_argument("Invalid qualified native LZ4 read worker profile");
  impl_=std::make_unique<Impl>(std::move(p),session,engines);
}
NativeLz4ReadWorker::~NativeLz4ReadWorker(){(void)stop();}

bool NativeLz4ReadWorker::bind_installed_route() noexcept {
  return !impl_->flight && impl_->session.bind(impl_->configuration);
}

WinFspGroupReadWindow NativeLz4ReadWorker::group_read_window() const noexcept {
  const auto& s=*impl_;
  const auto& profile=s.profile;
  const auto lead=s.group_timing.start_lead(profile.minimum_lead_ms);
  if(!profile.packed_groups || s.status.configuration_refused ||
      !s.engine_available(GetTickCount64()).value_or(false))return {};
  if(s.flight) {
    const auto& f=*s.flight;
    if(f.request || f.superseded)return {};
    const auto pending_lead=s.preparation_lead(f,GetTickCount64(),f.worker.child.wait_prepared());
    if(!pending_lead)return {};
    return {f.preparation.maximum_members,pending_lead,profile.minimum_frames};
  }
  if(!lead || !s.status.retirement_confirmed || !WorkerExchangeSession::available())return {};
  const auto grant=detail::plan_native_group_preparation(profile.maximum_frames,profile.maximum_slab_bytes,profile.mixed_groups);
  if(grant.maximum_members<profile.minimum_frames)return {};
  return {grant.maximum_members,lead,profile.minimum_frames};
}

bool NativeLz4ReadWorker::prepare_group(const WinFspReadAheadWork& work,const NativeMaintenanceObservation& observed,
    const std::atomic<bool>& stopped) noexcept {
  auto& s=*impl_;
  const auto& profile=s.profile;
  if(!profile.prepared_group_submit_lead_ms || s.flight || !s.status.retirement_confirmed ||
      !s.engine_available(GetTickCount64()).value_or(false) ||
      s.status.configuration_refused || !Impl::current(work,stopped) || !WorkerExchangeSession::available())return false;
  const auto need=s.group_timing.start_lead(profile.minimum_lead_ms);
  if(!s.has_lead(work,need,GetTickCount64()))return false;
  const auto frames=(std::min)(work.bytes/kNativeExtentBytes,profile.maximum_frames);
  const auto grant=detail::plan_native_group_preparation(frames,profile.maximum_slab_bytes,profile.mixed_groups);
  if(grant.maximum_members<profile.minimum_frames)return false;
  try {
    const Impl::LaunchPlan plan{L"--driver-native-group-prepared-read-adapter",{grant.request_bytes,grant.reply_bytes},
        grant.device_bytes,grant.maximum_members,profile.mixed_groups?group_decode::mixed_version:group_decode::version,{}};
    auto worker=s.launch(work,observed,stopped,plan,need);
    if(!worker)return false;
    WinFspReadAheadWork target{work.owner,work.token,work.offset,work.foreground_epoch,work.bytes};
    target.hint_created_ms=work.hint_created_ms;
    target.observed_lead_ms=work.observed_lead_ms;
    target.stream_revision=work.stream_revision;
    target.sequential_since=work.sequential_since;
    target.checkpoint=work.checkpoint;
    s.flight.emplace(Impl::Flight{{},std::move(*worker),std::move(target),0,0,grant});
    ++s.status.prepared_groups_started;
    return true;
  }catch(...) {++s.status.declined;return false;}
}

bool NativeLz4ReadWorker::start(WinFspReadAheadWork& work,const NativeMaintenanceObservation& observed,
    const std::atomic<bool>& stop_requested) noexcept {
  auto& s=*impl_;
  const bool grouped=work.groups!=nullptr;
  const auto available=grouped?work.groups->members().size():work.frames.size();
  if(!s.status.retirement_confirmed || s.status.configuration_refused ||
      !s.engine_available(GetTickCount64()).value_or(false) || available<s.profile.minimum_frames ||
      (grouped && (!s.profile.packed_groups || (work.groups->mixed_methods() && !s.profile.mixed_groups))) ||
      !Impl::current(work,stop_requested))
    return false;
  const auto& profile=s.profile;
  if(s.flight) {
    auto& f=*s.flight;
    const auto& target=f.work;
    if(!grouped || f.request || f.superseded || available>f.preparation.maximum_members ||
        target.owner!=work.owner || target.token!=work.token || target.checkpoint!=work.checkpoint)return false;
    const bool retarget=target.sequential_since && work.sequential_since==target.sequential_since &&
        work.stream_revision>target.stream_revision && work.foreground_epoch>target.foreground_epoch;
    const bool original=target.offset==work.offset && target.bytes==work.bytes &&
        target.foreground_epoch==work.foreground_epoch && target.stream_revision==work.stream_revision &&
        target.hint_created_ms==work.hint_created_ms && target.observed_lead_ms==work.observed_lead_ms;
    if(!original && !retarget)return false;
    const auto now=GetTickCount64();
    const auto lead=s.preparation_lead(f,now,f.worker.child.wait_prepared());
    if(!lead || !s.has_lead(work,lead,now) || !s.device_ready())return false;
    try {
      if(retarget) {
        const auto resources=detail::maintenance_resources(observed,now,work.owner->group_maintenance_policy());
        if(!resources || resources->cpu_busy) {
          ++s.status.declined;
          s.cancel();
          return false;
        }
        const auto budget=s.observe_budget(observed,{f.preparation.request_bytes,f.preparation.reply_bytes});
        if(!budget || budget->gpu.status!=WindowsGpuBudgetStatus::success ||
            check_gpu_memory_budget(budget->gpu.snapshot,profile.luid,profile.node_mask,GetTickCount64(),
                f.preparation.device_bytes)!=GpuGateCode::admitted) {
          ++s.status.declined;
          s.cancel();
          return false;
        }
        const auto checked=GetTickCount64();
        const auto required=s.preparation_lead(f,checked,f.worker.child.wait_prepared());
        if(!required || !s.has_lead(work,required,checked) || !Impl::current(work,stop_requested))return false;
      }
      auto exchange=detail::NativeGroupReadExchange::prepare(*work.groups,detail::next_gpu_request_id(),
          f.preparation.maximum_members,f.preparation.device_bytes);
      const auto ready_at_submit=f.worker.child.wait_prepared();
      const auto submitted_at=GetTickCount64();
      const auto required=s.preparation_lead(f,submitted_at,ready_at_submit);
      if(!required || exchange.reply_capacity()>f.preparation.reply_bytes || !Impl::current(work,stop_requested) ||
          !s.engine_available(submitted_at).value_or(false) || !s.has_lead(work,required,submitted_at) ||
          !f.worker.child.submit_prepared(exchange.request()))return false;
      f.request.emplace(std::move(exchange));
      f.work=std::move(work);
      f.submitted_at=submitted_at;
      f.ready_at_submit=ready_at_submit;
      f.frames=static_cast<std::uint32_t>(available);
      ++s.status.prepared_groups_submitted;
      if(retarget)++s.status.prepared_groups_retargeted;
      ++s.status.submitted;
      s.status.submitted_frames+=f.frames;
      return true;
    }catch(...) {++s.status.declined;return false;}
  }
  if(!WorkerExchangeSession::available())return false;
  const auto lead=grouped?s.group_timing.start_lead(profile.minimum_lead_ms):
      profile.minimum_lead_ms?(std::max)(std::uint64_t{profile.minimum_lead_ms},s.status.last_turnaround_ms):0;
  if(!s.has_lead(work,lead,GetTickCount64()))return false;
  try {
    std::uint32_t frames=0,encoded=0,decoded=0;
    std::uint64_t slab=0;
    const auto prepare=[&]() -> std::optional<ReadExchange> {
      if(grouped) {
        if(available>profile.maximum_frames)return {};
        auto exchange=detail::NativeGroupReadExchange::prepare(*work.groups,detail::next_gpu_request_id(),
            profile.maximum_frames,profile.maximum_slab_bytes);
        frames=static_cast<std::uint32_t>(available);
        slab=exchange.device_bytes();
        return std::move(exchange);
      }
      for(const auto& frame:work.frames) {
        if(frames==profile.maximum_frames)break;
        const auto e=(std::max)(encoded,static_cast<std::uint32_t>(frame.encoded().size()));
        const auto d=(std::max)(decoded,frame.key().decoded_bytes);
        const auto layout=detail::native_lz4_layout(frames+1,e,d);
        const auto next=native_lz4_decode::output_at(layout,layout.chunks);
        if(next>profile.maximum_slab_bytes)break;
        ++frames;encoded=e;decoded=d;slab=next;
      }
      if(frames<profile.minimum_frames)return {};
      return detail::NativeLz4ReadExchange::prepare_captured(std::span(work.frames).first(frames),
          detail::next_gpu_request_id(),{frames,slab});
    };
    auto exchange=prepare();
    if(!exchange) {++s.status.declined;return false;}
    const auto request=std::visit([](const auto& prepared){return prepared.request();},*exchange);
    const auto reply_bytes=std::visit([](const auto& prepared){return prepared.reply_capacity();},*exchange);
    const WorkerChannelCapacity capacity{request->bytes().size(),reply_bytes};
    const Impl::LaunchPlan plan{grouped?L"--driver-native-group-read-adapter":L"--driver-native-lz4-read-adapter",
        capacity,slab,frames,frames,request};
    auto worker=s.launch(work,observed,stop_requested,plan,lead);
    if(!worker)return false;
    const auto submitted_at=worker->started_at;
    work.frames.clear();
    s.flight.emplace(Impl::Flight{std::move(exchange),std::move(*worker),std::move(work),submitted_at,frames});
    ++s.status.submitted;
    s.status.submitted_frames+=frames;
    return true;
  }catch(...) {++s.status.declined;}
  return false;
}
void NativeLz4ReadWorker::abandon_unused_preparation(const WinFspNativeWritable& adapter) noexcept {
  auto& s=*impl_;
  if(s.flight && !s.flight->request && !s.keep_preparation(adapter,*s.flight))s.cancel();
}

void NativeLz4ReadWorker::observe(const WinFspNativeWritable& adapter,bool cancel) noexcept {
  auto& s=*impl_;
  if(!s.flight)return;
  auto& f=*s.flight;
  using State=WinFspReadAheadState;
  const auto state=!cancel && f.request?adapter.read_ahead_state(f.work):State::busy;
  const auto handle=static_cast<HANDLE>(f.worker.child.supervisor_wait_handle());
  const auto now=GetTickCount64();
  const auto waited=handle?WaitForSingleObject(handle,0):WAIT_FAILED;
  if(state==State::demand_served && !f.superseded) {
    ++s.status.group_demand_misses;
    const auto started=f.ready_at_submit?f.submitted_at:f.worker.started_at;
    // Only an unfinished exchange establishes a lower bound on delivery.
    if(waited==WAIT_TIMEOUT && now>started && now-started<=s.profile.worker.timeout_ms)
      s.group_timing.observe_unfinished(now-started,f.ready_at_submit);
  }
  // A missing observation pauses submission without discarding preparation
  // between samples. A fresh refusal can retire an unused child.
  const bool engine_refused=!f.request && !s.engine_available(now).value_or(true);
  const bool verification_expired=f.work.groups && f.request && waited==WAIT_OBJECT_0 &&
      now>=f.worker.started_at && now-f.worker.started_at>=s.profile.worker.timeout_ms;
  const bool obsolete=cancel || engine_refused || verification_expired ||
      state==State::superseded || state==State::demand_served ||
      (!f.request && (adapter.foreground_pending() || adapter.foreground_activity()!=f.work.foreground_epoch) &&
          !s.keep_preparation(adapter,f));
  if(waited!=WAIT_OBJECT_0) {
    if(obsolete)s.cancel();
    return;
  }
  const auto exact=f.worker.child.retirement_confirmed();
  const auto* result=f.worker.child.try_result();
  // Already observed failures inform recovery even when demand has moved on.
  // A completed result for obsolete work is discarded without publication.
  if(obsolete && exact && result && result->code==WorkerExchangeCode::completed)f.superseded=true;
  if(!exact || f.superseded || !f.request || !result || result->code!=WorkerExchangeCode::completed)s.retire(exact);
}

bool NativeLz4ReadWorker::ready_to_verify() const noexcept {
  const auto& f=impl_->flight;
  if(!f || f->superseded || !f->request || !f->worker.child.retirement_confirmed())return false;
  const auto* result=f->worker.child.try_result();
  return result && result->code==WorkerExchangeCode::completed;
}

bool NativeLz4ReadWorker::verify_and_publish(WinFspNativeWritable& adapter,
    const NativeMaintenanceObservation& observed,std::uint64_t now,const std::atomic<bool>& stopped) noexcept {
  if(!ready_to_verify())return false;
  auto& s=*impl_;
  auto& f=*s.flight;
  const auto* result=f.worker.child.try_result();
  const auto resources=detail::maintenance_resources(observed,now,adapter.group_maintenance_policy());
  const auto verification_epoch=adapter.foreground_activity();
  if(!resources || resources->cpu_busy || (!f.verified_groups && resources->memory_allowance<result->response.size()) ||
      stopped.load(std::memory_order_acquire) || adapter.read_ahead_state(f.work)!=WinFspReadAheadState::current ||
      adapter.foreground_activity()!=verification_epoch)return false;
  bool published=false;
  try {
    const auto interrupted=[&]{
      return stopped.load(std::memory_order_acquire) || adapter.foreground_pending() ||
          adapter.foreground_activity()!=verification_epoch;
    };
    if(const auto* exchange=std::get_if<detail::NativeGroupReadExchange>(&*f.request)) {
      if(!f.verified_groups) {
        if(!f.group_verification)
          f.group_verification=exchange->begin_verification(*f.work.groups,result->response,WorkerExecution::hip);
        if(!f.group_verification)throw std::invalid_argument("Grouped extent reply does not match its capture");
        using State=detail::NativeGroupReplyVerifier::State;
        const auto verified=f.group_verification->advance(interrupted);
        if(verified==State::paused)return false;
        f.verified_groups=f.group_verification->take_verified();
        f.group_verification.reset();
        if(!f.verified_groups)throw std::invalid_argument("Grouped extent reply verification failed");
        s.status.verified_frames+=f.frames;
      }
      // Publication may contend with a callback after the last digest.
      // Keep the exact result until the owner checks the target again.
      if(interrupted() || !adapter.publish_read_ahead(f.work,{},std::move(f.verified_groups),stopped,true))
        return false;
      published=true;
    }else {
      auto ready=std::get<detail::NativeLz4ReadExchange>(*f.request).accept(result->response,WorkerExecution::hip);
      s.status.verified_frames+=f.frames;
      published=adapter.publish_read_ahead(f.work,std::move(ready),{},stopped);
    }
    if(published) {
      ++s.status.published;
      const auto complete=GetTickCount64();
      const auto turnaround=complete-f.worker.started_at;
      const auto after_submit=complete-f.submitted_at;
      if(f.work.groups)s.group_timing.observe_accepted(turnaround,after_submit,f.ready_at_submit);
      else s.status.last_turnaround_ms=turnaround;
      if(f.preparation.maximum_members)s.status.last_prepared_submit_ms=after_submit;
      (void)s.session.recovery.finish(f.worker.ticket,GpuRecoveryOutcome::accepted,true);
    }else {
      ++s.status.superseded;
      (void)s.session.recovery.abandon(f.worker.ticket,true);
    }
  }catch(...) {
    ++s.status.failures;
    s.status.configuration_refused=true;
    (void)s.session.recovery.finish(f.worker.ticket,GpuRecoveryOutcome::integrity_failure,true);
  }
  s.status.last_worker_exit_code=result->process.exit_code;
  s.status.last_worker_elapsed_ms=result->process.elapsed_ms;
  s.flight.reset();
  return published;
}

void* NativeLz4ReadWorker::wake_handle() const noexcept {
  const auto& f=impl_->flight;
  return f && !ready_to_verify()?f->worker.child.supervisor_wait_handle():nullptr;
}

bool NativeLz4ReadWorker::stop() noexcept {
  auto& s=*impl_;
  if(s.flight) {
    s.cancel();
    const auto handle=static_cast<HANDLE>(s.flight->worker.child.supervisor_wait_handle());
    const bool exact=handle && WaitForSingleObject(handle,retirement_grace_ms)==WAIT_OBJECT_0 &&
        s.flight->worker.child.retirement_confirmed();
    s.retire(exact);
  }
  return s.status.retirement_confirmed;
}

NativeLz4ReadWorkerStats NativeLz4ReadWorker::stats() const noexcept {
  auto result=impl_->status;
  result.pending=impl_->flight.has_value();
  result.retirement_confirmed=result.retirement_confirmed && !result.pending;
  return result;
}

}
