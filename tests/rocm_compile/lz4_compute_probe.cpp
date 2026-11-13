#include "hip_lz4_compute.hpp"
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
void kernel32(unsigned char*,unsigned,candidate_lz4::Layout){}
void kernel64(unsigned char*,unsigned,candidate_lz4::Layout){}
const auto kernels=candidate_lz4::make_kernel_api(&kernel32,&kernel64);
const std::array<std::byte,8> identity{std::byte{7}};
constexpr Lz4KernelCoverage coverage{"gfx1100",3};

Lz4WorkerRequest request(std::vector<Bytes> chunks={Bytes(17,std::byte{0x51}),Bytes(33,std::byte{0x62})}) {
  std::vector<Lz4WorkerBytes> views;
  for(const auto& chunk:chunks)views.emplace_back(chunk);
  return {519,PreparedLz4WorkerBatch::prepare(views)};
}
GpuMemorySnapshot baseline() {
  GpuMemorySnapshot s;s.luid=identity;s.node_mask=1;s.captured_ms=1000;
  s.dedicated_bytes=1ULL<<30;s.local_budget=s.dedicated_bytes;s.local_usage=4096;
  s.nonlocal_budget=8ULL<<30;s.nonlocal_usage=8192;s.valid=true;s.amd_hardware=true;
  s.physical_total=16ULL<<30;s.physical_available=8ULL<<30;
  s.commit_limit=32ULL<<30;s.commit_available=16ULL<<30;return s;
}
struct State {
  const Lz4WorkerRequest& source;
  std::vector<std::uint64_t> arena=std::vector<std::uint64_t>(262144/8);
  std::vector<std::string> calls;
  std::string fault,secondary_fault;
  hipDeviceProp_t properties{};
  bool stream_live=false,allocated=false,submitted=false,completed=false;
  bool bad_alignment=false,null_allocation=false,missing_attributes=false,inconsistent_attributes=false;
  unsigned property_queries=0,priority_queries=0,attribute_queries=0;
  int stream_tag=0;
  void* allocation=nullptr;
  explicit State(const Lz4WorkerRequest& input,unsigned wave=32):source(input) {
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
    calls.push_back(name);return fault==name || secondary_fault==name?hipErrorUnknown:hipSuccess;
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
  require(!active->allocated && size==active->source.batch.device_layout().total_bytes && size<=262144,
      "Allocation is not the complete bounded slab");
  const auto status=active->step("allocate");
  if(status!=hipSuccess)return active->fault=="allocate"?hipErrorOutOfMemory:status;
  if(active->null_allocation){*out=nullptr;return hipSuccess;}
  active->allocation=active->bytes().data()+(active->bad_alignment?1:0);
  active->allocated=true;*out=active->allocation;return hipSuccess;
}
hipError_t release(void* value){
  require(active->allocated && value==active->allocation,"Foreign/duplicate slab release");
  active->allocated=false;return active->step("release");
}
hipError_t copy(void* out,const void* in,std::size_t size,hipMemcpyKind kind){
  require(active->allocated && size==active->source.batch.device_layout().total_bytes,"Copy slab extent");
  const auto name=kind==hipMemcpyHostToDevice?"upload":"readback";
  require(kind==hipMemcpyHostToDevice || kind==hipMemcpyDeviceToHost,"Unexpected copy direction");
  if(kind==hipMemcpyHostToDevice)require(out==active->allocation,"Upload address");
  else require(in==active->allocation && active->completed,"Read before confirmed completion");
  const auto status=active->step(name);if(status!=hipSuccess)return status;
  std::memcpy(out,in,size);return hipSuccess;
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
  const auto& expected=active->source.batch.device_layout();
  require(active->allocated && active->stream_live && stream==active->stream() && !shared,
      "Launch resource ownership");
  require((entry==reinterpret_cast<const void*>(&kernel32) && block.x==32) ||
      (entry==reinterpret_cast<const void*>(&kernel64) && block.x==64),"Typed wave selection");
  require(grid.x==expected.count && grid.y==1 && grid.z==1 && block.y==1 && block.z==1,
      "One-wave-per-chunk launch");
  auto* slab=*static_cast<unsigned char**>(args[0]);
  const auto allocated=*static_cast<unsigned*>(args[1]);
  const auto plan=*static_cast<candidate_lz4::Layout*>(args[2]);
  require(slab==active->allocation && allocated==expected.total_bytes &&
      plan.count==expected.count && candidate_lz4::valid_layout(plan,allocated),
      "Native by-value kernel arguments");
  const auto status=active->step("launch");if(status!=hipSuccess)return status;
  active->submitted=true;
  auto bytes=active->bytes().first(allocated);
  // CPU-only proposed output. No GPU kernel body or registered HIP stub runs.
  for(unsigned i=0;i<plan.count;++i){
    const auto& chunk=plan.chunks[i];
    const auto payload=literal(bytes.subspan(chunk.input_offset,chunk.input_bytes));
    require(payload.size()<=chunk.output_capacity,"Independent fixture capacity");
    std::copy(payload.begin(),payload.end(),bytes.begin()+chunk.output_offset);
    word(bytes,plan.output_sizes+8U*i,payload.size());
  }
  const auto& last=plan.chunks[plan.count-1];
  if(active->fault=="guard")bytes[last.output_offset+last.output_capacity]^=std::byte{1};
  if(active->fault=="input")bytes[last.input_offset]^=std::byte{1};
  if(active->fault=="pointer")bytes[plan.input_pointers]^=std::byte{1};
  if(active->fault=="size-zero")word(bytes,plan.output_sizes+8U*(plan.count-1),0);
  if(active->fault=="size-large")word(bytes,plan.output_sizes+8U*(plan.count-1),UINT64_MAX);
  if(active->fault=="payload")bytes[last.output_offset]^=std::byte{1};
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
std::vector<Bytes> compute(State& state,Lz4KernelCoverage covered=coverage){
  Activate owner(state);const auto api=functions();
  const auto initialize=[&] {
    const auto priority=initialize_device(api,0,identity,1);
    hipDeviceProp_t actual{};
    check_hip(api.properties(&actual,0),"compute properties",HipWorkerStage::device_properties);
    return lz4_device_info(actual,covered,state.source.batch.count(),priority);
  };
  const auto memory=[&](HipMemoryPoint point){
    const auto index=point==HipMemoryPoint::pre_initialize?0U:point==HipMemoryPoint::initialized?1U:2U;
    state.calls.push_back("memory-"+std::to_string(index));
    auto current=baseline();current.captured_ms+=index;
    if(index==2)current.local_usage+=state.source.batch.device_layout().total_bytes;
    if(state.fault=="memory-"+std::to_string(index))current.valid=false;
    if(state.fault=="stale-"+std::to_string(index))current.captured_ms=1;
    if(state.fault=="nonlocal-"+std::to_string(index))current.nonlocal_usage+=1ULL<<30;
    const auto before=baseline();
    require_hip_memory(current,identity,1,1000+index,state.source.batch.device_layout().total_bytes,
        point,index?&before:nullptr);
  };
  return execute_lz4_compute(api,&attributes,kernels,state.source,covered,initialize,memory);
}
void refused(State& state,HipWorkerExit code,Lz4KernelCoverage covered=coverage){
  ++scenarios;++refusals;std::vector<Bytes> escaped;
  try{escaped=compute(state,covered);throw std::runtime_error("Missing compute refusal");}
  catch(const HipWorkerFailure& failure){require(failure.code==code,"Wrong compute recovery category");}
  require(escaped.empty() && !state.allocated && !state.stream_live,"Failure escaped data/resources");
}
void successful(const Lz4WorkerRequest& source,unsigned wave){
  ++scenarios;++successes;State state(source,wave);
  const auto result=compute(state);
  require(result.size()==source.batch.count(),"Compute candidate count");
  for(std::size_t i=0;i<result.size();++i)require(result[i]==literal(source.batch.input(i)),"Owned CPU-verified result");
  require(!state.allocated && !state.stream_live && state.completed,"Successful resources remained");
  require(state.priority_queries==2 && state.attribute_queries==2,"Metadata query bound");
  require(state.calls.size()>=2 && state.calls[state.calls.size()-2]=="stream-destroy" &&
      state.calls.back()=="release","Candidates preceded final resource retirement");
}
} // namespace

int wmain(int argc,wchar_t** argv) {
  try {
    require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME) &&
        !GetModuleHandleW(L"flitzfs_rocm_lz4_candidate.dll"),"Compute CPU probe loaded AMD code");
    const auto source=request();
    if(argc==2 && std::wstring_view(argv[1])==L"--guarded-refusal"){
      try {
        (void)run_hip_lz4_in_worker(nullptr,kernels,0,identity,1,source,coverage);
        throw std::runtime_error("Null runtime unexpectedly admitted");
      }catch(const HipWorkerFailure& e){
        const auto expected=gpu_lz4_cpu_verification_available()?HipWorkerExit::invalid_request:
            HipWorkerExit::unsupported_configuration;
        require(e.code==expected,"Guarded wrapper refusal category");
        return 0;
      }
    }
    require(argc==1,"Unexpected compute probe arguments");
    const auto plaintext=Bytes(source.batch.plaintext().begin(),source.batch.plaintext().end());
    if(!gpu_lz4_cpu_verification_available()){
      State state(source);refused(state,HipWorkerExit::unsupported_configuration);
      require(state.calls.empty(),"Missing CPU decoder touched a runtime or memory query");
    }else{
      successful(source,32);successful(source,64);
      successful(request({Bytes(1,std::byte{1})}),32);
      successful(request({Bytes(65536,std::byte{1})}),32);
      successful(request(std::vector<Bytes>(8,Bytes(257,std::byte{2}))),32);
      for(const auto* name:{"initialize","properties-1","properties-2","select","priority-1","priority-2",
          "attributes-1","attributes-2","stream-create","upload","launch","wait","readback",
          "stream-destroy","release"}){
        State state(source);state.fault=name;refused(state,HipWorkerExit::device_fault);
      }
      {State state(source);state.fault="allocate";refused(state,HipWorkerExit::temporary_pressure);}
      {State state(source);state.fault="allocate";state.secondary_fault="stream-destroy";
        refused(state,HipWorkerExit::device_fault);}
      for(unsigned i=0;i<3;++i){
        State invalid(source);invalid.fault="memory-"+std::to_string(i);
        refused(invalid,HipWorkerExit::device_fault);
        State stale(source);stale.fault="stale-"+std::to_string(i);
        refused(stale,HipWorkerExit::temporary_pressure);
        if(i){State spill(source);spill.fault="nonlocal-"+std::to_string(i);
          refused(spill,HipWorkerExit::temporary_pressure);}
      }
      for(const auto* name:{"guard","input","pointer","size-zero","size-large","payload"}){
        State state(source);state.fault=name;refused(state,HipWorkerExit::integrity_failure);
      }
      {State state(source);state.bad_alignment=true;refused(state,HipWorkerExit::device_fault);}
      {State state(source);state.null_allocation=true;refused(state,HipWorkerExit::device_fault);}
      {State state(source);state.missing_attributes=true;refused(state,HipWorkerExit::unsupported_configuration);
        require(!state.allocated && !state.stream_live,"Missing attributes allocated resources");}
      {State state(source);state.inconsistent_attributes=true;refused(state,HipWorkerExit::unsupported_configuration);}
      for(unsigned change=0;change<11;++change){
        State state(source);
        switch(change){
          case 0:state.properties.warpSize=16;break;
          case 1:state.properties.maxThreadsPerBlock=16;break;
          case 2:state.properties.maxThreadsDim[0]=16;break;
          case 3:state.properties.maxThreadsDim[1]=0;break;
          case 4:state.properties.maxThreadsDim[2]=0;break;
          case 5:state.properties.maxGridSize[0]=1;break;
          case 6:state.properties.maxGridSize[1]=0;break;
          case 7:state.properties.maxGridSize[2]=0;break;
          case 8:state.properties.sharedMemPerBlock=0;break;
          case 9:state.properties.gcnArchName[3]='9';break;
          case 10:std::fill(std::begin(state.properties.gcnArchName),
              std::end(state.properties.gcnArchName),'x');break;
        }
        refused(state,HipWorkerExit::unsupported_configuration);
        require(!state.allocated && !state.stream_live,"Unqualified geometry allocated resources");
      }
      {State state(source,64);refused(state,HipWorkerExit::unsupported_configuration,{"gfx1100",1});}
      for(const auto covered:{Lz4KernelCoverage{"",1},{"gfx1100",0},{"gfx1100",4},{"gfx1100:bad",1}}){
        State state(source);refused(state,HipWorkerExit::invalid_request,covered);
        require(state.calls.empty(),"Malformed coverage touched runtime");
      }
      {auto invalid=source;invalid.request_id=0;State state(invalid);
        refused(state,HipWorkerExit::invalid_request);require(state.calls.empty(),"Invalid request touched runtime");}
    }
    require(std::equal(plaintext.begin(),plaintext.end(),source.batch.plaintext().begin()),
        "Immutable source changed");
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    require(length && length<path.size(),"Own executable identity");
    const auto child=run_bounded_process(std::filesystem::path(std::wstring(path.data(),length)),
        {L"--guarded-refusal"},ProcessBudget{128,2000,15000});
    require(child.process_id && child.exit_confirmed && !child.timed_out && !child.cancelled &&
        child.exit_code==0 && child.peak_committed_bytes<=128ULL*1024*1024,
        "Guarded wrapper refusal did not retire");
    std::cout<<"HIP LZ4 compute CPU qualification: {\"checks\":"<<checks
        <<",\"scenarios\":"<<scenarios<<",\"successes\":"<<successes<<",\"refusals\":"<<refusals
        <<",\"decoder_available\":"<<(gpu_lz4_cpu_verification_available()?"true":"false")
        <<",\"attribute_symbol\":\""<<lz4_attributes_symbol
        <<"\",\"guarded_exit_confirmed\":true,\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
