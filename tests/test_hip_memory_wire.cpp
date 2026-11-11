#include "hip_memory_wire.hpp"
#include "hip_identity_wire.hpp"
#include "flitzfs/worker_wire.hpp"
#include <iostream>
#include <string_view>
#include <utility>
namespace {
using namespace flitzfs;
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejected(F&& f){try{f();}catch(const std::invalid_argument&){++checks;return;}
  throw std::runtime_error("accepted malformed memory metadata");}
const HipMemoryBinding binding{0x0807060504030201ULL,3,1,
    {std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7},std::byte{8}},16492,12288};
HipMemoryObservation observed(){
  HipMemoryObservation v;v.binding=binding;v.cpu_verified_bytes=12288;
  v.failure_category=0x46530005;v.failure_stage=31;v.captured=7;v.flags=59;
  for(unsigned i=0;i<3;++i){
    auto& s=v.snapshots[i];s.luid=binding.luid;s.node_mask=1;s.captured_ms=1000+i;
    s.dedicated_bytes=16ULL<<30;s.local_budget=15ULL<<30;s.local_usage=1024ULL*(i+1);
    s.nonlocal_budget=8ULL<<30;s.nonlocal_usage=4096ULL*i;s.valid=true;s.amd_hardware=true;
  }
  return v;
}
}
int main(){
  try{
    // Independent field-position oracle, including the observed refusal fields.
    std::array<std::byte,256> golden{};
    constexpr std::string_view magic="FLHMEM01";
    for(unsigned i=0;i<8;++i){golden[i]=static_cast<std::byte>(magic[i]);golden[8+i]=static_cast<std::byte>(i+1);golden[24+i]=static_cast<std::byte>(i+1);}
    const auto put=[&](std::size_t at,std::uint64_t n,unsigned width=8){for(unsigned i=0;i<width;++i)golden[at+i]=static_cast<std::byte>((n>>(8*i))&255U);};
    put(16,3,4);put(20,1,4);put(32,16492);put(40,12288);put(48,0x46530005,4);put(52,31,4);put(56,7,4);put(60,59,4);
    for(unsigned i=0;i<3;++i){
      const unsigned at=64+64*i;put(at,1000+i);put(at+8,16ULL<<30);put(at+16,15ULL<<30);
      put(at+24,1024ULL*(i+1));put(at+32,8ULL<<30);put(at+40,4096ULL*i);
      for(unsigned j=0;j<8;++j)golden[at+48+j]=static_cast<std::byte>(j+1);
      put(at+56,1,4);put(at+60,3,4);
    }
    const auto value=observed();require(encode_hip_memory_observation(value)==golden,"memory metadata golden bytes changed");
    const auto decoded=decode_hip_memory_observation(golden,binding);
    require(decoded.failure_stage==31 && decoded.failure_category==0x46530005 && decoded.cpu_verified_bytes==12288 &&
        decoded.snapshots[2].nonlocal_usage==8192 && decoded.snapshots[2].nonlocal_budget==(8ULL<<30),"memory observation changed values");
    require(decoded.snapshots[2].physical_total==0 && decoded.snapshots[2].commit_limit==0 &&
        check_gpu_memory_budget(decoded.snapshots[2],binding.luid,1,1002,0)==GpuGateCode::invalid_budget,
        "observation-only historical wire became complete admission evidence");
    for(std::size_t n=0;n<golden.size();++n)rejected([&]{(void)decode_hip_memory_observation(std::span(golden).first(n),binding);});
    auto trailing=std::vector<std::byte>(golden.begin(),golden.end());trailing.push_back(std::byte{0});
    rejected([&]{(void)decode_hip_memory_observation(trailing,binding);});
    for(unsigned at=0;at<40;++at){
      auto bad=golden;bad[at]^=std::byte{1};rejected([&]{(void)decode_hip_memory_observation(bad,binding);});
    }
    for(const auto row:{std::pair{40U,1U},std::pair{48U,0U},std::pair{48U,7U},std::pair{52U,32U},
        std::pair{56U,2U},std::pair{56U,4U},std::pair{56U,5U},std::pair{56U,6U},std::pair{56U,8U},
        std::pair{60U,0U},std::pair{60U,57U},std::pair{60U,64U},std::pair{60U,51U},
        std::pair{60U,43U},std::pair{60U,27U},std::pair{124U,7U}}){
      auto bad=golden;for(unsigned i=0;i<4;++i)bad[row.first+i]=static_cast<std::byte>(row.second>>(8*i));
      rejected([&]{(void)decode_hip_memory_observation(bad,binding);});
    }
    for(unsigned point=0;point<3;++point){
      auto bad=golden;bad[64+64*point+48]^=std::byte{1};
      rejected([&]{(void)decode_hip_memory_observation(bad,binding);});
      bad=golden;bad[64+64*point+56]=std::byte{2};
      rejected([&]{(void)decode_hip_memory_observation(bad,binding);});
    }
    for(unsigned captured:{0U,1U,3U}){
      auto partial=value;partial.captured=captured;partial.cpu_verified_bytes=0;partial.flags=2;
      partial.failure_category=0x46530002;partial.failure_stage=4;
      for(unsigned i=0;i<3;++i)if(!(captured&(1U<<i)))partial.snapshots[i]={};
      const auto bytes=encode_hip_memory_observation(partial);
      require(decode_hip_memory_observation(bytes,binding).captured==captured,"partial observation lost");
      auto bad=bytes;bad[192]=std::byte{1};rejected([&]{(void)decode_hip_memory_observation(bad,binding);});
    }
    auto success=value;success.flags=63;success.failure_category=0;success.failure_stage=0;
    require(decode_hip_memory_observation(encode_hip_memory_observation(success),binding).cpu_verified_bytes==12288,"completed diagnostic lost bytes");
    success.flags&=~1U;rejected([&]{(void)encode_hip_memory_observation(success);});
    success=value;success.flags=63;rejected([&]{(void)encode_hip_memory_observation(success);});
    success=value;success.flags=63;success.failure_category=0;success.failure_stage=0;success.cpu_verified_bytes=0;
    rejected([&]{(void)encode_hip_memory_observation(success);});
    for(unsigned field=0;field<8;++field){
      auto bad=value;
      switch(field){case 0:bad.binding.request_id=0;break;case 1:bad.binding.ordinal=64;break;
        case 2:bad.binding.node_mask=0;break;case 3:bad.binding.node_mask=3;break;
        case 4:bad.binding.luid={};break;case 5:bad.binding.device_bytes=0;break;
        case 6:bad.binding.device_bytes=262145;break;case 7:bad.binding.decoded_bytes=262145;break;}
      rejected([&]{(void)encode_hip_memory_observation(bad);});
    }
    auto wrong=binding;wrong.decoded_bytes=12287;rejected([&]{(void)decode_hip_memory_observation(golden,wrong);});
    rejected([&]{(void)decode_worker_request(golden);});
    const std::array<std::byte,1> raw{std::byte{3}};
    const std::array<GpuBlockView,1> blocks{{{0,0,1,raw}}};
    const auto batch=PreparedGpuBatch::prepare(blocks);
    rejected([&]{(void)accept_worker_reply(golden,batch,1,WorkerExecution::hip);});
    rejected([&]{(void)decode_hip_identity_reply(golden,{1,3,1,binding.luid});});
    const auto request=encode_worker_request(batch,1);rejected([&]{(void)decode_hip_memory_observation(request,binding);});
    std::cout<<"HIP memory metadata wire: "<<checks<<" checks; CPU only\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
