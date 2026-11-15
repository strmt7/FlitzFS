#pragma once
#include "native_deferred_write.hpp"
#include "gpu_budget.hpp"
#include "../gpu_probe_budget.hpp"
#include "../locked_ram_fixture.hpp"
#include "../native_lz4_fixture.hpp"
#include "../native_security_fixture.hpp"
#include "../../src/record_digest.hpp"
#include "../../src/store_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Explicit finite qualification only. No WinFsp call, mount or driver-management action.
namespace deferred_native_fixture {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
inline void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
enum class Invocation {hosted_cpu,explicit_cpu,gpu};
struct Configuration {
  Invocation invocation=Invocation::hosted_cpu;
  NativeUniformWorkerBinding worker;
  std::array<std::byte,8> luid{};
  std::uint32_t node_mask=0;
};
struct Completion {
  FSP_FILE_SYSTEM* expected=nullptr;
  std::array<std::byte,sizeof(FSP_FSCTL_TRANSACT_RSP)> bytes{};
  std::atomic<unsigned> count{0};
  bool identity_matches=true;
};
inline Completion* receiver=nullptr;
inline constexpr UINT64 request_hint=0xfedcba9876543210ULL;
inline bool hint(UINT64& value) noexcept{value=request_hint;return receiver!=nullptr;}
inline void respond(FSP_FILE_SYSTEM* filesystem,FSP_FSCTL_TRANSACT_RSP& response) noexcept{
  if(!receiver)return;
  receiver->identity_matches=receiver->identity_matches&&filesystem==receiver->expected;
  std::memcpy(receiver->bytes.data(),&response,sizeof(response));
  receiver->count.fetch_add(1,std::memory_order_release);
}
struct ReceiverOwner {
  explicit ReceiverOwner(Completion& completion){check(!receiver,"deferred fixture already active");receiver=&completion;}
  ~ReceiverOwner(){receiver=nullptr;}
};
inline std::uint64_t integer(std::span<const std::byte> bytes,std::size_t at,unsigned width){
  check(width&&width<=8&&at<=bytes.size()&&width<=bytes.size()-at,"deferred record bounds");
  std::uint64_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(8*i);
  return value;
}
inline bool stored_advice_matches(const std::shared_ptr<StoreByteIo>& backing,
    const NativeUniformWriteResult& completion,std::span<const std::byte> block){
  if(!completion.candidate_count)return false;
  if(!completion.advice.captured||!completion.advice.uniform_payloads)return false;
  auto objects=ObjectStoreSnapshot::open(backing);const auto info=objects.info();
  std::array<std::byte,4096> root{};
  backing->read_exact((info.generation%2)*kStoreCommitUnit,root);
  check(integer(root,16,8)==info.generation,"deferred root generation changed");
  const auto count=integer(root,44,4),index_bytes=integer(root,40,4);
  check(count&&count<=kStoreMaxObjects&&index_bytes==64+64*count,"deferred index shape");
  Bytes index(static_cast<std::size_t>(index_bytes));backing->read_exact(integer(root,32,8),index);
  unsigned matched=0;
  for(const auto& object:objects.list()){
    if(object.logical_bytes!=block.size()||objects.read(object.id)!=Bytes(block.begin(),block.end()))continue;
    bool located=false,exact=false;
    for(std::size_t i=0;i<count;++i){
      const auto at=64+64*i;if(integer(index,at,8)!=object.id)continue;
      check(!located,"deferred duplicate object identity");located=true;
      std::array<std::byte,128> record{};const auto offset=integer(index,at+16,8);
      backing->read_exact(offset,record);
      if(integer(record,12,4)!=4||integer(record,32,4)!=block.size())continue;
      const auto size=integer(record,36,4);if(!size||size>kNativeExtentBytes)continue;
      Bytes payload(static_cast<std::size_t>(size));backing->read_exact(offset+128,payload);
      const auto digest=detail::record_digest(payload);
      if(completion.advice.payload_bytes==size&&completion.advice.payload_digest==digest)exact=true;
    }
    check(located,"deferred object absent from current index");
    if(!exact)return false;
    ++matched;
  }
  return matched!=0;
}
inline int run(Configuration configuration,const gpu_probe_fixture::Budget& controller){
  const bool gpu=configuration.invocation==Invocation::gpu;
  check(gpu||configuration.invocation==Invocation::hosted_cpu||
      configuration.invocation==Invocation::explicit_cpu,"invalid deferred fixture invocation");
  // Invocation names retain historical report compatibility. All CPU modes
  // use the same resource-contained memory fixture; location is not admission.
  check(store_lz4_available()&&gpu_lz4_cpu_verification_available(),"independent deferred CPU recovery unavailable");
  check(!GetModuleHandleW(L"winfsp-x64.dll"),"deferred fixture unexpectedly loaded WinFsp runtime");
  check(WorkerExchangeSession::available(),"deferred fixture entered occupied transport");
  const auto block=native_lz4_fixture::source();Bytes input;
  for(unsigned i=0;i<16;++i)input.insert(input.end(),block.begin(),block.end());
  const auto expected=input;const Bytes sentinel{std::byte{1},std::byte{0x53},std::byte{0x7a},std::byte{9}};
  constexpr char hexadecimal[]="0123456789abcdef";
  std::string source_digest;source_digest.reserve(64);
  for(const auto byte:detail::record_digest(expected)){
    const auto value=std::to_integer<unsigned>(byte);
    source_digest.push_back(hexadecimal[value>>4]);source_digest.push_back(hexadecimal[value&15]);
  }
  NativeDeferredWriteStats stats;NativeUniformWriteResult committed{};
  std::uint64_t image_bytes=0,payload_bytes=0,elapsed_ms=0,elapsed_ns=0;
  bool stored_match=false,admitted=false;
  locked_ram_fixture::WorkingSet working;
  {
    auto backing=std::make_shared<locked_ram_fixture::Store>(2U*1024*1024);
    StoreIdentity id{};id[0]=std::byte{0x39};id[1]=std::byte{0x4b};
    auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(
        backing,id,{StoreCompressionMode::fast,6,false},{false}));
    NativeFileMutation create;create.operation=NativeFileOperation::create_file;create.path="candidate.bin";
    const NativeBasicMutation basic{0,1,{11,22,33,44,2}};
    const NativeSecurityMutation security{0,1,native_security_fixture::descriptor()};
    writer.apply_with_metadata({&create,1},{&basic,1},{&security,1});
    const auto inode=writer.snapshot().stat("candidate.bin").inode;
    writer.write_inode_storage_result(inode,0,sentinel);
    writer.reserve_inode_result(inode,kNativeAllocationUnit);
    auto session=NativeFileSession::create(std::move(writer));
    const auto handle=session.open("candidate.bin",{true,true,true});const auto prior=session.snapshot();
    std::mutex control;std::atomic<bool> offline{false};FSP_FILE_SYSTEM fake{};
    Completion completion;completion.expected=&fake;ReceiverOwner receiver_owner(completion);
    NativeDeferredWriteOptions options;options.expected=gpu?WorkerExecution::hip:WorkerExecution::cpu_semantics;
    options.hint_query=hint;options.response_sink=respond;
    options.binding=std::move(configuration.worker);options.budget={128,2000,5000};options.deadline_ms=5000;
    options.binding.capture_advice_receipt=true;
    GpuHealthEvidence evidence;evidence.sequence=1;evidence.supervisor_quiescent=true;
    if(gpu){
      const auto observed=query_windows_gpu_budget(configuration.luid,configuration.node_mask);
      admitted=observed.status==WindowsGpuBudgetStatus::success&&
          check_gpu_memory_budget(observed.snapshot,configuration.luid,configuration.node_mask,
              GetTickCount64(),0)==GpuGateCode::admitted;
      evidence.memory=observed.snapshot;evidence.compatible=admitted;
      Bytes identity(options.binding.runtime_sha256.begin(),options.binding.runtime_sha256.end());
      identity.insert(identity.end(),configuration.luid.begin(),configuration.luid.end());
      for(unsigned i=0;i<4;++i)identity.push_back(static_cast<std::byte>(configuration.node_mask>>(8*i)));
      identity.push_back(static_cast<std::byte>(options.binding.ordinal));
      evidence.configuration=detail::record_digest(identity);options.binding.configuration=evidence.configuration;
      uniform_lz4::Layout quantum{};
      check(uniform_lz4::make_layout(2U*kNativeExtentBytes,kNativeExtentBytes,quantum),"deferred diagnostic quantum");
      options.binding.sweep=NativeUniformSweepGrant{2,quantum.total_bytes};
      // A manually reviewed finite synthetic grant, NOT a measured reusable cost model.
      auto& cost=options.cost;cost.fixed_bytes=256ULL<<20;cost.input_copies=6;cost.slab_copies=2;
      cost.output_copies=3;cost.per_chunk_bytes=64;cost.configuration=evidence.configuration;
      cost.recovery_generation=evidence.recovery_generation;cost.profiled_chunk_bytes=kNativeExtentBytes;
      cost.maximum_grid_x=65535;cost.kernel_ns_per_chunk=10000000;
      cost.qualified_kernel_quantum_ns=200000000;cost.qualified=true;
      options.window={16,200000000};options.allow_gpu=admitted;
      options.workload_qualified=admitted;options.helper_independent=true;
    }
    {
      NativeDeferredWriteService service(session,control,offline,std::move(options));
      const auto request_started=std::chrono::steady_clock::now();
      const auto start=GetTickCount64();
      {
        std::lock_guard guard(control);service.publish_locked(evidence);
        check(!service.completion_receipt_locked(),"new deferred service retained a completion");
        check(service.enqueue_locked(&fake,handle,input,0,false,false),"deferred request not admitted");
        check(!service.completion_receipt_locked(),"pending deferred request exposed old completion");
      }
      std::fill(input.begin(),input.end(),std::byte{0xff});
      auto last_observation=start;
      while(completion.count.load(std::memory_order_acquire)==0){
        const auto now=GetTickCount64();
        if(now-start>7000){std::unique_lock guard(control);service.drain_locked(guard);break;}
        // Active-operation monitoring only: no device discovery or idle polling.
        // The service can select CPU independently if this diagnostic query fails/blocks.
        if(gpu&&admitted&&now-last_observation>=50){
          const auto observed=query_windows_gpu_budget(configuration.luid,configuration.node_mask);
          evidence.memory=observed.snapshot;evidence.compatible=observed.status==WindowsGpuBudgetStatus::success;
          ++evidence.sequence;
          {std::lock_guard guard(control);service.publish_locked(evidence);}
          last_observation=GetTickCount64();
        }
        Sleep(1);
      }
      NativeDeferredStop stopped;
      do{stopped=service.stop();if(stopped==NativeDeferredStop::running)Sleep(1);}
      while(stopped==NativeDeferredStop::running&&GetTickCount64()-start<10000);
      check(stopped==NativeDeferredStop::quiescent,"deferred service retirement unconfirmed");
      elapsed_ms=GetTickCount64()-start;
      const auto measured=std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now()-request_started).count();
      check(measured>0,"deferred request clock did not advance");
      elapsed_ns=static_cast<std::uint64_t>(measured);
      {
        std::lock_guard guard(control);stats=service.stats_locked();
        const auto receipt=service.completion_receipt_locked();check(receipt.has_value(),"deferred commit receipt absent");
        committed=*receipt;
      }
      FSP_FSCTL_TRANSACT_RSP response;std::memset(&response,0,sizeof(response));
      std::memcpy(&response,completion.bytes.data(),sizeof(response));
      check(completion.count.load()==1&&completion.identity_matches&&response.Hint==request_hint&&
          response.Kind==FspFsctlTransactWriteKind&&response.IoStatus.Status==STATUS_SUCCESS&&
          response.IoStatus.Information==expected.size()&&response.Rsp.Write.FileInfo.FileSize==expected.size(),
          "deferred response did not match its single committed write");
      check(!offline.load()&&stats.queued==1&&stats.completed==1&&stats.response_attempts==1&&
          stats.thread_exited&&stats.retirement_confirmed&&!stats.native_pending&&!stats.worker_busy&&!stats.disabled,
          "deferred terminal ownership differs");
    }
    session.cleanup(handle,false);session.close(handle);
    check(session.stats().contexts==0,"deferred context retained");
    auto objects=ObjectStoreSnapshot::open(backing);
    payload_bytes=objects.info().stored_payload_bytes;
    auto recovered=NativeFilesystemSnapshot::open(std::move(objects));
    recovered.verify_all();
    check(recovered.read("candidate.bin",0,static_cast<std::uint32_t>(expected.size()))==expected&&
        prior.read("candidate.bin",0,4)==sentinel,"deferred current/prior independent CPU readback");
    if(committed.candidate_count)stored_match=stored_advice_matches(backing,committed,block);
    image_bytes=backing->extent();
    backing->verify();
  }
  working.restore();
  check(locked_ram_fixture::locked_bytes==0&&locked_ram_fixture::cleanup_ok&&WorkerExchangeSession::available(),
      "deferred fixture resource retirement");
  check(!GetModuleHandleW(L"winfsp-x64.dll"),"deferred fixture loaded WinFsp runtime");
  const bool gpu_verified=gpu&&stats.advised!=0;
  const char* invocation=gpu?"gpu":configuration.invocation==Invocation::explicit_cpu?"explicit_cpu":"hosted_cpu";
  std::cout<<"Deferred native composition: {\"complete\":true,\"invocation\":\""<<invocation
      <<"\",\"gpu_requested\":"<<(gpu?"true":"false")
      <<",\"gpu_result_verified\":"<<(gpu_verified?"true":"false")
      <<",\"gpu_executed\":"<<(gpu_verified?"true":stats.worker_submitted?"null":"false")
      <<",\"stored_advice_digest_matches\":"<<(stored_match?"true":"false")
      <<",\"CPU_fallback_selected\":"<<(!gpu_verified?"true":"false")
      <<",\"source_bytes\":"<<expected.size()<<",\"source_digest_blake3\":\""<<source_digest
      <<"\",\"candidate_count\":"<<committed.candidate_count
      <<",\"candidate_bytes\":"<<committed.candidate_bytes<<",\"image_bytes\":"<<image_bytes
      <<",\"active_payload_bytes\":"<<payload_bytes<<",\"owned_request_ms\":"<<elapsed_ms
      <<",\"owned_request_ns\":"<<elapsed_ns
      <<",\"controller_job_peak_commit_bytes\":"<<controller.peak_commit()
      <<",\"locked_peak_bytes\":"<<locked_ram_fixture::peak_locked_bytes.load()
      <<",\"locked_after_bytes\":0,\"response_count\":1,\"owned_input\":true,\"control_joined\":true,"
        "\"current_CPU_readback\":true,\"prior_CPU_readback\":true,\"working_set_restored\":true,"
        "\"transport_quiescent\":true,\"winfsp_runtime_loaded\":false,\"mounted\":false,"
        "\"normal_io_route\":false,\"cost_profile_qualified\":false}\n";
  return gpu?((gpu_verified&&stored_match)?0:2):0;
}
enum class Comparison {cpu_only,matched};
inline int run_comparison(Configuration configuration,const gpu_probe_fixture::Budget& controller,
    Comparison comparison){
  check(comparison==Comparison::cpu_only||comparison==Comparison::matched,
      "invalid deferred comparison invocation");
  constexpr std::array<Invocation,6> order{Invocation::explicit_cpu,Invocation::gpu,
      Invocation::gpu,Invocation::explicit_cpu,Invocation::explicit_cpu,Invocation::gpu};
  const auto started=std::chrono::steady_clock::now();
  unsigned completed=0,gpu_completed=0;
  for(const auto invocation:order){
    // Do not start another request after the finite series admission window.
    // Each request retains its own deadlines, process containment and retirement.
    check(std::chrono::steady_clock::now()-started < std::chrono::seconds(5),
        "deferred comparison wall admission exhausted");
    const auto selected=comparison==Comparison::matched?invocation:Invocation::explicit_cpu;
    auto trial=configuration;trial.invocation=selected;
    if(selected!=Invocation::gpu){trial.worker={};trial.luid={};trial.node_mask=0;}
    const auto code=run(std::move(trial),controller);
    if(code!=0)return code; // No further work or automatic retry after any refusal.
    ++completed;
    if(selected==Invocation::gpu)++gpu_completed;
  }
  std::cout<<"Deferred native comparison: {\"complete\":true,\"trials\":"<<completed
      <<",\"GPU_trials\":"<<gpu_completed<<",\"CPU_trials\":"<<completed-gpu_completed
      <<",\"source_bytes_per_trial\":1048576,\"wall_admission_ms\":5000,"
        "\"controller_peak_scope\":\"cumulative across series\",\"mounted\":false,"
        "\"normal_io_route\":false,\"cost_profile_qualified\":false}\n";
  return 0;
}
} // namespace deferred_native_fixture
