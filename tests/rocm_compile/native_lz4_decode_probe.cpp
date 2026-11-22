#include "native_lz4_capture.hpp"
#include "native_lz4_decode_wire.hpp"
#include "worker_exchange.hpp"
#include "gpu_request_id.hpp"
#include "flitzfs/gpu_batch.hpp"
#include "hip_module_verification.hpp"
#include "gpu_budget.hpp"
#include "../native_fs_fixture.hpp"
#include "../native_lz4_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/paged_store.hpp"
#include <chrono>
#include <iostream>
#include <utility>

namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using native_fixture::Bytes;

void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

std::string ascii(std::wstring_view value) {
  require(std::ranges::all_of(value,[](wchar_t c){return c>0 && c<128;}),"non-ASCII probe argument");
  return {value.begin(),value.end()};
}

std::array<std::byte,8> parse_luid(std::wstring_view value) {
  const auto text=ascii(value);
  require(text.size()==16,"invalid LUID length");
  const auto digit=[](char c) {
    if(c>='0' && c<='9')return unsigned(c-'0');
    require(c>='a' && c<='f',"invalid LUID digit");
    return unsigned(c-'a'+10);
  };
  std::array<std::byte,8> luid{};
  for(std::size_t i=0;i<luid.size();++i)
    luid[i]=static_cast<std::byte>(16*digit(text[2*i])+digit(text[2*i+1]));
  return luid;
}

std::filesystem::path module_path(HMODULE module) {
  std::wstring path(32768,L'\0');
  const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
  require(length && length<path.size(),"unavailable module path");
  path.resize(length);
  return path;
}

enum class Trial {cold,prepared,discard_prepared,refuse_module,refuse_prepared_module};

void run(std::wstring_view runtime_name,std::wstring_view runtime_sha,std::wstring_view module_sha,
    std::wstring_view adapter,std::wstring_view architecture,std::wstring_view ordinal_text,
    const std::optional<std::filesystem::path>& corpus,Trial trial) {
  const bool prepared=trial==Trial::prepared || trial==Trial::discard_prepared || trial==Trial::refuse_prepared_module;
  const bool refuse_module=trial==Trial::refuse_module || trial==Trial::refuse_prepared_module;
  require(store_lz4_available(),"native LZ4 CPU reference unavailable");
  require(ordinal_text.size()==1 && ordinal_text[0]>=L'0' && ordinal_text[0]<=L'9',"invalid HIP ordinal");
  const int ordinal=ordinal_text[0]-L'0';
  const auto luid=parse_luid(adapter);
  const auto arch=ascii(architecture);
  const auto wave_mask=arch=="gfx1201"?1U:arch=="gfx90a"?2U:0U;
  require(wave_mask!=0,"architecture not in the compiled fixture");

  auto backing=std::make_shared<native_fixture::Ram>();
  backing->failure_unit=4096;
  constexpr PagedStoreLimits limits{4U*1024U*1024U};
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto writer=NativeFilesystemWriter::initialize_indexed(
      PagedObjectStoreWriter::create_mirrored(std::make_shared<native_fixture::ConcurrentRam>(backing),
          native_fixture::identity(),limits,fast));
  const auto create=native_fixture::file_create("read-data");
  writer.apply({&create,1});
  auto session=NativeFileSession::create(std::move(writer));
  const auto reader=session.open("read-data",{true,true,true});
  Bytes first(kNativeExtentBytes),second(kNativeExtentBytes-37);
  if(corpus) {
    const auto content=ReadOnlyFile::open(*corpus);
    require(content.size()>=first.size()+second.size(),"public fixture is shorter than the requested prefix");
    content.read_exact_at(0,first.data(),first.size());
    content.read_exact_at(first.size(),second.data(),second.size());
    (void)session.write(reader,0,first);
  }else {
    first=native_lz4_fixture::source();
    const auto block=native_lz4_fixture::alternative_block(first);
    const NativeLz4ExtentCandidate hint{0,block};
    (void)session.write_storage_with_lz4_result(reader,0,first,{&hint,1});
    std::copy_n(first.begin(),second.size(),second.begin());
    second[31]^=std::byte{1};
  }
  (void)session.write(reader,kNativeExtentBytes,second);
  const auto first_size=static_cast<std::uint32_t>(first.size());
  const auto second_size=static_cast<std::uint32_t>(second.size());
  std::array<NativeLz4CapturedFrame,2> captured;
  using Clock=std::chrono::steady_clock;
  const auto capture_begin=Clock::now();
  {
    auto first_lease=session.lease_native_lz4_capture(reader,0,kNativeExtentBytes);
    auto second_lease=session.lease_native_lz4_capture(reader,kNativeExtentBytes,kNativeExtentBytes);
    require(first_lease && second_lease && first_lease->capture(captured[0]) &&
        second_lease->capture(captured[1]),"native read-ahead did not capture both selected frames");
  }
  const auto capture_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-capture_begin).count();
  const std::array sources{
      NativeLz4DecodeSource{captured[0].key(),captured[0].encoded()},
      NativeLz4DecodeSource{captured[1].key(),captured[1].encoded()}};
  constexpr NativeLz4DecodeGrant grant{2,256U*1024U};
  const auto exchange=NativeLz4ReadExchange::prepare_captured(captured,detail::next_gpu_request_id(),grant);
  const WorkerChannelCapacity capacity{exchange.request()->bytes().size(),exchange.reply_capacity()};
  const auto first_batch=NativeLz4DecodeBatch::prepare({&sources[0],1},grant);
  const auto second_batch=NativeLz4DecodeBatch::prepare({&sources[1],1},grant);
  const auto peak=(std::max)(first_batch.upload().size(),second_batch.upload().size());

  WindowsGpuBudgetSampler gpu(luid,1);
  const auto available=gpu.sample().snapshot;
  const auto device_reserve=prepared?grant.maximum_slab_bytes:peak;
  require(check_gpu_memory_budget(available,luid,1,GetTickCount64(),device_reserve)==GpuGateCode::admitted,
      "current AMD GPU budget refused the probe");
  const hip_detail::VerifiedSystemModule system(runtime_name,ascii(runtime_sha));
  const auto native=module_path(nullptr).parent_path()/"flitzfs_rocm_native_lz4_decode.dll";
  const hip_detail::DirectoryPin pin(native.parent_path());
  const auto pinned=ReadOnlyFile::open(native);
  require(hip_detail::matches_sha256(pinned,ascii(module_sha)),"native decode module hash mismatch");
  auto backing_before=backing->read_bytes;
  const auto cpu_start=Clock::now();
  require(session.read(reader,0,first_size)==first &&
      session.read(reader,kNativeExtentBytes,second_size)==second,
      "matched CPU fixture reads failed");
  const auto cpu_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now()-cpu_start).count();
  const auto cpu_backing=backing->read_bytes-backing_before;
  constexpr ProcessBudget child{160,1200,14000,16};
  require(valid_worker_channel_capacity(capacity,child),"native LZ4 worker channel refused");
  std::wstring worker_module_sha(module_sha);
  if(refuse_module)worker_module_sha.front()=worker_module_sha.front()==L'0'?L'1':L'0';
  const std::vector<std::wstring> arguments{prepared?L"--driver-native-lz4-prepared-read-adapter":
      L"--driver-native-lz4-read-adapter",std::to_wstring(ordinal),
      std::wstring(adapter),L"1",std::wstring(runtime_sha),std::move(worker_module_sha),std::wstring(architecture),
      std::to_wstring(wave_mask),std::to_wstring(capacity.request_bytes),std::to_wstring(capacity.reply_bytes),
      std::to_wstring(grant.maximum_frames),std::to_wstring(grant.maximum_slab_bytes),L"1"};
  const auto executable=native.parent_path()/"flitzfs_native_lz4_read_worker.exe";
  const auto started=Clock::now();
  auto work=prepared?WorkerExchangeSession::start_prepared_sized(executable,arguments,capacity,child):
      WorkerExchangeSession::start_sized(executable,arguments,exchange.request(),capacity,child);
  require(session.read(reader,0,first_size)==first,"pending worker prevented immediate CPU file read");
  const auto done=static_cast<HANDLE>(work.supervisor_wait_handle());
  require(done!=nullptr,"native LZ4 worker has no retirement handle");
  auto submitted=started;
  std::int64_t ready_ns=0;
  if(prepared && !refuse_module) {
    const auto deadline=GetTickCount64()+child.timeout_ms;
    while(!work.wait_prepared()) {
      require(GetTickCount64()<deadline && WaitForSingleObject(done,10)==WAIT_TIMEOUT,
          "native LZ4 worker did not become prepared");
    }
    require(!work.try_result(),"prepared readiness exposed a result before submission");
    submitted=Clock::now();
    ready_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(submitted-started).count();
    if(trial==Trial::discard_prepared)require(work.request_cancel(),"native LZ4 prepared discard signal");
    else require(work.submit_prepared(exchange.request()),"native LZ4 prepared submit");
  }
  require(WaitForSingleObject(done,child.timeout_ms+2500)==WAIT_OBJECT_0 && work.retirement_confirmed(),
      "native LZ4 child retirement unconfirmed");
  const auto* result=work.try_result();
  if(trial==Trial::discard_prepared) {
    require(result && result->code==WorkerExchangeCode::cancelled && result->process.exit_confirmed &&
        result->process.exit_code==0 && !result->publication_observed && result->response.empty(),
        "unused prepared decoder did not retire without publication");
    require(session.read(reader,0,first_size)==first &&
        session.read(reader,kNativeExtentBytes,second_size)==second,"native CPU reads changed after prepared discard");
    std::cout<<"native_lz4_prepared_discard {\"ready_ns\":"<<ready_ns
        <<",\"gpu_kernel_launched\":false,\"exact_child_retired\":true,\"cpu_fallback_exact\":true}\n";
    return;
  }
  if(refuse_module) {
    require(result && result->code==WorkerExchangeCode::worker_failed && result->process.exit_confirmed &&
        result->process.exit_code==static_cast<std::uint32_t>(HipWorkerExit::integrity_failure) &&
        result->response.empty(),"wrong module identity did not refuse the worker before device access");
    require(session.read(reader,0,first_size)==first &&
        session.read(reader,kNativeExtentBytes,second_size)==second,"indexed reads changed after worker refusal");
    std::cout<<"native_lz4_refusal {\"worker_refused_module\":true,\"gpu_launched\":false,"
        "\"exact_child_retired\":true,\"cpu_fallback_exact\":true,\"prepared\":"
        <<(prepared?"true":"false")<<"}\n";
    return;
  }
  if(!result || result->code!=WorkerExchangeCode::completed || !result->process.exit_confirmed) {
    if(result)std::cerr<<"native read worker exit="<<result->process.exit_code<<" state="
        <<static_cast<unsigned>(result->code)<<'\n';
    throw std::runtime_error("native LZ4 worker did not complete");
  }
  auto ready=exchange.accept(result->response,WorkerExecution::hip);
  const auto ended=Clock::now();
  const auto exchange_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(ended-started).count();
  const auto after_submit_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(ended-submitted).count();
  backing_before=backing->read_bytes;
  const auto ready_start=Clock::now();
  require(session.read(reader,0,first_size,ready.get())==first &&
      session.read(reader,kNativeExtentBytes,second_size,ready.get())==second &&
      ready->indexed_consumed()==2,"verified AMD bytes did not reach both indexed ready reads");
  const auto ready_read_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-ready_start).count();
  const auto ready_backing=backing->read_bytes-backing_before;
  require(cpu_backing>=ready_backing+sources[0].encoded.size()+sources[1].encoded.size(),
      "indexed AMD ready hits reread their compressed payloads");
  require(session.read(reader,0,first_size,ready.get())==first && ready->consumed()==2,
      "CPU fallback after consuming ready results failed");
  session.snapshot().verify_all();
  std::cout<<"native_lz4_hip {\"architecture\":\""<<arch
      <<"\",\"frames\":2,\"gpu_launched\":true,\"bytes_exact\":true,\"cpu_fallback_exact\":true"
      <<",\"first_decoded_bytes\":"<<sources[0].key.decoded_bytes
      <<",\"second_decoded_bytes\":"<<sources[1].key.decoded_bytes
      <<",\"exact_child_retired\":true,\"pending_cpu_read_exact\":true"
      <<",\"kernel_launches\":2,\"reused_device_slab_bytes\":"<<peak
      <<",\"exchange_ns\":"<<exchange_ns<<",\"device_reserved_bytes\":"<<device_reserve
      <<",\"prepared\":"<<(prepared?"true":"false")<<",\"ready_ns\":"<<ready_ns
      <<",\"after_submit_ns\":"<<after_submit_ns<<",\"capture_ns\":"<<capture_ns<<",\"leased_frames\":2"
      <<",\"cpu_session_read_ns\":"<<cpu_ns<<",\"ready_session_read_ns\":"<<ready_read_ns
      <<",\"cpu_backing_bytes\":"<<cpu_backing<<",\"ready_backing_bytes\":"<<ready_backing
      <<",\"indexed_hits\":"<<ready->indexed_consumed()
      <<",\"public_corpus\":"<<(corpus?"true":"false")<<"}\n";
}
}

int wmain(int argc,wchar_t** argv) {
  try {
    require(argc>=7 && argc<=9,"expected runtime name and hash, module hash, LUID, architecture and HIP ordinal");
    auto trial=Trial::cold;
    int input=7;
    if(argc>input) {
      const std::wstring_view mode=argv[input];
      if(mode==L"--prepared-native-read")trial=Trial::prepared;
      else if(mode==L"--discard-prepared-read")trial=Trial::discard_prepared;
      else if(mode==L"--refuse-native-module")trial=Trial::refuse_module;
      else if(mode==L"--refuse-prepared-native-module")trial=Trial::refuse_prepared_module;
      else require(!mode.starts_with(L"--"),"unknown native LZ4 trial mode");
      if(trial!=Trial::cold)++input;
    }
    require(argc<=input+1,"unexpected native LZ4 input");
    const auto corpus=argc==input+1?std::optional{std::filesystem::path(argv[input])}:std::nullopt;
    run(argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],corpus,trial);
  }catch(const flitzfs::HipWorkerFailure& failure) {
    std::cerr<<failure.what()<<" (stage "<<static_cast<unsigned>(failure.stage)<<")\n";
    return 2;
  }catch(const std::exception& failure) {
    std::cerr<<failure.what()<<'\n';
    return 1;
  }
}
