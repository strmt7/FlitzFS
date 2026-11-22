#include "native_lz4_read_worker.hpp"
#include "gpu_budget.hpp"
#include "hip_module_verification.hpp"
#include "winfsp_native.hpp"
#include "worker_exchange.hpp"
#include "../gpu_request_id.hpp"
#include "../native_lz4_decode_wire.hpp"
#include "../native_maintenance_source.hpp"
#include "../record_digest.hpp"
#include "flitzfs/gpu_recovery.hpp"
#include "flitzfs/windows_path.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace flitzfs {
namespace {
constexpr auto mib=1024ULL*1024;
constexpr DWORD retirement_grace_ms=2000;
constexpr wchar_t module_name[]=L"flitzfs_rocm_native_lz4_decode.dll";

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
      }) && p.minimum_frames && p.minimum_frames<=p.maximum_frames &&
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
  struct Flight {
    detail::NativeLz4ReadExchange request;
    WorkerExchangeSession child;
    WinFspReadAheadWork work;
    std::uint64_t ticket,submitted_at;
    std::uint32_t frames;
    bool superseded=false;
  };
  const NativeLz4ReadWorkerProfile profile;
  const std::shared_ptr<const PinnedReadPackage> package;
  const GpuConfigurationId configuration;
  GpuRecoveryPolicy policy;
  NativeLz4ReadWorkerStats status;
  std::optional<Flight> flight;
  std::uint64_t sequence=0;

  explicit Impl(NativeLz4ReadWorkerProfile config)
      :profile(std::move(config)),package(std::make_shared<PinnedReadPackage>(profile)),
       configuration(configuration_id(profile)) {}

  void cancel() noexcept {
    if(flight && !flight->superseded) {
      flight->superseded=true;
      (void)flight->child.request_cancel();
    }
  }
  void retire(bool exact) noexcept {
    const auto* result=flight->child.try_result();
    if(result) {
      status.last_worker_exit_code=result->process.exit_code;
      status.last_worker_elapsed_ms=result->process.elapsed_ms;
    }
    if(flight->superseded) {
      ++status.superseded;
      (void)policy.abandon(flight->ticket,exact);
    }else {
      ++status.failures;
      const auto outcome=refusal(result);
      status.configuration_refused=outcome!=GpuRecoveryOutcome::temporary_pressure;
      (void)policy.finish(flight->ticket,outcome,exact);
    }
    status.retirement_confirmed=exact;
    flight.reset();
  }
};

NativeLz4ReadWorker::NativeLz4ReadWorker(NativeLz4ReadWorkerProfile p) {
  p.helper.make_preferred();
  if(!valid_profile(p))throw std::invalid_argument("Invalid qualified native LZ4 read worker profile");
  impl_=std::make_unique<Impl>(std::move(p));
}
NativeLz4ReadWorker::~NativeLz4ReadWorker(){(void)stop();}

bool NativeLz4ReadWorker::start(WinFspReadAheadWork& work,const NativeMaintenanceObservation& observed,
    const std::atomic<bool>& stop_requested) noexcept {
  auto& s=*impl_;
  if(s.flight || !s.status.retirement_confirmed || s.status.configuration_refused ||
      work.frames.size()<s.profile.minimum_frames ||
      !work.owner || stop_requested.load(std::memory_order_acquire) || !WorkerExchangeSession::available())
    return false;
  const auto& profile=s.profile;
  const auto enough_lead=[&](std::uint64_t now) {
    if(!profile.minimum_lead_ms)return true;
    const auto required=(std::max)(std::uint64_t{profile.minimum_lead_ms},s.status.last_turnaround_ms);
    if(work.has_read_lead(required,now))return true;
    ++s.status.lead_refusals;
    ++s.status.declined;
    return false;
  };
  if(!enough_lead(GetTickCount64()))return false;
  try {
    std::uint32_t frames=0,encoded=0,decoded=0;
    std::uint64_t slab=0;
    for(const auto& frame:work.frames) {
      if(frames==profile.maximum_frames)break;
      const auto e=(std::max)(encoded,static_cast<std::uint32_t>(frame.encoded().size()));
      const auto d=(std::max)(decoded,frame.key().decoded_bytes);
      const auto layout=detail::native_lz4_layout(frames+1,e,d);
      const auto next=native_lz4_decode::output_at(layout,layout.chunks);
      if(next>profile.maximum_slab_bytes)break;
      ++frames;encoded=e;decoded=d;slab=next;
    }
    if(frames<profile.minimum_frames) {++s.status.declined;return false;}
    auto exchange=detail::NativeLz4ReadExchange::prepare_captured(std::span(work.frames).first(frames),
        detail::next_gpu_request_id(),{frames,slab});
    const WorkerChannelCapacity capacity{exchange.request()->bytes().size(),exchange.reply_capacity()};
    ++s.status.budget_queries;
    const auto sample=query_windows_gpu_budget(profile.luid,profile.node_mask);
    const auto& memory=sample.snapshot;
    const auto physical=(std::min)(observed.physical_available,memory.physical_available);
    const auto commit=(std::min)(observed.commit_available,memory.commit_available);
    const auto budget=plan_resource_process_budget(profile.worker.memory_mib*mib,physical,commit,
        profile.worker.memory_share_divisor,profile.worker.cpu_rate,profile.worker.timeout_ms);
    const auto required=required_sized_worker_headroom(capacity,budget);
    if(!required || physical<required || commit<required ||
        stop_requested.load(std::memory_order_acquire) || work.owner->foreground_pending() ||
        work.owner->foreground_activity()!=work.foreground_epoch) {++s.status.declined;return false;}
    const std::vector<std::wstring> arguments{L"--driver-native-lz4-read-adapter",std::to_wstring(profile.ordinal),
      hex(profile.luid),std::to_wstring(profile.node_mask),wide(profile.runtime_sha256),wide(profile.module_sha256),
      profile.architecture,std::to_wstring(profile.wave_mask),std::to_wstring(capacity.request_bytes),
      std::to_wstring(capacity.reply_bytes),std::to_wstring(frames),std::to_wstring(slab),std::to_wstring(frames)};
    const auto submitted_at=GetTickCount64();
    if(!enough_lead(submitted_at))return false;
    const auto decision=s.policy.consider_native_lz4_read(true,
        {s.configuration,++s.sequence,0,sample.status==WindowsGpuBudgetStatus::success,true,memory},
        slab,submitted_at);
    if(!decision.try_gpu()) {++s.status.declined;return false;}
    try {
      auto child=WorkerExchangeSession::start_sized(profile.helper,arguments,exchange.request(),capacity,
          budget,s.package);
      work.frames.clear();
      s.flight.emplace(Impl::Flight{std::move(exchange),std::move(child),std::move(work),decision.ticket,submitted_at,frames});
      ++s.status.submitted;
      s.status.submitted_frames+=frames;
      return true;
    }catch(...) {
      const bool quiescent=WorkerExchangeSession::available();
      s.status.retirement_confirmed=quiescent;
      (void)s.policy.finish(decision.ticket,GpuRecoveryOutcome::temporary_pressure,quiescent);
      ++s.status.failures;
    }
  }catch(...) {++s.status.declined;}
  return false;
}

void NativeLz4ReadWorker::observe(const WinFspNativeWritable& adapter,bool cancel) noexcept {
  auto& s=*impl_;
  if(!s.flight)return;
  auto& f=*s.flight;
  if(cancel || adapter.foreground_pending() || adapter.foreground_activity()!=f.work.foreground_epoch)s.cancel();
  const auto handle=static_cast<HANDLE>(f.child.supervisor_wait_handle());
  if(!handle || WaitForSingleObject(handle,0)!=WAIT_OBJECT_0)return;
  const auto exact=f.child.retirement_confirmed();
  const auto* result=f.child.try_result();
  if(!exact || f.superseded || !result || result->code!=WorkerExchangeCode::completed)s.retire(exact);
}

bool NativeLz4ReadWorker::ready_to_verify() const noexcept {
  const auto& f=impl_->flight;
  if(!f || f->superseded || !f->child.retirement_confirmed())return false;
  const auto* result=f->child.try_result();
  return result && result->code==WorkerExchangeCode::completed;
}

bool NativeLz4ReadWorker::verify_and_publish(WinFspNativeWritable& adapter,
    const NativeMaintenanceObservation& observed,std::uint64_t now,const std::atomic<bool>& stopped) noexcept {
  if(!ready_to_verify())return false;
  auto& s=*impl_;
  auto& f=*s.flight;
  const auto* result=f.child.try_result();
  const auto resources=detail::maintenance_resources(observed,now,adapter.group_maintenance_policy());
  if(!resources || resources->cpu_busy || resources->memory_allowance<result->response.size() ||
      stopped.load(std::memory_order_acquire) || adapter.foreground_pending())return false;
  bool published=false;
  try {
    auto ready=f.request.accept(result->response,WorkerExecution::hip);
    s.status.verified_frames+=f.frames;
    published=adapter.publish_native_lz4_read_ahead(f.work,std::move(ready),stopped);
    if(published) {
      ++s.status.published;
      s.status.last_turnaround_ms=GetTickCount64()-f.submitted_at;
      (void)s.policy.finish(f.ticket,GpuRecoveryOutcome::accepted,true);
    }else {
      ++s.status.superseded;
      (void)s.policy.abandon(f.ticket,true);
    }
  }catch(...) {
    ++s.status.failures;
    s.status.configuration_refused=true;
    (void)s.policy.finish(f.ticket,GpuRecoveryOutcome::integrity_failure,true);
  }
  s.status.last_worker_exit_code=result->process.exit_code;
  s.status.last_worker_elapsed_ms=result->process.elapsed_ms;
  s.flight.reset();
  return published;
}

void* NativeLz4ReadWorker::wake_handle() const noexcept {
  const auto& f=impl_->flight;
  return f && !ready_to_verify()?f->child.supervisor_wait_handle():nullptr;
}

bool NativeLz4ReadWorker::stop() noexcept {
  auto& s=*impl_;
  if(s.flight) {
    s.cancel();
    const auto handle=static_cast<HANDLE>(s.flight->child.supervisor_wait_handle());
    const bool exact=handle && WaitForSingleObject(handle,retirement_grace_ms)==WAIT_OBJECT_0 &&
        s.flight->child.retirement_confirmed();
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
