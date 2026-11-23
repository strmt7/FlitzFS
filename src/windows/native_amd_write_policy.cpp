#include "native_amd_write_policy.hpp"
#include "memory_volume.hpp"
#include "gpu_budget.hpp"
#include "../gpu_request_id.hpp"
#include "../record_digest.hpp"
#include "../store_codec.hpp"
#include "../store_read_scope.hpp"
#include "flitzfs/paged_store.hpp"
#include <algorithm>
#include <chrono>
#include <limits>

namespace flitzfs {
namespace {
constexpr auto indexed_file_info=NativeFileInfoAllocation::indexed_reserved_cache;
bool valid_shape(const NativeAmdWriteShape& shape) noexcept {
  return native_amd_write_geometry(indexed_file_info,0,shape.input_bytes,false,false) &&
      valid_store_io_contract({shape.isolation_unit,true}) &&
      (shape.foreground.mode==StoreCompressionMode::fast || shape.foreground.mode==StoreCompressionMode::automatic) &&
      shape.foreground.zstd_level>=1 && shape.foreground.zstd_level<=9;
}
bool verified(const NativeAmdWriteSample& sample,bool amd) noexcept {
  return sample.elapsed_ns && sample.occupied_bytes && sample.input_digest!=decltype(sample.input_digest){} &&
      sample.cpu_readback_verified && sample.retirement_confirmed && sample.device_executed==amd &&
      (amd || !sample.stored_lz4_candidates);
}
}

NativeAmdWritePreference evaluate_native_amd_writes(const NativeAmdWriteEvidence& evidence) noexcept {
  using Result=NativeAmdWritePreference;
  const auto& shape=evidence.shape;
  if(evidence.configuration==GpuConfigurationId{} || !valid_shape(shape) ||
      evidence.comparisons.size()<kNativeAmdWriteMinimumPairs)
    return Result::unqualified;
  auto cpu_min=(std::numeric_limits<std::uint64_t>::max)();
  std::uint64_t cpu_max=0,amd_max=0;
  bool smaller=true,nonexpanding=true;
  bool device_output_stored=true;
  auto previous_order=!evidence.comparisons.front().amd_first;
  for(const auto& trial:evidence.comparisons) {
    if(trial.amd_first==previous_order || trial.cpu.input_digest!=trial.amd.input_digest ||
        !verified(trial.cpu,false) || !verified(trial.amd,true) ||
        trial.amd.stored_lz4_candidates>shape.input_bytes/kNativeExtentBytes)
      return Result::unqualified;
    previous_order=trial.amd_first;
    cpu_min=(std::min)(cpu_min,trial.cpu.elapsed_ns);
    cpu_max=(std::max)(cpu_max,trial.cpu.elapsed_ns);
    amd_max=(std::max)(amd_max,trial.amd.elapsed_ns);
    smaller=smaller && trial.amd.occupied_bytes<trial.cpu.occupied_bytes;
    nonexpanding=nonexpanding && trial.amd.occupied_bytes<=trial.cpu.occupied_bytes;
    device_output_stored=device_output_stored && trial.amd.stored_lz4_candidates;
  }
  if(device_output_stored && ((nonexpanding && amd_max<cpu_min) || (smaller && amd_max<=cpu_max)))return Result::amd;
  return Result::cpu;
}

namespace {
using TrialClock=std::chrono::steady_clock;
std::uint64_t elapsed_ns(TrialClock::time_point started) {
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(TrialClock::now()-started).count();
  return elapsed>0?static_cast<std::uint64_t>(elapsed):0;
}
std::optional<NativeAmdWriteSample> record_private_write(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,std::span<const NativeLz4ExtentCandidate> advice,
    std::uint64_t allowance,MemoryVolumeDomain* existing,const std::function<bool()>& interrupted) noexcept {
  try {
    if(!valid_shape(shape) || source.size()!=shape.input_bytes)return {};
    detail::StoreCaptureReadScope read_scope(interrupted);
    // Charge the detached source, staging copies, and decoder before handing
    // the remaining resource grant to a demand-committed memory provider.
    const auto scratch=4ULL*source.size()+detail::kStoreDecoderMemoryCap+4ULL*kStoreCommitUnit;
    if(allowance<=scratch || read_scope.interrupted())return {};
    auto capacity=allowance-scratch;
    if(existing) {
      const auto used=existing->usage();
      if(used.reserved>=used.capacity)return {};
      capacity=(std::min)(capacity,used.capacity-used.reserved);
    }
    capacity-=capacity%shape.isolation_unit;
    if(capacity<2ULL*source.size()+4ULL*kStoreCommitUnit)return {};
    NativeAmdWriteSample sample;
    {
      std::optional<MemoryVolumeDomain> owned;
      if(!existing)owned.emplace(MemoryVolumeDomain::create(capacity));
      auto& domain=existing?*existing:*owned;
      auto image=domain.create_store(capacity,shape.isolation_unit);
      if(image->contract().write_failure_unit_bytes!=shape.isolation_unit || read_scope.interrupted())return {};
      StoreIdentity id{};
      id[0]=std::byte{1}; // Private image with no externally addressable identity.
      constexpr auto name="qualification";
      {
        auto writer=NativeFilesystemWriter::initialize_indexed(PagedObjectStoreWriter::create_mirrored(
            image,id,{capacity},shape.foreground));
        NativeFileMutation seed;
        seed.path=name;
        writer.apply(std::span{&seed,1});
        const auto inode=writer.snapshot().stat(name).inode;
        if(read_scope.interrupted())return {};
        const auto started=TrialClock::now();
        const auto written=writer.write_inode_storage_with_lz4(inode,0,source,advice);
        sample.elapsed_ns=elapsed_ns(started);
        if(!sample.elapsed_ns || written.bytes!=source.size() || read_scope.interrupted())return {};
        sample.stored_lz4_candidates=written.transaction.storage.stored_lz4_candidates;
      }
      sample.occupied_bytes=image->extent();
      {
        const auto recovered=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,capacity});
        recovered.verify_all();
        if(recovered.stat(name).logical_bytes!=source.size() || read_scope.interrupted())return {};
        for(std::uint32_t offset=0;offset<shape.input_bytes;offset+=kNativeExtentBytes) {
          if(read_scope.interrupted())return {};
          const auto bytes=recovered.read(name,offset,kNativeExtentBytes);
          if(bytes.size()!=kNativeExtentBytes || !std::equal(bytes.begin(),bytes.end(),source.begin()+offset))return {};
        }
      }
      sample.input_digest=detail::record_digest(source);
      sample.cpu_readback_verified=true;
    }
    if(!MemoryVolumeDomain::cleanup_healthy() || read_scope.interrupted())return {};
    sample.retirement_confirmed=true;
    return sample;
  }catch(...) {return {};}
}

}
std::optional<NativeAmdWriteSample> record_native_amd_cpu_trial(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,std::uint64_t allowance,MemoryVolumeDomain* existing,
    const std::function<bool()>& interrupted) noexcept {
  return record_private_write(shape,source,{},allowance,existing,interrupted);
}

NativeAmdDeviceTrial record_native_amd_device_trial(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,const HipDiscoveredRoute& route,NativeUniformSweepGrant grant,
    ProcessBudget budget,std::uint64_t allowance,MemoryVolumeDomain* existing,NativeAmdTrialLaunch launch,
    std::stop_token stop) noexcept {
  NativeAmdDeviceTrial trial;
  try {
    uniform_lz4::Layout layout{};
    if(stop.stop_requested() || !valid_shape(shape) || source.size()!=shape.input_bytes ||
        !gpu_lz4_cpu_verification_available() ||
        !uniform_lz4::make_layout(source.size(),kNativeExtentBytes,layout))return trial;
    const auto started=TrialClock::now();
    // The supplied live host grant funds the worker tree, parent transport and
    // immutable source before any runtime or adapter access.
    const auto limits=uniform_lz4_limits(layout);
    if(!uniform_worker_host_budget_covers(limits,budget,allowance) || !route.pins ||
        route.identity==GpuConfigurationId{} || route.ordinal>=64 || !route.pins->uniform_module_pinned() ||
        !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) ||
        !WorkerExchangeSession::available())return trial;
    const auto plan=plan_uniform_lz4_sweep(layout,grant.maximum_chunks,grant.maximum_device_bytes);
    if(!route.pins->device_ready() || stop.stop_requested())return trial;
    WindowsGpuBudgetSampler sampler(route.memory.luid,route.memory.node_mask);
    const auto before=sampler.sample();
    if(before.status!=WindowsGpuBudgetStatus::success ||
        check_gpu_memory_budget(before.snapshot,route.memory.luid,route.memory.node_mask,
            GetTickCount64(),plan.device_peak_bytes)!=GpuGateCode::admitted)return trial;

    const auto id=detail::next_gpu_request_id();
    if(!id || stop.stop_requested())return trial;
    const GpuHealthEvidence evidence{route.identity,id,0,true,true,before.snapshot};
    const NativeUniformWorkerBinding binding{.helper=route.helper,.configuration=route.identity,
        .ordinal=static_cast<int>(route.ordinal),.runtime_sha256=route.runtime_sha256,.sweep=grant};
    const auto batch=PreparedUniformLz4::prepare(source,limits);
    auto dispatch=prepare_native_uniform_dispatch(binding,evidence,batch,WorkerExecution::hip);
    const WorkerChannelCapacity channel{static_cast<std::size_t>(limits.request_bytes),
        static_cast<std::size_t>(limits.reply_bytes)};
    auto frame=std::make_shared<const WorkerRequestSnapshot>(encode_uniform_lz4_request(batch,id));
    WorkerExchangeResult result;
    if(launch==NativeAmdTrialLaunch::prepared) {
      dispatch.arguments.front()=L"--driver-uniform-lz4-prepared-adapter";
      const auto setup=TrialClock::now();
      const auto ready=[&] {
        trial.ready_ns=elapsed_ns(setup);
        const auto current=sampler.sample();
        return !stop.stop_requested() && current.status==WindowsGpuBudgetStatus::success &&
            current.snapshot.captured_ms>before.snapshot.captured_ms &&
            check_gpu_memory_transition(before.snapshot,current.snapshot,route.memory.luid,
                route.memory.node_mask,GetTickCount64(),plan.device_peak_bytes)==GpuGateCode::admitted;
      };
      result=WorkerExchangeSession::run_prepared_sized(binding.helper,dispatch.arguments,std::move(frame),
          channel,budget,stop,ready,route.pins);
    } else {
      result=WorkerExchangeSession::run_sized(binding.helper,dispatch.arguments,std::move(frame),
          channel,budget,stop,route.pins);
    }
    trial.exchange=result.code;
    trial.worker_exit=result.process.exit_code;
    trial.worker_retired=result.process.exit_confirmed && WorkerExchangeSession::available();
    if(result.code!=WorkerExchangeCode::completed || !trial.worker_retired || stop.stop_requested())return trial;
    const auto candidates=accept_uniform_lz4_reply(result.response,batch,id,WorkerExecution::hip);
    const auto after=sampler.sample();
    if(after.status!=WindowsGpuBudgetStatus::success ||
        after.snapshot.captured_ms<=before.snapshot.captured_ms ||
        check_gpu_memory_transition(before.snapshot,after.snapshot,route.memory.luid,
            route.memory.node_mask,GetTickCount64(),plan.device_peak_bytes)!=GpuGateCode::admitted)return trial;
    trial.exchange_ns=elapsed_ns(started);
    if(!trial.exchange_ns)return trial;

    std::vector<NativeLz4ExtentCandidate> advice;
    advice.reserve(candidates.sizes.size());
    std::size_t offset=0;
    for(std::size_t index=0;index<candidates.sizes.size();++index) {
      const auto bytes=candidates.sizes[index];
      if(bytes)advice.push_back({index,std::span(candidates.payload).subspan(offset,bytes)});
      offset+=bytes;
    }
    // The worker has retired. The source and accepted reply are still owned
    // while the CPU writer and reopen consume the private image grant.
    const auto retained=2ULL*source.size()+result.response.size()+candidates.payload.size();
    if(retained>=allowance)return trial;
    const auto interrupted=[&] {return stop.stop_requested();};
    auto sample=record_private_write(shape,source,advice,allowance-retained,existing,interrupted);
    if(!sample || stop.stop_requested() ||
        !uniform_lz4::add(sample->elapsed_ns,trial.exchange_ns,sample->elapsed_ns))return trial;
    sample->device_executed=true;
    trial.sample=sample;
  }catch(...) {}
  return trial;
}

std::optional<NativeDeferredWriteOptions> qualify_native_amd_writes(const HipDiscoveredRoute& route,
    const NativeAmdWriteEvidence& evidence,const GpuLz4ResidentCost& cost,GpuLz4WindowPolicy window,
    ProcessBudget budget,std::uint32_t deadline,std::optional<NativeUniformSweepGrant> sweep) {
  if(evaluate_native_amd_writes(evidence)!=NativeAmdWritePreference::amd ||
      evidence.configuration!=route.identity || cost.configuration!=route.identity ||
      !cost.qualified || cost.recovery_generation || !route.pins ||
      !route.pins->uniform_module_pinned() || route.ordinal>=64 ||
      !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) ||
      !valid_process_budget(budget) || !deadline || deadline>budget.timeout_ms)
    return std::nullopt;
  NativeDeferredWriteOptions result;
  result.binding.helper=route.helper;
  result.binding.configuration=route.identity;
  result.binding.ordinal=static_cast<int>(route.ordinal);
  result.binding.runtime_sha256=route.runtime_sha256;
  result.binding.sweep=sweep;
  result.controller_health=GpuHealthEvidence{route.identity,1,0,true,true,route.memory};
  result.controller_route_pins=route.pins;
  result.measured_shape=evidence.shape;
  result.cost=cost;
  result.window=window;
  result.budget=budget;
  result.deadline_ms=deadline;
  result.allow_gpu=result.workload_qualified=result.helper_independent=true;
  if(!NativeDeferredWriteService::measured_write_funded(result,*result.controller_health,route.memory.captured_ms))
    return std::nullopt;
  return result;
}
}
