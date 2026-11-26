#include "winfsp_controller.hpp"
#include "../native_capture.hpp"
#include "winfsp_native.hpp"
#include "gpu_budget.hpp"
#include "memory_volume.hpp"
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
#include "worker_exchange.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#endif
#include "../native_maintenance_source.hpp"
#include <bitset>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace flitzfs {
namespace {
struct Event {
  HANDLE value=nullptr;
  explicit Event(bool manual):value(CreateEventW(nullptr,manual,FALSE,nullptr)) {
    if(!value)throw std::runtime_error("Controller event creation failed");
  }
  explicit Event(HANDLE notification) {
    if(notification && !DuplicateHandle(GetCurrentProcess(),notification,GetCurrentProcess(),
        &value,EVENT_MODIFY_STATE,FALSE,0))
      throw std::runtime_error("Lifecycle event duplication failed");
  }
  ~Event(){if(value)CloseHandle(value);}
};
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
std::uint32_t write_arrival_wait_ms(const NativeAmdWriteDemand& demand,const NativeAmdWriteBoundary& boundary,
    std::uint32_t timeout_ms,const NativeMaintenancePolicy& policy) noexcept {
  constexpr std::uint64_t ns_per_ms=1000000;
  std::uint64_t arrival_gap=0;
  for(std::size_t i=1;i<demand.samples.size();++i) {
    const auto earlier=demand.samples[i-1].completed_ms;
    const auto later=demand.samples[i].completed_ms;
    if(!earlier || later<earlier)return 0;
    arrival_gap=(std::max)(arrival_gap,later-earlier);
  }
  const auto budget=timeout_ms*ns_per_ms;
  if(!boundary.previous_service_ns || boundary.elapsed_ns>=budget ||
      boundary.previous_service_ns>=budget-boundary.elapsed_ns)return 0;
  const auto remaining=(budget-boundary.elapsed_ns-boundary.previous_service_ns)/ns_per_ms;
  return static_cast<std::uint32_t>((std::min)({arrival_gap,boundary.first_ready_ns/ns_per_ms,
      remaining,std::uint64_t{policy.maximum_cpu_window_ms}}));
}
#endif
}
struct WinFspVolumeController::Impl {
  // Wait handles outlive the worker, session, callbacks and adapter duplicate.
  Event stop{true},activity{false};
  Event status_changed;
  std::atomic<bool> stop_requested{false};
  mutable std::mutex mutex;
  WinFspControllerView current;
  std::optional<NativeDeferredWriteOptions> foreground_offer;
  std::unique_ptr<WinFspNativeSession> session;
  WinFspNativeWritable* adapter=nullptr; // Valid only until session.stop().
  NativeWriteMountPlan plan;
  MemoryVolumeDomain* residency=nullptr;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  WindowsGpuEngineSampler gpu_engines;
#endif
  std::unique_ptr<NativeAnchorSearchController> anchor;
  std::unique_ptr<NativeLz4ReadWorker> read_worker;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  std::uint64_t last_write_engine_sample=0,last_read_engine_sample=0,last_anchor_engine_sample=0;
  std::optional<std::array<std::byte,8>> write_engine_wait_adapter;
  bool observe_write_reference=false;
  std::optional<bool> write_package_present;
  std::optional<NativeAmdWriteDemand> write_reference_demand;
  std::optional<InstalledUniformWriteTrialProfile> installed_write_trial;
  std::unique_ptr<InstalledHipInputWitness> write_trial_inputs;
  WinFspAmdWriteTrial write_trial;
  NativeAmdWriteWorkerPlan write_trial_plan{};
  bool write_trial_memory_refused=false;
  std::uint64_t next_write_trial_ms=0,next_write_poll_ms=0;
  std::atomic<std::uint64_t> write_trial_idle_ms{0},write_trial_epoch{0};
  std::atomic<std::uint32_t> write_waiting_boundary{0};
  Event write_arrival{false};
  struct WriteArrival {
    detail::NativeCapturedWrite capture;
    std::uint64_t epoch=0;
  };
  std::optional<WriteArrival> queued_write; // Guarded by mutex.
  std::optional<NativeMaintenanceObservation> write_arrival_observation;
  NativeAmdWriteShape write_trial_shape;
  NativeManagedForeground write_calibration_policy;
  std::uint64_t last_captured_write=0;
  bool acquiring_write=false,write_trial_requires_evidence=false;
  std::bitset<kStoreMaxTransactionBytes/kNativeExtentBytes> recorded_write_sizes,recorded_group_sizes,completed_write_sizes;
  std::optional<InstalledAnchorProfile> installed;
  HipRouteDiscoveryResult acquisition;
  std::optional<NativeAnchorRecoveryProof> acquisition_proof;
  std::unique_ptr<InstalledHipInputWitness> acquisition_inputs;
  std::optional<InstalledNativeReadProfile> installed_read;
  HipRouteDiscoveryResult read_acquisition;
  std::unique_ptr<InstalledHipInputWitness> read_inputs;
  std::unique_ptr<NativeLz4ReadWorker> acquired_read_worker;
  std::uint64_t next_read_evidence_ms=0;
  bool read_acquisition_started=false,acquiring_read=false;
  std::uint64_t last_acquisition_candidate=0,last_anchor_candidate=0,qualification_activity=0;
  std::atomic<bool> acquisition_finished{false};
  bool acquisition_started=false,acquisition_retired=true,qualifying_device=false;
  std::jthread acquisition_thread;
#endif
  std::thread worker;
  Impl(std::unique_ptr<WinFspNativeWritable> source,WinFspSessionApi api,NativeWriteMountPlan p,
      MemoryVolumeDomain* memory,std::optional<NativeAnchorSearchOptions> acceleration,HANDLE notification,
      std::optional<NativeLz4ReadWorkerProfile> read_profile):
      status_changed(notification),plan(std::move(p)),residency(memory) {
    if(!source || !valid_native_write_plan(plan))throw std::invalid_argument("Invalid controller source or mount plan");
    current.file_backing=source->attached_file_profile();
    if(!memory)source->enable_paired_seal_memory();
    if(read_profile) {
      try {
        if(source->enable_native_lz4_read_ahead())
          read_worker=std::make_unique<NativeLz4ReadWorker>(std::move(*read_profile));
        else current.amd_read.configuration_refused=true;
      }catch(...) {current.amd_read.configuration_refused=true;}
    }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    else {
      installed_read=installed_native_read_profile();
      if(installed_read && !source->enable_native_lz4_read_ahead())installed_read.reset();
    }
#endif
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    observe_write_reference=has_configured_uniform_write_package() && source->enable_amd_write_demand();
    if(observe_write_reference)installed_write_trial=installed_uniform_write_trial_profile();
#endif
    source->set_maintenance_event(activity.value);adapter=source.get();
    session=std::make_unique<WinFspNativeSession>(std::move(source),api);
    if(acceleration)anchor=std::make_unique<NativeAnchorSearchController>(std::move(*acceleration));
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    else if(adapter->supports_anchor_search())installed=installed_anchor_profile();
#endif
    worker=std::thread([this]{run();});
  }
  ~Impl(){request_stop();if(worker.joinable())worker.join();}
  void request_stop() noexcept {stop_requested.store(true,std::memory_order_release);SetEvent(stop.value);}
  void apply_foreground_offer() {
    std::lock_guard lock(mutex);
    if(!foreground_offer || stop_requested.load(std::memory_order_acquire))return;
    const auto result=adapter->activate_qualified_deferred_writes(*foreground_offer);
    if(result==NativeDeferredActivation::busy)return;
    current.foreground_activation=result;
    foreground_offer.reset();
    if(status_changed.value)SetEvent(status_changed.value);
  }
  bool waiting_for_accelerator() const noexcept {
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    if(qualifying_device)return true;
#endif
    return anchor && anchor->waiting();
  }
  void run() noexcept {
    bool disabled=false,failed=false;NTSTATUS started=STATUS_CANCELLED;
    try {
      if(!stop_requested.load(std::memory_order_acquire))started=session->start(plan);
      {
        std::lock_guard lock(mutex);current.start_status=started;current.state=session->state();
      }
      if(NT_SUCCESS(started) && status_changed.value)SetEvent(status_changed.value);
      if(NT_SUCCESS(started)) {
        // Age can make a scan due. Live resource observations admit each step.
        WindowsMaintenanceHost host;
        using Continuation=WinFspMaintenanceContinuation;
        std::array<detail::MaintenanceBackingTiming,static_cast<std::size_t>(Continuation::payload_scrub)+1> backing_timing{};
        detail::MaintenanceBackingTiming read_ahead_timing;
        constexpr auto maximum_finite_wait=std::uint64_t{INFINITE-1};
        constexpr auto scrub_day_ms=24ULL*60*60*1000;
        std::uint64_t next=0,next_scrub=0;
        bool reference_refreshed=false,scrub_pass_faulted=false,maintenance_quiescent=false;
        while(!stop_requested.load(std::memory_order_acquire)) {
          if(session->requires_offline() || session->dispatcher_stopped())break;
          if(!disabled && adapter->maintenance_event_failed()) {
            disabled=true;session->cancel_background_maintenance();
          }
          const auto now=GetTickCount64();
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
          const bool parked=write_waiting_boundary.load(std::memory_order_acquire)!=0;
          const auto active_epoch=acquiring_write?write_trial_epoch.load(std::memory_order_acquire):qualification_activity;
          if(qualifying_device && acquisition_thread.joinable() &&
              (disabled || (!parked && (adapter->foreground_pending() || adapter->foreground_activity()!=active_epoch))))
            acquisition_thread.request_stop();
          settle_acquisition(true);
          if(acquiring_write && now>=next_write_poll_ms)observe_write_trial(host);
#endif
          if(anchor) {
            if(disabled)anchor->cancel();
            anchor->observe(adapter->foreground_activity(),now,adapter->foreground_pending()?
                NativeAnchorOpportunity::foreground:NativeAnchorOpportunity::poll);
          }
          adapter->signal_foreground_for_anchor(waiting_for_accelerator());
          if(read_worker)read_worker->observe(*adapter,disabled);
          if(const auto delivery=session->group_read_delivery();read_worker || delivery) {
            std::lock_guard lock(mutex);
            if(read_worker)current.amd_read=read_worker->stats();
            if(delivery)current.group_read_delivery=*delivery;
          }
          if(const auto demand=adapter->take_amd_write_demand()) {
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
            if(observe_write_reference) {
              write_reference_demand.reset();
              const auto& shape=demand->shape;
              if(shape.storage!=write_calibration_policy) {
                write_calibration_policy=shape.storage;
                recorded_write_sizes.reset();
                recorded_group_sizes.reset();
                completed_write_sizes.reset();
              }
              const auto size=shape.input_bytes/kNativeExtentBytes-1;
              const bool awaiting=installed_write_trial && installed_write_trial->await_later_writes &&
                  (acquiring_write || write_trial_due(*demand,now));
              if(write_package_present!=false && (awaiting || !recorded_write_sizes[size] ||
                  (demand->grouped_candidate && (!recorded_group_sizes[size] || write_trial_due(*demand,now)))))
                write_reference_demand=demand;
            }
#endif
            std::lock_guard lock(mutex);
            current.amd_write_demand=demand;
          }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
          if(!disabled && acquiring_write)offer_arriving_write();
#endif
          apply_foreground_offer();
          const auto gpu=adapter->gpu_budget_task();
          if(gpu && !gpu->wait_ms(now)) {
            // CPU completion uses a different owner and can proceed while the
            // operating system supplies telemetry. Never query under the lock.
            const bool device_ready=!gpu->pins || gpu->pins->device_ready();
            LARGE_INTEGER capture_started{};
            const bool clock_ready=!gpu->validation_counter ||
                (QueryPerformanceCounter(&capture_started) && capture_started.QuadPart>0);
            const bool collect=device_ready && clock_ready;
            const auto observation=collect?query_windows_gpu_budget(gpu->luid,gpu->node_mask):WindowsGpuBudgetResult{};
            if(stop_requested.load(std::memory_order_acquire))break;
            const auto applied=adapter->complete_gpu_budget_task(*gpu,observation.snapshot,GetTickCount64(),
                capture_started.QuadPart);
            std::lock_guard lock(mutex);
            if(collect)++current.gpu_budget_queries;
            if(!applied)++current.gpu_budget_refusals;
            continue;
          }
          const bool scrub_due=now>=next_scrub;
          bool qualification_pending=false,write_trial_running=false;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
          qualification_pending=write_reference_demand.has_value();
          write_trial_running=acquiring_write;
#endif
          const bool pending=!disabled && !write_trial_running &&
              (qualification_pending || session->background_maintenance_pending(scrub_due) ||
              (read_worker && read_worker->ready_to_verify()));
          if(anchor && !pending)anchor->cancel();
          const bool awaiting_anchor=anchor && anchor->waiting();
          const bool quiescent=!pending && !disabled && !write_trial_running && !awaiting_anchor;
          if(quiescent!=maintenance_quiescent) {
            std::lock_guard lock(mutex);
            current.maintenance_quiescent=maintenance_quiescent=quiescent;
          }
          const auto reference_delay=pending && now<next && !reference_refreshed && !adapter->foreground_pending()
              ?host.reference_refresh_delay_ms(now,next):std::nullopt;
          const bool refresh_reference=reference_delay && !*reference_delay;
          if((!pending || now<next) && !refresh_reference) {
            auto delay=pending?(std::min)(next-now,maximum_finite_wait):std::uint64_t{INFINITE};
            if(reference_delay)delay=(std::min)(delay,*reference_delay);
            if(!disabled && !scrub_due)delay=(std::min)({delay,next_scrub-now,maximum_finite_wait});
            if(gpu)delay=(std::min)({delay,gpu->wait_ms(now),maximum_finite_wait});
            if(awaiting_anchor) {
              const auto due=anchor->deadline();
              delay=(std::min)(delay,due>now?due-now:std::uint64_t{0});
            }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
            if(acquiring_write)
              delay=(std::min)(delay,next_write_poll_ms>now?next_write_poll_ms-now:std::uint64_t{0});
#endif
            HANDLE events[4]={stop.value,activity.value};
            DWORD count=2;
            if(anchor)if(const auto event=anchor->wake_handle())events[count++]=event;
            if(read_worker)if(const auto event=read_worker->wake_handle())events[count++]=event;
            const auto waited=WaitForMultipleObjects(count,events,FALSE,static_cast<DWORD>(delay));
            if(waited==WAIT_OBJECT_0)break;
            if(waited!=WAIT_TIMEOUT && (waited<WAIT_OBJECT_0+1 || waited>=WAIT_OBJECT_0+count))
              throw std::runtime_error("Controller event wait failed");
            continue;
          }
          const auto begin=std::chrono::steady_clock::now();
          WinFspMaintenanceResult result;
          using Scrub=PagedPayloadScrubState;
          std::uint64_t scrub_checks=0,scrub_passes=0,scrub_faults=0;
          PagedPayloadScrubResult last_scrub_fault;
          std::optional<PagedPayloadCheckpointObservation> payload_checkpoint;
          std::uint64_t continued=0,continued_compaction=0,continued_range_retirement=0,continued_packed_tail=0;
          decltype(WinFspControllerView::file_reclamation_steps) file_reclamation_steps{};
          decltype(WinFspControllerView::file_rebuild_phases) file_rebuild_phases{};
          std::optional<PagedRewriteProgress> last_file_rebuild_entry;
          detail::MaintenanceBackingUse backing_use,file_retirement_backing;
          bool backing_limited=false,read_ahead_prepared=false,read_ahead_backing_refused=false;
          bool read_ahead_demand_sampled=false;
          bool amd_cpu_trial_attempted=false;
          std::optional<WinFspAmdCpuTrial> amd_cpu_trial;
          MaintenanceThreadScope priority;
          if(!priority.active())disabled=true;
          else try {
            const auto epoch=adapter->foreground_activity();
            const auto interrupted=[&] {
              return stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
                  adapter->foreground_activity()!=epoch;
            };
            auto observed=host.observe(stop_requested.load(std::memory_order_acquire) ||
                (refresh_reference && interrupted()));
            if(!refresh_reference && !stop_requested.load(std::memory_order_acquire) &&
                host.status()!=MaintenanceHostStatus::waiting) {
              if(residency && observed.valid)residency->verify_working_set();
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
              if((installed || installed_read || installed_write_trial) && !interrupted()) {
                const auto resources=detail::maintenance_resources(observed,GetTickCount64(),
                    adapter->group_maintenance_policy());
                if(resources && !resources->cpu_busy)gpu_engines.observe(resources->memory_allowance);
              }
#endif
              // A background commit can invalidate the completed read's
              // checkpoint. Give the admitted reply this quantum first.
              const bool verify_before_maintenance=read_worker && read_worker->ready_to_verify() && !interrupted();
              if(verify_before_maintenance) {
                const detail::MaintenanceBackingScope measure(backing_use);
                read_ahead_prepared=read_worker->verify_and_publish(
                    *adapter,observed,GetTickCount64(),stop_requested);
              }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
              if(!read_ahead_prepared && !interrupted() && write_reference_demand) {
                const detail::MaintenanceBackingScope measure(backing_use);
                amd_cpu_trial=record_ordinary_cpu_reference(observed,epoch,amd_cpu_trial_attempted);
              }
#endif
              auto continuation=WinFspMaintenanceContinuation::none;
              while(!read_ahead_prepared && !amd_cpu_trial_attempted) {
                if(observed.continued && residency && observed.valid)residency->verify_working_set();
                if(observed.continued && interrupted())break;
                const auto backing_before=backing_use.elapsed_ns();
                auto step=[&] {
                  detail::MaintenanceBackingUse step_backing;
                  auto step=[&] {
                    const detail::MaintenanceBackingScope measure(step_backing);
                    return advance(observed,continuation,scrub_due);
                  }();
                  backing_use.add(step_backing);
                  if(step.file_reclamation==NativeVolumeReclaimProgress::retiring)
                    file_retirement_backing.add(step_backing);
                  if(step.file_rebuild_entry) {
                    const auto& entry=*step.file_rebuild_entry;
                    auto& measured=file_rebuild_phases[static_cast<std::size_t>(entry.phase)];
                    ++measured.steps;
                    measured.backing.add(step_backing);
                    last_file_rebuild_entry=entry;
                  }
                  const detail::MaintenanceBackingScope measure(backing_use);
                  if(!verify_before_maintenance && !observed.continued && read_worker && !interrupted())
                    read_ahead_prepared=read_worker->verify_and_publish(
                        *adapter,observed,GetTickCount64(),stop_requested);
                  // A group duty refusal consumes no group work. A pending
                  // read can use the remaining host window under its own
                  // backing and memory admission.
                  const bool group_yielded=step.continuation==Continuation::group &&
                      step.progress.deferral==NativeMaintenanceDeferral::duty && !waiting_for_accelerator();
                  const auto demand_sample=!read_ahead_prepared && !observed.continued &&
                      (step.state==WinFspMaintenanceState::unchanged || group_yielded) && !interrupted()?
                      adapter->take_native_lz4_read_ahead_sample(epoch):std::nullopt;
                  if(demand_sample) {
                    read_ahead_timing.observe(*demand_sample);
                    read_ahead_demand_sampled=*demand_sample!=0;
                    const auto allowance=host.continue_preparation(adapter->group_maintenance_policy().maximum_age_ms,
                        read_ahead_timing.predicted_ns(),&read_ahead_backing_refused);
                    if(allowance && !interrupted()) {
                      const auto before=backing_use.elapsed_ns();
                      auto work=adapter->capture_native_lz4_read_ahead(*allowance,GetTickCount64(),epoch,stop_requested,
                          [&](const auto& forecast) {
                            if(read_worker)(void)read_worker->prepare_group(forecast,*allowance,stop_requested);
                          },read_worker?read_worker->group_read_window():WinFspGroupReadWindow{});
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
                      if(work && !read_worker)begin_read_acquisition(*work,*allowance);
#endif
                      const bool submitted=read_worker && work && read_worker->start(*work,*allowance,stop_requested);
                      if(read_worker && !submitted)read_worker->abandon_unused_preparation(*adapter);
                      if(work && !submitted && !work->expanded_group_window)
                        read_ahead_prepared=adapter->prepare_captured_native_lz4_read(*work,stop_requested);
                      read_ahead_timing.observe(backing_use.elapsed_ns()-before);
                      if(!submitted && !read_ahead_prepared)adapter->discard_native_lz4_read_ahead(epoch);
                    } else if(read_ahead_backing_refused)adapter->discard_native_lz4_read_ahead(epoch,true);
                  }
                  return step;
                }();
                if(observed.continued && step.state==WinFspMaintenanceState::busy)break;
                auto& timing=backing_timing[static_cast<std::size_t>(step.continuation)];
                if(step.continuation!=Continuation::none && step.progress.deferral==NativeMaintenanceDeferral::none)
                  timing.observe(backing_use.elapsed_ns()-backing_before);
                if(step.file_reclamation!=NativeVolumeReclaimProgress::idle)
                  ++file_reclamation_steps[static_cast<std::size_t>(step.file_reclamation)];
                result=std::move(step);
                const auto scrub=result.payload_scrub;
                scrub_checks+=scrub.examined_objects;
                if(scrub.verified_checkpoint)payload_checkpoint=scrub.verified_checkpoint;
                if(scrub.state==Scrub::unverified_payload || scrub.state==Scrub::unverified_index) {
                  ++scrub_faults;
                  last_scrub_fault=scrub;
                  scrub_pass_faulted=true;
                }
                if(scrub.state==Scrub::repair_queued)scrub_pass_faulted=true;
                if(scrub.state==Scrub::complete || scrub.state==Scrub::unverified_index) {
                  if(scrub.state==Scrub::complete)++scrub_passes;
                  next_scrub=GetTickCount64()+scrub_day_ms*(scrub_pass_faulted?1:28);
                  scrub_pass_faulted=false;
                }
                if(result.payload_repaired && !scrub_due)next_scrub=0;
                if(result.state==WinFspMaintenanceState::unavailable ||
                    result.progress.phase==NativeMaintenancePhase::failed)disabled=true;
                if(observed.continued && result.state==WinFspMaintenanceState::advanced &&
                    result.progress.deferral==NativeMaintenanceDeferral::none) {
                  ++continued;
                  if(continuation==WinFspMaintenanceContinuation::compaction)++continued_compaction;
                  if(continuation==Continuation::range_retirement)++continued_range_retirement;
                  if(continuation==Continuation::packed_tail_retirement)++continued_packed_tail;
                }
                continuation=result.continuation;
                if(disabled || continuation==WinFspMaintenanceContinuation::none ||
                    !can_continue_preparation(result.progress) || interrupted() ||
                    (gpu && !gpu->wait_ms(GetTickCount64())))break;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
                if(acquisition_thread.joinable())break;
#endif
                const auto refreshed=host.continue_preparation(adapter->group_maintenance_policy().maximum_age_ms,
                    timing.predicted_ns(),&backing_limited);
                if(!refreshed || interrupted())break;
                observed=*refreshed;
              }
            }
            if(host.status()==MaintenanceHostStatus::unavailable)
              disabled=true;
          }catch(...){disabled=true;}
          if(!priority.finish())disabled=true;
          if(disabled)session->cancel_background_maintenance();
          const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
          const auto used=elapsed>0?static_cast<std::uint64_t>(elapsed):1;
          // 1% elapsed-work share charges telemetry and wrapper work too. File
          // activity wakes the owner but never erases accumulated pacing debt.
          const auto duty_ms=maintenance_delay_ms(used);
          if(refresh_reference) {
            // This sample refreshes telemetry for the existing grant deadline.
            // It neither admits backing work nor repays outstanding duty.
            reference_refreshed=true;
            next=detail::saturated_backing_sum(next,duty_ms);
            std::lock_guard lock(mutex);
            current.host=host.status();current.maintenance_disabled=disabled;
            ++current.maintenance_reference_refresh_attempts;
            current.maintenance_elapsed_ns+=used;
            continue;
          }
          reference_refreshed=false;
          const auto delay=(std::max)({std::uint64_t(1),host.next_delay_ms(),maintenance_progress_delay_ms(result.progress),duty_ms});
          next=GetTickCount64()+(std::min)(delay,maximum_finite_wait);
          bool notify_integrity_fault=false;
          {
            std::lock_guard lock(mutex);current.host=host.status();current.maintenance_disabled=disabled;
            if(result.state==WinFspMaintenanceState::advanced || result.state==WinFspMaintenanceState::unchanged)
              current.maintenance=result.progress;
            ++current.maintenance_quanta;
            current.continued_preparation_steps+=continued;
            current.continued_compaction_steps+=continued_compaction;
            current.continued_range_retirement_steps+=continued_range_retirement;
            current.continued_packed_tail_steps+=continued_packed_tail;
            for(std::size_t i=0;i<file_reclamation_steps.size();++i)
              current.file_reclamation_steps[i]+=file_reclamation_steps[i];
            current.file_retirement_backing.add(file_retirement_backing);
            for(std::size_t i=0;i<file_rebuild_phases.size();++i) {
              auto& phase=current.file_rebuild_phases[i];
              phase.steps+=file_rebuild_phases[i].steps;
              phase.backing.add(file_rebuild_phases[i].backing);
            }
            if(last_file_rebuild_entry)current.last_file_rebuild_entry=last_file_rebuild_entry;
            if(result.reclaimed)++current.reclamation_quanta;
            if(result.payload_repaired)++current.payload_repairs;
            current.payload_scrub_checks+=scrub_checks;
            current.payload_scrub_passes+=scrub_passes;
            if(payload_checkpoint)current.last_payload_checkpoint=payload_checkpoint;
            if(scrub_faults) {
              notify_integrity_fault=current.payload_scrub_faults==0;
              current.payload_scrub_faults+=scrub_faults;
              current.last_payload_scrub_fault=last_scrub_fault;
            }
            if(result.compacted)++current.compactions;
            if(result.sealed_image)++current.sealed_compactions;
            if(result.compaction_failed)++current.compaction_failures;
            if(anchor)current.anchor_search=anchor->stats();
            if(read_worker)current.amd_read=read_worker->stats();
            current.maintenance_elapsed_ns+=used;
            current.maximum_quantum_ns=(std::max)(current.maximum_quantum_ns,used);
            current.backing_limited_continuations+=backing_limited;
            current.maintenance_backing.add(backing_use);
            current.read_ahead_prepared+=read_ahead_prepared;
            current.read_ahead_backing_refusals+=read_ahead_backing_refused;
            current.read_ahead_demand_samples+=read_ahead_demand_sampled;
            current.amd_cpu_trial_attempts+=amd_cpu_trial_attempted;
            if(amd_cpu_trial)current.amd_cpu_trial=amd_cpu_trial;
          }
          if(notify_integrity_fault && status_changed.value)SetEvent(status_changed.value);
        }
      }
    }catch(...){disabled=true;failed=true;}
    // Normal priority, no view lock, and no independent maintenance thread while
    // the deferred service/dispatcher are joined. Never publish retirement early.
    if(anchor)anchor->cancel();
    if(read_worker)read_worker->observe(*adapter,true);
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    settle_acquisition(false);
#endif
    session->stop();adapter=nullptr;
    const bool read_retired=!read_worker || read_worker->stop();
    bool anchor_retired=!anchor || anchor->stop();
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    anchor_retired=anchor_retired && acquisition_retired;
    acquisition_inputs.reset();
    read_inputs.reset();
    write_trial_inputs.reset();
#endif
    std::lock_guard lock(mutex);current.state=session->state();current.start_status=started;
    current.offline=session->requires_offline();
    current.write_restart_ready=anchor_retired && read_retired && session->write_restart_ready();
    if(foreground_offer) {
      foreground_offer.reset();
      current.foreground_activation=NativeDeferredActivation::refused;
    }
    current.controller_failed=failed || !anchor_retired || !read_retired;
    if(anchor)current.anchor_search=anchor->stats();
    if(read_worker) {
      current.amd_read=read_worker->stats();
      read_worker.reset();
    }
    if(const auto delivery=session->group_read_delivery())current.group_read_delivery=*delivery;
    current.maintenance_disabled=disabled;current.retired=anchor_retired && read_retired;
    if(status_changed.value)SetEvent(status_changed.value);
  }
  WinFspMaintenanceResult advance(const NativeMaintenanceObservation& observed,
      WinFspMaintenanceContinuation continuation,bool scrub_due) {
    const auto now=GetTickCount64();
    if(continuation==WinFspMaintenanceContinuation::compaction ||
        continuation==WinFspMaintenanceContinuation::range_retirement ||
        continuation==WinFspMaintenanceContinuation::packed_tail_retirement ||
        continuation==WinFspMaintenanceContinuation::payload_scrub)
      return session->advance_background_maintenance(observed,now,{},continuation,scrub_due);
    if(!anchor) {
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
      const bool may_acquire=installed && acquisition_retired;
      auto result=session->advance_background_maintenance(observed,now,{0,nullptr,may_acquire},continuation,scrub_due);
      begin_acquisition(result,observed);
      return result;
#else
      return session->advance_background_maintenance(observed,now,{},continuation,scrub_due);
#endif
    }
    const auto epoch=adapter->foreground_activity();
    const auto resources=detail::maintenance_resources(observed,now,adapter->group_maintenance_policy());
    const auto funded_extents=resources?detail::maintenance_group_member_limit(*resources,true):0;
    const bool admitted=resources && !resources->cpu_busy && anchor->fits_group_budget(funded_extents);
    const bool foreground=adapter->foreground_pending() || adapter->foreground_activity()!=epoch;
    if(!admitted)anchor->cancel();
    const bool idle=admitted && !foreground;
    const auto opportunity=idle?NativeAnchorOpportunity::idle:
        foreground?NativeAnchorOpportunity::foreground:NativeAnchorOpportunity::poll;
    anchor->observe(epoch,now,opportunity);
    if(const auto prefix=anchor->pending_prefix();!prefix.empty()) {
      return session->advance_background_maintenance(observed,GetTickCount64(),{0,nullptr,true,{},prefix},
          continuation,scrub_due);
    }
    const auto replies=anchor->replies();
    auto result=session->advance_background_maintenance(observed,GetTickCount64(),
        {idle?anchor->stage_grant(now,funded_extents):0,nullptr,true,replies},continuation,scrub_due);
    if(!replies.empty()) {
      anchor->settle_reply_checks();
      if(!result.retained_search && result.state!=WinFspMaintenanceState::busy &&
          (result.progress.deferral==NativeMaintenanceDeferral::none ||
           result.progress.deferral==NativeMaintenanceDeferral::capacity))anchor->discard_reply();
    } else if(!stop_requested.load(std::memory_order_acquire)) {
      auto windows=std::span<const NativeSharedBaseSearchWindow>(result.search_batch);
      if(result.search_window)windows=std::span(&result.search_window,1);
      if(!windows.empty()) {
        adapter->signal_foreground_for_anchor(true);
        if(!adapter->foreground_pending() && adapter->foreground_activity()==epoch) {
          anchor->start(windows,epoch);
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
          if(anchor->waiting())last_anchor_candidate=result.progress.attempted_groups;
#endif
        }
        anchor->observe(adapter->foreground_activity(),GetTickCount64());
        adapter->signal_foreground_for_anchor(waiting_for_accelerator());
      }
    }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
    begin_acquisition(result,observed);
#endif
    return result;
  }
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  std::optional<WinFspAmdCpuTrial> record_ordinary_cpu_reference(
      const NativeMaintenanceObservation& observed,std::uint64_t epoch,bool& attempted) {
    if(!write_reference_demand)return {};
    const auto now=GetTickCount64();
    const auto policy=adapter->group_maintenance_policy();
    const auto& demand=*write_reference_demand;
    // Retain the checkpoint receipt through CPU counter warm-up. The live
    // observation has its own shorter freshness bound at admission.
    const auto receipt_window=2ULL*policy.maximum_cpu_window_ms;
    const auto completed=demand.samples.back().completed_ms;
    if(!demand.samples.front().elapsed_ns || completed>now || now-completed>receipt_window) {
      write_reference_demand.reset();
      return {};
    }
    const auto allowance=detail::maintenance_resources(observed,now,policy);
    if(!allowance || allowance->cpu_busy || observed.foreground_pending)return {};
    if(!write_package_present)write_package_present=installed_uniform_write_files_present();
    const auto selected=*std::exchange(write_reference_demand,std::nullopt);
    if(!*write_package_present)return {};
    attempted=true;
    const auto interrupted=[&] {
      return stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
          adapter->foreground_activity()!=epoch || adapter->requires_offline() || adapter->dispatcher_stopped();
    };
    const auto size=selected.shape.input_bytes/kNativeExtentBytes-1;
    const bool arriving=installed_write_trial && installed_write_trial->await_later_writes;
    const bool compare=(selected.grouped_candidate || arriving) && may_sample_write_trial(selected,now);
    const auto trial_bytes=compare?std::uint64_t(installed_write_trial->maximum_batch_chunks)*kNativeExtentBytes:UINT64_MAX;
    auto capture=adapter->capture_amd_write_group(selected,observed,GetTickCount64(),epoch,stop_requested,
        !(compare && arriving) && selected.grouped_candidate && (!recorded_group_sizes[size] || compare),trial_bytes);
    if(capture.empty() || (capture.size()==1 && recorded_write_sizes[size] && !(compare && arriving)) || interrupted() ||
        GetTickCount64()-observed.captured_ms>policy.maximum_age_ms)return {};
    std::vector<UniformLz4BatchInput> inputs;
    inputs.reserve(capture.size());
    std::uint64_t bytes=0;
    for(const auto& item:capture) {
      inputs.push_back({item.sequence,item.bytes});
      bytes+=item.bytes.size();
    }
    const auto& storage=selected.shape.storage;
    const auto started=std::chrono::steady_clock::now();
    const auto result=record_native_amd_cpu_batch(storage,inputs,allowance->memory_allowance,residency,interrupted);
    if(!result || interrupted())return {};
    (inputs.size()>1?recorded_group_sizes:recorded_write_sizes).set(size);
    const WinFspAmdCpuTrial reference{selected,*result,bytes,observed_ns(started)};
    if(compare && (arriving || inputs.size()>1))
      begin_write_acquisition(reference,std::move(capture),inputs,observed,allowance->memory_allowance,epoch);
    return reference;
  }

  static std::uint64_t observed_ns(std::chrono::steady_clock::time_point started) noexcept {
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
    return elapsed>0?static_cast<std::uint64_t>(elapsed):0;
  }

  bool write_trial_due(const NativeAmdWriteDemand& demand,std::uint64_t now) const noexcept {
    if(!installed_write_trial || !acquisition_retired || acquisition_thread.joinable() ||
        now<next_write_trial_ms ||
        demand.shape.storage!=write_calibration_policy ||
        !engine_activity(last_write_engine_sample,write_engine_wait_adapter))return false;
    const auto chunks=demand.shape.input_bytes/kNativeExtentBytes;
    const auto maximum=installed_write_trial->maximum_batch_chunks;
    return chunks && chunks<=completed_write_sizes.size() && !completed_write_sizes[chunks-1] &&
        (installed_write_trial->await_later_writes?chunks<=maximum:chunks<maximum);
  }

  bool may_sample_write_trial(const NativeAmdWriteDemand& demand,std::uint64_t now) {
    if(!write_trial_due(demand,now) || (anchor && anchor->waiting()) || !WorkerExchangeSession::available())return false;
    if(!write_trial_requires_evidence)return true;
    if(write_trial_inputs && write_trial_inputs->changed()) {
      write_trial_requires_evidence=false;
      return true;
    }
    next_write_trial_ms=now+installed_write_trial->worker.timeout_ms;
    return false;
  }

  void observe_write_trial(WindowsMaintenanceHost& host) {
    const auto stop=acquisition_thread.get_stop_token();
    if(stop.stop_requested()) {
      next_write_poll_ms=GetTickCount64()+installed_write_trial->worker.timeout_ms;
      return;
    }
    const bool parked=write_waiting_boundary.load(std::memory_order_acquire)!=0;
    const auto observed=host.observe(!parked && adapter->foreground_pending());
    const auto now=GetTickCount64();
    next_write_poll_ms=now+(std::max)(host.next_delay_ms(),std::uint64_t{1});
    if(host.status()==MaintenanceHostStatus::waiting)return;
    const auto resources=detail::maintenance_resources(observed,now,adapter->group_maintenance_policy());
    if(!resources || resources->cpu_busy || observed.foreground_pending ||
        !write_trial_plan.fits(installed_write_trial->worker,resources->memory_allowance)) {
      write_arrival_observation.reset();
      acquisition_thread.request_stop();
    } else {
      gpu_engines.observe(resources->memory_allowance);
      write_arrival_observation=observed;
      write_trial_idle_ms.store(observed.captured_ms,std::memory_order_release);
    }
  }

  void offer_arriving_write() {
    const auto waiting=write_waiting_boundary.load(std::memory_order_acquire);
    if(!waiting || acquisition_thread.get_stop_token().stop_requested() || adapter->foreground_pending())return;
    const bool allow_smaller=installed_write_trial->allow_smaller_writes;
    if(allow_smaller) {
      if(const auto arrival=adapter->take_amd_write_demand(true);arrival &&
          (!write_reference_demand || arrival->source_sequence>=write_reference_demand->source_sequence))
        write_reference_demand=arrival;
    }
    if(!write_reference_demand || !write_arrival_observation)return;
    const auto& available=*write_reference_demand;
    const auto bytes=available.shape.input_bytes;
    if(available.source_sequence<=last_captured_write || bytes>write_trial_shape.input_bytes ||
        (!allow_smaller && bytes!=write_trial_shape.input_bytes) ||
        available.shape.storage!=write_trial_shape.storage)return;
    const auto now=GetTickCount64();
    const auto& observation=*write_arrival_observation;
    if(now<observation.captured_ms || now-observation.captured_ms>adapter->group_maintenance_policy().maximum_age_ms)return;
    const auto epoch=adapter->foreground_activity();
    MaintenanceThreadScope priority;
    if(!priority.active()) {acquisition_thread.request_stop();return;}
    const auto selected=*std::exchange(write_reference_demand,std::nullopt);
    auto captured=adapter->capture_amd_write_group(selected,observation,now,epoch,stop_requested,false,
        write_trial_shape.input_bytes,allow_smaller);
    if(!priority.finish()) {acquisition_thread.request_stop();return;}
    if(captured.size()!=1 || captured.front().sequence!=selected.source_sequence ||
        captured.front().bytes.size()!=selected.shape.input_bytes || adapter->foreground_pending() || adapter->foreground_activity()!=epoch)return;
    const std::lock_guard lock(mutex);
    if(write_waiting_boundary.load(std::memory_order_acquire)!=waiting || queued_write)return;
    last_captured_write=captured.front().sequence;
    queued_write=WriteArrival{std::move(captured.front()),epoch};
    if(!SetEvent(write_arrival.value))acquisition_thread.request_stop();
  }

  PreparedRequestAction wait_for_arriving_write(const NativeAmdWriteBoundary& boundary,
      detail::NativeCapturedWrite& capture,const InstalledUniformWriteTrialProfile& profile,std::stop_token cancelled) {
    using Action=PreparedRequestAction;
    const auto wait=write_arrival_wait_ms(write_trial.reference.demand,boundary,profile.worker.timeout_ms,
        adapter->group_maintenance_policy());
    if(!wait)return Action::finish;
    if(!ResetEvent(write_arrival.value))return Action::cancel;
    const std::stop_callback wake(cancelled,[this]() noexcept {(void)SetEvent(write_arrival.value);});
    if(cancelled.stop_requested())return Action::cancel;
    write_waiting_boundary.store(boundary.completed_requests,std::memory_order_release);
    const HANDLE events[]{stop.value,write_arrival.value};
    const auto waited=SetEvent(activity.value)?WaitForMultipleObjects(2,events,FALSE,wait):WAIT_FAILED;
    const std::lock_guard lock(mutex);
    auto offered=std::exchange(queued_write,std::nullopt);
    const auto epoch=offered?offered->epoch:adapter->foreground_activity();
    write_trial_epoch.store(epoch,std::memory_order_release);
    write_waiting_boundary.store(0,std::memory_order_release);
    if(cancelled.stop_requested() || stop_requested.load(std::memory_order_acquire) ||
        adapter->foreground_pending() || adapter->foreground_activity()!=epoch)return Action::cancel;
    if(waited==WAIT_TIMEOUT)return Action::finish;
    if(waited!=WAIT_OBJECT_0+1 || !offered)return Action::cancel;
    capture=std::move(offered->capture);
    return Action::submit;
  }

  std::optional<HipDiscoveryEngineActivity> engine_activity(std::uint64_t last_used,
      const std::optional<std::array<std::byte,8>>& waiting_adapter,
      std::uint64_t duty_ppm=maintenance_wall_share_ppm) const noexcept {
    const auto now=GetTickCount64();
    auto snapshot=gpu_engines.current(now);
    if(!snapshot || last_used==snapshot->captured_ms)return {};
    // Keep scheduling margin for other GPU clients and cover the existing
    // two-quantum burst when the role requests a larger work share.
    const auto idle=background_gpu_idle_ppm(duty_ppm);
    if(waiting_adapter && std::ranges::find(snapshot->adapters,*waiting_adapter,&WindowsGpuEngineActivity::Adapter::luid)
        !=snapshot->adapters.end() && !snapshot->admits(*waiting_adapter,now,idle))return {};
    return HipDiscoveryEngineActivity{std::move(snapshot),idle};
  }

  void run_write_trial(const InstalledUniformWriteTrialProfile& profile,NativeUniformSweepGrant grant,
      const NativeAmdWriteWorkerPlan& worker_plan,NativeAmdTrialLaunch launch,
      std::span<const detail::NativeCapturedWrite> capture,
      std::uint64_t allowance,HipDiscoveryEngineActivity activity,std::stop_token stop) {
    const auto started=std::chrono::steady_clock::now();
    write_trial_inputs=std::make_unique<InstalledHipInputWitness>(HipRouteRole::uniform);
    auto acquired=discover_installed_hip_write_trial_route(stop,std::move(activity));
    write_trial.discovery_ns=observed_ns(started);
    write_trial.status=acquired.status;
    write_engine_wait_adapter=acquired.engine_wait_adapter;
    if(acquired.status==HipDiscoveryStatus::engine_deferred)return;
    if(acquired.status==HipDiscoveryStatus::device_not_ready)write_trial_inputs->note_device_unavailable();
    if(acquired.memory_pressure) {
      write_trial_inputs->note_memory_pressure(*acquired.memory_pressure);
      write_trial_memory_refused=true;
    }
    if(acquired.status!=HipDiscoveryStatus::ready)return;
    if(!acquired.route) {
      write_trial.status=HipDiscoveryStatus::profile_mismatch;
      return;
    }
    write_trial.configuration=acquired.route->identity;
    const auto policy=adapter->group_maintenance_policy();
    bool resource_refused=false,engine_refused=false;
    const auto adapter_luid=acquired.route->memory.luid;
    const auto min_idle=background_gpu_idle_ppm(maintenance_wall_share_ppm);
    const auto may_submit=[&]() {
      if(stop.stop_requested() || stop_requested.load(std::memory_order_acquire) ||
          adapter->foreground_pending() ||
          adapter->foreground_activity()!=write_trial_epoch.load(std::memory_order_acquire))return false;
      const auto now=GetTickCount64();
      const auto idle=write_trial_idle_ms.load(std::memory_order_acquire);
      bool funded=false;
      if(idle && now>=idle && now-idle<=policy.maximum_cpu_window_ms) {
        try {
          const auto memory=usable_memory_volume_headroom(observe_memory_volume_headroom())/policy.memory_share_divisor;
          funded=worker_plan.fits(profile.worker,(std::min)(allowance,memory));
        }catch(const std::exception&) {} // Treat changing or missing telemetry as refusal.
      }
      if(!funded) {
        resource_refused=true;
        return false;
      }
      const bool available=gpu_engines.available(adapter_luid,GetTickCount64(),min_idle).value_or(false);
      engine_refused|=!available;
      return available;
    };
    std::vector<UniformLz4BatchInput> inputs;
    inputs.reserve(capture.size());
    for(const auto& item:capture)inputs.push_back({item.sequence,item.bytes});
    const auto& shape=write_trial.reference.demand.shape;
    const auto& storage=shape.storage;
    const auto device=std::chrono::steady_clock::now();
    const bool finishable=profile.await_later_writes && launch==NativeAmdTrialLaunch::prepared_requests;
    if(finishable) {
      const auto continuation=[&](const NativeAmdWriteBoundary& boundary,detail::NativeCapturedWrite& next) {
        return wait_for_arriving_write(boundary,next,profile,stop);
      };
      write_trial.device=record_native_amd_device_burst(storage,inputs.front(),worker_plan.maximum_requests,*acquired.route,
          grant,profile.worker,allowance,residency,stop,continuation,may_submit,profile.allow_smaller_writes);
    } else {
      write_trial.device=record_native_amd_device_batch(storage,inputs,*acquired.route,grant,profile.worker,
          allowance,residency,launch,stop,may_submit);
    }
    write_trial.device_wall_ns=observed_ns(device);
    const auto& observed=write_trial.device;
    write_trial.device_wall_ns-=(std::min)(write_trial.device_wall_ns,observed.cpu_wall_ns);
    if(finishable && observed.sample && observed.matched_cpu) {
      write_trial.reference.sample=*observed.matched_cpu;
      write_trial.reference.input_bytes=observed.verified_input_bytes;
      write_trial.reference.wall_ns=observed.cpu_wall_ns;
    }
    const auto& reference=write_trial.reference.sample;
    if(observed.exchange==WorkerExchangeCode::exit_unconfirmed ||
        observed.exchange==WorkerExchangeCode::supervisor_exit_unconfirmed)
      write_trial.status=HipDiscoveryStatus::retirement_unconfirmed;
    else if(stop.stop_requested())write_trial.status=HipDiscoveryStatus::cancelled;
    else if(engine_refused) {
      write_trial.status=HipDiscoveryStatus::engine_deferred;
      write_engine_wait_adapter=adapter_luid;
    }
    else if(resource_refused || (!observed.sample && !observed.supervised_ns))
      write_trial.status=HipDiscoveryStatus::resource_deferred;
    else if(!observed.sample || !observed.worker_retired || !observed.sample->cpu_readback_verified ||
        !observed.sample->retirement_confirmed || !observed.sample->device_executed ||
        observed.sample->transactions!=reference.transactions || observed.sample->shape!=reference.shape ||
        observed.sample->input_digest!=reference.input_digest ||
        (finishable && (!observed.matched_cpu || observed.admitted_requests!=reference.transactions)) ||
        (!finishable && launch==NativeAmdTrialLaunch::prepared_requests && observed.admitted_requests!=capture.size())) {
      write_trial.status=HipDiscoveryStatus::configuration_quarantined;
      write_trial.device.sample.reset();
    }
  }

  void begin_write_acquisition(const WinFspAmdCpuTrial& reference,std::vector<detail::NativeCapturedWrite> capture,
      std::span<const UniformLz4BatchInput> inputs,const NativeMaintenanceObservation& observed,
      std::uint64_t allowance,std::uint64_t epoch) {
    const auto now=GetTickCount64();
    if(!may_sample_write_trial(reference.demand,now) || stop_requested.load(std::memory_order_acquire) ||
        adapter->foreground_pending() || adapter->foreground_activity()!=epoch ||
        now<observed.captured_ms || now-observed.captured_ms>adapter->group_maintenance_policy().maximum_age_ms)return;
    const auto profile=*installed_write_trial;
    const auto& shape=reference.demand.shape;
    const auto& storage=shape.storage;
    auto launch=profile.separate_requests?NativeAmdTrialLaunch::prepared_requests:NativeAmdTrialLaunch::prepared;
    const auto extent_count=shape.input_bytes/kNativeExtentBytes;
    auto worker_plan=profile.await_later_writes?plan_native_amd_write_burst(storage,shape.input_bytes,
        profile.maximum_batch_chunks/extent_count,profile.worker,allowance):
        plan_native_amd_write_worker(storage,inputs,launch,profile.worker,allowance);
    if(!worker_plan && launch==NativeAmdTrialLaunch::prepared_requests) {
      // Use a single sealed exchange if the current grant cannot fund two
      // complete arriving writes.
      launch=NativeAmdTrialLaunch::prepared;
      worker_plan=plan_native_amd_write_worker(storage,inputs,launch,profile.worker,allowance);
    }
    if(!worker_plan || worker_plan->group.chunks>profile.maximum_batch_chunks ||
        (profile.await_later_writes?(capture.size()!=1 || worker_plan->work.input_bytes!=reference.input_bytes):
            worker_plan->group.input_bytes!=reference.input_bytes))return;
    const auto chunks=(std::min)(worker_plan->work.chunks,profile.maximum_launch_chunks);
    uniform_lz4::Layout quantum{};
    if(!uniform_lz4::make_layout(std::uint64_t(chunks)*kNativeExtentBytes,kNativeExtentBytes,quantum))return;
    const NativeUniformSweepGrant grant{quantum.chunks,quantum.total_bytes};
    auto engine=engine_activity(last_write_engine_sample,write_engine_wait_adapter);
    if(!engine)return;
    last_write_engine_sample=engine->snapshot->captured_ms;
    write_trial={};
    write_trial_memory_refused=false;
    write_trial_plan=*worker_plan;
    write_trial.reference=reference;
    write_trial.status=HipDiscoveryStatus::resource_deferred;
    write_trial_inputs.reset();
    write_trial_shape=shape;
    last_captured_write=capture.front().sequence;
    write_arrival_observation=observed;
    write_trial_epoch.store(epoch,std::memory_order_release);
    write_trial_idle_ms.store(observed.captured_ms,std::memory_order_release);
    next_write_poll_ms=now;
    acquiring_write=true;
    qualifying_device=true;
    qualification_activity=epoch;
    adapter->signal_foreground_for_anchor(true);
    acquisition_finished.store(false,std::memory_order_relaxed);
    try {
      acquisition_thread=std::jthread([this,profile,grant,worker_plan=*worker_plan,launch,allowance,
          capture=std::move(capture),engine=std::move(*engine)](std::stop_token stop) noexcept {
        try {
          MaintenanceThreadScope priority;
          if(priority.active()) {
            run_write_trial(profile,grant,worker_plan,launch,capture,allowance,engine,stop);
            if(!priority.finish() && write_trial.status==HipDiscoveryStatus::ready)
              write_trial.status=HipDiscoveryStatus::resource_deferred;
          }
        }catch(...) {write_trial.status=HipDiscoveryStatus::resource_deferred;}
        acquisition_finished.store(true,std::memory_order_release);
        SetEvent(activity.value);
      });
    }catch(...) {settle_write_acquisition(true);}
    std::lock_guard lock(mutex);
    ++current.amd_write_trial_attempts;
  }

  void settle_write_acquisition(bool use_pair) {
    acquiring_write=false;
    qualifying_device=false;
    acquisition_retired=write_trial.status!=HipDiscoveryStatus::retirement_unconfirmed;
    if(acquisition_retired && (!use_pair || adapter->foreground_pending() ||
        adapter->foreground_activity()!=write_trial_epoch.load(std::memory_order_acquire) ||
        adapter->managed_foreground()!=write_trial_shape.storage || stop_requested.load(std::memory_order_acquire)))
      write_trial.status=HipDiscoveryStatus::cancelled;
    const bool matched=write_trial.status==HipDiscoveryStatus::ready && write_trial.device.sample.has_value();
    if(matched && write_trial_shape.storage==write_calibration_policy)
      completed_write_sizes.set(write_trial_shape.input_bytes/kNativeExtentBytes-1);
    if(!matched)write_trial.device.sample.reset();
    const auto duration=write_trial.discovery_ns+write_trial.device_wall_ns+write_trial.device.cpu_wall_ns;
    const auto delay=(std::max)(maintenance_delay_ms(duration),std::uint64_t(installed_write_trial->worker.timeout_ms));
    next_write_trial_ms=GetTickCount64()+delay;
    write_trial.next_eligible_ms=next_write_trial_ms;
    using Status=HipDiscoveryStatus;
    write_trial_requires_evidence=write_trial.status!=Status::ready && write_trial.status!=Status::cancelled &&
        write_trial.status!=Status::transport_busy && write_trial.status!=Status::engine_deferred &&
        (write_trial.status!=Status::resource_deferred || write_trial_memory_refused);
    if(!write_trial_requires_evidence)write_trial_inputs.reset();
    adapter->signal_foreground_for_anchor(waiting_for_accelerator());
    std::lock_guard lock(mutex);
    current.amd_write_trial=write_trial;
    if(status_changed.value)SetEvent(status_changed.value);
  }

  void begin_read_acquisition(const WinFspReadAheadWork& work,const NativeMaintenanceObservation& observed) {
    if(!installed_read || !acquisition_retired || acquisition_thread.joinable() ||
        (work.groups && (!installed_read->packed_groups ||
            (work.groups->mixed_methods() && !installed_read->mixed_groups))) ||
        stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
        !WorkerExchangeSession::available())return;
    const auto now=GetTickCount64();
    const auto refused=[&](std::uint64_t WinFspControllerView::* count) {
      std::lock_guard lock(mutex);
      ++(current.*count);
    };
    const auto frames=work.groups?work.groups->members().size():work.frames.size();
    if(frames<installed_read->minimum_frames || (work.groups && frames>installed_read->maximum_frames)) {
      refused(&WinFspControllerView::installed_read_shape_refusals);
      return;
    }
    if(!work.has_read_lead(installed_read->minimum_lead_ms,now)) {
      refused(&WinFspControllerView::installed_read_lead_refusals);
      return;
    }
    const auto& budget=installed_read->worker;
    if(!valid_process_budget(plan_resource_process_budget(budget.memory_mib*1024ULL*1024,observed.physical_available,
        observed.commit_available,budget.memory_share_divisor,budget.cpu_rate,budget.timeout_ms))) {
      refused(&WinFspControllerView::installed_read_resource_refusals);
      return;
    }
    if(read_acquisition_started && read_acquisition.status!=HipDiscoveryStatus::transport_busy &&
        read_acquisition.status!=HipDiscoveryStatus::engine_deferred) {
      if(now<next_read_evidence_ms || !read_inputs)return;
      // A sampled input must change. The interval bounds metadata checks.
      next_read_evidence_ms=now+budget.timeout_ms;
      if(!read_inputs->changed())return;
    }
    if(stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
        adapter->foreground_activity()!=work.foreground_epoch)return;
    auto engine=engine_activity(last_read_engine_sample,read_acquisition.engine_wait_adapter);
    if(!engine)return;
    last_read_engine_sample=engine->snapshot->captured_ms;
    read_acquisition_started=true;
    next_read_evidence_ms=now+budget.timeout_ms;
    read_acquisition={};
    acquiring_read=true;
    acquisition_finished.store(false,std::memory_order_relaxed);
    try {
      acquisition_thread=std::jthread([this,engine=std::move(*engine)](std::stop_token stop) noexcept {
        try {
          MaintenanceThreadScope priority;
          if(priority.active()) {
            read_inputs=std::make_unique<InstalledHipInputWitness>(HipRouteRole::native_lz4_read);
            read_acquisition=discover_installed_hip_native_read_route(stop,engine);
            if(read_acquisition.status==HipDiscoveryStatus::ready && read_acquisition.route && !stop.stop_requested()) {
              auto profile=bind_installed_native_read(*read_acquisition.route);
              if(profile)acquired_read_worker=std::make_unique<NativeLz4ReadWorker>(std::move(*profile),&gpu_engines);
              else read_acquisition.status=HipDiscoveryStatus::profile_mismatch;
            }
            if(!priority.finish()) {
              acquired_read_worker.reset();
              if(read_acquisition.status==HipDiscoveryStatus::ready)read_acquisition.status=HipDiscoveryStatus::resource_deferred;
            }
          }else read_acquisition.status=HipDiscoveryStatus::resource_deferred;
        }catch(...) {
          acquired_read_worker.reset();
          read_acquisition.status=HipDiscoveryStatus::resource_deferred;
        }
        acquisition_finished.store(true,std::memory_order_release);
        SetEvent(activity.value);
      });
    }catch(...) {
      acquiring_read=false;
      read_acquisition.status=HipDiscoveryStatus::resource_deferred;
      std::lock_guard lock(mutex);
      current.installed_read_status=read_acquisition.status;
    }
    std::lock_guard lock(mutex);
    ++current.installed_read_attempts;
  }
  void settle_read_acquisition(bool use_route) {
    acquiring_read=false;
    acquisition_retired=read_acquisition.status!=HipDiscoveryStatus::retirement_unconfirmed;
    if(read_inputs && read_acquisition.status==HipDiscoveryStatus::device_not_ready)read_inputs->note_device_unavailable();
    if(read_inputs && read_acquisition.memory_pressure)read_inputs->note_memory_pressure(*read_acquisition.memory_pressure);
    if(read_acquisition.status==HipDiscoveryStatus::engine_deferred)read_inputs.reset();
    if(read_acquisition.status==HipDiscoveryStatus::ready) {
      if(use_route && acquired_read_worker && !stop_requested.load(std::memory_order_acquire)) {
        read_worker=std::move(acquired_read_worker);
        read_inputs.reset();
      }else read_acquisition.status=HipDiscoveryStatus::cancelled;
    }
    acquired_read_worker.reset();
    read_acquisition.route.reset();
    std::lock_guard lock(mutex);
    current.installed_read_status=read_acquisition.status;
    current.installed_read_queries=read_acquisition.identity_queries;
    current.installed_read_worker_exit=read_acquisition.worker_exit;
    if(status_changed.value)SetEvent(status_changed.value);
  }
  void begin_acquisition(const WinFspMaintenanceResult& result,const NativeMaintenanceObservation& observed) {
    if(!installed)return;
    if(anchor && anchor->take_device_interruption()) {
      if(acquisition_inputs)acquisition_inputs->note_device_unavailable();
      std::lock_guard lock(mutex);
      current.installed_anchor_status=HipDiscoveryStatus::device_not_ready;
    }
    if(!acquisition_retired || acquisition_thread.joinable() ||
        (anchor && !anchor->can_reacquire()) || result.progress.phase!=NativeMaintenancePhase::preparing ||
        result.progress.deferral!=NativeMaintenanceDeferral::none ||
        result.group_members<2 ||
        stop_requested.load(std::memory_order_acquire) || adapter->foreground_pending() ||
        !WorkerExchangeSession::available())return;
    constexpr std::uint64_t mib=1024*1024;
    const auto& budget=installed->worker;
    if(!valid_process_budget(plan_resource_process_budget(budget.memory_mib*mib,observed.physical_available,
        observed.commit_available,budget.memory_share_divisor,budget.cpu_rate,budget.timeout_ms)))return;
    const auto candidate=result.progress.attempted_groups;
    const auto target=anchor?anchor->recovery_target():std::nullopt;
    if(acquisition_started) {
      if(candidate==last_acquisition_candidate)return;
      last_acquisition_candidate=candidate;
      const auto resources=target && candidate>last_anchor_candidate && !observed.continued?detail::maintenance_resources(
          observed,GetTickCount64(),adapter->group_maintenance_policy()):std::nullopt;
      const bool deadline_check=resources && !resources->cpu_busy && anchor->take_deadline_recovery();
      if(!deadline_check && acquisition.status!=HipDiscoveryStatus::transport_busy &&
          acquisition.status!=HipDiscoveryStatus::engine_deferred &&
          (!acquisition_inputs || !acquisition_inputs->changed()))return;
    }
    auto engine=engine_activity(last_anchor_engine_sample,acquisition.engine_wait_adapter,
        (std::max)(maintenance_wall_share_ppm,std::uint64_t(installed->duty_ppm)));
    if(!engine)return;
    last_anchor_engine_sample=engine->snapshot->captured_ms;
    last_acquisition_candidate=candidate;
    acquisition_started=true;
    acquisition_inputs.reset();
    try {acquisition_inputs=std::make_unique<InstalledHipInputWitness>();}
    catch(...) {}
    acquisition={};
    acquisition_proof.reset();
    qualifying_device=target.has_value();
    qualification_activity=adapter->foreground_activity();
    if(qualifying_device)adapter->signal_foreground_for_anchor(true);
    acquisition_finished.store(false,std::memory_order_relaxed);
    try {
      acquisition_thread=std::jthread([this,target,budget,engine=std::move(*engine)](std::stop_token stop) noexcept {
        try {
          MaintenanceThreadScope priority;
          acquisition=priority.active()?discover_installed_hip_anchor_route(stop,engine):
              HipRouteDiscoveryResult{HipDiscoveryStatus::resource_deferred};
          if(target && acquisition.status==HipDiscoveryStatus::ready && acquisition.route &&
              acquisition.route->identity==target->identity) {
            auto checked=qualify_native_anchor_recovery(*target,*acquisition.route,budget,stop);
            acquisition.status=checked.status;
            acquisition_proof=std::move(checked.proof);
          }
        }catch(...) {acquisition={HipDiscoveryStatus::resource_deferred};}
        acquisition_finished.store(true,std::memory_order_release);
        SetEvent(activity.value);
      });
    }catch(...) {
      qualifying_device=false;
      adapter->signal_foreground_for_anchor(waiting_for_accelerator());
      acquisition.status=HipDiscoveryStatus::resource_deferred;
      std::lock_guard lock(mutex);
      current.installed_anchor_status=acquisition.status;
    }
    std::lock_guard lock(mutex);
    ++current.installed_anchor_attempts;
  }
  void settle_acquisition(bool use_route) {
    if(!acquisition_thread.joinable())return;
    if(use_route && !acquisition_finished.load(std::memory_order_acquire))return;
    if(!use_route)acquisition_thread.request_stop();
    acquisition_thread.join();
    if(acquiring_read){settle_read_acquisition(use_route);return;}
    if(acquiring_write){settle_write_acquisition(use_route);return;}
    if(qualifying_device && (adapter->foreground_pending() || adapter->foreground_activity()!=qualification_activity)) {
      use_route=false;
      if(acquisition.status==HipDiscoveryStatus::ready)acquisition.status=HipDiscoveryStatus::cancelled;
    }
    qualifying_device=false;
    acquisition_retired=acquisition.status!=HipDiscoveryStatus::retirement_unconfirmed;
    if(acquisition.status==HipDiscoveryStatus::engine_deferred)acquisition_inputs.reset();
    if(acquisition_inputs && acquisition.status==HipDiscoveryStatus::device_not_ready)
      acquisition_inputs->note_device_unavailable();
    if (use_route && acquisition_inputs && acquisition.memory_pressure)
      acquisition_inputs->note_memory_pressure(*acquisition.memory_pressure);
    if(use_route && acquisition.status==HipDiscoveryStatus::ready && acquisition.route &&
        !stop_requested.load(std::memory_order_acquire)) {
      try {
        if(anchor) {
          if(!anchor->rebind(std::move(*acquisition.route),acquisition_proof?&*acquisition_proof:nullptr))
            acquisition.status=HipDiscoveryStatus::configuration_quarantined;
        } else {
          anchor=std::make_unique<NativeAnchorSearchController>(NativeAnchorSearchOptions{
              .route=std::move(*acquisition.route),.worker=installed->worker,.minimum_members=installed->minimum_members,
              .maximum_members=installed->maximum_members,.duty_ppm=installed->duty_ppm,
              .output=installed->output,
              .maximum_dispatch_members=installed->maximum_dispatch_members,
              .dispatches_per_wait=installed->dispatches_per_wait,
              .prefer_reference=installed->prefer_reference},&gpu_engines);
        }
      }catch(...) {acquisition.status=HipDiscoveryStatus::resource_deferred;}
    }
    acquisition_proof.reset();
    acquisition.route.reset();
    std::lock_guard lock(mutex);
    current.installed_anchor_status=acquisition.status;
    current.installed_anchor_queries=acquisition.identity_queries;
  }
#endif
};
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,std::optional<NativeAnchorSearchOptions> anchor,HANDLE state_changed,
    std::optional<NativeLz4ReadWorkerProfile> read_worker):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),nullptr,std::move(anchor),state_changed,
        std::move(read_worker))){}
WinFspVolumeController::~WinFspVolumeController()=default;
WinFspVolumeController::WinFspVolumeController(std::unique_ptr<WinFspNativeWritable> adapter,
    WinFspSessionApi api,NativeWriteMountPlan plan,MemoryVolumeDomain& memory,
    std::optional<NativeAnchorSearchOptions> anchor,HANDLE state_changed,
    std::optional<NativeLz4ReadWorkerProfile> read_worker):
    impl_(std::make_unique<Impl>(std::move(adapter),api,std::move(plan),&memory,std::move(anchor),state_changed,
        std::move(read_worker))){}
void WinFspVolumeController::request_stop() noexcept {impl_->request_stop();}
bool WinFspVolumeController::offer_qualified_foreground_route(NativeDeferredWriteOptions options) {
  auto& self=*impl_;
  std::lock_guard lock(self.mutex);
  if(self.current.state!=SessionState::running || self.stop_requested.load(std::memory_order_acquire) ||
      self.foreground_offer || self.current.foreground_activation==NativeDeferredActivation::active)return false;
  self.foreground_offer.emplace(std::move(options));
  if(!SetEvent(self.activity.value)) {
    self.foreground_offer.reset();
    return false;
  }
  self.current.foreground_activation=NativeDeferredActivation::busy;
  return true;
}
void WinFspVolumeController::stop_and_join() noexcept {
  impl_->request_stop();if(impl_->worker.joinable())impl_->worker.join();
}
WinFspControllerView WinFspVolumeController::view() const {
  const std::lock_guard lock(impl_->mutex);
  auto result=impl_->current;
#ifdef FLITZFS_ACQUIRE_INSTALLED_AMD
  result.amd_write_waiting=impl_->write_waiting_boundary.load(std::memory_order_acquire)!=0;
#endif
  return result;
}
}
