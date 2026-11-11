#include "hip_identity_wire.hpp"
#include "flitzfs/worker_wire.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using namespace flitzfs;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("malformed metadata accepted");}
}
int main() {
  try {
    const HipIdentityRequest request{0x0807060504030201ULL,7,1,
        {std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7},std::byte{8}}};
    const std::array<std::byte,40> golden{
      std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'I'},std::byte{'D'},std::byte{'0'},std::byte{'0'},std::byte{'1'},
      std::byte{1},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},
      std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7},std::byte{8},
      std::byte{7},std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{0},std::byte{0},std::byte{0},
      std::byte{1},std::byte{2},std::byte{3},std::byte{4},std::byte{5},std::byte{6},std::byte{7},std::byte{8}};
    require(encode_hip_identity_request(request)==golden,"request little-endian layout differs");
    const auto decoded=decode_hip_identity_request(golden);
    require(decoded.request_id==request.request_id && decoded.ordinal==7 && decoded.node_mask==1 &&
        decoded.luid==request.luid,"golden request identity lost");
    for(const auto mask:{0U,1U,2U,0x80000000U,0xffffffffU}) {
      auto reply=golden;reply[8]=std::byte{2};
      for(unsigned i=0;i<4;++i)reply[28+i]=static_cast<std::byte>(mask>>(8*i));
      const HipIdentityObservation observed{request.request_id,7,mask,request.luid};
      require(encode_hip_identity_reply(observed)==reply,"reply little-endian layout differs");
      const auto actual=decode_hip_identity_reply(reply,request);
      require(actual.node_mask==mask && actual.luid==request.luid,"raw metadata mask was normalized");
      rejects([&]{(void)decode_hip_identity_request(reply);});
    }
    const HipIdentityObservation unreported{request.request_id,7,0,{}};
    const auto reply=encode_hip_identity_reply(unreported);
    const auto zero=decode_hip_identity_reply(reply,request);
    require(zero.luid==unreported.luid && zero.node_mask==0,"unreported metadata was fabricated");
    for(std::size_t length=0;length<golden.size();++length) {
      rejects([&]{(void)decode_hip_identity_request(std::span(golden).first(length));});
      rejects([&]{(void)decode_hip_identity_reply(std::span(reply).first(length),request);});
    }
    auto extra=std::vector<std::byte>(golden.begin(),golden.end());extra.push_back(std::byte{0});
    rejects([&]{(void)decode_hip_identity_request(extra);});
    rejects([&]{(void)decode_hip_identity_reply(extra,request);});
    for(std::size_t at=0;at<16;++at) {
      auto bad=golden;bad[at]^=std::byte{0x80};
      rejects([&]{(void)decode_hip_identity_request(bad);});
      auto invalid=reply;invalid[at]^=std::byte{0x80};
      rejects([&]{(void)decode_hip_identity_reply(invalid,request);});
    }
    for(unsigned field=0;field<5;++field) {
      auto frame=golden;
      if(field==0)std::fill_n(frame.begin()+16,8,std::byte{0});
      if(field==1)frame[24]=std::byte{64};
      if(field==2)std::fill_n(frame.begin()+28,4,std::byte{0});
      if(field==3)frame[28]=std::byte{3};
      if(field==4)std::fill_n(frame.begin()+32,8,std::byte{0});
      rejects([&]{(void)decode_hip_identity_request(frame);});
    }
    for(unsigned field=0;field<5;++field) {
      auto bad=request;
      if(field==0)bad.request_id=0;
      if(field==1)bad.ordinal=64;
      if(field==2)bad.node_mask=0;
      if(field==3)bad.node_mask=3;
      if(field==4)bad.luid={};
      rejects([&]{(void)encode_hip_identity_request(bad);});
      rejects([&]{(void)decode_hip_identity_reply(reply,bad);});
    }
    for(unsigned field=0;field<2;++field) {
      auto changed=request;if(field==0)++changed.request_id;else ++changed.ordinal;
      rejects([&]{(void)decode_hip_identity_reply(reply,changed);});
    }
    auto different=unreported;different.luid[0]=std::byte{99};
    require(decode_hip_identity_reply(encode_hip_identity_reply(different),request).luid==different.luid,
        "metadata hid a different observed LUID");
    const GpuBlockView fill{1,42,32,{}};
    const auto batch=PreparedGpuBatch::prepare({&fill,1});
    const auto ordinary=encode_worker_request(batch,1);
    rejects([&]{(void)decode_hip_identity_request(ordinary);});
    rejects([&]{(void)accept_worker_reply(reply,batch,1,WorkerExecution::hip);});
    std::cout<<"HIP identity metadata wire: checks="<<checks<<" gpu=false\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
