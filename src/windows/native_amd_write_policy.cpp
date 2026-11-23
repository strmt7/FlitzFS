#include "native_amd_write_policy.hpp"
#include "memory_volume.hpp"
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

std::optional<NativeAmdWriteSample> record_native_amd_cpu_trial(const NativeAmdWriteShape& shape,
    std::span<const std::byte> source,std::uint64_t allowance,MemoryVolumeDomain* existing,
    const std::function<bool()>& interrupted) noexcept {
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
        const auto started=std::chrono::steady_clock::now();
        const auto written=writer.write_inode_storage_result(inode,0,source);
        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-started).count();
        if(elapsed<=0 || written.bytes!=source.size() || read_scope.interrupted())return {};
        sample.elapsed_ns=static_cast<std::uint64_t>(elapsed);
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
