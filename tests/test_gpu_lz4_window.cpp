#include "flitzfs/gpu_lz4_window.hpp"
#include "rocm_compile/hipcomp_uniform_lz4_api.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
using namespace flitzfs;using namespace flitzfs::uniform_lz4;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
GpuHealthEvidence evidence(std::uint64_t ram,std::uint64_t gpu){
  GpuHealthEvidence e;e.configuration[0]=std::byte{7};e.sequence=1;
  e.compatible=e.supervisor_quiescent=true;
  auto& m=e.memory;m.luid[0]=std::byte{3};m.node_mask=1;m.captured_ms=100;
  m.valid=m.amd_hardware=true;m.dedicated_bytes=m.local_budget=gpu;
  m.local_usage=gpu/8;m.nonlocal_budget=ram/2;m.nonlocal_usage=ram/64;
  m.physical_total=ram;m.physical_available=ram*3/4;
  m.commit_limit=ram*2;m.commit_available=ram;return e;
}
GpuLz4ResidentCost cost(const GpuHealthEvidence& e){
  GpuLz4ResidentCost c;c.fixed_bytes=8ULL<<20;c.input_copies=2;c.slab_copies=2;
  c.output_copies=1;c.per_chunk_bytes=64;c.configuration=e.configuration;c.recovery_generation=e.recovery_generation;
  c.profiled_chunk_bytes=65536;c.maximum_grid_x=1U<<24;c.kernel_ns_per_chunk=65536;
  c.qualified_kernel_quantum_ns=200000000;c.qualified=true;return c;
}
auto plan(std::uint64_t n,const GpuHealthEvidence& e,std::uint32_t divisor=16){
  return plan_gpu_lz4_window(n,65536,e,100,cost(e),{divisor,100000000});
}
void stub32(unsigned char*,uniform_lz4::u64,uniform_lz4::Layout){}
void stub64(unsigned char*,uniform_lz4::u64,uniform_lz4::Layout){}
void canonical(){
  auto api=uniform_lz4::make_kernel_api(stub32,stub64);
  require(uniform_lz4::compatible_kernel_api(api),"typed adaptive kernel API rejected");
  for(unsigned fault=0;fault<6;++fault){
    auto changed=api;
    if(fault==0)changed.size_bytes--;
    if(fault==1)changed.layout_bytes++;
    if(fault==2)changed.version++;
    if(fault==3)changed.chunk_bytes--;
    if(fault==4)changed.wave32=nullptr;
    if(fault==5)changed.wave64=nullptr;
    require(!uniform_lz4::compatible_kernel_api(changed),"incompatible adaptive kernel API admitted");
  }
  for(const auto width:{1U,14U,15U,16U,254U,255U,256U,4096U,16385U,65536U}){
    Layout p;require(make_layout(std::uint64_t(width)*3+1,width,p) &&
        valid_layout(p,p.total_bytes) && p.chunks==4,"variable block layout rejected");
    Chunk last;require(chunk(p,3,last) && last.input_bytes==1,"partial final block lost");
  }
  require(valid_geometry(32,32,32,1,1,256,1,1,256) &&
      !valid_geometry(32,32,32,1,1,255,1,1,256) &&
      !valid_geometry(32,64,32,1,1,256,1,1,256),"dynamic grid/wave admission");

  for(const auto size:{1ULL,65535ULL,65536ULL,65537ULL,262144ULL,1048576ULL,
      16ULL<<20,1ULL<<30,8ULL<<30}){
    Layout p;require(make_layout(size,65536,p) && valid_layout(p,p.total_bytes),
        "canonical scalable layout rejected");
    require(p.input_bytes==size && p.chunks==size/65536+(size%65536!=0) &&
        p.output_sizes==0 && p.input_offset==std::uint64_t(p.chunks)*8,
        "layout input/count/table arithmetic");
    require(p.input_offset+size<=p.output_offset &&
        p.output_offset+std::uint64_t(p.chunks)*p.output_stride<=p.scratch_offset &&
        p.scratch_offset+std::uint64_t(p.chunks)*p.scratch_stride==p.total_bytes,
        "canonical arenas overlap or omit allocation bytes");
    for(const auto index:std::array<std::uint32_t,2>{0,p.chunks-1}){
      Chunk out;require(chunk(p,index,out) && out.input==p.input_offset+std::uint64_t(index)*65536 &&
          out.input_bytes && out.input_bytes<=65536 &&
          out.output+p.output_stride<=p.scratch_offset &&
          out.scratch+p.scratch_stride<=p.total_bytes,"constant-time chunk bounds");
    }
    Chunk out;require(!chunk(p,p.chunks,out) && !out.input_bytes,"out-of-range chunk admitted");
  }
  Layout p;require(make_layout(16ULL<<20,65536,p) && p.chunks>8 && p.total_bytes>262144,
      "legacy device or descriptor ceiling retained");
  // Independent field mutations include all 80 ABI bytes.
  using Words=std::array<std::uint32_t,sizeof(Layout)/sizeof(std::uint32_t)>;
  const auto original=std::bit_cast<Words>(p);
  for(std::size_t i=0;i<original.size();++i)for(unsigned bit=0;bit<32;++bit){
    auto words=original;words[i]^=std::uint32_t{1}<<bit;
    require(!valid_layout(std::bit_cast<Layout>(words),p.total_bytes),"changed canonical layout admitted");
  }
  for(const auto chunk_bytes:{0U,65537U,(std::numeric_limits<std::uint32_t>::max)()}){
    p.input_bytes=99;require(!make_layout(100,chunk_bytes,p) && !p.total_bytes && !p.input_bytes,
        "invalid chunk size retained partial output");
  }
  require(!make_layout(0,65536,p),"empty work admitted");
  require(!make_layout((std::numeric_limits<std::uint64_t>::max)(),65536,p),
      "unrepresentable grid/offset work admitted");
  std::uint64_t out=0;
  require(!add(~std::uint64_t{0},1,out) && !multiply(~std::uint64_t{0},2,out),
      "checked allocation arithmetic overflowed");
}
void automatic(){
  const auto small=evidence(4ULL<<30,1ULL<<30),large=evidence(64ULL<<30,16ULL<<30);
  const auto a=plan(8ULL<<30,small),b=plan(8ULL<<30,large);
  require(a.code==GpuGateCode::admitted && b.code==GpuGateCode::admitted &&
      a.layout.input_bytes<b.layout.input_bytes && b.layout.total_bytes>262144,
      "capacity did not adapt to different physical/GPU budgets");
  require(a.remaining_input_bytes==(8ULL<<30)-a.layout.input_bytes &&
      b.remaining_input_bytes==(8ULL<<30)-b.layout.input_bytes,"stream remainder lost");
  const auto background=plan(8ULL<<30,large,64);
  require(background.code==GpuGateCode::admitted &&
      background.layout.input_bytes<b.layout.input_bytes,"background share did not reduce work");
  const auto requested=plan(65537,large);
  require(requested.code==GpuGateCode::admitted && requested.layout.input_bytes==65537 &&
      !requested.remaining_input_bytes,"small workload overallocated to available memory");
  auto pressure=large;pressure.memory.physical_available=pressure.memory.physical_total/8;
  const auto reduced=plan(8ULL<<30,pressure);
  require(reduced.code==GpuGateCode::admitted && reduced.layout.input_bytes<b.layout.input_bytes,
      "physical pressure did not reduce window");
  pressure=large;pressure.memory.commit_available=pressure.memory.commit_limit/8;
  require(plan(8ULL<<30,pressure).layout.input_bytes<b.layout.input_bytes,
      "commit pressure did not reduce window");
  pressure=large;pressure.memory.local_usage=pressure.memory.local_budget*3/4;
  require(plan(8ULL<<30,pressure).layout.input_bytes<b.layout.input_bytes,
      "GPU pressure did not reduce window");
  for(unsigned fault=0;fault<14;++fault){
    auto e=large;auto c=cost(e);
    if(fault==0)e.memory.valid=false;
    if(fault==1)e.memory.captured_ms=49;
    if(fault==2)e.memory.physical_available=0;
    if(fault==3)e.compatible=false;
    if(fault==4)c.qualified=false;
    if(fault==5)c.configuration[1]=std::byte{1};
    if(fault==6)e.recovery_generation++;
    if(fault==7)c.input_copies=0;
    if(fault==8)c.per_chunk_bytes=(std::numeric_limits<std::uint64_t>::max)();
    if(fault==9)c.fixed_bytes=(std::numeric_limits<std::uint64_t>::max)();
    if(fault==10)c.profiled_chunk_bytes=4096;
    if(fault==11)c.kernel_ns_per_chunk=0;
    if(fault==12)c.qualified_kernel_quantum_ns=1;
    if(fault==13)c.maximum_grid_x=0;
    const auto refused=plan_gpu_lz4_window(16ULL<<20,65536,e,100,c,{16,100000000});
    require(refused.code!=GpuGateCode::admitted && !refused.layout.total_bytes &&
        refused.remaining_input_bytes==(16ULL<<20),"unknown/stale/unqualified resources admitted");
  }
  const auto c=cost(large);
  require(plan_gpu_lz4_window(16ULL<<20,65536,large,100,c,{1,100000000}).code!=GpuGateCode::admitted,
      "unsafe work share bypassed interference guard");
  auto slower=cost(large);slower.kernel_ns_per_chunk*=8;
  const auto slow=plan_gpu_lz4_window(8ULL<<30,65536,large,100,slower,{16,100000000});
  const auto interactive=plan_gpu_lz4_window(8ULL<<30,65536,large,100,c,{16,10000000});
  require(slow.code==GpuGateCode::admitted && slow.layout.input_bytes<b.layout.input_bytes &&
      slow.estimated_kernel_ns<=100000000,"measured GPU cost did not reduce work");
  require(interactive.code==GpuGateCode::admitted &&
      interactive.layout.input_bytes<b.layout.input_bytes &&
      interactive.estimated_kernel_ns<=10000000,"foreground quantum did not reduce work");
  auto limited=cost(large);limited.maximum_grid_x=2;
  const auto grid=plan_gpu_lz4_window(16ULL<<20,65536,large,100,limited,{16,100000000});
  require(grid.code==GpuGateCode::admitted && grid.layout.chunks==2 &&
      grid.remaining_input_bytes==(16ULL<<20)-2*65536,"detected grid capacity ignored");
  std::cout<<"Adaptive LZ4 windows: small_input="<<a.layout.input_bytes
      <<" large_input="<<b.layout.input_bytes<<" background_input="<<background.layout.input_bytes
      <<" gpu_executed=false\n";
}
}
int main(){try{canonical();automatic();std::cout<<"PASS: "<<checks
    <<" scalable layout/resource window checks; no payload allocation or GPU execution\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
