#include "hip_decode_worker.hpp"
#include "flitzfs/gpu_match_layout.hpp"
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

void require_hip_worker_job() {
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
      limits.ProcessMemoryLimit==128ULL*1024*1024 && limits.JobMemoryLimit==limits.ProcessMemoryLimit &&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) &&
      cpu.CpuRate==2000, "unexpected HIP worker job limits",HipWorkerStage::worker_job);
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

}
