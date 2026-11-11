#include "hip_decode_worker.hpp"
#include "gpu_budget.hpp"
#include "hip_worker_exit.hpp"
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

void check_hip(hipError_t status, const char* operation) {
  if (status == hipSuccess) return;
  auto code = HipWorkerExit::device_fault;
  if (status == hipErrorOutOfMemory) code = HipWorkerExit::temporary_pressure;
  if (status == hipErrorInsufficientDriver) code = HipWorkerExit::runtime_unavailable;
  if (status == hipErrorNoDevice || status == hipErrorInvalidDevice || status == hipErrorNotSupported)
    code = HipWorkerExit::unsupported_configuration;
  throw HipWorkerFailure(code, operation);
}

template<class Function>
Function required_export(HMODULE module, const char* name) {
  const auto address = GetProcAddress(module, name);
  if (!address)
    throw HipWorkerFailure(HipWorkerExit::runtime_unavailable, name);
  return std::bit_cast<Function>(address);
}

struct HipApi {
  decltype(&hipInit) init;
  decltype(&hipDeviceGet) get_device;
  decltype(&hipDeviceGetLuid) get_luid;
  decltype(&hipSetDevice) set_device;
  decltype(&hipDeviceGetStreamPriorityRange) priority_range;
  decltype(&hipStreamCreateWithPriority) create_stream;
  decltype(&hipStreamDestroy) destroy_stream;
  decltype(&hipStreamSynchronize) synchronize;
  decltype(&hipMalloc) allocate;
  decltype(&hipFree) release;
  decltype(&hipMemcpy) copy;
  decltype(&hipLaunchKernel) launch;
};

HipApi bind_api(HMODULE runtime) {
  return {
    required_export<decltype(&hipInit)>(runtime, "hipInit"),
    required_export<decltype(&hipDeviceGet)>(runtime, "hipDeviceGet"),
    required_export<decltype(&hipDeviceGetLuid)>(runtime, "hipDeviceGetLuid"),
    required_export<decltype(&hipSetDevice)>(runtime, "hipSetDevice"),
    required_export<decltype(&hipDeviceGetStreamPriorityRange)>(runtime, "hipDeviceGetStreamPriorityRange"),
    required_export<decltype(&hipStreamCreateWithPriority)>(runtime, "hipStreamCreateWithPriority"),
    required_export<decltype(&hipStreamDestroy)>(runtime, "hipStreamDestroy"),
    required_export<decltype(&hipStreamSynchronize)>(runtime, "hipStreamSynchronize"),
    required_export<decltype(&hipMalloc)>(runtime, "hipMalloc"),
    required_export<decltype(&hipFree)>(runtime, "hipFree"),
    required_export<decltype(&hipMemcpy)>(runtime, "hipMemcpy"),
    required_export<decltype(&hipLaunchKernel)>(runtime, "hipLaunchKernel"),
  };
}

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
  check_hip(api.init(0), "initialize");
  hipDevice_t device{};
  check_hip(api.get_device(&device, ordinal), "lookup device");
  std::array<std::byte,8> actual_luid{};
  unsigned actual_mask = 0;
  check_hip(api.get_luid(reinterpret_cast<char*>(actual_luid.data()), &actual_mask, device), "query device LUID");
  if (actual_luid != luid || actual_mask != node_mask)
    throw HipWorkerFailure(HipWorkerExit::unsupported_configuration, "HIP/DXGI adapter identity mismatch");
  check_hip(api.set_device(ordinal), "select device");
  int least = (std::numeric_limits<int>::min)();
  int greatest = (std::numeric_limits<int>::max)();
  check_hip(api.priority_range(&least, &greatest), "query stream priorities");
  require(least >= greatest, "HIP stream priority range unavailable");

  DeviceResources resources(api);
  const auto before = query_windows_gpu_budget(luid, node_mask).snapshot;
  require(check_gpu_memory_budget(before, luid, node_mask, GetTickCount64(), allocation)
      == GpuGateCode::admitted && before.nonlocal_usage <= before_init.nonlocal_usage,
      "HIP device budget changed");
  check_hip(api.create_stream(&resources.stream, hipStreamNonBlocking, least), "create stream");

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
}
