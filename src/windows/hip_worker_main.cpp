#include "hip_decode_worker.hpp"
#include "anchor_sweep_profile.hpp"
#include "anchor_batch_profile.hpp"
#include "flitzfs/gpu_lz4_verify.hpp"
#include "gpu_budget.hpp"
#include "hip_worker_exit.hpp"
#include "hip_api.hpp"
#include "hip_identity_wire.hpp"
#include "hip_worker_bundle.hpp"
#ifdef FLITZFS_HIP_LZ4_RUNTIME_FIXTURE
#include "hip_lz4_compute.hpp"
#include "hip_uniform_lz4_compute.hpp"
#include "uniform_lz4_profile.hpp"
#include "uniform_lz4_sweep_profile.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#include "hip_lz4_bundle.hpp"
#include "hip_uniform_lz4_bundle.hpp"
#endif
#ifdef FLITZFS_HIP_NATIVE_LZ4_READ_FIXTURE
#include "../native_lz4_decode_wire.hpp"
#include "hip_uniform_lz4_compute.hpp"
#endif
#include "hip_module_verification.hpp"
#include "worker_exchange.hpp"
#include "flitzfs/read_only_file.hpp"
#include <bcrypt.h>
#include <cstdint>
#include <algorithm>
#include <array>
#include <bit>
#include <iterator>
#include <new>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include <optional>
#include <cstdlib>

namespace flitzfs {
namespace {
namespace fs = std::filesystem;
using Exit = HipWorkerExit;

void require(bool condition, Exit reason, const char* message,
    HipWorkerStage stage = HipWorkerStage::none) {
  if (!condition) throw HipWorkerFailure(reason, message, stage);
}

using hip_detail::DirectoryPin;
using hip_detail::matches_sha256;

fs::path module_path(HMODULE handle) {
  std::array<wchar_t,32768> buffer{};
  const auto length = GetModuleFileNameW(handle, buffer.data(), static_cast<DWORD>(buffer.size()));
  require(length && length < buffer.size(), Exit::runtime_unavailable, "module path unavailable");
  return fs::path(std::wstring_view(buffer.data(), length));
}
bool same_path(const fs::path& left, const fs::path& right) {
  return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

class Bundle {
 public:
  explicit Bundle(bool kernel_only = false, bool compression_only = false, bool uniform_only=false)
      : root_(module_path(nullptr).parent_path() / (uniform_only ? L"hip-uniform-lz4" : compression_only ? L"hip-lz4" : L"hip")),
        kernel_only_(kernel_only),compression_only_(compression_only),uniform_only_(uniform_only) {
#ifndef FLITZFS_HIP_LZ4_RUNTIME_FIXTURE
    require(!compression_only_ && !uniform_only_,Exit::unsupported_configuration,"compression fixture not compiled");
#endif
    require(!(kernel_only_ && (compression_only_ || uniform_only_)) && !(compression_only_ && uniform_only_),
        Exit::invalid_request,"conflicting bundle scope");
    const auto drive = root_.root_name().native();
    require(root_.is_absolute() && drive.size() == 2 && drive[1] == L':',
        Exit::runtime_unavailable, "HIP bundle must be on a local drive path");
    auto parent = root_.root_path();
    directories_.emplace_back(parent);
    for (const auto& component : root_.relative_path()) {
      require(component != L"." && component != L".." && directories_.size() < 32,
          Exit::runtime_unavailable, "HIP bundle path depth");
      parent /= component;
      directories_.emplace_back(parent);
    }
    const auto expected_count = kernel_only_ || compression_only_ || uniform_only_ ? std::size_t{1} : std::size(hip_bundle::files);
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(root_)) {
      ++count;
      bool known=false;
      entries([&](const auto& file) {
        known=known || (included(file.name) && same_path(entry.path().filename(),file.name));
      });
      require(count <= expected_count && known,
          Exit::integrity_failure, "unexpected HIP bundle entry");
    }
    require(count == expected_count, Exit::runtime_unavailable, "HIP bundle incomplete");
    files_.reserve(count);
    entries([&](const auto& entry) {
      if (!included(entry.name)) return;
      auto file = ReadOnlyFile::open(root_ / entry.name);
      require(file.size() == entry.size && matches_sha256(file, entry.sha256),
          Exit::integrity_failure, "HIP bundle file identity");
      files_.push_back(std::move(file));
    });
  }
  fs::path path(const wchar_t* name) const { return root_ / name; }
  void check_loaded_paths() const {
    entries([&](const auto& entry) {
      if (!included(entry.name)) return;
      if (const auto loaded = GetModuleHandleW(entry.name)) {
        require(same_path(path(entry.name), module_path(loaded)),
            Exit::integrity_failure, "different HIP dependency already loaded");
      }
    });
  }
 private:
  template<class Visit> void entries(Visit&& visit) const {
#ifdef FLITZFS_HIP_LZ4_RUNTIME_FIXTURE
    if(uniform_only_){for(const auto& entry:hip_uniform_lz4_bundle::files)visit(entry);return;}
    if(compression_only_){for(const auto& entry:hip_lz4_bundle::files)visit(entry);return;}
#endif
    for(const auto& entry:hip_bundle::files)visit(entry);
  }
  bool included(const wchar_t* name) const {
    return uniform_only_ || compression_only_ || !kernel_only_ || same_path(name, hip_bundle::kernel);
  }
  fs::path root_;
  bool kernel_only_ = false,compression_only_ = false,uniform_only_ = false;
  std::vector<DirectoryPin> directories_;
  std::vector<ReadOnlyFile> files_;
};

Bundle verify_bundle(bool kernel_only = false,bool compression_only=false,bool uniform_only=false) {
  try { return Bundle(kernel_only,compression_only,uniform_only); }
  catch (const HipWorkerFailure&) { throw; }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) {
    throw HipWorkerFailure(Exit::runtime_unavailable, "HIP bundle cannot be opened");
  }
}

class Module {
 public:
  explicit Module(const fs::path& path, HipWorkerStage stage) {
    handle_ = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(handle_ != nullptr, Exit::runtime_unavailable, "HIP module load failed", stage);
    try {
      require(same_path(path, module_path(handle_)), Exit::integrity_failure, "wrong loaded HIP module", stage);
    } catch (...) {
      FreeLibrary(handle_);
      throw;
    }
  }
  ~Module() { if (handle_) FreeLibrary(handle_); }
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  HMODULE get() const noexcept { return handle_; }
  void close() {
    if (auto handle = std::exchange(handle_, nullptr))
      require(FreeLibrary(handle) != 0, Exit::device_fault, "HIP module release failed", HipWorkerStage::module_retire);
  }
 private:
  HMODULE handle_ = nullptr;
};

unsigned decimal(std::wstring_view input, unsigned limit) {
  require(!input.empty() && input.size() <= 10, Exit::invalid_request, "adapter decimal field");
  unsigned value = 0;
  for (const auto digit : input) {
    require(digit >= L'0' && digit <= L'9', Exit::invalid_request, "adapter decimal digit");
    const auto next = static_cast<unsigned>(digit - L'0');
    require(next <= limit && value <= (limit - next) / 10, Exit::invalid_request, "adapter decimal range");
    value = 10 * value + next;
  }
  return value;
}

std::uint64_t wide_decimal(std::wstring_view input){
  require(!input.empty() && input.size()<=20,Exit::invalid_request,"uniform decimal field");
  std::uint64_t value=0;
  for(const auto digit:input){
    require(digit>=L'0' && digit<=L'9',Exit::invalid_request,"uniform decimal digit");
    const auto next=static_cast<unsigned>(digit-L'0');
    require(value<=((std::numeric_limits<std::uint64_t>::max)()-next)/10,
        Exit::invalid_request,"uniform decimal overflow");value=value*10+next;
  }
  return value;
}

unsigned hex_value(wchar_t digit) {
  if (digit >= L'0' && digit <= L'9') return static_cast<unsigned>(digit - L'0');
  if (digit >= L'a' && digit <= L'f') return static_cast<unsigned>(digit - L'a' + 10);
  throw HipWorkerFailure(Exit::invalid_request, "adapter LUID digit");
}

std::array<std::byte,8> parse_luid(std::wstring_view text) {
  require(text.size() == 16, Exit::invalid_request, "adapter LUID length");
  std::array<std::byte,8> luid{};
  for (std::size_t i = 0; i < luid.size(); ++i)
    luid[i] = static_cast<std::byte>(16 * hex_value(text[2*i]) + hex_value(text[2*i+1]));
  require(std::any_of(luid.begin(), luid.end(), [](auto byte) { return byte != std::byte{}; }),
      Exit::invalid_request, "zero adapter LUID");
  return luid;
}

WorkerBatchRequest read_request(std::span<const std::byte> bytes) {
  try { return decode_worker_request(bytes); }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception&) {
    throw HipWorkerFailure(Exit::invalid_request, "invalid HIP worker request");
  }
}

MatchWorkerRequest read_match_request(std::span<const std::byte> bytes) {
  try{return decode_match_worker_request(bytes);}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid HIP match request");}
}
std::string runtime_digest(std::wstring_view wide) {
  require(wide.size() == 64, Exit::invalid_request, "system runtime hash length");
  std::string expected;
  expected.reserve(64);
  for (const auto character : wide) {
    require((character >= L'0' && character <= L'9') ||
        (character >= L'a' && character <= L'f'), Exit::invalid_request, "system runtime hash digit");
    expected.push_back(static_cast<char>(character));
  }
  return expected;
}

int worker(int argc, wchar_t** argv) {
  const std::wstring_view mode = argc > 1 ? argv[1] : L"";
  const bool wide_copy_profile=mode==L"--driver-anchor-wide-transfer-profile";
  const bool wide_copy=mode==L"--driver-anchor-wide-transfer-batch" || wide_copy_profile;
  const bool modular_delta=mode==L"--driver-anchor-modular-delta-batch";
  const bool sparse_slot_profile=mode==L"--driver-anchor-sparse-xor-slot-profile";
  const bool sparse_xor_profile=mode==L"--driver-anchor-sparse-xor-setup-profile" || sparse_slot_profile;
  const bool sparse_xor=mode==L"--driver-anchor-sparse-xor-batch" || sparse_xor_profile;
  const bool collective=mode==L"--driver-anchor-collective-batch";
  const bool reference=mode==L"--driver-anchor-reference-batch";
  const bool wide_xor_setup_profile=mode==L"--driver-anchor-xor-wide-setup-profile";
  const bool wide_xor_profile=mode==L"--driver-anchor-xor-wide-batch-profile" || wide_xor_setup_profile;
  const bool anchor_batch_profile=mode==L"--driver-anchor-transfer-batch-profile" ||
      wide_copy_profile || wide_xor_profile || sparse_xor_profile;
  const bool wide_xor=mode==L"--driver-anchor-xor-wide-batch" || wide_xor_profile;
  const bool packed_xor=wide_xor || mode==L"--driver-anchor-xor-batch";
  const auto anchor_output=reference?anchor_match::BatchOutput::reference_xor:
      collective?anchor_match::BatchOutput::collective:
      sparse_xor?anchor_match::BatchOutput::sparse_xor:
      modular_delta?anchor_match::BatchOutput::modular_delta:
      wide_xor?anchor_match::BatchOutput::wide_packed_xor:
      packed_xor?anchor_match::BatchOutput::packed_xor:
      wide_copy?anchor_match::BatchOutput::wide_anchors:anchor_match::BatchOutput::anchors;
  const bool anchor_transfer=mode==L"--driver-anchor-transfer-batch" || anchor_batch_profile || wide_copy ||
      packed_xor || modular_delta || sparse_xor || collective || reference;
  const bool anchor_batch=mode==L"--driver-anchor-batch" || anchor_transfer;
  const bool anchor_profile=mode==L"--driver-anchor-sweep-profile";
  const bool anchor_sweep=mode==L"--driver-anchor-sweep" || anchor_profile;
  const bool native_host_guard=mode==L"--profile-native-lz4-host-guard-adapter";
  const bool native_recording=native_host_guard || mode==L"--profile-native-lz4-read-adapter" ||
      mode==L"--profile-native-lz4-prepared-read-adapter";
  const bool native_group_prepared=mode==L"--driver-native-group-prepared-read-adapter";
  const bool native_prepared=native_group_prepared || mode==L"--driver-native-lz4-prepared-read-adapter" ||
      mode==L"--profile-native-lz4-prepared-read-adapter";
  const bool native_groups=native_group_prepared || mode==L"--driver-native-group-read-adapter";
  const bool native_read=native_groups || native_prepared || native_recording || mode==L"--driver-native-lz4-read-adapter";
  const bool sized_identity=mode==L"--inspect-system-adapter-sized";
  require_hip_worker_job(anchor_batch || anchor_sweep || native_read || sized_identity);
  // Compiler discovery must not leak into an application runtime. This changes
  // only this disposable child; the launching process remains untouched.
  require(_wputenv_s(L"LLVM_PATH", L"") == 0,
      Exit::runtime_unavailable, "cannot isolate HIP compiler environment");
  SetLastError(ERROR_SUCCESS);
  require(GetEnvironmentVariableW(L"LLVM_PATH", nullptr, 0) == 0 &&
      (GetLastError() == ERROR_ENVVAR_NOT_FOUND || GetLastError() == ERROR_SUCCESS),
      Exit::runtime_unavailable, "HIP compiler environment remains visible");
  if (argc == 2 && std::wstring_view(argv[1]) == L"--verify-cpu-lz4") {
    // Explicit CPU-only diagnostic. No bundle lookup, device query, runtime
    // initialization, kernel/module loading or publication is permitted here.
    require(GetModuleHandleW(hip_bundle::runtime)==nullptr &&
        GetModuleHandleW(hip_bundle::kernel)==nullptr,Exit::integrity_failure,
        "CPU decoder diagnostic unexpectedly contains AMD modules");
    require(gpu_lz4_cpu_verification_available(),Exit::unsupported_configuration,
        "independent CPU LZ4 verification unavailable");
    return 0;
  }


#ifdef FLITZFS_HIP_NATIVE_LZ4_READ_FIXTURE
  if(native_read) {
    HipPhaseProfile measurement;
    NativeReadTransferTrace transfer;
    HipPhaseRecorder trace(native_recording?&measurement.trace:nullptr,hip_phase_detail::clock_ns,
        HipPhaseOrder::accumulate,native_read_trace_limit_ns);
    const auto endpoint=native_group_prepared?kPreparedWorkerBoundedArgument:
        native_prepared?kPreparedWorkerChannelArgument:std::wstring_view(L"--worker-channel");
    require(argc==(native_prepared?17:16) && std::wstring_view(argv[14])==endpoint,
        Exit::invalid_request,"explicit native LZ4 read channel and bounded grant required");
    const auto ordinal=decimal(argv[2],63);
    const auto luid=parse_luid(argv[3]);
    const auto node=decimal(argv[4],(std::numeric_limits<unsigned>::max)());
    const auto expected_runtime=runtime_digest(argv[5]),expected_kernel=runtime_digest(argv[6]);
    const std::wstring_view architecture=argv[7];
    require(std::has_single_bit(node) && architecture.size()>3 && architecture.size()<=32 &&
        architecture.starts_with(L"gfx") && std::ranges::all_of(architecture,[](wchar_t c){
          return (c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9');
        }),Exit::invalid_request,"native LZ4 read adapter coverage");
    const std::string arch(architecture.begin(),architecture.end());
    const hip_detail::Lz4KernelCoverage coverage{arch,decimal(argv[8],3)};
    const auto message_limit=(std::numeric_limits<std::int32_t>::max)();
    const WorkerChannelCapacity capacity{decimal(argv[9],message_limit),decimal(argv[10],message_limit)};
    const detail::NativeLz4DecodeGrant grant{decimal(argv[11],message_limit),wide_decimal(argv[12])};
    const auto mode_grant=decimal(argv[13],message_limit);
    const auto per_batch=native_group_prepared?grant.maximum_frames:mode_grant;
    require(per_batch && grant.maximum_frames && grant.maximum_slab_bytes,Exit::invalid_request,
        "empty native LZ4 read grant");
    const auto max_dispatch=(std::min)(grant.maximum_frames,per_batch);
    // A dispatch can only hold max_dispatch frames of the supported store size.
    // Charge the entire reserved slab before initialization and after readiness.
    constexpr auto max_frame_slab=2ULL*kStoreMaxObjectBytes+3*native_lz4_decode::alignment+
        sizeof(native_lz4_decode::Record);
    const auto reserve=native_group_prepared?grant.maximum_slab_bytes:
        (std::min<std::uint64_t>)(grant.maximum_slab_bytes,max_dispatch*max_frame_slab);
    require(reserve<=(std::numeric_limits<std::size_t>::max)(),Exit::invalid_request,"native LZ4 slab address limit");
    if(native_group_prepared) {
      require(mode_grant==group_decode::version || mode_grant==group_decode::mixed_version,
          Exit::invalid_request,"grouped extent preparation ABI");
      const auto plan=detail::plan_native_group_preparation(per_batch,reserve,mode_grant==group_decode::mixed_version);
      require(plan.maximum_members==per_batch && plan.device_bytes==reserve &&
          plan.request_bytes==capacity.request_bytes && plan.reply_bytes==capacity.reply_bytes,
          Exit::invalid_request,"grouped extent preparation shape");
    }
    auto channel=native_group_prepared?WorkerChannel::open_prepared_bounded(argv[15],capacity,
        decimal(argv[16],(std::numeric_limits<std::uint32_t>::max)())):
        native_prepared?WorkerChannel::open_prepared_sized(argv[15],capacity,
        decimal(argv[16],(std::numeric_limits<std::uint32_t>::max)())):
        WorkerChannel::open_sized(argv[15],capacity);
    std::optional<detail::NativeLz4WireRequest> request;
    std::optional<detail::NativeGroupWireRequest> groups;
    const auto decode=[&] {
      trace.begin(HipPhase::request_decode);
      const auto owned=channel.take_request_snapshot();
      require(owned && owned->bytes().size()==capacity.request_bytes,Exit::invalid_request,
          "native LZ4 request mapping length differs");
      try {request.emplace(detail::decode_native_lz4_request(owned->bytes(),grant,per_batch));}
      catch(const std::invalid_argument&) {throw HipWorkerFailure(Exit::invalid_request,"invalid native LZ4 read frame");}
      const auto trailer=native_recording?native_read_trace_bytes:0;
      require(request->reply_bytes<=capacity.reply_bytes && capacity.reply_bytes-request->reply_bytes==trailer,
          Exit::invalid_request,"native LZ4 reply mapping length differs");
      trace.end(HipPhase::request_decode);
    };
    const auto decode_groups=[&] {
      const auto owned=channel.take_request_snapshot();
      require(owned && (native_group_prepared?owned->bytes().size()<=capacity.request_bytes:
          owned->bytes().size()==capacity.request_bytes),Exit::invalid_request,
          "native group request mapping length differs");
      try {groups.emplace(detail::decode_native_group_request(owned->bytes(),max_dispatch,grant.maximum_slab_bytes));}
      catch(const std::invalid_argument&) {throw HipWorkerFailure(Exit::invalid_request,"invalid native group read frame");}
      const auto reply=detail::native_group_reply_bytes(groups->layout);
      require(native_group_prepared?reply<=capacity.reply_bytes && groups->layout.abi<=mode_grant:
          reply==capacity.reply_bytes,Exit::invalid_request,
          "native group reply mapping length differs");
    };
    if(!native_prepared) {
      if(native_groups)decode_groups();
      else decode();
    }

    trace.begin(HipPhase::bundle_verify);
    const hip_detail::VerifiedSystemModule installed(hip_bundle::runtime,expected_runtime);
    const auto parent=module_path(nullptr).parent_path();
    std::vector<DirectoryPin> pins;
    auto current=parent.root_path();
    require(parent.is_absolute() && parent.root_name().native().size()==2,Exit::runtime_unavailable,
        "native LZ4 worker requires a local package");
    pins.emplace_back(current);
    for(const auto& part:parent.relative_path()){
      require(part!=L"." && part!=L".." && pins.size()<32,Exit::runtime_unavailable,"native LZ4 package depth");
      current/=part;
      pins.emplace_back(current);
    }
    constexpr wchar_t kernel_name[]=L"flitzfs_rocm_native_lz4_decode.dll";
    const auto path=parent/kernel_name;
    const auto file=ReadOnlyFile::open(path);
    require(matches_sha256(file,expected_kernel),Exit::integrity_failure,"native LZ4 device module identity");
    require(!GetModuleHandleW(kernel_name) && !GetModuleHandleW(hip_bundle::runtime),
        Exit::integrity_failure,"native LZ4 helper inherited an unexpected module");
    trace.end(HipPhase::bundle_verify);
    trace.begin(HipPhase::adapter_admit);
    std::uint64_t peak=native_prepared?reserve:0;
    if(groups)peak=group_decode::slab_bytes(groups->layout);
    if(request)for(const auto& batch:request->batches)peak=(std::max)(peak,std::uint64_t(batch.slab_bytes()));
    std::optional<WindowsGpuBudgetSampler> sampler(std::in_place,luid,node);
    (void)read_fresh_hip_memory([&]{return sampler->sample().snapshot;},GetTickCount64,
        luid,node,peak,HipMemoryPoint::pre_initialize);
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=0,
        Exit::runtime_unavailable,"restrict native LZ4 DLL lookup");
    trace.end(HipPhase::adapter_admit);
    trace.begin(HipPhase::modules_load);
    Module runtime(installed.path(),HipWorkerStage::runtime_load);
    Module kernel(path,HipWorkerStage::kernel_load);
    require(GetModuleHandleW(hip_bundle::runtime)==runtime.get(),Exit::integrity_failure,
        "native LZ4 runtime identity changed");
    trace.end(HipPhase::modules_load);
    if(native_groups) {
      std::array<group_decode::KernelApi,2> apis{};
      std::size_t api_count=0;
      const auto bind=[&](std::uint32_t abi) {
        const auto entry_name=abi==group_decode::mixed_version?"flitzfs_native_group_decode_api_v2":
            "flitzfs_native_group_decode_api";
        const auto address=GetProcAddress(kernel.get(),entry_name);
        require(address!=nullptr,Exit::unsupported_configuration,"native group API entry unavailable",HipWorkerStage::kernel_entry);
        const auto entry=std::bit_cast<group_decode::KernelApiEntry>(address);
        const auto* api=entry();
        require(api && group_decode::compatible(*api,abi),Exit::unsupported_configuration,
            "incompatible native group read table",HipWorkerStage::kernel_table);
        apis[api_count++]=*api;
      };
      if(native_group_prepared) {
        bind(group_decode::version);
        if(mode_grant==group_decode::mixed_version)bind(group_decode::mixed_version);
      }else bind(groups->layout.abi);
      const auto receive=[&]() -> const detail::NativeGroupWireRequest* {
        if(native_group_prepared) {
          if(!channel.receive_prepared())return nullptr;
          decode_groups();
        }
        return &*groups;
      };
      const auto members=native_group_prepared?max_dispatch:groups->layout.members;
      const auto reply=run_hip_native_group_read_in_worker(runtime.get(),std::span(apis).first(api_count),
          static_cast<int>(ordinal),*sampler,members,peak,{&coverage,1},receive);
      sampler.reset();
      kernel.close();
      runtime.close();
      if(groups)channel.publish(reply);
      return 0;
    }
    trace.begin(HipPhase::entry_bind);
    const auto address=GetProcAddress(kernel.get(),"flitzfs_native_lz4_decode_api");
    require(address!=nullptr,Exit::unsupported_configuration,"native LZ4 API entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<native_lz4_decode::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && native_lz4_decode::compatible(*api),Exit::unsupported_configuration,
        "incompatible native LZ4 read table",HipWorkerStage::kernel_table);
    trace.end(HipPhase::entry_bind);
    const auto receive=[&]() -> std::span<const detail::NativeLz4DecodeBatch> {
      if(!channel.receive_prepared())return {};
      decode();
      return request->batches;
    };
    auto* const timing=native_recording?&trace:nullptr;
    auto* const traffic=native_recording?&transfer:nullptr;
    const auto frames=native_prepared?
        run_hip_native_lz4_prepared_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),
            *sampler,grant,max_dispatch,static_cast<std::size_t>(reserve),{&coverage,1},receive,timing,traffic):
        run_hip_native_lz4_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),
            *sampler,request->batches,grant,{&coverage,1},timing,traffic,!native_host_guard);
    sampler.reset();
    if(!request) {
      kernel.close();
      runtime.close();
      return 0;
    }
    trace.begin(HipPhase::reply_encode);
    auto reply=detail::encode_native_lz4_reply(*request,frames,WorkerExecution::hip);
    trace.end(HipPhase::reply_encode);
    trace.begin(HipPhase::modules_release);
    kernel.close();
    runtime.close();
    trace.end(HipPhase::modules_release);
    if(native_recording) {
      trace.finish();
      measurement.binding={request->id,ordinal,node,static_cast<std::uint32_t>(capacity.request_bytes),
          static_cast<std::uint32_t>(request->keys.size()),luid,request->request_digest};
      measurement.execution=WorkerExecution::hip;
      const auto metadata=encode_native_read_trace(measurement,transfer);
      reply.insert(reply.end(),metadata.begin(),metadata.end());
    }
    channel.publish(reply);
    return 0;
  }
#endif

#ifdef FLITZFS_HIP_LZ4_RUNTIME_FIXTURE
  if(argc==2 && std::wstring_view(argv[1])==L"--verify-uniform-lz4-kernel-bundle"){
    const auto bundle=verify_bundle(false,false,true);
    require(GetModuleHandleW(hip_uniform_lz4_bundle::kernel)==nullptr &&
        GetModuleHandleW(hip_bundle::runtime)==nullptr,Exit::integrity_failure,
        "uniform metadata verification loaded AMD code");return 0;
  }
  if(argc>1 && (std::wstring_view(argv[1])==L"--driver-uniform-lz4-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-profile-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-stream-sweep-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-profile-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-timeline-adapter" ||
      std::wstring_view(argv[1])==L"--driver-uniform-lz4-prepared-adapter")) {
    const bool recording=std::wstring_view(argv[1])==L"--driver-uniform-lz4-profile-adapter";
    const bool timeline=std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-timeline-adapter";
    const bool sweep_recording=timeline || std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-profile-adapter";
    const auto sweep_mode=timeline?UniformLz4SweepProfileMode::timeline:UniformLz4SweepProfileMode::aggregate;
    const bool prepared=std::wstring_view(argv[1])==L"--driver-uniform-lz4-prepared-adapter";
    const bool finishable=prepared && argc==15 && std::wstring_view(argv[12])==kPreparedWorkerBurstArgument;
    const bool repeated=finishable || (prepared && argc==15 && std::wstring_view(argv[12])==kPreparedWorkerRequestsArgument);
    const bool windowed=prepared && argc==14 && std::wstring_view(argv[11])==kPreparedWorkerWindowsArgument;
    const auto endpoint=repeated?12:11;
    const bool stream_bound=std::wstring_view(argv[1])==L"--driver-uniform-lz4-stream-sweep-adapter";
    const bool sweeping=prepared || sweep_recording || stream_bound ||
        std::wstring_view(argv[1])==L"--driver-uniform-lz4-sweep-adapter";
    require(argc==(repeated?15:prepared?14:13) && std::wstring_view(argv[endpoint])==
        (finishable?kPreparedWorkerBurstArgument:repeated?kPreparedWorkerRequestsArgument:windowed?kPreparedWorkerWindowsArgument:
            prepared?kPreparedWorkerChannelArgument:std::wstring_view(L"--worker-channel")),
        Exit::invalid_request,"explicit uniform adapter/channel/capacities required");
    require(gpu_lz4_cpu_verification_available(),Exit::unsupported_configuration,
        "independent CPU uniform recovery unavailable");
    const auto ordinal=decimal(argv[2],63);const auto luid=parse_luid(argv[3]);
    const auto mask=decimal(argv[4],(std::numeric_limits<unsigned>::max)());
    require(mask && std::has_single_bit(mask),Exit::invalid_request,"uniform node mask");
    const auto expected_runtime=runtime_digest(argv[5]);
    const auto request_capacity=decimal(argv[6],(std::numeric_limits<std::int32_t>::max)());
    const auto reply_capacity=decimal(argv[7],(std::numeric_limits<std::int32_t>::max)());
    const hip_detail::UniformLz4ComputeGrant grant{
        decimal(argv[8],(std::numeric_limits<unsigned>::max)()),wide_decimal(argv[9]),
        decimal(argv[10],uniform_lz4::max_chunk_bytes)};
    require(grant.maximum_chunks && grant.maximum_device_bytes && grant.chunk_bytes,Exit::invalid_request,"missing uniform work grant");
    const auto profile_bytes=recording?kUniformLz4ProfileBytes:sweep_recording?uniform_lz4_sweep_profile_bytes(sweep_mode):0;
    require(reply_capacity>profile_bytes,Exit::invalid_request,"uniform profile capacity");
    const auto requests=repeated?decimal(argv[11],(std::numeric_limits<unsigned>::max)()):1U;
    constexpr auto prefix_bytes=sizeof(std::uint32_t);
    if(repeated)require(requests>=2 && reply_capacity%requests==0 && reply_capacity/requests>prefix_bytes,
        Exit::invalid_request,"repeated uniform reply capacity");
    const auto data_capacity=repeated?reply_capacity/requests-prefix_bytes:reply_capacity-profile_bytes;
    const WorkerChannelCapacity channel_capacity{request_capacity,reply_capacity};
    const auto timeout=prepared?decimal(argv[endpoint+2],(std::numeric_limits<unsigned>::max)()):0U;
    auto channel=repeated?WorkerChannel::open_prepared_requests(argv[endpoint+1],channel_capacity,timeout,requests,finishable):
        prepared?WorkerChannel::open_prepared_sized(argv[endpoint+1],channel_capacity,timeout,windowed):
        WorkerChannel::open_sized(argv[endpoint+1],channel_capacity);
    HipPhaseTrace trace;HipPhaseRecorder clock(recording?&trace:nullptr);
    UniformLz4SweepTrace sweep_trace;
    clock.begin(HipPhase::request_decode);
    std::optional<UniformLz4Request> request;
    const auto decode=[&](bool allow_smaller=false) {
      try {
        const auto frame=channel.request();
        const auto limits=inspect_uniform_lz4_request(frame,request_capacity,data_capacity,allow_smaller);
        request.emplace(decode_uniform_lz4_request(frame,limits));
      }catch(const std::bad_alloc&){throw;}
      catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid uniform compression frame");}
      require(request->batch.layout().chunk_bytes==grant.chunk_bytes,
          Exit::invalid_request,"uniform frame/grant width mismatch");
    };
    if(!prepared)decode();
    std::optional<UniformLz4SweepPlan> sweep_plan;
    if(sweeping) {
      uniform_lz4::Layout layout{};
      if(prepared) {
        require(request_capacity>kUniformLz4HeaderBytes &&
            uniform_lz4::make_layout(request_capacity-kUniformLz4HeaderBytes,grant.chunk_bytes,layout),
            Exit::invalid_request,"prepared uniform frame geometry");
      } else layout=request->batch.layout();
      try{sweep_plan=plan_uniform_lz4_sweep(layout,grant.maximum_chunks,grant.maximum_device_bytes);}
      catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"uniform sweep exceeds work grant");}
      if(prepared)require(sweep_plan->whole.request_bytes==request_capacity && sweep_plan->whole.reply_bytes==data_capacity,
          Exit::invalid_request,"prepared uniform frame capacity mismatch");
      if(windowed)require(sweep_plan->windows>1,Exit::invalid_request,"prepared burst requires multiple windows");
    } else require(request->batch.layout().chunks<=grant.maximum_chunks &&
        request->batch.layout().total_bytes<=grant.maximum_device_bytes,
        Exit::invalid_request,"uniform frame exceeds qualified work grant");
    const auto device_bytes=sweeping?sweep_plan->device_peak_bytes:request->batch.layout().total_bytes;
    clock.end(HipPhase::request_decode);
    clock.begin(HipPhase::bundle_verify);
    const auto bundle=verify_bundle(false,false,true);
    clock.end(HipPhase::bundle_verify);
    clock.begin(HipPhase::adapter_admit);
    const hip_detail::VerifiedSystemModule installed(hip_bundle::runtime,expected_runtime);
    std::optional<WindowsGpuBudgetSampler> sampler(std::in_place,luid,mask);
    (void)read_fresh_hip_memory([&]{return sampler->sample().snapshot;},GetTickCount64,
        luid,mask,device_bytes,HipMemoryPoint::pre_initialize);
    clock.end(HipPhase::adapter_admit);clock.begin(HipPhase::modules_load);
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=0,
        Exit::runtime_unavailable,"restrict uniform DLL lookup");
    bundle.check_loaded_paths();
    Module runtime(installed.path(),HipWorkerStage::runtime_load);
    Module kernels(bundle.path(hip_uniform_lz4_bundle::kernel),HipWorkerStage::kernel_load);
    bundle.check_loaded_paths();
    require(GetModuleHandleW(hip_bundle::runtime)==runtime.get(),Exit::integrity_failure,
        "uniform runtime identity changed");
    clock.end(HipPhase::modules_load);clock.begin(HipPhase::entry_bind);
    const auto address=GetProcAddress(kernels.get(),"flitzfs_uniform_lz4_kernel_api");
    require(address!=nullptr,Exit::unsupported_configuration,"uniform API entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<uniform_lz4::KernelApiEntry>(address);const auto* api=entry();
    require(api && uniform_lz4::compatible_kernel_api(*api),Exit::unsupported_configuration,
        "incompatible uniform API table",HipWorkerStage::kernel_table);
    clock.end(HipPhase::entry_bind);
    const auto targets=std::span<const hip_detail::Lz4KernelCoverage>(hip_uniform_lz4_bundle::targets);
    const auto receive=[&]() -> const UniformLz4Request* {
      if(!channel.receive_prepared())return nullptr;
      decode();
      return &*request;
    };
    std::function<bool()> await_window;
    if(windowed)await_window=[&]{return channel.await_window();};
    std::vector<std::byte> replies;
    std::uint32_t completed=0;
    bool prefix_finished=false;
    UniformLz4NextRequest next_request;
    if(repeated)next_request=[&](UniformLz4Candidates part) -> const UniformLz4Request* {
      const auto reply=encode_uniform_lz4_reply(request->batch,request->request_id,part,WorkerExecution::hip);
      append_prepared_reply(replies,reply,reply_capacity);
      if(++completed==requests)return nullptr;
      // Quanta have completed, including CPU readback and the memory recheck.
      // Release this job's decoded input while waiting for the next permit.
      request.reset();
      const auto receipt=channel.receive_prepared_continuation();
      if(receipt==PreparedRequestReceipt::declined)throw hip_detail::UniformLz4WindowDeclined{};
      if(receipt==PreparedRequestReceipt::finished) {
        prefix_finished=true;
        return nullptr;
      }
      decode(finishable);
      return &*request;
    };
    std::optional<UniformLz4Candidates> candidates;
    try {
      candidates.emplace(prepared?run_hip_uniform_lz4_prepared_sweep_in_worker(runtime.get(),*api,
          static_cast<int>(ordinal),*sampler,*sweep_plan,targets,receive,await_window,next_request,finishable):
          sweeping?run_hip_uniform_lz4_sweep_in_worker(runtime.get(),*api,
              static_cast<int>(ordinal),*sampler,*request,grant,targets,sweep_recording?&sweep_trace:nullptr,timeline,stream_bound):
              run_hip_uniform_lz4_in_worker(runtime.get(),*api,static_cast<int>(ordinal),*sampler,
                  *request,grant,targets,recording?&clock:nullptr));
    }catch(const hip_detail::UniformLz4WindowDeclined&) {
      require(windowed || repeated,Exit::invalid_request,"unexpected prepared admission refusal");
    }
    sampler.reset();
    if(!candidates || (prepared && !request && !prefix_finished)) {
      kernels.close();
      runtime.close();
      return 0;
    }
    if(repeated) {
      require(completed==requests || (prefix_finished && completed && completed<requests),
          Exit::invalid_request,"incomplete repeated uniform replies");
      kernels.close();
      runtime.close();
      channel.publish(replies);
      return 0;
    }
    clock.begin(HipPhase::reply_encode);
    auto frame=encode_uniform_lz4_reply(request->batch,request->request_id,*candidates,WorkerExecution::hip);
    clock.end(HipPhase::reply_encode);clock.begin(HipPhase::modules_release);
    kernels.close();runtime.close();clock.end(HipPhase::modules_release);clock.finish();
    if(recording || sweep_recording){
      const UniformLz4ProfileBinding binding{request->request_id,ordinal,mask,grant.maximum_chunks,
          grant.chunk_bytes,grant.maximum_device_bytes,luid};
      frame=sweep_recording?encode_uniform_lz4_sweep_profile_reply(request->batch,binding,sweep_trace,frame,WorkerExecution::hip,sweep_mode):
          encode_uniform_lz4_profile_reply(request->batch,binding,trace,frame,WorkerExecution::hip);
    }
    channel.publish(frame);return 0;
  }
  if(argc==2 && std::wstring_view(argv[1])==L"--verify-lz4-kernel-bundle"){
    const auto bundle=verify_bundle(false,true);
    require(GetModuleHandleW(hip_lz4_bundle::kernel)==nullptr &&
        GetModuleHandleW(hip_bundle::runtime)==nullptr,Exit::integrity_failure,
        "compression bundle metadata loaded AMD code");
    return 0;
  }
  if(argc>1 && std::wstring_view(argv[1])==L"--driver-lz4-adapter"){
    require(argc==8 && std::wstring_view(argv[6])==L"--worker-channel",
        Exit::invalid_request,"explicit compression adapter/channel required");
    require(gpu_lz4_cpu_verification_available(),Exit::unsupported_configuration,
        "independent CPU compression recovery unavailable");
    const auto ordinal=decimal(argv[2],63);
    const auto luid=parse_luid(argv[3]);
    const auto node_mask=decimal(argv[4],(std::numeric_limits<unsigned>::max)());
    require(node_mask && std::has_single_bit(node_mask),Exit::invalid_request,"compression node mask");
    const auto expected_runtime=runtime_digest(argv[5]);
    auto channel=WorkerChannel::open(argv[7]);
    std::optional<Lz4WorkerRequest> request;
    try{request.emplace(decode_lz4_worker_request(channel.request()));}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid compression frame");}
    const auto bundle=verify_bundle(false,true);
    const hip_detail::VerifiedSystemModule installed(hip_bundle::runtime,expected_runtime);
    const auto before=query_windows_gpu_budget(luid,node_mask);
    require(before.status==WindowsGpuBudgetStatus::success,Exit::unsupported_configuration,
        "compression adapter unavailable",HipWorkerStage::adapter_budget);
    require_hip_memory(before.snapshot,luid,node_mask,GetTickCount64(),
        request->batch.device_layout().total_bytes,HipMemoryPoint::pre_initialize);
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=0,
        Exit::runtime_unavailable,"restrict compression DLL lookup");
    bundle.check_loaded_paths();
    Module runtime(installed.path(),HipWorkerStage::runtime_load);
    Module kernels(bundle.path(hip_lz4_bundle::kernel),HipWorkerStage::kernel_load);
    bundle.check_loaded_paths();
    require(GetModuleHandleW(hip_bundle::runtime)==runtime.get(),
        Exit::integrity_failure,"compression runtime identity changed");
    const auto address=GetProcAddress(kernels.get(),"flitzfs_candidate_lz4_api");
    require(address!=nullptr,Exit::unsupported_configuration,"compression API entry unavailable",
        HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<candidate_lz4::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && candidate_lz4::compatible_kernel_api(*api),Exit::unsupported_configuration,
        "incompatible compression API table",HipWorkerStage::kernel_table);
    auto candidates=run_hip_lz4_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,
        *request,std::span<const hip_detail::Lz4KernelCoverage>(hip_lz4_bundle::targets));
    std::vector<Lz4WorkerBytes> views;views.reserve(candidates.size());
    for(const auto& bytes:candidates)views.emplace_back(bytes);
    const auto frame=encode_lz4_worker_reply(request->batch,request->request_id,views,WorkerExecution::hip);
    kernels.close();runtime.close();
    channel.publish(frame);
    return 0;
  }
#endif
  if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-system-runtime") {
    const hip_detail::VerifiedSystemModule verified(hip_bundle::runtime, runtime_digest(argv[2]));
    return 0;
  }
  if (argc == 7 && (mode==L"--inspect-system-adapter" || sized_identity)) {
    require(std::wstring_view(argv[5]) == L"--worker-channel",
        Exit::invalid_request,"identity metadata channel required");
    const auto ordinal=decimal(argv[2],63);
    const auto luid=parse_luid(argv[3]);
    const auto expected_runtime=runtime_digest(argv[4]);
    auto channel=WorkerChannel::open(argv[6]);
    HipIdentityRequest request;
    try{request=decode_hip_identity_request(channel.request());}
    catch(const std::exception&){throw HipWorkerFailure(Exit::invalid_request,"invalid identity metadata request");}
    require(request.ordinal==ordinal && request.luid==luid,
        Exit::invalid_request,"identity metadata argument/frame mismatch");
    const hip_detail::VerifiedSystemModule installed(hip_bundle::runtime,expected_runtime);
    const auto budget=query_windows_gpu_budget(luid,request.node_mask);
    require(budget.status==WindowsGpuBudgetStatus::success,
        Exit::unsupported_configuration,"identity metadata adapter budget unavailable",HipWorkerStage::adapter_budget);
    require(check_gpu_memory_budget(budget.snapshot,luid,request.node_mask,GetTickCount64(),0)==GpuGateCode::admitted,
        Exit::temporary_pressure,"identity metadata memory budget",HipWorkerStage::adapter_budget);
    require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)!=0,
        Exit::runtime_unavailable,"restrict metadata DLL lookup");
    require(GetModuleHandleW(hip_bundle::kernel)==nullptr,
        Exit::integrity_failure,"identity metadata unexpectedly contains a kernel module");
    Module runtime(installed.path(),HipWorkerStage::runtime_load);
    const auto observed=hip_detail::query_device_identity(hip_detail::bind_api(runtime.get()),
        static_cast<int>(ordinal));
    require(GetModuleHandleW(hip_bundle::kernel)==nullptr,
        Exit::integrity_failure,"identity metadata loaded a kernel module");
    const auto frame=encode_hip_identity_reply(
        {request.request_id,request.ordinal,observed.node_mask,observed.luid});
    runtime.close(); // Metadata cannot be published before confirmed module retirement.
    channel.publish(frame);
    return 0;
  }
  if (argc == 2 && (std::wstring_view(argv[1]) == L"--verify-bundle" ||
      std::wstring_view(argv[1]) == L"--verify-kernel-bundle")) {
    const auto verified = verify_bundle(std::wstring_view(argv[1]) == L"--verify-kernel-bundle");
    return 0;
  }
  const bool memory_observation=mode==L"--driver-memory-adapter";
  const bool phase_observation=mode==L"--driver-match-profile";
  const bool anchoring=mode==L"--driver-anchor-match" || anchor_sweep;
  const bool driver = mode == L"--driver-adapter" || mode == L"--driver-match-adapter" ||
      memory_observation || phase_observation || anchoring || anchor_batch;
  const bool matching = mode == L"--match-adapter" || mode == L"--driver-match-adapter" || phase_observation;
  const auto channel_argument=anchor_batch_profile?13:anchor_transfer?12:anchor_batch?11:anchor_sweep?10:driver?6:5;
  require(argc==channel_argument+2 &&
      (driver || mode == L"--adapter" || mode == L"--match-adapter") &&
      std::wstring_view(argv[channel_argument]) == L"--worker-channel",
      Exit::invalid_request, "explicit HIP adapter and channel required");
  const auto expected_runtime = driver ? runtime_digest(argv[5]) : std::string{};
  const auto ordinal = decimal(argv[2], 63);
  const auto luid = parse_luid(argv[3]);
  const auto node_mask = decimal(argv[4], (std::numeric_limits<unsigned>::max)());
  require(node_mask && !(node_mask & (node_mask - 1)), Exit::invalid_request, "adapter node mask");
  const auto dispatches_per_wait=anchor_batch_profile?decimal(argv[12],kAnchorProfileMaxQueuedDispatches):1;
  require(dispatches_per_wait!=0,Exit::invalid_request,"anchor batch wait grant");
  HipPhaseProfile phase_value;
  AnchorSetupTiming anchor_setup;
  auto* const setup=(wide_xor_setup_profile || sparse_xor_profile)?&anchor_setup:nullptr;
  const bool profile_anchor=anchor_profile || anchor_batch_profile;
  HipPhaseRecorder phase_clock(phase_observation || profile_anchor?&phase_value.trace:nullptr,
      hip_phase_detail::clock_ns,profile_anchor?HipPhaseOrder::accumulate:HipPhaseOrder::ordered,
      profile_anchor?kAnchorSweepProfileLimitNs:hip_profile_limit_ns);
  phase_clock.begin(HipPhase::request_decode);
  std::optional<detail::AnchorMatchSweepPlan> anchor_plan;
  constexpr auto max_uint=(std::numeric_limits<unsigned>::max)();
  if (anchor_sweep) {
    try {
      anchor_plan=detail::plan_anchor_match_sweep(decimal(argv[6],max_uint),
          decimal(argv[7],anchor_match::max_members),wide_decimal(argv[8]),decimal(argv[9],max_uint));
    } catch(const std::invalid_argument&) {
      throw HipWorkerFailure(Exit::invalid_request,"invalid shared base work grant",HipWorkerStage::worker_request);
    }
  }
  const auto token=argv[channel_argument+1];
  const auto profile_bytes=profile_anchor?kAnchorSweepProfileBytes:0;
  std::optional<WorkerChannelCapacity> sized_capacity;
  if(anchor_batch) sized_capacity=WorkerChannelCapacity{decimal(argv[6],max_uint),decimal(argv[7],max_uint)};
  else if(anchor_plan) sized_capacity=WorkerChannelCapacity{anchor_plan->whole.request_bytes,
      anchor_plan->whole.reply_bytes+profile_bytes};
  auto channel=sized_capacity?WorkerChannel::open_sized(token,*sized_capacity):WorkerChannel::open(token);
  std::optional<WorkerBatchRequest> decode;
  std::optional<MatchWorkerRequest> match;
  std::optional<detail::AnchorMatchRequest> anchor;
  std::optional<detail::AnchorMatchBatchRequest> anchors;
  std::uint32_t transfer_grant=0;
  std::uint64_t allocation=0;
  if(anchor_batch) {
    require(sized_capacity->reply_bytes>profile_bytes,Exit::invalid_request,"anchor batch reply capacity");
    try {
      anchors.emplace(detail::decode_anchor_match_batch_request(channel.take_request_snapshot(),
          static_cast<std::uint32_t>(sized_capacity->request_bytes),
          static_cast<std::uint32_t>(sized_capacity->reply_bytes-profile_bytes),
          decimal(argv[8],anchor_match::dispatch_member_limit(anchor_output)),
          wide_decimal(argv[9]),decimal(argv[10],max_uint),anchor_output));
      allocation=anchors->plan().device_bytes;
      if(anchor_transfer) {
        transfer_grant=decimal(argv[11],max_uint);
        allocation=detail::plan_anchor_match_transfers(anchors->plan(),transfer_grant).device_bytes;
      }
    } catch(const std::invalid_argument&) {
      throw HipWorkerFailure(Exit::invalid_request,"invalid shared base batch",HipWorkerStage::worker_request);
    }
  } else if(anchoring) {
    const auto admitted=anchor_plan?std::optional{anchor_plan->whole}:std::nullopt;
    try {anchor.emplace(detail::decode_anchor_match_request(channel.request(),admitted));}
    catch(const std::invalid_argument&) {
      throw HipWorkerFailure(Exit::invalid_request,"invalid shared base match request",HipWorkerStage::worker_request);
    }
    if(!anchor_plan) {
      const auto members=detail::anchor_match_members(anchor->source);
      anchor_plan=detail::plan_anchor_match_sweep(members,members,anchor_match::device_bytes(members),1);
    }
    allocation=anchor_plan->device_bytes;
  } else if(matching) {
    match.emplace(read_match_request(channel.request()));
    allocation=match->search.device_buffer_bytes();
  } else {
    decode.emplace(read_request(channel.request()));
    allocation=decode->batch.device_buffer_bytes();
  }
  phase_clock.end(HipPhase::request_decode);
  if(phase_observation) {
    phase_value.execution=WorkerExecution::hip;
    phase_value.binding={match->request_id,ordinal,node_mask,
        static_cast<std::uint32_t>(match->search.input().size()),match->search.lanes(),luid,{}};
    // The canonical request digest was verified during decode above.
    std::copy_n(channel.request().begin()+32,32,phase_value.binding.source_digest.begin());
  }
  std::optional<HipMemoryBinding> diagnostic_binding;
  if(memory_observation){
    diagnostic_binding=HipMemoryBinding{decode->request_id,ordinal,node_mask,luid,
        allocation,decode->batch.cpu_reference().size()};
    try{hip_memory_detail::binding_valid(*diagnostic_binding);}
    catch(const std::invalid_argument&){
      throw HipWorkerFailure(Exit::invalid_request,"memory diagnostic binding",HipWorkerStage::worker_request);
    } // Before loading a runtime/kernel.
  }
  phase_clock.begin(HipPhase::bundle_verify);
  const auto bundle = verify_bundle(driver);
  std::optional<hip_detail::VerifiedSystemModule> installed;
  if (driver) installed.emplace(hip_bundle::runtime, expected_runtime);
  phase_clock.end(HipPhase::bundle_verify);
  phase_clock.begin(HipPhase::adapter_admit);
  const auto observed = query_windows_gpu_budget(luid, node_mask);
  require(observed.status == WindowsGpuBudgetStatus::success,
      Exit::unsupported_configuration, "AMD adapter unavailable", HipWorkerStage::adapter_budget);
  require(check_gpu_memory_budget(observed.snapshot, luid, node_mask, GetTickCount64(),
      allocation) == GpuGateCode::admitted,
      Exit::temporary_pressure, "AMD worker memory budget", HipWorkerStage::adapter_budget);

  phase_clock.end(HipPhase::adapter_admit);
  phase_clock.begin(HipPhase::modules_load);
  // Per-process search settings. Imported private DLLs are pinned in hip/.
  require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) != 0,
      Exit::runtime_unavailable, "restrict worker DLL lookup");
  bundle.check_loaded_paths();
  const auto runtime_path = driver ? installed->path() : bundle.path(hip_bundle::runtime);
  Module runtime(runtime_path, HipWorkerStage::runtime_load);
  Module kernels(bundle.path(hip_bundle::kernel), HipWorkerStage::kernel_load);
  bundle.check_loaded_paths();
  const auto loaded_runtime = GetModuleHandleW(hip_bundle::runtime);
  require(loaded_runtime && same_path(runtime_path, module_path(loaded_runtime)),
      Exit::integrity_failure, "different installed HIP runtime loaded");
  phase_clock.end(HipPhase::modules_load);
  std::vector<std::byte> frame;
  if(anchor || anchors) {
    phase_clock.begin(HipPhase::entry_bind);
    const auto address=GetProcAddress(kernels.get(),"flitzfs_rocm_anchor_match_api_v1");
    require(address!=nullptr,Exit::unsupported_configuration,"HIP anchor match entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<anchor_match::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && anchor_match::compatible_kernel_api(*api),Exit::unsupported_configuration,
        "incompatible HIP anchor match table",HipWorkerStage::kernel_table);
    const anchor_match::WindowSearchApi* window=nullptr;
    const anchor_match::AnchorArenaApi* arena=nullptr;
    const anchor_match::AnchorReferenceApi* pricing=nullptr;
    const auto dense_arena=collective?
        GetProcAddress(kernels.get(),"flitzfs_rocm_anchor_collective_arena_api_v12"):nullptr;
    const auto legacy_arena_version=collective?anchor_match::collective_arena_abi_version:
        anchor_match::sparse_xor_arena_abi_version;
    const auto expected_arena_version=dense_arena?anchor_match::collective_dense_arena_abi_version:legacy_arena_version;
    const auto arena_address=dense_arena?dense_arena:
        (collective || reference || (sparse_xor && !sparse_slot_profile))?
            GetProcAddress(kernels.get(),collective?"flitzfs_rocm_anchor_collective_arena_api_v9":
                "flitzfs_rocm_anchor_sparse_xor_arena_api_v8"):nullptr;
    require(!(collective || reference) || arena_address,Exit::unsupported_configuration,"HIP anchor arena entry unavailable",
        HipWorkerStage::kernel_entry);
    if(arena_address) {
      arena=std::bit_cast<anchor_match::AnchorArenaApiEntry>(arena_address)();
      require(arena && arena->version==expected_arena_version && anchor_match::compatible_anchor_arena_api(*arena,anchor_output),
          Exit::unsupported_configuration,
          "incompatible HIP anchor arena table",HipWorkerStage::kernel_table);
      if(reference) {
        const auto price=GetProcAddress(kernels.get(),"flitzfs_rocm_anchor_reference_api_v10");
        require(price,Exit::unsupported_configuration,"HIP reference price entry unavailable",HipWorkerStage::kernel_entry);
        pricing=std::bit_cast<anchor_match::AnchorReferenceApiEntry>(price)();
        require(pricing && anchor_match::compatible_anchor_reference_api(*pricing),Exit::unsupported_configuration,
            "incompatible HIP reference price table",HipWorkerStage::kernel_table);
      }
    } else if(anchor_transfer) {
      const auto window_address=GetProcAddress(kernels.get(),
          sparse_xor?"flitzfs_rocm_anchor_sparse_xor_api_v7":modular_delta?"flitzfs_rocm_anchor_modular_delta_api_v6":
          wide_xor?"flitzfs_rocm_anchor_xor_api_v5":
          packed_xor?"flitzfs_rocm_anchor_xor_api_v3":
          wide_copy?"flitzfs_rocm_anchor_window_api_v11":"flitzfs_rocm_anchor_window_api_v2");
      require(window_address!=nullptr,Exit::unsupported_configuration,"HIP anchor window entry unavailable",
          HipWorkerStage::kernel_entry);
      window=std::bit_cast<anchor_match::WindowSearchApiEntry>(window_address)();
      const auto version=anchor_match::window_version(anchor_output);
      require(window && anchor_match::compatible_window_search_api(*window,version),Exit::unsupported_configuration,
          "incompatible HIP anchor window table",HipWorkerStage::kernel_table);
    }
    phase_clock.end(HipPhase::entry_bind);
    try {
      // Match records have byte checks here. The native writer independently
      // proves the decoded packed XOR candidate before it can publish.
      if(anchors) {
        frame=run_hip_anchor_batch_in_worker(runtime.get(),*api,static_cast<int>(ordinal),
            luid,node_mask,*anchors,phase_clock,window,arena,transfer_grant,dispatches_per_wait,setup,profile_bytes,pricing);
      } else {
        const auto records=run_hip_anchor_match_in_worker(runtime.get(),*api,static_cast<int>(ordinal),
            luid,node_mask,*anchor,*anchor_plan,phase_clock,anchor_sweep);
        phase_clock.begin(HipPhase::reply_encode);
        frame=detail::encode_anchor_match_reply(anchor->view(),records,profile_bytes);
        phase_clock.end(HipPhase::reply_encode);
      }
    } catch(const std::invalid_argument&) {
      throw HipWorkerFailure(Exit::integrity_failure,"HIP shared base suggestion failed independent byte verification");
    }
  } else if(matching) {
    phase_clock.begin(HipPhase::entry_bind);
    const auto address=GetProcAddress(kernels.get(),"flitzfs_rocm_match_api_v1");
    require(address!=nullptr,Exit::unsupported_configuration,"HIP match entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<match_detail::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && match_detail::compatible_kernel_api(*api),
        Exit::unsupported_configuration,"incompatible HIP match table",HipWorkerStage::kernel_table);
    phase_clock.end(HipPhase::entry_bind);
    const auto reply=run_hip_match_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*match,
        phase_observation?&phase_clock:nullptr);
    phase_clock.begin(HipPhase::reply_encode);
    frame=encode_match_worker_reply(match->search,match->request_id,reply,WorkerExecution::hip);
    phase_clock.end(HipPhase::reply_encode);
  } else {
    const auto address=GetProcAddress(kernels.get(),"flitzfs_rocm_kernel_api_v1");
    require(address!=nullptr,Exit::unsupported_configuration,"HIP kernel entry unavailable",HipWorkerStage::kernel_entry);
    const auto entry=std::bit_cast<gpu_detail::KernelApiEntry>(address);
    const auto* api=entry();
    require(api && gpu_detail::compatible_kernel_api(*api),
        Exit::unsupported_configuration,"incompatible HIP kernel table",HipWorkerStage::kernel_table);
    if(memory_observation){
      HipMemoryObservation observation;
      observation.binding=*diagnostic_binding;
      try{(void)run_hip_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*decode,&observation);}
      catch(const HipWorkerFailure& failure){
        observation.failure_category=static_cast<std::uint32_t>(failure.code);
        observation.failure_stage=static_cast<std::uint32_t>(failure.stage);
      }
      // A diagnostic may describe a rejected computation, but never publish
      // its data bytes. Unconfirmed module retirement still publishes nothing.
      kernels.close();runtime.close();observation.flags|=hip_memory_modules_retired;
      const auto metadata=encode_hip_memory_observation(observation);
      channel.publish(metadata);return 0;
    }
    const auto reply=run_hip_decode_in_worker(runtime.get(),*api,static_cast<int>(ordinal),luid,node_mask,*decode);
    frame=encode_worker_reply(reply,WorkerExecution::hip);
  }
  phase_clock.begin(HipPhase::modules_release);
  kernels.close();
  runtime.close();
  phase_clock.end(HipPhase::modules_release);
  if(phase_observation) {
    phase_clock.finish();
    const auto metadata=encode_hip_phase_profile(phase_value);
    channel.publish(metadata);return 0;
  }
  if(anchor_batch_profile) {
    phase_clock.finish();
    const auto transfer=detail::plan_anchor_match_transfers(anchors->plan(),transfer_grant);
    const auto binding=anchor_batch_profile_binding(*anchors,transfer,ordinal,luid,node_mask,dispatches_per_wait);
    const auto metadata=encode_anchor_batch_profile(binding,phase_value.trace,setup);
    frame.insert(frame.end(),metadata.begin(),metadata.end());
  } else if(anchor_profile) {
    phase_clock.finish();
    const auto binding=anchor_sweep_profile_binding(*anchor,*anchor_plan,ordinal,luid,node_mask);
    const auto metadata=encode_anchor_sweep_profile(binding,phase_value.trace);
    frame.insert(frame.end(),metadata.begin(),metadata.end());
  }
  channel.publish(frame);
  return 0;
}
}
}

int wmain(int argc, wchar_t** argv) {
  try { return flitzfs::worker(argc, argv); }
  catch (const flitzfs::HipWorkerFailure& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(flitzfs::hip_worker_diagnostic_exit(error.code, error.stage));
  } catch (const std::bad_alloc&) {
    return static_cast<int>(flitzfs::HipWorkerExit::temporary_pressure);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return static_cast<int>(flitzfs::HipWorkerExit::device_fault);
  }
}

