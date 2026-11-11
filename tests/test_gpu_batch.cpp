#include "flitzfs/gpu_batch.hpp"
#include "flitzfs/gpu_kernel_api.hpp"
#include "extent_codec_fixture.hpp"
#include "gpu/decode_kernel.hpp"
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
using namespace flitzfs;
using namespace extent_fixture;
std::size_t checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& f){try{f();}catch(const std::exception&){++checks;return;}throw std::runtime_error("unsafe batch accepted");}
PreparedGpuBatch prepare(const std::vector<Block>& blocks) {
  std::vector<GpuBlockView> views;
  for(const auto& b:blocks)views.push_back({b.kind,b.fill,static_cast<std::uint32_t>(b.plain.size()),b.encoded});
  return PreparedGpuBatch::prepare(views);
}
// A synthetic typed kernel entry tests the module's host argument contract;
// this is CPU execution, not dynamic loading or proof that a GPU was used.
void semantic_entry(const gpu_detail::u8* input,gpu_detail::u32 input_size,
    const gpu_detail::Task* tasks,gpu_detail::u32 task_count,gpu_detail::u8* output,
    gpu_detail::u32 output_size,gpu_detail::u32* status,gpu_detail::u32 version) {
  if(version!=gpu_detail::abi_version || task_count>gpu_detail::max_tasks)return;
  for(gpu_detail::u32 i=task_count;i>0;--i)
    status[i-1]=gpu_detail::decode_task(tasks[i-1],input,input_size,output,output_size);
}
void module_contract() {
  const auto good=gpu_detail::make_kernel_api(semantic_entry);
  check(gpu_detail::compatible_kernel_api(good),"typed kernel interface rejected");
  for(auto field:{&gpu_detail::KernelApi::size_bytes,&gpu_detail::KernelApi::api_version,
      &gpu_detail::KernelApi::task_abi,&gpu_detail::KernelApi::task_bytes,
      &gpu_detail::KernelApi::task_limit,&gpu_detail::KernelApi::batch_limit,
      &gpu_detail::KernelApi::threads,&gpu_detail::KernelApi::reserved}) {
    auto invalid=good;invalid.*field^=1;
    check(!gpu_detail::compatible_kernel_api(invalid),"incompatible kernel interface admitted");
  }
  auto absent=good;absent.decode=nullptr;
  check(!gpu_detail::compatible_kernel_api(absent),"missing kernel entry admitted");
}
GpuBatchReply emulate(const PreparedGpuBatch& b,std::uint64_t generation=1) {
  // Explicit host semantic execution, NOT evidence of GPU execution.
  std::vector<unsigned char> guarded(b.cpu_reference().size()+32,0x5a);
  unsigned char dummy=0;
  const auto* input=b.payload().empty()?&dummy:reinterpret_cast<const unsigned char*>(b.payload().data());
  GpuBatchReply reply;reply.generation=generation;
  // Reverse task order tests independence from serial output progression.
  reply.status.resize(b.tasks().size(),gpu_detail::pending);
  const auto api=gpu_detail::make_kernel_api(semantic_entry);
  check(gpu_detail::compatible_kernel_api(api),"invalid typed semantic interface");
  api.decode(input,static_cast<unsigned>(b.payload().size()),b.tasks().data(),
      static_cast<unsigned>(b.tasks().size()),guarded.data()+16,
      static_cast<unsigned>(b.cpu_reference().size()),reply.status.data(),gpu_detail::abi_version);
  for(const auto status:reply.status)check(status==0,"device semantic task failed");
  check(std::all_of(guarded.begin(),guarded.begin()+16,[](auto c){return c==0x5a;}) &&
      std::all_of(guarded.end()-16,guarded.end(),[](auto c){return c==0x5a;}),"output canary changed");
  const auto* first=reinterpret_cast<const std::byte*>(guarded.data()+16);
  reply.bytes.assign(first,first+b.cpu_reference().size());
  check(std::equal(reply.bytes.begin(),reply.bytes.end(),b.cpu_reference().begin()),"independent CPU/device semantics mismatch");
  return reply;
}
GpuMemorySnapshot snapshot(std::uint64_t ms=1000) {
  GpuMemorySnapshot s;s.valid=true;s.amd_hardware=true;s.luid[0]=std::byte{7};s.node_mask=1;
  s.captured_ms=ms;s.dedicated_bytes=16ULL<<30;s.local_budget=8ULL<<30;
  s.local_usage=1ULL<<30;s.nonlocal_usage=1024;
  s.nonlocal_budget=8ULL<<30;s.physical_total=16ULL<<30;s.physical_available=8ULL<<30;
  s.commit_limit=32ULL<<30;s.commit_available=16ULL<<30;return s;
}
void accepted(const PreparedGpuBatch& batch) {
  auto s=snapshot();GpuResultGate gate(batch,s.luid,s.node_mask);
  check(gate.begin(false,s,1000).code==GpuGateCode::no_consent,"implicit GPU admission");
  check(gate.state()==GpuGateState::idle && gate.accepted_bytes().empty(),"denial leaked data");
  auto attempt=gate.begin(true,s,1000);check(attempt.code==GpuGateCode::admitted && attempt.generation,"admission failed");
  check(gate.begin(true,s,1000).code==GpuGateCode::wrong_state,"second outstanding request admitted");
  check(gate.complete(emulate(batch,attempt.generation),snapshot(1001),1001)==GpuGateCode::accepted,"valid output rejected");
  check(std::equal(gate.accepted_bytes().begin(),gate.accepted_bytes().end(),batch.cpu_reference().begin()),"accepted output parity");
  check(gate.complete(emulate(batch,attempt.generation),snapshot(1002),1002)==GpuGateCode::wrong_state,"duplicate completion accepted");
}
void memory_envelopes() {
  auto before=snapshot();before.nonlocal_usage=0;
  const auto envelope=gpu_memory_envelope(before);
  check(envelope.code==GpuGateCode::admitted && envelope.local_reserve==(512ULL<<20) &&
      envelope.physical_reserve==(1ULL<<30) && envelope.commit_reserve==(2ULL<<30) &&
      envelope.nonlocal_growth==(28ULL<<20),"independent resource envelope oracle");
  // Growing hardware capacity/headroom changes the bound; no owner specs are used.
  auto large=before;large.physical_total*=2;large.physical_available*=2;
  large.commit_limit*=2;large.commit_available*=2;
  check(gpu_memory_envelope(large).nonlocal_growth==(56ULL<<20),"capacity adaptation");
  auto after=before;after.captured_ms++;
  after.nonlocal_usage=28ULL<<20;
  check(check_gpu_memory_transition(before,after,before.luid,1,1001,0)==GpuGateCode::admitted,
      "exact bounded shared growth rejected");
  ++after.nonlocal_usage;
  check(check_gpu_memory_transition(before,after,before.luid,1,1001,0)==GpuGateCode::nonlocal_growth,
      "one-byte over envelope accepted");
  after=before;after.captured_ms++;after.nonlocal_usage=1ULL<<20;
  after.physical_available=(1ULL<<30)+(128ULL<<20);
  check(check_gpu_memory_transition(before,after,before.luid,1,1001,0)==GpuGateCode::nonlocal_growth,
      "falling physical headroom retained old growth allowance");
  after=before;after.captured_ms++;after.nonlocal_usage=1ULL<<20;
  after.commit_available=(2ULL<<30)+(128ULL<<20);
  check(check_gpu_memory_transition(before,after,before.luid,1,1001,0)==GpuGateCode::nonlocal_growth,
      "falling process commit retained old growth allowance");
  for(unsigned field=0;field<9;++field) {
    auto bad=before;
    switch(field) {
      case 0:bad.nonlocal_budget=0;break;
      case 1:bad.nonlocal_usage=bad.nonlocal_budget+1;break;
      case 2:bad.physical_total=0;break;
      case 3:bad.physical_available=bad.physical_total+1;break;
      case 4:bad.commit_limit=0;break;
      case 5:bad.commit_available=bad.commit_limit+1;break;
      case 6:bad.physical_available=0;break;
      case 7:bad.commit_available=0;break;
      case 8:bad.nonlocal_usage=bad.nonlocal_budget;break;
    }
    const auto expected=field<6 && field!=1?GpuGateCode::invalid_budget:GpuGateCode::insufficient_headroom;
    check(check_gpu_memory_budget(bad,before.luid,1,1000,0)==expected,"malformed/pressured resource observation");
  }
  check(gpu_memory_envelope(before,(std::numeric_limits<std::uint64_t>::max)()).code==
      GpuGateCode::insufficient_headroom,"allocation arithmetic overflow");
  after=before;after.captured_ms=999;
  check(check_gpu_memory_transition(before,after,before.luid,1,1000,0)==GpuGateCode::stale_budget,
      "pre-submission transition accepted");
  after=before;after.captured_ms=1001;after.dedicated_bytes++;
  check(check_gpu_memory_transition(before,after,before.luid,1,1001,0)==GpuGateCode::wrong_adapter,
      "changed device capacity accepted");
  // Original baseline remains in force across intermediate observations.
  auto middle=before;middle.captured_ms=1001;middle.nonlocal_usage=16ULL<<20;
  after=before;after.captured_ms=1002;after.nonlocal_usage=32ULL<<20;
  check(check_gpu_memory_transition(before,middle,before.luid,1,1001,0)==GpuGateCode::admitted &&
      check_gpu_memory_transition(before,after,before.luid,1,1002,0)==GpuGateCode::nonlocal_growth,
      "moving intermediate baseline hid cumulative growth");
  // Safe synthetic result is still byte-verified and terminal on CPU fallback.
  const auto plan=prepare({fill(8192,51)});
  GpuResultGate gate(plan,before.luid,1);
  const auto attempt=gate.begin(true,before,1000);
  after=before;after.captured_ms++;after.nonlocal_usage=16ULL<<20;
  check(attempt.code==GpuGateCode::admitted &&
      gate.complete(emulate(plan,attempt.generation),after,1001)==GpuGateCode::accepted,
      "bounded growth did not reach independent result verification");
  const auto cpu=gate.use_cpu_fallback();
  check(cpu.size()==plan.cpu_reference().size() && gate.accepted_bytes().empty(),
      "CPU fallback retained an accepted candidate");
}
}
int main(){try{
  memory_envelopes();
  module_contract();
  static_assert(!std::is_copy_constructible_v<GpuResultGate> && !std::is_move_constructible_v<GpuResultGate>);
  for(unsigned n:{1U,2U,3U,4095U,4096U,4097U,8191U,8192U,8193U,65535U,65536U}){
    accepted(prepare({raw(pattern(n,"raw sample").plain),fill(n,217)}));
    if(n>8)accepted(prepare({pattern(n,"motif")}));
    if(n>64){accepted(prepare({prefix_sample(false,n),prefix_sample(true,n)}));}
    if(n>16384)accepted(prepare({huffman(n)}));
  }
  const std::vector<Block> mixed{raw(text("123456789")),fill(4101,11),pattern(8197),prefix_sample(),prefix_sample(true),huffman()};
  const auto batch=prepare(mixed);accepted(batch);
  auto full=prepare({raw(Bytes(65536)),raw(Bytes(65536)),raw(Bytes(65536)),raw(Bytes(65536))});
  check(full.cpu_reference().size()==262144 && full.payload().size()==262144,"exact full batch rejected");
  accepted(full);
  check(batch.device_buffer_bytes()<=2*gpu_detail::max_batch_bytes+gpu_detail::max_tasks*36,"buffer accounting exceeds cap");
  // Zero input still needs a one-byte device allocation, independent of logical payload.
  const auto filled=prepare({fill(1,0)});check(filled.device_buffer_bytes()==38,"empty payload allocation not accounted");
  for(auto kind:{2U,6U,7U,8U,9U,11U,12U,13U,255U}){
    auto block=prefix_sample();block.kind=static_cast<std::uint8_t>(kind);rejects([&]{prepare({block});});
  }
  rejects([&]{PreparedGpuBatch::prepare({});});
  rejects([&]{prepare(std::vector<Block>(9,fill(1,0)));});
  rejects([&]{prepare({fill(0,0)});});rejects([&]{prepare({fill(65537,0)});});
  rejects([&]{prepare(std::vector<Block>(5,fill(65536,0)));});
  GpuBlockView malicious{1,0,0xffffffffU,{}};rejects([&]{PreparedGpuBatch::prepare({&malicious,1});});
  for(auto kind:{4U,5U,10U}){
    auto block=kind==10?huffman():prefix_sample(kind==5);
    for(std::size_t i=0;i<(std::min)(block.encoded.size(),std::size_t{24});++i){
      auto truncated=block;truncated.encoded.resize(i);rejects([&]{prepare({truncated});});
    }
    auto corrupt=block;const unsigned offset=kind==10?8192:(kind==5?84:0);
    replace(corrupt.encoded,offset,1,4);rejects([&]{prepare({corrupt});});
  }
  // Mutating caller-owned input after preparation cannot change the batch.
  auto originals=mixed;const auto stable=prepare(originals);originals[0].encoded[0]^=std::byte{1};accepted(stable);
  const auto good=snapshot();
  auto denied=[&](GpuMemorySnapshot bad,GpuGateCode expected,std::uint64_t now=1000){
    GpuResultGate gate(batch,good.luid,good.node_mask);
    check(gate.begin(true,bad,now).code==expected,"bad budget incorrectly classified");
    check(gate.state()==GpuGateState::idle && gate.accepted_bytes().empty(),"rejected budget altered gate");
  };
  {auto s=good;s.valid=false;denied(s,GpuGateCode::invalid_budget);}
  {auto s=good;s.amd_hardware=false;denied(s,GpuGateCode::invalid_budget);}
  {auto s=good;s.node_mask=3;denied(s,GpuGateCode::invalid_budget);}
  {auto s=good;s.local_budget=s.dedicated_bytes+1;denied(s,GpuGateCode::invalid_budget);}
  {auto s=good;s.local_usage=s.local_budget+1;denied(s,GpuGateCode::insufficient_headroom);}
  {auto s=good;s.local_budget=0;denied(s,GpuGateCode::invalid_budget);}
  {auto s=good;s.luid[1]=std::byte{1};denied(s,GpuGateCode::wrong_adapter);}
  {auto s=good;s.node_mask=2;denied(s,GpuGateCode::wrong_adapter);}
  {auto s=good;s.captured_ms=949;denied(s,GpuGateCode::stale_budget);}
  {auto s=good;s.captured_ms=1001;denied(s,GpuGateCode::stale_budget);}
  {auto s=good;s.local_usage=s.local_budget-(512ULL<<20);denied(s,GpuGateCode::insufficient_headroom);}
  {auto s=good;s.local_usage=s.local_budget-(512ULL<<20)-batch.device_buffer_bytes()+1;denied(s,GpuGateCode::insufficient_headroom);}
  {auto s=good;s.local_usage=s.local_budget-(512ULL<<20)-batch.device_buffer_bytes();
    GpuResultGate gate(batch,s.luid,1);check(gate.begin(true,s,1000).code==GpuGateCode::admitted,"exact memory headroom rejected");}
  rejects([&]{GpuResultGate gate(batch,{},1);});rejects([&]{GpuResultGate gate(batch,good.luid,0);});
  for(unsigned ms:{0U,101U,0xffffffffU})rejects([&]{GpuResultGate gate(batch,good.luid,1,ms);});
  auto failed=[&](auto mutate,GpuGateCode reason){
    GpuResultGate gate(batch,good.luid,1);const auto attempt=gate.begin(true,good,1000);
    auto reply=emulate(batch,attempt.generation);auto after=snapshot(1001);std::uint64_t now=1001;
    mutate(reply,after,now);
    check(gate.complete(std::move(reply),after,now)==reason,"invalid reply accepted or misclassified");
    check(gate.state()==GpuGateState::poisoned && gate.accepted_bytes().empty(),"failed output exposed");
    check(gate.begin(true,good,1000).code==GpuGateCode::wrong_state,"failed gate reopened");
    check(gate.complete(emulate(batch,attempt.generation),snapshot(1001),1001)==GpuGateCode::wrong_state,"late output accepted");
  };
  failed([](auto& r,auto&,auto&){r.generation=0;},GpuGateCode::stale_result);
  failed([](auto& r,auto&,auto&){r.bytes.pop_back();},GpuGateCode::invalid_output);
  failed([](auto& r,auto&,auto&){r.bytes.push_back(std::byte{0});},GpuGateCode::invalid_output);
  failed([](auto& r,auto&,auto&){r.status.pop_back();},GpuGateCode::invalid_output);
  failed([](auto& r,auto&,auto&){r.status[0]=gpu_detail::pending;},GpuGateCode::task_failed);
  failed([](auto& r,auto&,auto&){r.bytes.back()^=std::byte{1};},GpuGateCode::mismatch);
  failed([](auto&,auto& a,auto&){a.nonlocal_usage+=29ULL<<20;},GpuGateCode::nonlocal_growth);
  failed([](auto&,auto& a,auto&){a.local_budget=a.local_usage;},GpuGateCode::insufficient_headroom);
  failed([](auto&,auto&,auto& now){now=1100;},GpuGateCode::expired);
  failed([](auto&,auto&,auto& now){now=999;},GpuGateCode::expired);
  failed([](auto&,auto& a,auto&){a.valid=false;},GpuGateCode::invalid_budget);
  failed([](auto&,auto& a,auto&){a.captured_ms=990;},GpuGateCode::stale_budget);
  {GpuResultGate gate(batch,good.luid,1);const auto g=gate.begin(true,good,1000);
    check(g.code==GpuGateCode::admitted && gate.expire(1099)==GpuGateCode::admitted,"early deadline");
    check(gate.expire(1100)==GpuGateCode::expired && gate.accepted_bytes().empty(),"timeout did not poison");}
  {GpuResultGate gate(batch,good.luid,1);gate.begin(true,good,1000);
    check(gate.worker_failed()==GpuGateCode::worker_failed && gate.accepted_bytes().empty(),"worker failure not latched");}
  {auto s=good;s.captured_ms=(std::numeric_limits<std::uint64_t>::max)();
    denied(s,GpuGateCode::invalid_budget,s.captured_ms);}
  {GpuResultGate one(batch,good.luid,1),two(batch,good.luid,1);
    check(one.begin(true,good,1000).generation!=two.begin(true,good,1000).generation,"generation reused across gates");}
  // Allocation-free kernel entry rejects hostile task metadata without corrupting guards.
  auto task=batch.tasks().front();const auto original=task;std::array<unsigned char,64> dst{};
  auto kernel_rejects=[&](gpu_detail::Task t){
    dst.fill(0x5a);const auto s=gpu_detail::decode_task(t,
      reinterpret_cast<const unsigned char*>(batch.payload().data()),static_cast<unsigned>(batch.payload().size()),dst.data()+8,9);
    check(s!=0,"malformed task accepted");
    check(std::all_of(dst.begin(),dst.begin()+8,[](auto c){return c==0x5a;}) &&
      std::all_of(dst.begin()+17,dst.end(),[](auto c){return c==0x5a;}),"malformed task crossed output bounds");
  };
  for(auto n:{0U,65537U,0xffffffffU}){task=original;task.decoded_bytes=n;kernel_rejects(task);}
  task=original;task.input_offset=0xffffffffU;kernel_rejects(task);
  task=original;task.output_offset=0xffffffffU;kernel_rejects(task);
  task=original;task.input_bytes=0xffffffffU;kernel_rejects(task);
  task=original;task.segment_offset=1;kernel_rejects(task);
  task=original;task.segment_length=4097;kernel_rejects(task);
  task=original;task.kind=255;kernel_rejects(task);
  task=original;task.fill=256;kernel_rejects(task);
  std::cout<<"PASS: "<<checks<<" batch/semantic/gate assertions; CPU execution only, no HIP runtime.\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"failed after "<<checks<<" assertions: "<<e.what()<<'\n';return 1;}}
