#include "flitzfs/gpu_lz4_layout.hpp"
#include "rocm_compile/hipcomp_lz4_api.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
namespace {
using namespace flitzfs;
using namespace flitzfs::candidate_lz4;
unsigned checks=0,shapes=0,mutations=0,geometries=0;
void require(bool value,const char* why) {
  ++checks;if(!value)throw std::runtime_error(why);
}
Layout convert(const GpuLz4DeviceLayout& p) {
  Layout result;
  for(unsigned i=0;i<8;++i)result.chunks[i]=
      {p.chunks[i].input_offset,p.chunks[i].input_bytes,p.chunks[i].output_offset,p.chunks[i].output_capacity};
  result.count=p.count;result.input_bytes=p.input_bytes;
  result.input_pointers=p.input_pointers;result.input_sizes=p.input_sizes;
  result.output_pointers=p.output_pointers;result.output_sizes=p.output_sizes;
  result.scratch_offset=p.scratch_offset;result.hash_table_entries=p.hash_table_entries;
  result.scratch_bytes=p.scratch_bytes;result.total_bytes=p.total_bytes;
  return result;
}
Layout admit(std::span<const u32> sizes) {
  const auto plan=gpu_lz4_device_layout(sizes);const auto p=convert(plan);
  require(valid_layout(p,plan.total_bytes),"Actual planner/entry layout disagreement");
  require(!valid_layout(p,0) && !valid_layout(p,plan.total_bytes-1) &&
      !valid_layout(p,plan.total_bytes+1),"Allocation extent must match exactly");
  ++shapes;return p;
}
void corrupt(const Layout& p) {
  using Words=std::array<u32,sizeof(Layout)/sizeof(u32)>;
  const auto original=std::bit_cast<Words>(p);
  for(std::size_t i=0;i<original.size();++i) {
    for(unsigned bit=0;bit<32;++bit) {
      auto words=original;words[i]^=u32{1}<<bit;
      const auto candidate=std::bit_cast<Layout>(words);
      require(!valid_layout(candidate,p.total_bytes),"Changed canonical field admitted");
      ++mutations;
    }
    auto words=original;words[i]=std::numeric_limits<u32>::max();
    require(!valid_layout(std::bit_cast<Layout>(words),p.total_bytes),"Arithmetic extreme admitted");
    ++mutations;
  }
  require(std::bit_cast<Words>(p)==original,"Pure admission changed source metadata");
}
unsigned calls32=0,calls64=0;
void stub32(unsigned char* output,u32 allocated,Layout p) {
  require(allocated==p.total_bytes && valid_layout(p,allocated),"Typed wave32 arguments");
  output[0]=32;++calls32;
}
void stub64(unsigned char* output,u32 allocated,Layout p) {
  require(allocated==p.total_bytes && valid_layout(p,allocated),"Typed wave64 arguments");
  output[0]=64;++calls64;
}
void kernel_api(const Layout& p) {
  const auto api=make_kernel_api(stub32,stub64);
  require(compatible_kernel_api(api),"Compatible private kernel API");
  require(select_kernel(api,32)==stub32 && select_kernel(api,64)==stub64,"Typed entry selection");
  unsigned char output=0;
  select_kernel(api,32)(&output,p.total_bytes,p);
  require(output==32 && calls32==1 && calls64==0,"CPU wave32 argument forwarding");
  select_kernel(api,64)(&output,p.total_bytes,p);
  require(output==64 && calls32==1 && calls64==1,"CPU wave64 argument forwarding");
  for(const u32 width:{0U,1U,31U,33U,63U,65U,0xffffffffU})
    require(!select_kernel(api,width),"Unknown wave selected");
  const std::array<u32 KernelApi::*,8> fields{
    &KernelApi::size_bytes,&KernelApi::layout_bytes,&KernelApi::allocation_bytes,
    &KernelApi::chunk_bytes,&KernelApi::chunks,&KernelApi::table_entry_bytes,
    &KernelApi::output_guard_bytes,&KernelApi::reserved};
  for(const auto field:fields)for(unsigned bit=0;bit<32;++bit) {
    auto changed=api;changed.*field^=u32{1}<<bit;
    require(!compatible_kernel_api(changed) && !select_kernel(changed,32) &&
        !select_kernel(changed,64),"Incompatible private API admitted");
  }
  for(unsigned missing=0;missing<3;++missing) {
    auto changed=api;
    if(missing!=1)changed.wave32=nullptr;
    if(missing!=0)changed.wave64=nullptr;
    require(!compatible_kernel_api(changed) && !select_kernel(changed,32) &&
        !select_kernel(changed,64),"Missing private kernel entry admitted");
  }
}
void geometry() {
  for(const u32 wave:{32U,64U})for(u32 count=1;count<=8;++count) {
    require(valid_geometry(wave,wave,wave,1,1,count,1,1,count),"Valid one-wave geometry refused");
    for(const u32 bad:{0U,1U,2U,31U,33U,63U,65U,0xffffffffU}) {
      require(!valid_geometry(bad,wave,wave,1,1,count,1,1,count),"Invalid selected wave");
      require(!valid_geometry(wave,bad,wave,1,1,count,1,1,count),"Invalid hardware wave");
      require(!valid_geometry(wave,wave,bad,1,1,count,1,1,count),"Invalid block width");
      ++geometries;
    }
    require(!valid_geometry(wave,wave==32?64:32,wave,1,1,count,1,1,count),"Compiled/reported width mismatch");
    for(const u32 bad:{0U,2U,0xffffffffU}) {
      require(!valid_geometry(wave,wave,wave,bad,1,count,1,1,count),"Block y");
      require(!valid_geometry(wave,wave,wave,1,bad,count,1,1,count),"Block z");
      require(!valid_geometry(wave,wave,wave,1,1,count,bad,1,count),"Grid y");
      require(!valid_geometry(wave,wave,wave,1,1,count,1,bad,count),"Grid z");
      ++geometries;
    }
    require(!valid_geometry(wave,wave,wave,1,1,count+1,1,1,count),"Extra block");
    require(!valid_geometry(wave,wave,wave,1,1,count-1,1,1,count),"Missing block");
    require(!valid_geometry(wave,wave,wave,1,1,0,1,1,0),"Empty geometry");
    require(!valid_geometry(wave,wave,wave,1,1,9,1,1,9),"Excessive geometry");
  }
}
}
int main() {
  try {
    require(!valid_layout({},0),"Empty layout");
    for(u32 size=1;size<=65536;++size) {
      const std::array<u32,1> sizes{size};(void)admit(sizes);
    }
    const std::array<u32,8> tiny{1,2,15,16,17,64,65,1024};
    for(std::size_t count=1;count<=tiny.size();++count)corrupt(admit(std::span(tiny).first(count)));
    const std::array<u32,1> maximum{65536};corrupt(admit(maximum));
    const std::array<u32,4> large{65536,65536,65536,65536};
    bool refused=false;
    try{(void)gpu_lz4_device_layout(large);}catch(const std::invalid_argument&){refused=true;}
    require(refused,"Complete original allocation cap was raised");
    auto extreme=admit(maximum);extreme.total_bytes=allocation_limit+1;
    require(!valid_layout(extreme,extreme.total_bytes),"Oversized slab");
    geometry();
    const auto api_layout=admit(maximum);
    kernel_api(api_layout);
    std::cout<<"LZ4 entry admission: "<<checks<<" checks, "<<shapes<<" planner shapes, "
        <<mutations<<" field refusals, "<<geometries<<" geometry groups; CPU only\n";
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
