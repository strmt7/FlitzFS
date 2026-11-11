#include "hip_decode_worker.hpp"
#include "gpu_budget.hpp"
#include "hip_worker_exit.hpp"
#include "hip_api.hpp"
#include <hip/hip_runtime_api.h>
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace flitzfs {
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
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
              &cpu, sizeof(cpu), nullptr), "missing HIP worker job");
  constexpr DWORD required=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|
      JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY|JOB_OBJECT_LIMIT_PRIORITY_CLASS;
  require((limits.BasicLimitInformation.LimitFlags&required)==required &&
      !(limits.BasicLimitInformation.LimitFlags&(JOB_OBJECT_LIMIT_BREAKAWAY_OK|JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) &&
      limits.BasicLimitInformation.ActiveProcessLimit==1 &&
      limits.BasicLimitInformation.PriorityClass==BELOW_NORMAL_PRIORITY_CLASS &&
      limits.ProcessMemoryLimit==128ULL*1024*1024 && limits.JobMemoryLimit==limits.ProcessMemoryLimit &&
      cpu.ControlFlags==(JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP) &&
      cpu.CpuRate==2000, "unexpected HIP worker job limits");
}

namespace {
struct DeviceResources {
  const HipApi& api;
  hipStream_t stream = nullptr;
  std::array<void*,4> buffers{};

  explicit DeviceResources(const HipApi& functions) : api(functions) {}
  DeviceResources(const DeviceResources&) = delete;
  DeviceResources& operator=(const DeviceResources&) = delete;

  hipError_t release_all() noexcept {
    hipError_t first = hipSuccess;
    if (stream) {
      first = api.destroy_stream(stream);
      stream = nullptr;
    }
    for (auto& buffer : buffers) {
      if (!buffer) continue;
      const auto status = api.release(buffer);
      if (first == hipSuccess) first = status;
      buffer = nullptr;
    }
    return first;
  }
  ~DeviceResources() { (void)release_all(); }
};
}

GpuBatchReply run_hip_decode_in_worker(HMODULE runtime,
    const gpu_detail::KernelApi& kernel, int ordinal, std::array<std::byte,8> luid,
    std::uint32_t node_mask, const WorkerBatchRequest& request) {
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
  require(check_gpu_memory_budget(before_init, luid, node_mask, GetTickCount64(), allocation)
      == GpuGateCode::admitted, "HIP pre-initialization budget");

  const auto api = bind_api(runtime);
  const auto least=initialize_device(api,ordinal,luid,node_mask);

  DeviceResources resources(api);
  const auto before = query_windows_gpu_budget(luid, node_mask).snapshot;
  require(check_gpu_memory_budget(before, luid, node_mask, GetTickCount64(), allocation)
      == GpuGateCode::admitted && before.nonlocal_usage <= before_init.nonlocal_usage,
      "HIP device budget changed");
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
  check_hip(api.synchronize(resources.stream), "complete decode");
  check_hip(api.copy(reply.status.data(), resources.buffers[3], sizes[3], hipMemcpyDeviceToHost), "read statuses");
  if (!std::all_of(reply.status.begin(), reply.status.end(), [](auto status) { return status == 0; }))
    throw HipWorkerFailure(HipWorkerExit::integrity_failure, "HIP decode task failed");
  check_hip(api.copy(reply.bytes.data(), resources.buffers[2], sizes[2], hipMemcpyDeviceToHost), "read decoded bytes");
  if (!std::equal(reply.bytes.begin(), reply.bytes.end(), reference.begin()))
    throw HipWorkerFailure(HipWorkerExit::integrity_failure, "HIP output differs from CPU");

  const auto after = query_windows_gpu_budget(luid, node_mask).snapshot;
  require(check_gpu_memory_budget(after, luid, node_mask, GetTickCount64(), 0)
      == GpuGateCode::admitted && after.nonlocal_usage <= before.nonlocal_usage,
      "HIP decode residency changed");
  check_hip(resources.release_all(), "release HIP batch");
  return reply;
}
std::vector<match_detail::Match> run_hip_match_in_worker(HMODULE runtime,
    const match_detail::KernelApi& kernel,int ordinal,std::array<std::byte,8> luid,
    std::uint32_t node_mask,const MatchWorkerRequest& request) {
  require_hip_worker_job();
  require(runtime && match_detail::compatible_kernel_api(kernel) && ordinal>=0 &&
      ordinal<64 && request.request_id,"invalid HIP match configuration");
  const auto input=request.search.input();
  const auto previous=request.search.predecessors();
  const auto allocation=request.search.device_buffer_bytes();
  require(input.size()>=4 && input.size()<=match_detail::input_limit &&
      allocation<=gpu_detail::max_batch_bytes,"HIP match batch bounds");
  const auto before_init=query_windows_gpu_budget(luid,node_mask).snapshot;
  require(check_gpu_memory_budget(before_init,luid,node_mask,GetTickCount64(),allocation)==GpuGateCode::admitted,
      "HIP match pre-initialization budget");
  const auto api=bind_api(runtime);
  const auto least=initialize_device(api,ordinal,luid,node_mask);
  DeviceResources resources(api);
  const auto before=query_windows_gpu_budget(luid,node_mask).snapshot;
  require(check_gpu_memory_budget(before,luid,node_mask,GetTickCount64(),allocation)==GpuGateCode::admitted &&
      before.nonlocal_usage<=before_init.nonlocal_usage,"HIP match device budget changed");
  hip_detail::check_hip(api.create_stream(&resources.stream,hipStreamNonBlocking,least),
      "create match stream",HipWorkerStage::stream_create);
  // Every unwritten lane is invalid at CPU admission.
  const match_detail::Match pending{65535,65535,65535,65535};
  std::vector<match_detail::Match> matches(request.search.lanes(),pending);
  const std::array<std::size_t,3> sizes{input.size(),previous.size_bytes(),matches.size()*sizeof(matches[0])};
  for(std::size_t i=0;i<sizes.size();++i)
    check_hip(api.allocate(&resources.buffers[i],sizes[i]),"allocate match buffer");
  check_hip(api.copy(resources.buffers[0],input.data(),sizes[0],hipMemcpyHostToDevice),"copy match input");
  check_hip(api.copy(resources.buffers[1],previous.data(),sizes[1],hipMemcpyHostToDevice),"copy match index");
  check_hip(api.copy(resources.buffers[2],matches.data(),sizes[2],hipMemcpyHostToDevice),"initialize matches");
  auto size=static_cast<unsigned>(input.size());
  auto previous_count=static_cast<unsigned>(previous.size());
  auto count=static_cast<unsigned>(matches.size());
  auto version=match_detail::abi_version;
  void* arguments[]{&resources.buffers[0],&size,&resources.buffers[1],&previous_count,
      &resources.buffers[2],&count,&version};
  const auto threads=match_detail::threads;
  check_hip(api.launch(reinterpret_cast<const void*>(kernel.search),
      dim3((count+threads-1)/threads),dim3(threads),arguments,0,resources.stream),"launch matches");
  check_hip(api.synchronize(resources.stream),"complete matches");
  check_hip(api.copy(matches.data(),resources.buffers[2],sizes[2],hipMemcpyDeviceToHost),"read matches");
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
  const auto after=query_windows_gpu_budget(luid,node_mask).snapshot;
  require(check_gpu_memory_budget(after,luid,node_mask,GetTickCount64(),0)==GpuGateCode::admitted &&
      after.nonlocal_usage<=before.nonlocal_usage,"HIP match residency changed");
  check_hip(resources.release_all(),"release HIP matches");
  return matches;
}

}
