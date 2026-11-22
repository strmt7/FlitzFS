#include "hip_uniform_lz4_reuse.hpp"
#include "gpu_topology.hpp"
#include "uniform_lz4_sweep_profile.hpp"
#include "flitzfs/uniform_lz4_sweep.hpp"
#include "bounded_process.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace flitzfs;
using namespace flitzfs::hip_detail;
using Bytes=std::vector<std::byte>;
unsigned checks=0,scenarios=0,refusals=0,successes=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void kernel32(unsigned char*,uniform_lz4::u64,uniform_lz4::Layout){}
void kernel64(unsigned char*,uniform_lz4::u64,uniform_lz4::Layout){}
const auto kernels=uniform_lz4::make_kernel_api(&kernel32,&kernel64);
const std::array<std::byte,8> identity{std::byte{7}};
constexpr Lz4KernelCoverage coverage{"gfx1100",3};

UniformLz4Request request(std::size_t bytes=16U*65536+37){
  Bytes input(bytes);for(std::size_t i=0;i<bytes;++i)input[i]=static_cast<std::byte>((i*17+i/101)%256);
  uniform_lz4::Layout p{};require(uniform_lz4::make_layout(bytes,65536,p),"uniform fixture layout");
  return {519,PreparedUniformLz4::prepare(input,uniform_lz4_limits(p))};
}
GpuMemorySnapshot baseline() {
  GpuMemorySnapshot s;s.luid=identity;s.node_mask=1;s.captured_ms=1000;
  s.dedicated_bytes=1ULL<<30;s.local_budget=s.dedicated_bytes;s.local_usage=4096;
  s.nonlocal_budget=8ULL<<30;s.nonlocal_usage=8192;s.valid=true;s.amd_hardware=true;
  s.physical_total=16ULL<<30;s.physical_available=8ULL<<30;
  s.commit_limit=32ULL<<30;s.commit_available=16ULL<<30;return s;
}
struct State {
  const UniformLz4Request& source;
  std::vector<std::uint64_t> arena;
  std::vector<std::string> calls;
  std::string fault,secondary_fault;
  hipDeviceProp_t properties{};
  bool stream_live=false,allocated=false,submitted=false,completed=false;
  bool bad_alignment=false,null_allocation=false,missing_attributes=false,inconsistent_attributes=false;
  unsigned property_queries=0,priority_queries=0,attribute_queries=0;
  int stream_tag=0;
  bool reuse=false,null_stream=false;
  std::uint64_t allocation_bytes=0,source_offset=0;
  std::size_t allocated_size=0;
  unsigned waits=0,completion_samples=0;uniform_lz4::Layout current_plan{};
  void* allocation=nullptr;
  explicit State(const UniformLz4Request& input,unsigned wave=32):source(input),arena((input.batch.layout().total_bytes+7)/8+1) {
    std::copy(identity.begin(),identity.end(),reinterpret_cast<std::byte*>(properties.luid));
    properties.luidDeviceNodeMask=1;properties.warpSize=static_cast<int>(wave);
    properties.maxThreadsPerBlock=1024;properties.maxThreadsDim[0]=1024;
    properties.maxThreadsDim[1]=properties.maxThreadsDim[2]=1;
    properties.maxGridSize[0]=65535;properties.maxGridSize[1]=properties.maxGridSize[2]=1;
    properties.sharedMemPerBlock=65536;
    std::memcpy(properties.gcnArchName,"gfx1100",8);
  }
  hipStream_t stream(){return reinterpret_cast<hipStream_t>(&stream_tag);}
  std::span<std::byte> bytes(){return {reinterpret_cast<std::byte*>(arena.data()),arena.size()*8};}
  hipError_t step(const std::string& name){
    calls.push_back(name);
    if(name=="wait" && ++waits==2 && fault=="reuse-wait-second")return hipErrorUnknown;
    return fault==name || secondary_fault==name?hipErrorUnknown:hipSuccess;
  }
};
State* active=nullptr;
struct Activate {
  explicit Activate(State& state){require(!active,"Nested fake API ownership");active=&state;}
  ~Activate(){active=nullptr;}
};
hipError_t init(unsigned flags){require(flags==0,"HIP init flags");return active->step("initialize");}
hipError_t properties(hipDeviceProp_t* out,int ordinal){
  require(ordinal==0,"HIP properties ordinal");++active->property_queries;
  const auto status=active->step("properties-"+std::to_string(active->property_queries));
  if(status==hipSuccess)*out=active->properties;return status;
}
hipError_t select(int ordinal){require(ordinal==0,"HIP selected ordinal");return active->step("select");}
hipError_t priorities(int* least,int* greatest){
  ++active->priority_queries;
  const auto status=active->step("priority-"+std::to_string(active->priority_queries));
  if(status==hipSuccess){*least=0;*greatest=-1;}return status;
}
hipError_t create_stream(hipStream_t* out,unsigned flags,int priority){
  require(flags==hipStreamNonBlocking && priority==0,"Lowest-priority stream admission");
  const auto status=active->step("stream-create");
  if(status==hipSuccess && active->null_stream){*out=nullptr;return status;}
  if(status==hipSuccess){require(!active->stream_live,"Duplicate stream");active->stream_live=true;*out=active->stream();}
  return status;
}
hipError_t destroy_stream(hipStream_t stream){
  require(stream==active->stream() && active->stream_live,"Foreign/duplicate stream retirement");
  active->stream_live=false;return active->step("stream-destroy");
}
hipError_t synchronize(hipStream_t stream){
  require(stream==active->stream() && active->stream_live && active->submitted,"Wait without submission");
  const auto status=active->step("wait");if(status==hipSuccess)active->completed=true;return status;
}
hipError_t allocate(void** out,std::size_t size){
  require(!active->allocated && size==(active->reuse?active->allocation_bytes:active->source.batch.layout().total_bytes) && size<=4U*1024*1024,
      "Allocation is not the complete bounded slab");
  const auto status=active->step("allocate");
  if(status!=hipSuccess)return active->fault=="allocate"?hipErrorOutOfMemory:status;
  if(active->null_allocation){*out=nullptr;return hipSuccess;}
  active->allocation=active->bytes().data()+(active->bad_alignment?1:0);
  active->allocated_size=size;active->allocated=true;*out=active->allocation;return hipSuccess;
}
hipError_t release(void* value){
  require(active->allocated && value==active->allocation,"Foreign/duplicate slab release");
  active->allocated=false;active->allocated_size=0;return active->step("release");
}
hipError_t copy(void* out,const void* in,std::size_t size,hipMemcpyKind kind){
  require(active->allocated && size==active->allocated_size,"Copy does not match the exact owned allocation");
  const auto name=kind==hipMemcpyHostToDevice?"upload":"readback";
  require(kind==hipMemcpyHostToDevice || kind==hipMemcpyDeviceToHost,"Unexpected copy direction");
  if(kind==hipMemcpyHostToDevice)require(out==active->allocation,"Upload address");
  else require(in==active->allocation && active->completed,"Read before confirmed completion");
  const auto status=active->step(name);if(status!=hipSuccess)return status;
  std::memcpy(out,in,size);
  if(active->reuse && kind==hipMemcpyHostToDevice){active->submitted=false;active->completed=false;}
  if(active->reuse && kind==hipMemcpyDeviceToHost)active->source_offset+=active->current_plan.input_bytes;
  return hipSuccess;
}
Bytes literal(std::span<const std::byte> source){
  Bytes out{static_cast<std::byte>((std::min)(source.size(),std::size_t{15})<<4)};
  if(source.size()>=15){
    auto left=source.size()-15;
    while(left>=255){out.push_back(std::byte{255});left-=255;}
    out.push_back(static_cast<std::byte>(left));
  }
  out.insert(out.end(),source.begin(),source.end());return out;
}
void word(std::span<std::byte> bytes,std::size_t at,std::uint64_t value){
  for(unsigned i=0;i<8;++i)bytes[at+i]=static_cast<std::byte>((value>>(8U*i))&255U);
}
hipError_t launch(const void* entry,dim3 grid,dim3 block,void** args,std::size_t shared,hipStream_t stream){
  auto expected=active->source.batch.layout();
  if(active->reuse){
    const auto left=active->source.batch.plaintext().size()-active->source_offset;
    require(uniform_lz4::make_layout((std::min)(std::uint64_t(left),std::uint64_t{2}*65536),65536,expected),
        "reused fake expected quantum");
  }
  require(active->allocated && active->stream_live && stream==active->stream() && !shared,"uniform launch resources");
  require((entry==reinterpret_cast<const void*>(&kernel32) && block.x==32) ||
      (entry==reinterpret_cast<const void*>(&kernel64) && block.x==64),"uniform typed wave selection");
  require(grid.x==expected.chunks && grid.y==1 && grid.z==1 && block.y==1 && block.z==1,"uniform dynamic grid");
  auto* slab=*static_cast<unsigned char**>(args[0]);
  const auto allocated=*static_cast<uniform_lz4::u64*>(args[1]);
  const auto plan=*static_cast<uniform_lz4::Layout*>(args[2]);
  require(slab==active->allocation && allocated==expected.total_bytes && allocated<=active->allocated_size &&
      plan.chunks==expected.chunks && uniform_lz4::valid_layout(plan,allocated),"uniform 64-bit by-value ABI");
  const auto status=active->step("launch");if(status!=hipSuccess)return status;
  active->submitted=true;active->current_plan=plan;
  auto bytes=active->bytes().first(static_cast<std::size_t>(allocated));
  const auto expected_source=active->source.batch.plaintext().subspan(
      static_cast<std::size_t>(active->reuse?active->source_offset:0),static_cast<std::size_t>(plan.input_bytes));
  require(std::equal(expected_source.begin(),expected_source.end(),bytes.begin()+static_cast<std::ptrdiff_t>(plan.input_offset)),
      "typed upload differs from exact quantum source");
  uniform_lz4::Chunk last{};
  for(std::uint32_t i=0;i<plan.chunks;++i){
    uniform_lz4::Chunk part{};require(uniform_lz4::chunk(plan,i,part),"uniform chunk mapping");
    const auto payload=literal(bytes.subspan(static_cast<std::size_t>(part.input),part.input_bytes));
    require(payload.size()<=part.output_capacity,"uniform independent fixture capacity");
    std::copy(payload.begin(),payload.end(),bytes.begin()+static_cast<std::ptrdiff_t>(part.output));
    word(bytes,static_cast<std::size_t>(part.size_slot),payload.size());last=part;
  }
  if(active->fault=="reuse-unused-tail" && allocated<active->allocation_bytes)
    active->bytes()[static_cast<std::size_t>(allocated)]^=std::byte{1};
  if(active->fault=="guard")bytes[last.output+last.output_capacity]^=std::byte{1};
  if(active->fault=="input")bytes[last.input]^=std::byte{1};
  if(active->fault=="padding")bytes[plan.output_offset-1]^=std::byte{1};
  if(active->fault=="size-zero")word(bytes,static_cast<std::size_t>(last.size_slot),0);
  if(active->fault=="size-large")word(bytes,static_cast<std::size_t>(last.size_slot),UINT64_MAX);
  if(active->fault=="payload"){
    const auto payload=literal(active->source.batch.input(plan.chunks-1));
    bytes[last.output+payload.size()-1]^=std::byte{1};
  }
  return hipSuccess;
}
hipError_t attributes(hipFuncAttributes* out,const void* entry){
  require(entry==reinterpret_cast<const void*>(&kernel32) ||
      entry==reinterpret_cast<const void*>(&kernel64),"Unknown function attribute entry");
  ++active->attribute_queries;
  const auto status=active->step("attributes-"+std::to_string(active->attribute_queries));
  if(status!=hipSuccess || active->missing_attributes)return status;
  out->maxThreadsPerBlock=active->inconsistent_attributes && active->attribute_queries==2?64:1024;
  out->sharedSizeBytes=128;return hipSuccess;
}
HipApi functions(){return {&init,&properties,&select,&priorities,&create_stream,&destroy_stream,
    &synchronize,&allocate,&release,&copy,&launch};}
UniformLz4Candidates compute(State& state,Lz4KernelCoverage covered=coverage,UniformLz4ComputeGrant grant={},HipPhaseTrace* trace=nullptr){
  Activate owner(state);const auto api=functions();
  const auto initialize=[&] {
    const auto priority=initialize_device(api,0,identity,1);
    hipDeviceProp_t actual{};
    check_hip(api.properties(&actual,0),"compute properties",HipWorkerStage::device_properties);
    return uniform_lz4_device_info(actual,covered,state.source.batch.layout().chunks,priority);
  };
  const auto memory=[&](HipMemoryPoint point){
    const auto index=point==HipMemoryPoint::pre_initialize?0U:point==HipMemoryPoint::initialized?1U:2U;
    state.calls.push_back("memory-"+std::to_string(index));
    auto current=baseline();current.captured_ms+=index;
    if(index==2)current.local_usage+=state.source.batch.layout().total_bytes;
    if(state.fault=="memory-"+std::to_string(index))current.valid=false;
    if(state.fault=="stale-"+std::to_string(index))current.captured_ms=1;
    if(state.fault=="nonlocal-"+std::to_string(index))current.nonlocal_usage+=1ULL<<30;
    const auto before=baseline();
    require_hip_memory(current,identity,1,1000+index,state.source.batch.layout().total_bytes,
        point,index?&before:nullptr);
  };
  if(!grant.maximum_chunks && !grant.maximum_device_bytes)
    grant={state.source.batch.layout().chunks,state.source.batch.layout().total_bytes,state.source.batch.layout().chunk_bytes};
  HipPhaseRecorder recorder(trace);
  if(trace)for(unsigned i=0;i<5;++i){recorder.begin(static_cast<HipPhase>(i));recorder.end(static_cast<HipPhase>(i));}
  auto result=execute_uniform_lz4_compute(api,&attributes,kernels,state.source,covered,grant,initialize,memory,trace?&recorder:nullptr);
  if(trace){
    recorder.begin(HipPhase::reply_encode);
    (void)encode_uniform_lz4_reply(state.source.batch,state.source.request_id,result,WorkerExecution::cpu_semantics);
    recorder.end(HipPhase::reply_encode);recorder.begin(HipPhase::modules_release);
    recorder.end(HipPhase::modules_release);recorder.finish();
  }
  return result;
}
UniformLz4Candidates reused_compute(State& state,UniformLz4ComputeGrant grant={},UniformLz4SweepTrace* trace=nullptr,
    bool prepared=false){
  Activate owner(state);state.reuse=true;
  uniform_lz4::Layout quantum{};require(uniform_lz4::make_layout(2U*65536,65536,quantum),"reused fixture peak");
  if(!grant.maximum_chunks && !grant.maximum_device_bytes)grant={2,quantum.total_bytes,65536};
  const auto admitted=plan_uniform_lz4_sweep(state.source.batch,2,quantum.total_bytes);
  state.allocation_bytes=admitted.device_peak_bytes;
  const auto api=functions();
  const auto initialize=[&]{
    const auto priority=initialize_device(api,0,identity,1);
    hipDeviceProp_t actual{};check_hip(api.properties(&actual,0),"reused compute properties",HipWorkerStage::device_properties);
    return uniform_lz4_device_info(actual,coverage,admitted.quantum_chunks,priority);
  };
  unsigned initialized=0;std::uint64_t tick=1000;
  const auto memory=[&](HipMemoryPoint point){
    state.calls.push_back(point==HipMemoryPoint::pre_initialize?"reuse-pre":
        point==HipMemoryPoint::initialized?"reuse-admit":"reuse-complete");
    if(point==HipMemoryPoint::initialized && ++initialized==4 && state.fault=="reuse-memory-next")
      throw HipWorkerFailure(HipWorkerExit::temporary_pressure,"next quantum pressure",HipWorkerStage::execution);
    const auto before=baseline();
    bool first=true;
    const auto sample=[&] {
      auto current=before;current.captured_ms=++tick;
      if(state.allocated)current.local_usage+=state.allocation_bytes;
      if(point==HipMemoryPoint::completed) {
        ++state.completion_samples;
        if(state.waits==2 && ((first && state.fault=="reuse-completion-refresh") ||
            state.fault=="reuse-completion-stale"))tick+=kGpuMaxBudgetAgeMs+1;
      }
      first=false;
      return current;
    };
    if(point==HipMemoryPoint::completed) {
      require_fresh_hip_completion(sample,[&]{return tick;},identity,1,state.allocation_bytes,before);
      return;
    }
    const auto current=sample();
    require_hip_memory(current,identity,1,tick,state.allocation_bytes,point,
        point==HipMemoryPoint::pre_initialize?nullptr:&before);
  };
  if(prepared) {
    auto shape=admitted;
    if(state.fault=="prepared-insufficient-slab")shape.device_peak_bytes=1;
    const auto receive=[&]() -> const UniformLz4Request* {
      require(state.allocated && state.stream_live && !state.waits,"prepared handoff preceded setup or followed launch");
      return state.fault=="prepared-discard"?nullptr:&state.source;
    };
    return execute_uniform_lz4_sweep_plan(api,&attributes,kernels,shape,coverage,receive,initialize,memory,trace);
  }
  return execute_uniform_lz4_reused_sweep(api,&attributes,kernels,state.source,coverage,grant,initialize,memory,trace);
}
void reused_success(const UniformLz4Request& source,unsigned wave,bool delayed_completion=false){
  ++scenarios;++successes;State state(source,wave);state.properties.maxGridSize[0]=2;
  if(delayed_completion)state.fault="reuse-completion-refresh";
  const auto c=reused_compute(state);verify_uniform_lz4_candidates(source.batch,c);
  const auto count=[&](const char* name){return std::count(state.calls.begin(),state.calls.end(),name);};
  const auto windows=(source.batch.layout().chunks+1)/2;
  require(count("initialize")==1 && count("stream-create")==1 && count("allocate")==1 &&
      count("stream-destroy")==1 && count("release")==1 && count("attributes-1")==1 && count("attributes-2")==1,
      "reused sweep repeated setup or resource lifetime");
  require(count("launch")==windows && count("wait")==windows && count("upload")==windows &&
      count("readback")==windows && count("reuse-complete")==windows && state.property_queries==2 &&
      state.priority_queries==2 && state.source_offset==source.batch.plaintext().size() &&
      !state.allocated && !state.stream_live,"reused sweep incomplete coverage/admission/retirement");
  require(state.completion_samples==windows+delayed_completion,"reused completion sampled beyond its evidence");
}
void reused_refused(State& state,HipWorkerExit code,UniformLz4ComputeGrant grant={}){
  ++scenarios;++refusals;UniformLz4Candidates escaped;
  try{escaped=reused_compute(state,grant);throw std::runtime_error("missing reused refusal: "+state.fault);}
  catch(const HipWorkerFailure& f){require(f.code==code,"wrong reused failure category");}
  require(escaped.payload.empty() && escaped.sizes.empty() && !state.allocated && !state.stream_live,
      "reused failure exposed partial output or resources");
  if(state.fault=="reuse-wait-second")require(state.waits==2,"reused wait failure retried");
  if(state.fault=="reuse-memory-next")require(state.waits==1,"reused pressure submitted next quantum");
  if(state.fault=="reuse-completion-stale")
    require(state.waits==2 && state.completion_samples==3,"stale completion resubmitted or kept polling");
  if(state.fault=="reuse-unused-tail")require(state.waits==(state.source.batch.layout().chunks+1)/2,
      "unused-tail refusal did not reach partial final quantum");
}
void refused(State& state,HipWorkerExit code,UniformLz4ComputeGrant grant={}){
  ++scenarios;++refusals;UniformLz4Candidates escaped;
  try{escaped=compute(state,coverage,grant);throw std::runtime_error("missing uniform compute refusal: "+state.fault);}
  catch(const HipWorkerFailure& f){require(f.code==code,"wrong uniform recovery category");}
  require(escaped.payload.empty() && escaped.sizes.empty() && !state.allocated && !state.stream_live,
      "uniform failure escaped candidate/resources");
}
void success(const UniformLz4Request& source,unsigned wave){
  ++scenarios;++successes;State state(source,wave);
  const auto c=compute(state);verify_uniform_lz4_candidates(source.batch,c);
  require(c.sizes.size()==source.batch.layout().chunks && !state.allocated && !state.stream_live &&
      state.completed,"uniform success ownership/retirement");
  const auto frame=encode_uniform_lz4_reply(source.batch,source.request_id,c,WorkerExecution::cpu_semantics);
  const auto accepted=accept_uniform_lz4_reply(frame,source.batch,source.request_id,WorkerExecution::cpu_semantics);
  require(accepted.payload==c.payload && accepted.sizes==c.sizes,"uniform compute/frame composition");
  require(state.calls[state.calls.size()-2]=="stream-destroy" && state.calls.back()=="release",
      "uniform result preceded retirement");
}
}
int wmain(int argc,wchar_t** argv){try{
  require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME) &&
      !GetModuleHandleW(L"flitzfs_rocm_uniform_lz4_candidate.dll"),"uniform CPU probe loaded AMD code");
  const auto source=request();
  if(argc==2 && std::wstring_view(argv[1])==L"--guarded-refusal"){
    try{(void)run_hip_uniform_lz4_in_worker(nullptr,kernels,0,identity,1,source,
        {source.batch.layout().chunks,source.batch.layout().total_bytes,source.batch.layout().chunk_bytes},std::span(&coverage,1));
      throw std::runtime_error("uniform null runtime admitted");}
    catch(const HipWorkerFailure& f){require(f.code==(gpu_lz4_cpu_verification_available()?
        HipWorkerExit::invalid_request:HipWorkerExit::unsupported_configuration),"uniform guarded refusal category");}
    try{(void)run_hip_uniform_lz4_sweep_in_worker(nullptr,kernels,0,identity,1,source,
        {2,source.batch.layout().total_bytes,65536},std::span(&coverage,1));
      throw std::runtime_error("reused null runtime admitted");}
    catch(const HipWorkerFailure& f){require(f.code==(gpu_lz4_cpu_verification_available()?
        HipWorkerExit::invalid_request:HipWorkerExit::unsupported_configuration),"reused guarded refusal category");}
    return 0;
  }
  require(argc==1,"invalid uniform probe arguments");
  {
    WindowsGpuTopologyProbe invalid(std::array<std::byte,8>{});
    require(invalid.sample().status==WindowsGpuTopologyStatus::invalid_identity,
        "uniform topology admitted an empty identity");
    require(invalid.retire()==0 && invalid.sample().status==WindowsGpuTopologyStatus::invalid_identity,
        "uniform topology changed a terminal refusal");
    WindowsGpuTopologyProbe unused(identity);
    require(unused.retire()==0 && unused.retire()==0,"cold topology retirement failed");
    const auto refused=unused.sample();
    require(refused.phase==WindowsGpuTopologyPhase::retire && !verified_single_gpu_node(refused,identity),
        "retired topology was reopened");
  }
  if(!gpu_lz4_cpu_verification_available()){
    State state(source);refused(state,HipWorkerExit::unsupported_configuration);
    require(state.calls.empty(),"raw uniform refusal touched runtime");
    State reused(source);reused_refused(reused,HipWorkerExit::unsupported_configuration);
    require(reused.calls.empty(),"raw reused sweep touched runtime");
  }else{
    success(source,32);success(source,64);success(request(1),32);
    reused_success(source,32);reused_success(source,64);reused_success(request(16U*65536),32);reused_success(request(1),32);
    reused_success(source,32,true);
    {
      ++scenarios;++successes;State state(source);state.fault="prepared-discard";UniformLz4SweepTrace trace;
      const auto result=reused_compute(state,{},&trace,true);
      require(result.payload.empty() && result.sizes.empty() && !state.allocated && !state.stream_live &&
          !state.waits && !trace.finished && !trace.completed_windows,"discarded prepared sweep performed or retained work");
    }
    {
      ++scenarios;++refusals;State state(source);state.fault="prepared-insufficient-slab";
      try {
        (void)reused_compute(state,{},nullptr,true);
        throw std::runtime_error("prepared sweep admitted a noncanonical slab");
      }catch(const HipWorkerFailure& failure) {
        require(failure.code==HipWorkerExit::invalid_request && state.calls.empty(),
            "invalid prepared sweep reached runtime or lost its error class");
      }
    }
    {State state(source);state.fault="reuse-completion-stale";reused_refused(state,HipWorkerExit::temporary_pressure);}
    {
      ++scenarios;++successes;State state(source);UniformLz4SweepTrace trace;
      const auto candidate=reused_compute(state,{},&trace);
      require(trace.finished && trace.completed_windows==9 &&
          trace.maximum_kernel_wall_ns<=trace.maximum_quantum_wall_ns &&
          !state.allocated && !state.stream_live,"reused actual compute trace incomplete");
      uniform_lz4::Layout q{};require(uniform_lz4::make_layout(2U*65536,65536,q),"reused profile peak");
      const UniformLz4ProfileBinding binding{source.request_id,0,1,2,65536,q.total_bytes,identity};
      const auto ordinary=encode_uniform_lz4_reply(source.batch,source.request_id,candidate,WorkerExecution::cpu_semantics);
      const auto frame=encode_uniform_lz4_sweep_profile_reply(source.batch,binding,trace,ordinary,WorkerExecution::cpu_semantics);
      const auto accepted=accept_uniform_lz4_sweep_profile_reply(frame,source.batch,binding,WorkerExecution::cpu_semantics);
      require(accepted.candidates.payload==candidate.payload && accepted.candidates.sizes==candidate.sizes &&
          accepted.trace.wall_ns==trace.wall_ns,"reused compute/envelope/CPU comparison mismatch");
      for(const auto* fault:{"reuse-wait-second","release","reuse-memory-next"}){
        ++scenarios;++refusals;State failed(source);failed.fault=fault;UniformLz4SweepTrace partial;
        UniformLz4Candidates escaped;
        try{escaped=reused_compute(failed,{},&partial);throw std::runtime_error("profiled reuse failure ignored");}
        catch(const HipWorkerFailure& f){
          require(f.code==(failed.fault=="reuse-memory-next"?HipWorkerExit::temporary_pressure:HipWorkerExit::device_fault),
              "profiled reuse failure category");
        }
        require(!partial.finished && escaped.payload.empty() && escaped.sizes.empty() &&
            !failed.allocated && !failed.stream_live,"failed reuse published observation or leaked resources");
      }
    }
    for(const auto* name:{"initialize","properties-1","properties-2","select","priority-1","priority-2",
        "attributes-1","attributes-2","stream-create","allocate","upload","launch","wait","readback",
        "stream-destroy","release","reuse-wait-second","reuse-unused-tail","reuse-memory-next",
        "guard","input","padding","size-zero","size-large","payload"}){
      State state(source);state.fault=name;
      const auto code=state.fault=="allocate" || state.fault=="reuse-memory-next"?HipWorkerExit::temporary_pressure:
          state.fault=="reuse-unused-tail" || state.fault=="guard" || state.fault=="input" ||
          state.fault=="padding" || state.fault=="size-zero" || state.fault=="size-large" ||
          state.fault=="payload"?HipWorkerExit::integrity_failure:HipWorkerExit::device_fault;
      reused_refused(state,code);
    }
    {State state(source);state.fault="guard";state.secondary_fault="release";reused_refused(state,HipWorkerExit::device_fault);}
    {State state(source);state.null_stream=true;reused_refused(state,HipWorkerExit::device_fault);
      require(state.waits==0 && std::count(state.calls.begin(),state.calls.end(),"allocate")==0,
          "null reused stream allocated or submitted work");}
    {State state(source);state.bad_alignment=true;reused_refused(state,HipWorkerExit::device_fault);}
    {State state(source);state.null_allocation=true;reused_refused(state,HipWorkerExit::device_fault);}
    for(const auto bad:std::array<UniformLz4ComputeGrant,3>{{{0,1,65536},{2,1,65536},{2,328288,32768}}}){
      State state(source);reused_refused(state,HipWorkerExit::invalid_request,bad);
      require(state.calls.empty(),"invalid reused grant touched runtime");
    }
    {State state(source);state.properties.maxGridSize[0]=1;
      reused_refused(state,HipWorkerExit::unsupported_configuration);
      require(!state.allocated && !state.stream_live,"unsupported reused grid allocated work");}


    {
      ++scenarios;++successes;State state(source);HipPhaseTrace trace;
      const auto candidate=compute(state,coverage,{},&trace);
      require(trace.finished && trace.completed==hip_phase_complete && !candidate.payload.empty() &&
          !state.allocated && !state.stream_live,"uniform actual compute trace incomplete");
      State failed(source);failed.fault="wait";HipPhaseTrace partial;
      ++scenarios;++refusals;
      try{(void)compute(failed,coverage,{},&partial);throw std::runtime_error("profile wait failure ignored");}
      catch(const HipWorkerFailure& f){require(f.code==HipWorkerExit::device_fault,"profile wait failure category");}
      require(!partial.finished && partial.completed!=hip_phase_complete &&
          !failed.allocated && !failed.stream_live,"failed compute emitted complete timing or leaked resources");
    }
    {
      uniform_lz4::Layout quantum{};require(uniform_lz4::make_layout(2U*65536,65536,quantum),"typed sweep quantum");
      const auto plan=plan_uniform_lz4_sweep(source.batch,2,quantum.total_bytes);
      unsigned windows=0;++scenarios;++successes;
      const auto all=compute_uniform_lz4_sweep(source,plan,[&](const UniformLz4Request& part){
        ++windows;State state(part);auto candidate=compute(state);
        require(!state.allocated && !state.stream_live && state.completed,"typed sweep quantum retirement");
        return candidate;
      });
      require(windows==9 && all.sizes.size()==17,"typed sweep complete source");
      UniformLz4Candidates escaped;windows=0;++scenarios;++refusals;
      try{escaped=compute_uniform_lz4_sweep(source,plan,[&](const UniformLz4Request& part){
        State state(part);if(++windows==2)state.fault="wait";return compute(state);
      });throw std::runtime_error("typed sweep ignored device failure");}
      catch(const HipWorkerFailure& f){require(f.code==HipWorkerExit::device_fault,"typed sweep failure category");}
      require(windows==2 && escaped.payload.empty() && escaped.sizes.empty(),"typed sweep partial exposure/retry");
    }
    for(const auto* name:{"initialize","properties-1","properties-2","select","priority-1","priority-2",
        "attributes-1","attributes-2","stream-create","upload","launch","wait","readback","stream-destroy","release"}){
      State state(source);state.fault=name;refused(state,HipWorkerExit::device_fault);
    }
    {State state(source);state.fault="allocate";refused(state,HipWorkerExit::temporary_pressure);}
    for(const auto* name:{"guard","input","padding","size-zero","size-large","payload"}){
      State state(source);state.fault=name;refused(state,HipWorkerExit::integrity_failure);
    }
    for(unsigned i=0;i<3;++i){
      State state(source);state.fault="memory-"+std::to_string(i);refused(state,HipWorkerExit::device_fault);
      State stale(source);stale.fault="stale-"+std::to_string(i);refused(stale,HipWorkerExit::temporary_pressure);
    }
    {State state(source);state.bad_alignment=true;refused(state,HipWorkerExit::device_fault);}
    {State state(source);state.null_allocation=true;refused(state,HipWorkerExit::device_fault);}
    {State state(source);state.missing_attributes=true;refused(state,HipWorkerExit::unsupported_configuration);}
    {State state(source);state.properties.maxGridSize[0]=8;refused(state,HipWorkerExit::unsupported_configuration);
      require(!state.allocated && !state.stream_live,"unsupported dynamic grid allocated work");}
    {State state(source);refused(state,HipWorkerExit::invalid_request,{8,source.batch.layout().total_bytes,source.batch.layout().chunk_bytes});
      require(state.calls.empty(),"kernel quantum grant touched runtime");}
    {State state(source);refused(state,HipWorkerExit::invalid_request,
        {source.batch.layout().chunks,source.batch.layout().total_bytes-1,source.batch.layout().chunk_bytes});
      require(state.calls.empty(),"memory grant touched runtime");}
  }
  if(gpu_lz4_cpu_verification_available()){
    State state(source);refused(state,HipWorkerExit::invalid_request,
        {source.batch.layout().chunks,source.batch.layout().total_bytes,32768});
    require(state.calls.empty(),"mismatched profiled width touched runtime");
  }
  require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME),"uniform probe initialized runtime");
  std::array<wchar_t,32768> path{};
  const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
  require(length && length<path.size(),"uniform own executable path");
  const auto child=run_bounded_process(std::filesystem::path(std::wstring(path.data(),length)),
      {L"--guarded-refusal"},ProcessBudget{128,2000,15000});
  require(child.process_id && child.exit_confirmed && !child.timed_out && !child.cancelled &&
      child.exit_code==0 && child.peak_committed_bytes<=128ULL*1024*1024,"uniform guarded child retirement");
  std::cout<<"HIP uniform LZ4 CPU qualification: {\"checks\":"<<checks<<",\"scenarios\":"<<scenarios
      <<",\"successes\":"<<successes<<",\"refusals\":"<<refusals
      <<",\"decoder_available\":"<<(gpu_lz4_cpu_verification_available()?"true":"false")
      <<",\"guarded_exit_confirmed\":true,\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
