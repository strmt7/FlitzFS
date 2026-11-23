#include "native_uniform_write.hpp"
#include "native_windows_boundary.hpp"
#include "../record_digest.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace flitzfs {
namespace {
void admit_source_copy(std::size_t bytes){
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory))throw std::bad_alloc();
  const auto physical_reserve=(std::max)(256ULL*1024*1024,memory.ullTotalPhys/16);
  const auto commit_reserve=(std::max)(256ULL*1024*1024,memory.ullTotalPageFile/16);
  const auto required=std::uint64_t(bytes)+65536;
  if(memory.ullAvailPhys<=physical_reserve || memory.ullAvailPageFile<=commit_reserve ||
      required>(memory.ullAvailPhys-physical_reserve)/16 ||
      required>(memory.ullAvailPageFile-commit_reserve)/16)throw std::bad_alloc();
}
template<std::size_t N>std::wstring hex(const std::array<std::byte,N>& bytes){
  constexpr wchar_t digits[]=L"0123456789abcdef";std::wstring result;result.reserve(N*2);
  for(const auto b:bytes){const auto v=std::to_integer<unsigned>(b);result+=digits[v>>4];result+=digits[v&15];}
  return result;
}
}
std::vector<std::byte> own_native_uniform_source(std::span<const std::byte> bytes,NativeFileInfoAllocation mode){
  const auto limit=native_windows_write_limit(mode);
  if(!limit || bytes.size()>limit || (!bytes.empty()&&!bytes.data()))
    throw std::invalid_argument("native write source bound");
  admit_source_copy(bytes.size());
  return bytes.empty()?std::vector<std::byte>{}:std::vector<std::byte>(bytes.begin(),bytes.end());
}
GpuLz4ResidentCost charge_native_uniform_source(GpuLz4ResidentCost cost,std::uint64_t bytes) noexcept {
  constexpr auto overhead=sizeof(NativeUniformWriteResult)+63*sizeof(NativeLz4ExtentCandidate);
  std::uint64_t owned=0;
  if(!uniform_lz4::add(bytes,overhead,owned) || !uniform_lz4::add(cost.fixed_bytes,owned,cost.fixed_bytes))
    cost.qualified=false;
  return cost;
}
NativeUniformDispatch prepare_native_uniform_dispatch(const NativeUniformWorkerBinding& binding,
    const GpuHealthEvidence& evidence,const PreparedUniformLz4& batch,WorkerExecution expected,
    const GpuLz4SweepGrant* admitted_sweep){
  if(!batch.ready() || binding.configuration!=evidence.configuration ||
      (expected!=WorkerExecution::hip && expected!=WorkerExecution::cpu_semantics) ||
      (admitted_sweep && (!binding.sweep || admitted_sweep->maximum_chunks>binding.sweep->maximum_chunks ||
          admitted_sweep->maximum_device_bytes>binding.sweep->maximum_device_bytes)))
    throw std::invalid_argument("native uniform dispatch binding mismatch");
  const auto& l=batch.limits();
  NativeUniformDispatch result;result.quantum_chunks=l.chunks;result.windows=1;
  result.device_peak_bytes=l.device_bytes;
  if(binding.sweep){
    const auto& grant=admitted_sweep?*admitted_sweep:*binding.sweep;
    const auto plan=plan_uniform_lz4_sweep(batch,grant.maximum_chunks,grant.maximum_device_bytes);
    result.reused=true;result.quantum_chunks=plan.quantum_chunks;result.windows=plan.windows;
    result.device_peak_bytes=plan.device_peak_bytes;
  }
  if(expected==WorkerExecution::cpu_semantics){result.arguments=binding.cpu_fixture_arguments;return result;}
  if(binding.ordinal<0 || binding.ordinal>=64 ||
      std::none_of(binding.runtime_sha256.begin(),binding.runtime_sha256.end(),[](auto b){return b!=std::byte{0};}))
    throw std::invalid_argument("native uniform runtime unqualified");
  result.arguments={result.reused?L"--driver-uniform-lz4-sweep-adapter":L"--driver-uniform-lz4-adapter",
      std::to_wstring(binding.ordinal),hex(evidence.memory.luid),std::to_wstring(evidence.memory.node_mask),
      hex(binding.runtime_sha256),std::to_wstring(l.request_bytes),std::to_wstring(l.reply_bytes),
      std::to_wstring(result.quantum_chunks),std::to_wstring(result.device_peak_bytes),std::to_wstring(l.chunk_bytes)};
  return result;
}

static_assert(std::is_nothrow_copy_assignable_v<NativeFileHandle>);
static_assert(std::is_trivially_copyable_v<NativeUniformWriteResult>);
static_assert(std::is_nothrow_copy_constructible_v<std::optional<NativeUniformWriteResult>>);
ControlledNativeUniformWrite::ControlledNativeUniformWrite(NativeFileSession& s,WorkerExecution expected)
    :session_(s),expected_(expected),controller_(expected){}
NativeUniformWriteStart ControlledNativeUniformWrite::begin(const NativeFileHandle& handle,
    std::span<const std::byte> bytes,UINT64 offset,bool append,bool constrained,
    const NativeAutomaticMetadata& effect,NativeFileInfoAllocation mode,
    const NativeUniformWorkerBinding& binding,bool consent,bool worthwhile,
    const GpuHealthEvidence& evidence,std::uint64_t now,const GpuLz4ResidentCost& cost,
    GpuLz4WindowPolicy window,ProcessBudget budget,std::uint32_t deadline,
    std::vector<std::byte>* transferable_source) noexcept {
  NativeUniformWriteStart start;
  SourceGeometry geometry;
  start.admission=bind_source(handle,bytes,offset,append,constrained,effect,mode,
      binding.capture_advice_receipt,transferable_source,geometry);
  if(NT_SUCCESS(start.admission.status))
    start_advice(start,geometry,binding,consent,worthwhile,evidence,now,cost,window,budget,deadline);
  return start;
}
NativeWindowsIoOutcome ControlledNativeUniformWrite::bind_source(const NativeFileHandle& handle,
    std::span<const std::byte> bytes,UINT64 offset,bool append,bool constrained,
    const NativeAutomaticMetadata& effect,NativeFileInfoAllocation mode,bool capture_advice,
    std::vector<std::byte>* transferable_source,SourceGeometry& geometry) noexcept {
  if(pending_)return win_detail::failure(session_,STATUS_INVALID_DEVICE_STATE);
  completed_=false;result_={};
  const auto write_limit=native_windows_write_limit(mode);
  if(!write_limit || bytes.size()>write_limit || (!bytes.empty() && !bytes.data()) ||
      append!=(offset==UINT64_MAX) || !native_cleanup_metadata_valid(effect) ||
      (transferable_source && (transferable_source->size()!=bytes.size() ||
          transferable_source->data()!=bytes.data())))
    return win_detail::failure(session_,STATUS_INVALID_PARAMETER);

  return win_detail::boundary(session_,[&] {
    const auto before=session_.mutation_info(handle);FSP_FSCTL_FILE_INFO prepared{};
    if(!encode_native_file_info(before,mode,prepared))return win_detail::failure(session_,STATUS_INVALID_DEVICE_STATE);
    auto copied=transferable_source?std::vector<std::byte>{}:own_native_uniform_source(bytes,mode);
    auto& owned=transferable_source?*transferable_source:copied;
    const auto position=append?before.info.logical_bytes:offset;
    auto accepted=std::uint64_t(owned.size());
    if(constrained)accepted=position>=before.info.logical_bytes?0:(std::min)(accepted,before.info.logical_bytes-position);

    geometry={position,accepted};
    source_.swap(owned);handle_=handle;effect_=effect;mode_=mode;offset_=offset;first_extent_=0;
    append_=append;constrained_=constrained;submitted_=false;capture_advice_=capture_advice;
    pending_=true;
    return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
}
void ControlledNativeUniformWrite::start_advice(NativeUniformWriteStart& start,SourceGeometry geometry,
    const NativeUniformWorkerBinding& binding,bool consent,bool worthwhile,
    const GpuHealthEvidence& evidence,std::uint64_t now,const GpuLz4ResidentCost& cost,
    GpuLz4WindowPolicy window,ProcessBudget budget,std::uint32_t deadline,bool require_full_window,
    const std::atomic_bool* force_cpu,std::shared_ptr<const void> pinned_route) noexcept {
  const auto yielding=[force_cpu] {return force_cpu && force_cpu->load(std::memory_order_acquire);};
  if(!consent || yielding()) {
    start.worker.decision.route.reason=GpuRouteReason::disabled;
    return;
  }
  start.worker.decision.route.reason=GpuRouteReason::unqualified;
  if(!worthwhile)return;
  const auto [position,accepted]=geometry;
  const auto maximum=native_windows_eof_limit(mode_);
  if(position>maximum || accepted>maximum-position)return;

  // Advice covers complete interior extents only. Prefix and tail retain CPU encoding.
  const auto gap=(kNativeExtentBytes-position%kNativeExtentBytes)%kNativeExtentBytes;
  if(gap>accepted)return;
  const auto eligible=(accepted-gap)/kNativeExtentBytes*kNativeExtentBytes;
  if(!eligible)return;
  try {
    const auto charged=charge_native_uniform_source(cost,source_.size());
    const auto plan=binding.sweep?plan_gpu_lz4_sweep_window(eligible,kNativeExtentBytes,evidence,now,
        charged,window,*binding.sweep,deadline):
        plan_gpu_lz4_window(eligible,kNativeExtentBytes,evidence,now,charged,window);
    if(binding.sweep && plan.code==GpuGateCode::invalid_budget) {
      start.worker.decision.route.reason=GpuRouteReason::bad_evidence;
      start.worker.decision.gate=plan.code;
    }
    if(plan.code!=GpuGateCode::admitted || plan.layout.input_bytes%kNativeExtentBytes ||
        (require_full_window && plan.layout.input_bytes!=source_.size()))return;
    const auto limits=uniform_lz4_limits(plan.layout);
    auto batch=PreparedUniformLz4::prepare(std::span<const std::byte>(source_).subspan(
        static_cast<std::size_t>(gap),static_cast<std::size_t>(limits.input_bytes)),limits);
    std::optional<GpuLz4SweepGrant> sweep;
    if(binding.sweep)sweep=GpuLz4SweepGrant{plan.quantum.chunks,plan.quantum.total_bytes};
    const auto dispatch=prepare_native_uniform_dispatch(binding,evidence,batch,expected_,sweep?&*sweep:nullptr);
    if(yielding())return;
    start.worker=controller_.begin(std::move(batch),binding.helper,dispatch.arguments,consent,worthwhile,
        evidence,now,charged,window,budget,deadline,sweep,std::move(pinned_route));
    first_extent_=(position+gap)/kNativeExtentBytes;
    start.eligible_bytes=static_cast<std::uint32_t>(plan.layout.input_bytes);
    start.quantum_chunks=dispatch.quantum_chunks;start.planned_windows=dispatch.windows;
    start.device_peak_bytes=dispatch.device_peak_bytes;start.reused=dispatch.reused;
    submitted_=start.worker.decision.submitted;
  }catch(const std::bad_alloc&) {
    start.worker.decision.route.reason=GpuRouteReason::memory_pressure;
  }catch(...) {
    start.worker.decision.route.reason=GpuRouteReason::bad_evidence;
  }
}
bool ControlledNativeUniformWrite::try_retire() noexcept {
  if(!controller_.try_retire())return false;
  if(completed_) {
    std::vector<std::byte>().swap(source_);
    controller_.release_retired_buffers();
  }
  return true;
}
GpuWorkerState ControlledNativeUniformWrite::poll(const GpuMemorySnapshot& after,std::uint64_t now) noexcept{
  return pending_ && submitted_?controller_.poll(after,now):GpuWorkerState::cpu_selected;
}
std::span<const std::byte> ControlledNativeUniformWrite::use_cpu_fallback() noexcept{
  if(!pending_)return {};
  if(submitted_)(void)controller_.use_cpu_fallback();return source_;
}
std::optional<NativeUniformWriteResult> ControlledNativeUniformWrite::finish() noexcept{
  if(completed_)return result_;if(!pending_)return {};
  if(submitted_ && controller_.state()==GpuWorkerState::waiting)return {};
  std::array<NativeLz4ExtentCandidate,63> hints{};std::size_t count=0;
  if(submitted_ && controller_.state()==GpuWorkerState::worker_selected){
    for(std::uint32_t i=0;i<controller_.count() && count<hints.size();++i){
      const auto candidate=controller_.candidate(i);if(candidate.empty())continue;
      hints[count]={first_extent_+i,candidate};
      if(capture_advice_){
        const auto digest=detail::record_digest(candidate);
        auto& receipt=result_.advice;
        if(!receipt.captured){
          receipt.payload_bytes=static_cast<std::uint32_t>(candidate.size());
          receipt.payload_digest=digest;receipt.captured=true;receipt.uniform_payloads=true;
        }else if(receipt.payload_bytes!=candidate.size()||receipt.payload_digest!=digest)
          receipt.uniform_payloads=false;
      }
      ++count;result_.candidate_bytes+=static_cast<std::uint32_t>(candidate.size());
    }
  }
  result_.candidate_count=static_cast<std::uint32_t>(count);
  if(capture_advice_ && submitted_)result_.worker=controller_.outcome();
  result_.outcome=dispatch_native_write_with_lz4(session_,handle_,source_.data(),offset_,
      static_cast<ULONG>(source_.size()),append_,constrained_,std::span(hints).first(count),effect_,mode_,
      &result_.transferred,&result_.info,&result_.stored_lz4_candidates);
  // Retain all owned buffers. Publication is followed only by trivial state
  // copies; no query, allocation, process operation or context release follows.
  pending_=false;completed_=true;return result_;
}
}
