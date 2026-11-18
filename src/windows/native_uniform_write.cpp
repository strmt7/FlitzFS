#include "native_uniform_write.hpp"
#include "native_windows_boundary.hpp"
#include "../record_digest.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
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
NativeUniformDispatch prepare_native_uniform_dispatch(const NativeUniformWorkerBinding& binding,
    const GpuHealthEvidence& evidence,const PreparedUniformLz4& batch,WorkerExecution expected){
  if(!batch.ready() || binding.configuration!=evidence.configuration ||
      (expected!=WorkerExecution::hip && expected!=WorkerExecution::cpu_semantics))
    throw std::invalid_argument("native uniform dispatch binding mismatch");
  const auto& l=batch.limits();
  NativeUniformDispatch result;result.quantum_chunks=l.chunks;result.windows=1;
  result.device_peak_bytes=l.device_bytes;
  if(binding.sweep){
    const auto plan=plan_uniform_lz4_sweep(batch,binding.sweep->maximum_chunks,binding.sweep->maximum_device_bytes);
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
    std::vector<std::byte>* transferable_source) noexcept{
  NativeUniformWriteStart start;
  if(pending_){start.admission=win_detail::failure(session_,STATUS_INVALID_DEVICE_STATE);return start;}
  completed_=false;result_={};
  const auto write_limit=native_windows_write_limit(mode);
  if(!write_limit || bytes.size()>write_limit || (!bytes.empty() && !bytes.data()) ||
      append!=(offset==UINT64_MAX) || !native_cleanup_metadata_valid(effect) ||
      (transferable_source && (transferable_source->size()!=bytes.size() ||
          transferable_source->data()!=bytes.data()))){
    start.admission=win_detail::failure(session_,STATUS_INVALID_PARAMETER);return start;
  }
  start.admission=win_detail::boundary(session_,[&]{
    const auto before=session_.mutation_info(handle);FSP_FSCTL_FILE_INFO prepared{};
    if(!encode_native_file_info(before,mode,prepared))return win_detail::failure(session_,STATUS_INVALID_DEVICE_STATE);
    auto copied=transferable_source?std::vector<std::byte>{}:own_native_uniform_source(bytes,mode);
    auto& owned=transferable_source?*transferable_source:copied;
    const auto position=append?before.info.logical_bytes:offset;
    auto accepted=std::uint64_t(owned.size());
    if(constrained)accepted=position>=before.info.logical_bytes?0:(std::min)(accepted,before.info.logical_bytes-position);
    start.worker.decision.route.reason=consent?GpuRouteReason::unqualified:GpuRouteReason::disabled;
    std::uint64_t first=0;
    // GPU advice covers every complete interior extent that the CURRENT
    // resource/time plan admits. Prefix/tail and other bytes retain CPU encoding.
    const auto maximum=native_windows_eof_limit(mode);
    if(consent && worthwhile && position<=maximum && accepted<=maximum-position){
      const auto gap=(kNativeExtentBytes-position%kNativeExtentBytes)%kNativeExtentBytes;
      if(gap<=accepted){
        const auto eligible=(accepted-gap)/kNativeExtentBytes*kNativeExtentBytes;
        if(eligible){
          try{
            auto charged=cost;
            if(!uniform_lz4::add(charged.fixed_bytes,owned.size()+sizeof(NativeUniformWriteResult)+
                  63*sizeof(NativeLz4ExtentCandidate),charged.fixed_bytes))
              throw std::invalid_argument("native uniform source accounting overflow");
            const auto plan=plan_gpu_lz4_window(eligible,kNativeExtentBytes,evidence,now,charged,window);
            if(plan.code==GpuGateCode::admitted && plan.layout.input_bytes%kNativeExtentBytes==0){
              const auto l=uniform_lz4_limits(plan.layout);
              auto batch=PreparedUniformLz4::prepare(std::span<const std::byte>(owned).subspan(
                  static_cast<std::size_t>(gap),static_cast<std::size_t>(l.input_bytes)),l);
              const auto dispatch=prepare_native_uniform_dispatch(binding,evidence,batch,expected_);
              start.worker=controller_.begin(std::move(batch),binding.helper,dispatch.arguments,consent,worthwhile,
                  evidence,now,charged,window,budget,deadline);
              first=(position+gap)/kNativeExtentBytes;
              start.eligible_bytes=static_cast<std::uint32_t>(plan.layout.input_bytes);
              start.quantum_chunks=dispatch.quantum_chunks;start.planned_windows=dispatch.windows;
              start.device_peak_bytes=dispatch.device_peak_bytes;start.reused=dispatch.reused;
            }
          }catch(const std::bad_alloc&){start.worker.decision.route.reason=GpuRouteReason::memory_pressure;}
          catch(...){start.worker.decision.route.reason=GpuRouteReason::bad_evidence;}
        }
      }
    }
    // No fallible operation after possible worker launch. GPU preparation
    // failures never fence the healthy storage session or acknowledge a write.
    source_.swap(owned);handle_=handle;effect_=effect;mode_=mode;offset_=offset;first_extent_=first;
    append_=append;constrained_=constrained;submitted_=start.worker.decision.submitted;
    capture_advice_=binding.capture_advice_receipt;
    pending_=true;completed_=false;result_={};return NativeWindowsIoOutcome{STATUS_SUCCESS,false};
  });
  return start;
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
  result_.outcome=dispatch_native_write_with_lz4(session_,handle_,source_.data(),offset_,
      static_cast<ULONG>(source_.size()),append_,constrained_,std::span(hints).first(count),effect_,mode_,
      &result_.transferred,&result_.info);
  // Retain all owned buffers. Publication is followed only by trivial state
  // copies; no query, allocation, process operation or context release follows.
  pending_=false;completed_=true;return result_;
}
}
