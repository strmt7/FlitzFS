#include "native_amd_write_policy.hpp"
#include "memory_volume.hpp"
#include "gpu_budget.hpp"
#include "../gpu_request_id.hpp"
#include "../native_capture.hpp"
#include "../record_digest.hpp"
#include "../store_codec.hpp"
#include "../store_read_scope.hpp"
#include "flitzfs/paged_store.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <string>

namespace flitzfs {
namespace {
constexpr auto indexed_file_info=NativeFileInfoAllocation::indexed_reserved_cache;
bool valid_shape(const NativeAmdWriteShape& shape) noexcept {
  const auto& storage=shape.storage;
  const auto& layout=storage.layout;
  const auto& compression=storage.compression;
  return native_amd_write_geometry(indexed_file_info,0,shape.input_bytes,false,false) &&
      valid_store_io_contract({layout.isolation_unit,true}) &&
      layout.isolation_unit>=detail::kPagedStoreCheckpointWriteBytes/2 &&
      (!layout.compact_roots || layout.isolation_unit<kStoreCommitUnit) &&
      (storage.data_layout==NativeIndexedDataLayout::extents ||
       storage.data_layout==NativeIndexedDataLayout::inline_small_files) &&
      (compression.mode==StoreCompressionMode::fast || compression.mode==StoreCompressionMode::automatic) &&
      compression.zstd_level>=1 && compression.zstd_level<=9;
}
bool verified(const NativeAmdWriteSample& sample,bool amd) noexcept {
  return sample.transactions==1 && sample.elapsed_ns && sample.occupied_bytes &&
      sample.input_digest!=decltype(sample.input_digest){} &&
      sample.cpu_readback_verified && sample.retirement_confirmed && sample.device_executed==amd &&
      sample.stored_lz4_without_cpu_comparison<=sample.stored_lz4_candidates &&
      (amd || (!sample.stored_lz4_candidates && !sample.prepared_before_write));
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
        trial.cpu.shape!=shape || trial.amd.shape!=shape ||
        trial.amd.prepared_before_write!=evidence.prepared_before_write ||
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
std::optional<uniform_lz4::Layout> replay_layout(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests) noexcept {
  std::uint64_t last=0,total=0;
  for(const auto& request:requests) {
    const auto bytes=request.source.size();
    if(request.request_id<=last || bytes>kStoreMaxTransactionBytes ||
        !valid_shape({static_cast<std::uint32_t>(bytes),storage}) ||
        !uniform_lz4::add(total,bytes,total))return {};
    last=request.request_id;
  }
  uniform_lz4::Layout layout{};
  UniformLz4Limits limits{};
  if(!uniform_lz4::make_layout(total,kNativeExtentBytes,layout) || !try_uniform_lz4_limits(layout,limits))return {};
  return layout;
}

detail::Digest replay_digest(std::span<const UniformLz4BatchInput> requests) noexcept {
  if(requests.size()==1)return detail::record_digest(requests.front().source);
  detail::Digest digest{};
  for(const auto& request:requests) {
    const std::array<std::uint64_t,2> boundary{request.request_id,request.source.size()};
    const auto member=detail::record_digest(std::as_bytes(std::span(boundary)),request.source);
    digest=detail::record_digest(digest,member);
  }
  return digest;
}

std::string replay_name(std::size_t member,std::size_t count) {
  return count==1?"qualification":"qualification-"+std::to_string(member);
}

std::optional<NativeAmdWriteSample> record_private_writes(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests,const uniform_lz4::Layout& layout,
    const VerifiedUniformLz4Batch* verified,std::uint64_t allowance,MemoryVolumeDomain* existing,
    const std::function<bool()>& interrupted) {
  detail::StoreCaptureReadScope read_scope(interrupted);
  // Charge retained source, staging and decoder before the demand-committed
  // provider. Layout and member count are bounded by the exact wire geometry.
  const auto scratch=4ULL*layout.input_bytes+detail::kStoreDecoderMemoryCap+4ULL*kStoreCommitUnit;
  if(!native_amd_cpu_replay_fits(layout.input_bytes,requests.size(),allowance) || read_scope.interrupted())return {};
  auto capacity=allowance-scratch;
  if(existing) {
    const auto used=existing->usage();
    if(used.reserved>=used.capacity)return {};
    capacity=(std::min)(capacity,used.capacity-used.reserved);
  }
  capacity-=capacity%storage.layout.isolation_unit;
  if(capacity<2ULL*layout.input_bytes+(2ULL+2ULL*requests.size())*kStoreCommitUnit)return {};
  NativeAmdWriteSample sample;
  sample.transactions=requests.size();
  sample.shape={static_cast<std::uint32_t>(layout.input_bytes),storage};
  {
    std::optional<MemoryVolumeDomain> owned;
    if(!existing)owned.emplace(MemoryVolumeDomain::create(capacity));
    auto& domain=existing?*existing:*owned;
    auto image=domain.create_store(capacity,storage.layout.isolation_unit);
    if(image->contract().write_failure_unit_bytes!=storage.layout.isolation_unit || read_scope.interrupted())return {};
    StoreIdentity id{};
    id[0]=std::byte{1}; // Private image with no externally addressable identity.
    {
      auto writer=NativeFilesystemWriter::initialize_indexed(PagedObjectStoreWriter::create_mirrored(
          image,id,{capacity},storage.compression,storage.layout),storage.data_layout);
      std::vector<NativeLz4ExtentCandidate> advice;
      for(std::size_t index=0;index<requests.size();++index) {
        if(read_scope.interrupted())return {};
        const auto source=requests[index].source;
        NativeFileMutation seed;
        seed.path=replay_name(index,requests.size());
        writer.apply(std::span{&seed,1});
        const auto inode=writer.snapshot().stat(seed.path).inode;
        advice.clear();
        if(verified) {
          const auto reply=verified->reply(index);
          std::size_t offset=0;
          advice.reserve(reply.sizes.size());
          for(std::size_t chunk=0;chunk<reply.sizes.size();++chunk) {
            const auto bytes=reply.sizes[chunk];
            if(bytes)advice.push_back({chunk,reply.payload.subspan(offset,bytes)});
            offset+=bytes;
          }
        }
        if(read_scope.interrupted())return {};
        const auto started=TrialClock::now();
        const auto written=writer.write_inode_storage_with_lz4(inode,0,source,advice);
        const auto elapsed=elapsed_ns(started);
        if(!elapsed || written.bytes!=source.size() ||
            !uniform_lz4::add(sample.elapsed_ns,elapsed,sample.elapsed_ns))return {};
        sample.stored_lz4_candidates+=written.transaction.storage.stored_lz4_candidates;
        sample.stored_lz4_without_cpu_comparison+=written.transaction.storage.stored_lz4_without_cpu_comparison;
      }
    }
    sample.occupied_bytes=image->extent();
    {
      if(read_scope.interrupted())return {};
      const auto recovered=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,capacity});
      recovered.verify_all();
      for(std::size_t index=0;index<requests.size();++index) {
        const auto name=replay_name(index,requests.size());
        const auto source=requests[index].source;
        if(read_scope.interrupted() || recovered.stat(name).logical_bytes!=source.size())return {};
        for(std::size_t offset=0;offset<source.size();offset+=kNativeExtentBytes) {
          if(read_scope.interrupted())return {};
          const auto bytes=recovered.read(name,offset,kNativeExtentBytes);
          if(bytes.size()!=kNativeExtentBytes || !std::ranges::equal(bytes,source.subspan(offset,kNativeExtentBytes)))return {};
        }
      }
    }
    sample.input_digest=replay_digest(requests);
    sample.cpu_readback_verified=true;
  }
  if(!MemoryVolumeDomain::cleanup_healthy() || read_scope.interrupted())return {};
  sample.retirement_confirmed=true;
  return sample;
}
}

bool native_amd_cpu_replay_fits(std::uint64_t bytes,std::size_t writes,std::uint64_t allowance) noexcept {
  std::uint64_t data=0,reservations=0;
  return bytes && writes && uniform_lz4::multiply(bytes,6,data) &&
      uniform_lz4::multiply(writes,2ULL*kStoreCommitUnit,reservations) &&
      uniform_lz4::add(data,reservations,data) &&
      uniform_lz4::add(data,detail::kStoreDecoderMemoryCap+6ULL*kStoreCommitUnit,data) && data<=allowance;
}

std::optional<NativeAmdWriteSample> record_native_amd_cpu_batch(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests,std::uint64_t allowance,MemoryVolumeDomain* existing,
    const std::function<bool()>& interrupted) noexcept {
  try {
    const auto layout=replay_layout(storage,requests);
    if(layout)return record_private_writes(storage,requests,*layout,nullptr,allowance,existing,interrupted);
  }catch(...) {}
  return {};
}

std::optional<NativeAmdWriteSample> record_native_amd_cpu_trial(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,std::uint64_t allowance,MemoryVolumeDomain* existing,
    const std::function<bool()>& interrupted) noexcept {
  if(source.size()!=shape.input_bytes)return {};
  const UniformLz4BatchInput request{1,source};
  return record_native_amd_cpu_batch(shape.storage,std::span{&request,1},allowance,existing,interrupted);
}

std::optional<NativeAmdWriteSample> record_native_amd_verified_batch(const NativeManagedForeground& storage,
    const PreparedUniformLz4Batch& batch,const VerifiedUniformLz4Batch& verified,std::uint64_t allowance,
    MemoryVolumeDomain* existing,const std::function<bool()>& interrupted) noexcept {
  try {
    if(batch.source().layout().chunk_bytes!=kNativeExtentBytes || batch.source().layout().input_bytes>allowance ||
        !verified.matches(batch) || interrupted())return {};
    std::vector<UniformLz4BatchInput> requests;
    requests.reserve(batch.members().size());
    std::uint64_t retained=0;
    for(std::size_t index=0;index<batch.members().size();++index) {
      requests.push_back({batch.members()[index].request_id,batch.input(index)});
      if(!uniform_lz4::add(retained,verified.reply(index).payload.size(),retained))return {};
    }
    const auto layout=replay_layout(storage,requests);
    if(layout && retained<allowance)
      return record_private_writes(storage,requests,*layout,&verified,allowance-retained,existing,interrupted);
  }catch(...) {}
  return {};
}

bool NativeAmdWriteWorkerPlan::fits(ProcessBudget budget,std::uint64_t allowance) const noexcept {
  return allowance>=parent_metadata_bytes && valid_worker_channel_capacity(channel,budget) &&
      uniform_worker_host_budget_covers(host_limits,budget,allowance-parent_metadata_bytes);
}

namespace {
std::optional<NativeAmdWriteWorkerPlan> make_native_amd_worker_plan(const uniform_lz4::Layout& group,
    const uniform_lz4::Layout& work,std::uint32_t requests,std::size_t metadata_per_request,
    ProcessBudget budget,std::uint64_t allowance) noexcept {
  NativeAmdWriteWorkerPlan result;
  result.group=group;
  result.work=work;
  result.maximum_requests=requests;
  if(!try_uniform_lz4_limits(group,result.host_limits))return {};
  auto request_bytes=result.host_limits.request_bytes;
  if(requests) {
    UniformLz4Limits member{};
    std::uint64_t framed_reply=0;
    if(!try_uniform_lz4_limits(work,member) ||
        !uniform_lz4::add(member.reply_bytes,sizeof(std::uint32_t),framed_reply) ||
        !uniform_lz4::multiply(framed_reply,requests,result.host_limits.reply_bytes) ||
        !uniform_lz4::multiply(requests,metadata_per_request,result.parent_metadata_bytes) ||
        result.host_limits.reply_bytes>static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()))return {};
    request_bytes=member.request_bytes;
  }
  if(request_bytes>(std::numeric_limits<std::size_t>::max)() ||
      result.host_limits.reply_bytes>(std::numeric_limits<std::size_t>::max)())return {};
  result.channel={static_cast<std::size_t>(request_bytes),static_cast<std::size_t>(result.host_limits.reply_bytes)};
  if(!result.fits(budget,allowance))return {};
  return result;
}
}

std::optional<NativeAmdWriteWorkerPlan> plan_native_amd_write_worker(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests,NativeAmdTrialLaunch launch,ProcessBudget budget,
    std::uint64_t allowance) noexcept {
  const auto group=replay_layout(storage,requests);
  if(!group)return {};
  if(launch!=NativeAmdTrialLaunch::prepared_requests)
    return make_native_amd_worker_plan(*group,*group,0,0,budget,allowance);
  if(requests.size()<2 || requests.size()>(std::numeric_limits<std::uint32_t>::max)())return {};
  const auto bytes=requests.front().source.size();
  uniform_lz4::Layout work{};
  if(!std::ranges::all_of(requests,[bytes](const auto& item){return item.source.size()==bytes;}) ||
      !uniform_lz4::make_layout(bytes,group->chunk_bytes,work))return {};
  return make_native_amd_worker_plan(*group,work,static_cast<std::uint32_t>(requests.size()),
      sizeof(UniformLz4Bytes),budget,allowance);
}

std::optional<NativeAmdWriteWorkerPlan> plan_native_amd_write_burst(const NativeManagedForeground& storage,
    std::uint32_t request_bytes,std::uint32_t maximum_requests,ProcessBudget budget,
    std::uint64_t allowance) noexcept {
  if(maximum_requests<2 || !valid_shape({request_bytes,storage}))return {};
  std::uint64_t bytes=0;
  uniform_lz4::Layout group{},work{};
  if(!uniform_lz4::multiply(request_bytes,maximum_requests,bytes) ||
      !uniform_lz4::make_layout(bytes,kNativeExtentBytes,group) ||
      !uniform_lz4::make_layout(request_bytes,kNativeExtentBytes,work))return {};
  constexpr auto metadata=sizeof(UniformLz4Bytes)+sizeof(UniformLz4BatchInput)+sizeof(UniformLz4Request);
  const auto plan=make_native_amd_worker_plan(group,work,maximum_requests,metadata,budget,allowance);
  if(!plan || plan->channel.reply_bytes>allowance-plan->parent_metadata_bytes ||
      !native_amd_cpu_replay_fits(bytes,maximum_requests,allowance-plan->parent_metadata_bytes-plan->channel.reply_bytes))return {};
  return plan;
}

NativeAmdDeviceTrial record_native_amd_device_trial(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,const HipDiscoveredRoute& route,NativeUniformSweepGrant grant,
    ProcessBudget budget,std::uint64_t allowance,MemoryVolumeDomain* existing,NativeAmdTrialLaunch launch,
    std::stop_token stop) noexcept {
  if(source.size()!=shape.input_bytes)return {};
  const UniformLz4BatchInput request{1,source};
  return record_native_amd_device_batch(shape.storage,std::span{&request,1},
      route,grant,budget,allowance,existing,launch,stop);
}

namespace {
NativeAmdDeviceTrial run_native_amd_write_trial(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests,const HipDiscoveredRoute& route,NativeUniformSweepGrant grant,
    ProcessBudget budget,std::uint64_t allowance,MemoryVolumeDomain* existing,NativeAmdTrialLaunch launch,
    std::stop_token stop,const std::function<bool()>& may_submit,std::uint32_t burst_limit=0,
    const NativeAmdWriteContinuation& continuation={},bool allow_smaller_writes=false) noexcept {
  NativeAmdDeviceTrial trial;
  try {
    if(stop.stop_requested() || !gpu_lz4_cpu_verification_available())return trial;
    const auto started=TrialClock::now();
    const bool finishable=burst_limit!=0;
    const auto worker=finishable?plan_native_amd_write_burst(storage,
        static_cast<std::uint32_t>(requests.front().source.size()),burst_limit,budget,allowance):
        plan_native_amd_write_worker(storage,requests,launch,budget,allowance);
    if(!worker || !route.pins ||
        route.identity==GpuConfigurationId{} || route.ordinal>=64 || !route.pins->uniform_module_pinned() ||
        !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) ||
        !WorkerExchangeSession::available())return trial;
    const bool repeated=launch==NativeAmdTrialLaunch::prepared_requests;
    const auto channel=worker->channel;
    const auto plan=plan_uniform_lz4_sweep(worker->work,grant.maximum_chunks,grant.maximum_device_bytes);
    if(stop.stop_requested() || (may_submit && !may_submit()) || !route.pins->device_ready())return trial;
    WindowsGpuBudgetSampler sampler(route.memory.luid,route.memory.node_mask);
    const auto before=sampler.sample();
    if(before.status!=WindowsGpuBudgetStatus::success ||
        check_gpu_memory_budget(before.snapshot,route.memory.luid,route.memory.node_mask,
            GetTickCount64(),plan.device_peak_bytes)!=GpuGateCode::admitted)return trial;

    const auto host_grant=[&](const GpuMemorySnapshot& memory) noexcept {
      try {
        const auto usable=usable_memory_volume_headroom({memory.physical_total,memory.physical_available,
            memory.commit_limit,memory.commit_available});
        return (std::min)(allowance,usable/sized_worker_memory_share_divisor(budget));
      }catch(...) {return std::uint64_t{0};}
    };
    const auto funded=host_grant(before.snapshot);
    if(!worker->fits(budget,funded))return trial;

    const auto id=detail::next_gpu_request_id();
    if(!id || stop.stop_requested())return trial;
    const GpuHealthEvidence evidence{route.identity,id,0,true,true,before.snapshot};
    const NativeUniformWorkerBinding binding{.helper=route.helper,.configuration=route.identity,
        .ordinal=static_cast<int>(route.ordinal),.runtime_sha256=route.runtime_sha256,.sweep=grant};
    auto batch=PreparedUniformLz4Batch::prepare(requests,finishable?worker->work:worker->group);
    const auto run_request=[&](const PreparedUniformLz4& source,std::uint64_t request_id)->SharedWorkerRequest {
      return std::make_shared<const WorkerRequestSnapshot>(encode_uniform_lz4_request(source,request_id));
    };
    NativeUniformDispatch dispatch;
    SharedWorkerRequest frame;
    if(repeated) {
      const auto first=batch.prepare_member(0);
      dispatch=prepare_native_uniform_dispatch(binding,evidence,first,WorkerExecution::hip,nullptr,
          worker->maximum_requests);
      frame=run_request(first,batch.members().front().request_id);
    }else {
      dispatch=prepare_native_uniform_dispatch(binding,evidence,batch.source(),WorkerExecution::hip);
      frame=run_request(batch.source(),id);
    }
    WorkerExchangeResult result;
    if(launch==NativeAmdTrialLaunch::prepared)
      dispatch.arguments.front()=L"--driver-uniform-lz4-prepared-adapter";
    const auto setup=TrialClock::now();
    trial.preparation_ns=elapsed_ns(started);
    const auto permitted=[&] {
      if(stop.stop_requested())return false;
      const auto current=sampler.sample();
      return !stop.stop_requested() && (!may_submit || may_submit()) &&
          current.status==WindowsGpuBudgetStatus::success &&
          current.snapshot.captured_ms>before.snapshot.captured_ms &&
          check_gpu_memory_transition(before.snapshot,current.snapshot,route.memory.luid,
              route.memory.node_mask,GetTickCount64(),plan.device_peak_bytes)==GpuGateCode::admitted &&
          worker->fits(budget,host_grant(current.snapshot));
    };
    std::vector<UniformLz4Request> following;
    if(finishable) {
      following.reserve(worker->maximum_requests-1);
      const auto member=uniform_lz4_limits(worker->work);
      TrialClock::time_point supplied_at{};
      const auto decide=[&](std::uint32_t index)->PreparedRequestDecision {
        const auto elapsed=elapsed_ns(setup);
        const auto previous_service=index?elapsed_ns(supplied_at):0;
        if(!index)trial.ready_ns=elapsed;
        if(!permitted())return {};
        if(!index) {
          ++trial.admitted_requests;
          supplied_at=TrialClock::now();
          return {PreparedRequestAction::submit,std::move(frame)};
        }
        detail::NativeCapturedWrite captured;
        const auto action=continuation({index,trial.ready_ns,elapsed,previous_service},captured);
        if(stop.stop_requested())return {};
        if(action==PreparedRequestAction::finish) {
          if(captured.sequence || !captured.bytes.empty() || (may_submit && !may_submit()))return {};
          return {action,{}};
        }
        const auto last=following.empty()?batch.members().front().request_id:following.back().request_id;
        const auto bytes=captured.bytes.size();
        if(action!=PreparedRequestAction::submit || captured.sequence<=last ||
            bytes>member.input_bytes || (!allow_smaller_writes && bytes!=member.input_bytes) ||
            captured.bytes.capacity()>bytes ||
            !valid_shape({static_cast<std::uint32_t>(bytes),storage}) || !permitted())return {};
        uniform_lz4::Layout active{};
        if(!uniform_lz4::make_layout(bytes,member.chunk_bytes,active))return {};
        auto request=PreparedUniformLz4::prepare_owned(std::move(captured.bytes),uniform_lz4_limits(active));
        auto supplied=run_request(request,captured.sequence);
        following.push_back({captured.sequence,std::move(request)});
        ++trial.admitted_requests;
        supplied_at=TrialClock::now();
        return {action,std::move(supplied)};
      };
      result=WorkerExchangeSession::run_prepared_burst(binding.helper,dispatch.arguments,channel,budget,
          worker->maximum_requests,stop,decide,route.pins);
    }else if(repeated) {
      const auto prepare=[&](std::uint32_t index)->SharedWorkerRequest {
        if(!index)trial.ready_ns=elapsed_ns(setup);
        if(!permitted())return {};
        if(index)frame=run_request(batch.prepare_member(index),batch.members()[index].request_id);
        ++trial.admitted_requests;
        return std::move(frame);
      };
      result=WorkerExchangeSession::run_prepared_requests(binding.helper,dispatch.arguments,channel,budget,
          static_cast<std::uint32_t>(requests.size()),stop,prepare,route.pins);
    }else if(launch==NativeAmdTrialLaunch::prepared) {
      const auto ready=[&] {
        trial.ready_ns=elapsed_ns(setup);
        return permitted();
      };
      result=WorkerExchangeSession::run_prepared_sized(binding.helper,dispatch.arguments,std::move(frame),
          channel,budget,stop,ready,route.pins);
    } else {
      result=WorkerExchangeSession::run_sized(binding.helper,dispatch.arguments,std::move(frame),
          channel,budget,stop,route.pins);
    }
    trial.supervised_ns=elapsed_ns(setup);
    trial.exchange=result.code;
    trial.worker_exit=result.process.exit_code;
    trial.worker_retired=result.process.exit_confirmed && result.code!=WorkerExchangeCode::exit_unconfirmed &&
        result.code!=WorkerExchangeCode::supervisor_exit_unconfirmed;
    if(result.code!=WorkerExchangeCode::completed || !trial.worker_retired || stop.stop_requested())return trial;
    const auto verification=TrialClock::now();
    if(finishable) {
      if(result.completed_requests!=1+following.size() ||
          result.completed_requests!=trial.admitted_requests) {
        trial.exchange=WorkerExchangeCode::invalid_response;
        return trial;
      }
      if(!following.empty()) {
        std::vector<UniformLz4BatchInput> supplied;
        supplied.reserve(result.completed_requests);
        supplied.push_back({batch.members().front().request_id,batch.input(0)});
        for(const auto& item:following)supplied.push_back({item.request_id,item.batch.plaintext()});
        const auto layout=replay_layout(storage,supplied);
        if(!layout || layout->input_bytes>worker->group.input_bytes)return trial;
        batch=PreparedUniformLz4Batch::prepare(supplied,*layout);
      }
      std::vector<UniformLz4Request>().swap(following);
    }
    const auto candidates=[&] {
      if(!repeated)return VerifiedUniformLz4Batch::accept(result.response,batch,id,WorkerExecution::hip);
      auto all=std::span<const std::byte>(result.response);
      std::vector<UniformLz4Bytes> replies;
      const auto count=batch.members().size();
      replies.reserve(count);
      for(std::size_t i=0;i<count;++i)replies.push_back(take_prepared_reply(all));
      if(!all.empty())throw std::invalid_argument("Unexpected native write reply tail");
      return VerifiedUniformLz4Batch::accept_members(replies,batch,WorkerExecution::hip);
    }();
    std::vector<std::byte>().swap(result.response);
    const auto after=sampler.sample();
    if(after.status!=WindowsGpuBudgetStatus::success ||
        after.snapshot.captured_ms<=before.snapshot.captured_ms ||
        check_gpu_memory_transition(before.snapshot,after.snapshot,route.memory.luid,
            route.memory.node_mask,GetTickCount64(),plan.device_peak_bytes)!=GpuGateCode::admitted)return trial;
    trial.verification_ns=elapsed_ns(verification);
    trial.exchange_ns=elapsed_ns(started);
    const auto replay_funded=(std::min)(funded,host_grant(after.snapshot));
    if(!trial.exchange_ns || replay_funded<=worker->parent_metadata_bytes)return trial;

    const auto interrupted=[&] {return stop.stop_requested();};
    const auto replay=TrialClock::now();
    auto sample=record_native_amd_verified_batch(storage,batch,candidates,replay_funded-worker->parent_metadata_bytes,
        existing,interrupted);
    trial.replay_ns=elapsed_ns(replay);
    if(!sample || stop.stop_requested() ||
        !uniform_lz4::add(sample->elapsed_ns,trial.exchange_ns,sample->elapsed_ns))return trial;
    sample->device_executed=true;
    if(finishable) {
      const auto funded_replay=replay_funded-worker->parent_metadata_bytes;
      if(worker->channel.reply_bytes>=funded_replay)return trial;
      std::vector<UniformLz4BatchInput> supplied;
      supplied.reserve(batch.members().size());
      for(std::size_t index=0;index<batch.members().size();++index)
        supplied.push_back({batch.members()[index].request_id,batch.input(index)});
      const auto cpu_started=TrialClock::now();
      trial.matched_cpu=record_native_amd_cpu_batch(storage,supplied,funded_replay-worker->channel.reply_bytes,
          existing,interrupted);
      trial.cpu_wall_ns=elapsed_ns(cpu_started);
      if(!trial.matched_cpu || stop.stop_requested())return trial;
    }
    trial.verified_input_bytes=batch.source().layout().input_bytes;
    trial.sample=sample;
  }catch(...) {}
  return trial;
}
}

NativeAmdDeviceTrial record_native_amd_device_batch(const NativeManagedForeground& storage,
    std::span<const UniformLz4BatchInput> requests,const HipDiscoveredRoute& route,NativeUniformSweepGrant grant,
    ProcessBudget budget,std::uint64_t allowance,MemoryVolumeDomain* existing,NativeAmdTrialLaunch launch,
    std::stop_token stop,const std::function<bool()>& may_submit) noexcept {
  return run_native_amd_write_trial(storage,requests,route,grant,budget,allowance,existing,launch,stop,may_submit);
}

NativeAmdDeviceTrial record_native_amd_device_burst(const NativeManagedForeground& storage,UniformLz4BatchInput first,
    std::uint32_t maximum_requests,const HipDiscoveredRoute& route,NativeUniformSweepGrant grant,
    ProcessBudget budget,std::uint64_t allowance,MemoryVolumeDomain* existing,std::stop_token stop,
    const NativeAmdWriteContinuation& continuation,const std::function<bool()>& may_submit,bool allow_smaller_writes) noexcept {
  if(!first.request_id || first.source.size()>(std::numeric_limits<std::uint32_t>::max)() ||
      maximum_requests<2 || !continuation)return {};
  return run_native_amd_write_trial(storage,std::span{&first,1},route,grant,budget,allowance,existing,
      NativeAmdTrialLaunch::prepared_requests,stop,may_submit,maximum_requests,continuation,allow_smaller_writes);
}

std::optional<NativeDeferredWriteOptions> qualify_native_amd_writes(const HipDiscoveredRoute& route,
    const NativeAmdWriteEvidence& evidence,const GpuLz4ResidentCost& cost,GpuLz4WindowPolicy window,
    ProcessBudget budget,std::uint32_t deadline,std::optional<NativeUniformSweepGrant> sweep) {
  if(evaluate_native_amd_writes(evidence)!=NativeAmdWritePreference::amd ||
      evidence.configuration!=route.identity || cost.configuration!=route.identity ||
      !cost.qualified || cost.recovery_generation || !route.pins ||
      !route.pins->uniform_module_pinned() || route.ordinal>=64 ||
      !route.pins->matches(route.helper,route.identity,route.runtime_sha256,static_cast<int>(route.ordinal),route.memory) ||
      !valid_process_budget(budget) || !deadline || deadline>budget.timeout_ms ||
      (evidence.prepared_before_write && (!sweep || deadline>=budget.timeout_ms)))
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
  result.prepared_only=evidence.prepared_before_write;
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
