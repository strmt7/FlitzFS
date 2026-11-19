#include "hip_decode_worker.hpp"
#include "anchor_batch_profile.hpp"
#include "flitzfs/gpu_match_layout.hpp"
#include "flitzfs/process_budget.hpp"
#include "gpu_budget.hpp"
#include "gpu_topology.hpp"
#include "hip_worker_budget.hpp"
#include "hip_worker_exit.hpp"
#include "hip_api.hpp"
#include "hip_device_resources.hpp"
#include <hip/hip_runtime_api.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <utility>

namespace flitzfs {
namespace {
void require(bool condition,const char* message,HipWorkerStage stage=HipWorkerStage::worker_request) {
  if(!condition)throw HipWorkerFailure(HipWorkerExit::device_fault,message,stage);
}

using hip_detail::HipApi;
void check_hip(hipError_t status, const char* operation) {
  hip_detail::check_hip(status, operation, HipWorkerStage::execution);
}
using hip_detail::bind_api;
using hip_detail::initialize_device;

}

void require_hip_worker_job(bool resource_sized) {
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  require(QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
              &limits, sizeof(limits), nullptr) &&
          QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation,
              &cpu, sizeof(cpu), nullptr), "missing HIP worker job",HipWorkerStage::worker_job);
  constexpr DWORD required=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  require((limits.BasicLimitInformation.LimitFlags&required)==required &&
      !(limits.BasicLimitInformation.LimitFlags&(JOB_OBJECT_LIMIT_BREAKAWAY_OK|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) &&
      limits.BasicLimitInformation.ActiveProcessLimit==1 &&
      limits.BasicLimitInformation.PriorityClass==BELOW_NORMAL_PRIORITY_CLASS &&
      limits.JobMemoryLimit==limits.ProcessMemoryLimit &&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP),
      "unexpected HIP worker job limits",HipWorkerStage::worker_job);
  constexpr std::size_t mib=1024*1024;
  if(!resource_sized) {
    require(limits.ProcessMemoryLimit==128*mib && cpu.CpuRate==2000,
        "unexpected fixed HIP worker job limits",HipWorkerStage::worker_job);
    return;
  }
  const ProcessBudget enforced{limits.ProcessMemoryLimit/mib,cpu.CpuRate,15000,16};
  require(limits.ProcessMemoryLimit%mib==0 && valid_process_budget(enforced),
      "invalid sized HIP worker job limits",HipWorkerStage::worker_job);
  MEMORYSTATUSEX memory{};memory.dwLength=sizeof(memory);
  if(!GlobalMemoryStatusEx(&memory) ||
      !valid_process_budget(plan_resource_process_budget(limits.ProcessMemoryLimit,
          memory.ullAvailPhys,memory.ullAvailPageFile,enforced.memory_share_divisor,enforced.cpu_rate,enforced.timeout_ms)))
    throw HipWorkerFailure(HipWorkerExit::temporary_pressure,"sized HIP worker lacks live headroom",HipWorkerStage::worker_job);
}

using hip_detail::DeviceResources;

GpuBatchReply run_hip_decode_in_worker(HMODULE runtime,
    const gpu_detail::KernelApi& kernel, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask, const WorkerBatchRequest& request,HipMemoryObservation* observation) {
  require_hip_worker_job();
  require(runtime && gpu_detail::compatible_kernel_api(kernel) && ordinal >= 0 &&
      ordinal < 64 && request.request_id, "invalid HIP worker configuration");
  const auto payload = request.batch.payload();
  const auto tasks = request.batch.tasks();
  const auto reference = request.batch.cpu_reference();
  require(!tasks.empty() && tasks.size() <= gpu_detail::max_tasks &&
      !reference.empty() && reference.size() <= gpu_detail::max_batch_bytes &&
      payload.size() <= gpu_detail::max_batch_bytes, "HIP batch bounds");
  const auto before_init = query_windows_gpu_budget(luid, node_mask).snapshot;
  const auto allocation = request.batch.device_buffer_bytes();
  if(observation){observation->snapshots[0]=before_init;observation->captured|=1;}
  require_hip_memory(before_init,luid,node_mask,GetTickCount64(),allocation,HipMemoryPoint::pre_initialize);

  const auto api = bind_api(runtime);
  const auto least=initialize_device(api,ordinal,luid,node_mask,[](auto identity) {
    return verified_single_gpu_node(query_windows_gpu_topology(identity),identity);
  });

  DeviceResources resources(api,observation);
  const auto before = query_windows_gpu_budget(luid, node_mask).snapshot;
  if(observation){observation->snapshots[1]=before;observation->captured|=2;}
  require_hip_memory(before,luid,node_mask,GetTickCount64(),allocation,
      HipMemoryPoint::initialized,&before_init);
  hip_detail::check_hip(api.create_stream(&resources.stream, hipStreamNonBlocking, least),
      "create stream", HipWorkerStage::stream_create);

  GpuBatchReply reply;
  reply.generation = request.request_id;
  reply.bytes.resize(reference.size());
  reply.status.assign(tasks.size(), (std::numeric_limits<std::uint32_t>::max)());
  const std::array<std::size_t,4> sizes{
    (std::max)(payload.size(), std::size_t{1}), tasks.size_bytes(),
    reply.bytes.size(), reply.status.size() * sizeof(reply.status[0]),
  };
  for (std::size_t i = 0; i < sizes.size(); ++i)
    check_hip(api.allocate(&resources.buffers[i], sizes[i]), "allocate batch buffer");

  if (!payload.empty())
    check_hip(api.copy(resources.buffers[0], payload.data(), payload.size(), hipMemcpyHostToDevice), "copy payload");
  check_hip(api.copy(resources.buffers[1], tasks.data(), sizes[1], hipMemcpyHostToDevice), "copy tasks");
  check_hip(api.copy(resources.buffers[3], reply.status.data(), sizes[3], hipMemcpyHostToDevice), "initialize status");

  auto input_size = static_cast<unsigned>(payload.size());
  auto task_count = static_cast<unsigned>(tasks.size());
  auto output_size = static_cast<unsigned>(reference.size());
  auto abi = gpu_detail::abi_version;
  void* args[]{
    &resources.buffers[0], &input_size, &resources.buffers[1], &task_count,
    &resources.buffers[2], &output_size, &resources.buffers[3], &abi,
  };
  const auto threads = gpu_detail::decode_threads;
  check_hip(api.launch(reinterpret_cast<const void*>(kernel.decode),
      dim3((task_count + threads - 1) / threads), dim3(threads), args, 0, resources.stream),
      "launch decode");
  if(observation)observation->flags|=hip_memory_launch_submitted;
  check_hip(api.synchronize(resources.stream), "complete decode");
  if(observation)observation->flags|=hip_memory_stream_completed;
  check_hip(api.copy(reply.status.data(), resources.buffers[3], sizes[3], hipMemcpyDeviceToHost), "read statuses");
  if (!std::all_of(reply.status.begin(), reply.status.end(), [](auto status) { return status == 0; }))
    throw HipWorkerFailure(HipWorkerExit::integrity_failure, "HIP decode task failed");
  if(observation)observation->flags|=hip_memory_statuses_verified;
  check_hip(api.copy(reply.bytes.data(), resources.buffers[2], sizes[2], hipMemcpyDeviceToHost), "read decoded bytes");
  if (!std::equal(reply.bytes.begin(), reply.bytes.end(), reference.begin()))
    throw HipWorkerFailure(HipWorkerExit::integrity_failure, "HIP output differs from CPU");

  if(observation)observation->cpu_verified_bytes=reference.size();
  const auto after = query_windows_gpu_budget(luid, node_mask).snapshot;
  if(observation){observation->snapshots[2]=after;observation->captured|=4;}
  require_hip_memory(after,luid,node_mask,GetTickCount64(),allocation,
      HipMemoryPoint::completed,&before_init);
  check_hip(resources.release_all(), "release HIP batch");
  if(observation)observation->flags|=hip_memory_admitted;
  return reply;
}
std::vector<match_detail::Match> run_hip_match_in_worker(HMODULE runtime,
    const match_detail::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const MatchWorkerRequest& request,HipPhaseRecorder* profile) {
  require_hip_worker_job();
  require(runtime && match_detail::compatible_kernel_api(kernel) && ordinal>=0 &&
      ordinal<64 && request.request_id,"invalid HIP match configuration");
  const auto input=request.search.input();
  const auto previous=request.search.predecessors();
  const auto allocation=request.search.device_buffer_bytes();
  require(input.size()>=4 && input.size()<=match_detail::input_limit && previous.size()==input.size() &&
      allocation<=gpu_detail::max_batch_bytes,"HIP match batch bounds");
  if(profile)profile->begin(HipPhase::device_initialize);
  const auto before_init=query_windows_gpu_budget(luid,node_mask).snapshot;
  require_hip_memory(before_init,luid,node_mask,GetTickCount64(),allocation,HipMemoryPoint::pre_initialize);
  const auto api=bind_api(runtime);
  const auto least=initialize_device(api,ordinal,luid,node_mask,[](auto identity) {
    return verified_single_gpu_node(query_windows_gpu_topology(identity),identity);
  });
  if(profile)profile->end(HipPhase::device_initialize);
  if(profile)profile->begin(HipPhase::stream_allocate);
  DeviceResources resources(api);
  const auto before=query_windows_gpu_budget(luid,node_mask).snapshot;
  require_hip_memory(before,luid,node_mask,GetTickCount64(),allocation,
      HipMemoryPoint::initialized,&before_init);
  hip_detail::check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,least),
      "create match stream",HipWorkerStage::stream_create);
  // Every unwritten lane is invalid at CPU admission.
  const match_detail::Match pending{65535,65535,65535,65535};
  std::vector<match_detail::Match> matches(request.search.lanes(),pending);
  const auto layout=gpu_match_device_layout(static_cast<std::uint32_t>(input.size()));
  const auto upload=gpu_match_device_upload(request.search);
  require(upload.size()==allocation && layout.total_bytes==allocation,"match slab admission differs");
  check_hip(api.allocate(&resources.buffers[0],upload.size()),"allocate match slab");
  const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
  require(address && address%8==0 &&
      address<=(std::numeric_limits<std::uintptr_t>::max)()-layout.total_bytes,
      "invalid match slab address",HipWorkerStage::execution);
  void* device_input=resources.buffers[0];
  void* device_previous=reinterpret_cast<void*>(address+layout.previous_offset);
  void* device_output=reinterpret_cast<void*>(address+layout.output_offset);
  if(profile)profile->end(HipPhase::stream_allocate);
  if(profile)profile->begin(HipPhase::host_to_device);
  check_hip(api.copy(device_input,upload.data(),upload.size(),hipMemcpyHostToDevice),"copy match slab");
  if(profile)profile->end(HipPhase::host_to_device);
  auto size=static_cast<unsigned>(input.size());
  auto previous_count=static_cast<unsigned>(previous.size());
  auto count=static_cast<unsigned>(matches.size());
  auto version=match_detail::abi_version;
  void* arguments[]{&device_input,&size,&device_previous,&previous_count,
      &device_output,&count,&version};
  const auto threads=match_detail::threads;
  if(profile)profile->begin(HipPhase::kernel_wait);
  check_hip(api.launch(reinterpret_cast<const void*>(kernel.search),
      dim3((count+threads-1)/threads),dim3(threads),arguments,0,resources.stream),"launch matches");
  check_hip(api.synchronize(resources.stream),"complete matches");
  if(profile)profile->end(HipPhase::kernel_wait);
  if(profile)profile->begin(HipPhase::device_to_host);
  check_hip(api.copy(matches.data(),device_output,matches.size()*sizeof(matches[0]),hipMemcpyDeviceToHost),"read matches");
  if(profile)profile->end(HipPhase::device_to_host);
  if(profile)profile->begin(HipPhase::cpu_verification);
  try {
    (void)request.search.verified_proposals(matches);
    // Qualification compares every field with CPU semantics, in addition to
    // independent byte validation. This qualification path duplicates CPU work.
    for(unsigned lane=0;lane<count;++lane) {
      const auto expected=match_detail::search(reinterpret_cast<const unsigned char*>(input.data()),size,
          previous.data(),previous_count,lane);
      if(matches[lane]!=expected)throw std::runtime_error("match output differs from CPU semantics");
    }
  }catch(const std::bad_alloc&) {throw;}
  catch(const std::exception&) {
    throw HipWorkerFailure(HipWorkerExit::integrity_failure,"HIP match output failed CPU verification");
  }
  if(profile)profile->end(HipPhase::cpu_verification);
  if(profile)profile->begin(HipPhase::post_admission);
  const auto after=query_windows_gpu_budget(luid,node_mask).snapshot;
  require_hip_memory(after,luid,node_mask,GetTickCount64(),allocation,
      HipMemoryPoint::completed,&before_init);
  if(profile)profile->end(HipPhase::post_admission);
  if(profile)profile->begin(HipPhase::resources_release);
  check_hip(resources.release_all(),"release HIP matches");
  if(profile)profile->end(HipPhase::resources_release);
  return matches;
}

namespace {
// Keep the launch arguments and slab alive through the last ordered command.
// A pressure or launch error after submission still drains the owned stream.
struct AnchorStreamWork {
  const DeviceResources& resources;
  bool pending=false;
  hipError_t wait() noexcept {
    pending=false;
    return resources.api.synchronize(resources.stream);
  }
  ~AnchorStreamWork() {if(pending)(void)wait();}
};

std::vector<std::vector<anchor_match::u32>> run_hip_anchor_groups(HMODULE runtime,
    const anchor_match::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,std::span<const detail::AnchorMatchSource> requests,
    std::span<const detail::AnchorMatchSweepPlan> plans,anchor_match::u32 quantum,HipPhaseRecorder& profile,
    anchor_match::u32 transfer_members,anchor_match::WindowSearchKernel window_search=nullptr,
    std::uint32_t dispatches_per_wait=1,anchor_match::BatchOutput mode=anchor_match::BatchOutput::anchors,
    AnchorSetupTiming* setup=nullptr) {
  namespace match=anchor_match;
  const bool packed=mode!=match::BatchOutput::anchors;
  const bool modular=mode==match::BatchOutput::modular_delta;
  const auto slot_bytes=match::reply_member_bytes(mode);
  const auto words_per_member=slot_bytes/static_cast<match::u32>(sizeof(match::u32));
  require(runtime && match::compatible_kernel_api(kernel) && ordinal>=0 && ordinal<64 &&
      !requests.empty() && requests.size()==plans.size() && quantum && quantum<=match::dispatch_member_limit(mode) &&
      transfer_members>=quantum && dispatches_per_wait && dispatches_per_wait<=kAnchorProfileMaxQueuedDispatches &&
      (window_search || (!packed && transfer_members==quantum && dispatches_per_wait==1)),
      "invalid HIP anchor match configuration");
  for(std::size_t i=0;i<requests.size();++i) {
    const auto& plan=plans[i];
    const auto count=detail::anchor_match_members(requests[i].source);
    require(requests[i].id && plan.quantum_members<=quantum &&
        plan==detail::plan_anchor_match_sweep(count,plan.quantum_members,plan.device_bytes,plan.windows,mode),
        "anchor match sweep differs from its admitted plan");
  }
  const auto index_bytes=packed?0U:match::index_bytes;
  const auto allocation=(1+transfer_members)*match::extent_bytes+index_bytes+transfer_members*slot_bytes;
  profile.begin(HipPhase::device_initialize);
  WindowsGpuBudgetSampler sampler(luid,node_mask);
  const auto before_init=sampler.sample().snapshot;
  require_hip_memory(before_init,luid,node_mask,GetTickCount64(),allocation,HipMemoryPoint::pre_initialize);

  const auto api=bind_api(runtime);
  const auto memset_address=GetProcAddress(runtime,"hipMemsetAsync");
  if (!memset_address) throw HipWorkerFailure(HipWorkerExit::runtime_unavailable,
      "hipMemsetAsync",HipWorkerStage::runtime_api);
  const auto clear=std::bit_cast<decltype(&hipMemsetAsync)>(memset_address);
  const auto least=initialize_device(api,ordinal,luid,node_mask,[](auto identity) {
    return verified_single_gpu_node(query_windows_gpu_topology(identity),identity);
  });
  if(packed) {
    hipDeviceProp_t properties{};
    hip_detail::check_hip(api.properties(&properties,ordinal),"packed group launch properties",
        HipWorkerStage::device_properties);
    if((!modular && ((properties.warpSize!=32 && properties.warpSize!=64) || !properties.arch.hasWarpShuffle)) ||
        (modular && properties.sharedMemPerBlock<sizeof(match::ModularDeltaScratch)) ||
        properties.maxThreadsPerBlock<static_cast<int>(match::threads) ||
        properties.maxThreadsDim[0]<static_cast<int>(match::threads) ||
        properties.maxGridSize[0]<static_cast<int>(quantum))
      throw HipWorkerFailure(HipWorkerExit::unsupported_configuration,
          "packed group device geometry unavailable",HipWorkerStage::device_properties);
  }
  const auto before=sampler.sample().snapshot;
  require_hip_memory(before,luid,node_mask,GetTickCount64(),allocation,HipMemoryPoint::initialized,&before_init);
  profile.end(HipPhase::device_initialize);

  profile.begin(HipPhase::stream_allocate);
  DeviceResources resources(api);
  const auto stream_started=setup?hip_phase_detail::clock_ns():0;
  hip_detail::check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,least),
      "create anchor match stream",HipWorkerStage::stream_create);
  const auto allocation_started=setup?hip_phase_detail::clock_ns():0;
  check_hip(api.allocate(&resources.buffers[0],allocation),"allocate anchor match slab");
  if(setup)*setup={allocation_started-stream_started,hip_phase_detail::clock_ns()-allocation_started};
  const auto address=reinterpret_cast<std::uintptr_t>(resources.buffers[0]);
  require(address && address%8==0 && address<=(std::numeric_limits<std::uintptr_t>::max)()-allocation,
      "invalid anchor match slab address",HipWorkerStage::execution);
  void* source=resources.buffers[0];
  const auto window_input=(1+transfer_members)*match::extent_bytes;
  void* member_input=reinterpret_cast<void*>(address+match::extent_bytes);
  void* table=reinterpret_cast<void*>(address+window_input);
  void* output=reinterpret_cast<void*>(address+window_input+index_bytes);
  profile.end(HipPhase::stream_allocate);

  auto version=match::abi_version;
  auto window_version=match::window_version(mode);
  void* index_arguments[]{&source,&table,&version};
  struct Dispatch {
    void* input=nullptr;
    void* result=nullptr;
    match::u32 members=0;
    std::array<void*,6> arguments{};
  };
  std::array<Dispatch,kAnchorProfileMaxQueuedDispatches> dispatches{};
  for(auto& command:dispatches)command.arguments={&source,&command.input,&command.members,&table,&command.result,&window_version};
  AnchorStreamWork submitted{resources};
  const auto check_admitted_slab=[&] {
    profile.begin(HipPhase::post_admission);
    const auto after=sampler.sample().snapshot;
    require_hip_memory(after,luid,node_mask,GetTickCount64(),allocation,HipMemoryPoint::completed,&before_init);
    profile.end(HipPhase::post_admission);
  };
  std::vector<std::vector<match::u32>> all_records;
  all_records.reserve(requests.size());
  for(std::size_t group=0;group<requests.size();++group) {
    const auto& request=requests[group];
    const auto& plan=plans[group];
    const auto whole_members=plan.whole.members;
    auto& records=all_records.emplace_back(whole_members*words_per_member);
    for (match::u32 first=0;first<whole_members;) {
      const auto staged=(std::min)(whole_members-first,transfer_members);
      auto window=std::span(records).subspan(first*words_per_member,staged*words_per_member);
      profile.begin(HipPhase::host_to_device);
      if (first) {
        const auto* input=request.source.data()+(first+1)*match::extent_bytes;
        check_hip(api.copy(member_input,input,staged*match::extent_bytes,hipMemcpyHostToDevice),
            "upload next anchor match window");
        submitted.pending=true;
        check_hip(clear(output,255,window.size_bytes(),resources.stream),"clear next anchor match result");
      } else {
        check_hip(api.copy(source,request.source.data(),(1+staged)*match::extent_bytes,hipMemcpyHostToDevice),
            "upload shared base and first match window");
        submitted.pending=true;
        check_hip(clear(table,255,allocation-window_input,resources.stream),"clear anchor match slab");
      }
      profile.end(HipPhase::host_to_device);
      if(window_search)check_admitted_slab();
      if(!first && !packed) {
        profile.begin(HipPhase::kernel_wait);
        check_hip(api.launch(reinterpret_cast<const void*>(kernel.index),
            dim3(match::extent_bytes/match::threads),dim3(match::threads),index_arguments,0,resources.stream),
            "launch shared base index");
        profile.end(HipPhase::kernel_wait);
      }
      // Each dispatch retains the original work quantum. Its output stays in
      // a disjoint slab slice until the entire transfer is ready to download.
      std::uint32_t queued=0;
      for(match::u32 at=0;at<staged;) {
        auto& command=dispatches[queued];
        command.members=(std::min)(staged-at,plan.quantum_members);
        const auto blocks=dim3(packed?command.members:command.members*match::tiles/match::threads);
        profile.begin(HipPhase::kernel_wait);
        submitted.pending=true;
        if(window_search) {
          command.input=reinterpret_cast<void*>(address+(1+at)*match::extent_bytes);
          command.result=reinterpret_cast<void*>(address+window_input+index_bytes+at*slot_bytes);
          check_hip(api.launch(reinterpret_cast<const void*>(window_search),blocks,dim3(match::threads),
              command.arguments.data(),0,resources.stream),"launch resident anchor match window");
        } else {
          void* arguments[]{&source,&command.members,&table,&output,&version};
          check_hip(api.launch(reinterpret_cast<const void*>(kernel.search),blocks,dim3(match::threads),
              arguments,0,resources.stream),"launch anchor match window");
        }
        at+=command.members;
        if(++queued==dispatches_per_wait || at==staged) {
          check_hip(submitted.wait(),"complete anchor match windows");
          queued=0;
        }
        profile.end(HipPhase::kernel_wait);
        if(at<staged)check_admitted_slab();
      }
      profile.begin(HipPhase::device_to_host);
      check_hip(api.copy(window.data(),output,window.size_bytes(),hipMemcpyDeviceToHost),
          "read anchor match window");
      profile.end(HipPhase::device_to_host);
      profile.begin(HipPhase::cpu_verification);
      require(packed || std::none_of(window.begin(),window.end(),[](auto record){return record==match::empty;}),
          "incomplete anchor match window",HipWorkerStage::execution);
      profile.end(HipPhase::cpu_verification);
      check_admitted_slab();
      first+=staged;
    }
  }
  profile.begin(HipPhase::resources_release);
  check_hip(resources.release_all(),"release anchor match slab");
  profile.end(HipPhase::resources_release);
  return all_records;
}
}

std::vector<anchor_match::u32> run_hip_anchor_match_in_worker(HMODULE runtime,
    const anchor_match::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const detail::AnchorMatchRequest& request,const detail::AnchorMatchSweepPlan& plan,
    HipPhaseRecorder& profile,bool resource_sized) {
  require_hip_worker_job(resource_sized);
  const auto source=request.view();
  auto records=run_hip_anchor_groups(runtime,kernel,ordinal,luid,node_mask,
      std::span(&source,1),std::span(&plan,1),plan.quantum_members,profile,plan.quantum_members);
  return std::move(records.front());
}

std::vector<std::vector<anchor_match::u32>> run_hip_anchor_batch_in_worker(HMODULE runtime,
    const anchor_match::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const detail::AnchorMatchBatchRequest& request,HipPhaseRecorder& profile,
    const anchor_match::WindowSearchApi* window,std::uint32_t transfer_grant,
    std::uint32_t dispatches_per_wait,AnchorSetupTiming* setup) {
  require_hip_worker_job(true);
  const auto& plan=request.plan();
  const auto version=anchor_match::window_version(plan.output);
  require(window?anchor_match::compatible_window_search_api(*window,version):
      !transfer_grant && plan.output==anchor_match::BatchOutput::anchors,
      "invalid anchor transfer kernel API");
  const auto members=window?detail::plan_anchor_match_transfers(plan,transfer_grant).members_per_transfer:
      plan.quantum_members;
  return run_hip_anchor_groups(runtime,kernel,ordinal,luid,node_mask,
      request.groups(),plan.groups,plan.quantum_members,profile,members,window?window->search:nullptr,
      dispatches_per_wait,plan.output,setup);
}

}
