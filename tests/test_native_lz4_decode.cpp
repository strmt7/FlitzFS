#include "native_lz4_decode.hpp"
#include "store_codec.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
using namespace flitzfs;
using namespace flitzfs::detail;
using namespace flitzfs::native_lz4_decode;
using Bytes=std::vector<std::byte>;

void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

template<class Action> void refused(Action&& action,const char* message) {
  try {action();}
  catch(const std::invalid_argument&) {return;}
  throw std::runtime_error(message);
}

void exercise() {
  const std::array<u8,11> encoded{0x1f,'A',1,0,50,0x50,'B','C','D','E','F'};
  constexpr u32 decoded=75;
  require(valid_block(encoded.data(),encoded.size(),decoded),"valid overlapping native LZ4 block");
  for(u32 size=0;size<encoded.size();++size)
    require(!valid_block(encoded.data(),size,decoded),"truncated native LZ4 block accepted");
  for(const auto offset:{u8{0},u8{2},u8{255}}) {
    auto bad=encoded;bad[2]=offset;
    require(!valid_block(bad.data(),bad.size(),decoded),"invalid backreference accepted");
  }
  require(!valid_block(encoded.data(),encoded.size(),decoded-1) &&
      !valid_block(encoded.data(),encoded.size(),decoded+1),"wrong decoded extent accepted");
  auto bad=encoded;bad[4]=255;
  require(!valid_block(bad.data(),bad.size(),decoded),"overflowing match length accepted");
  bad=encoded;bad[5]=0x40;
  require(!valid_block(bad.data(),bad.size()-1,decoded-1),"short final literal sequence accepted");
  require(!valid_layout({0xffffffffU,0xfffffff0U,0xfffffff0U,version},0xffffffffffffffffULL),
      "invalid allocation product accepted");

  const auto input=std::as_bytes(std::span(encoded));
  StoreIdentity id{};id[0]=std::byte{1};
  NativeLz4DecodeSource frame{{id,2,3,decoded,{}},input};
  if(!store_lz4_available()) {
    refused([&]{(void)NativeLz4DecodeBatch::prepare({&frame,1},{1,4096});},
        "raw-only build admitted a read with no CPU decoder");
    std::cout<<"native_lz4_decode framing=passed cpu_decoder=0\n";
    return;
  }
  StoreCodecDecoder cpu;
  const auto plain=cpu.decode(kStoreLz4,input,decoded);
  require(plain.size()==decoded && plain.front()==std::byte{'A'} && plain.back()==std::byte{'F'},
      "independent LZ4 reference differs");
  frame.key.decoded_digest=record_digest(plain);
  refused([&]{(void)NativeLz4DecodeBatch::prepare({&frame,1},{1,64});},"overgrant slab allocated");
  const std::array frames{frame,frame};
  refused([&]{(void)NativeLz4DecodeBatch::prepare(frames,{1,4096});},"overgrant batch allocated");
  const auto batch=NativeLz4DecodeBatch::prepare(frames,{2,4096});
  const auto layout=batch.layout();
  const auto upload=batch.upload();
  Bytes completed(upload.begin(),upload.end());
  for(u32 i=0;i<layout.chunks;++i) {
    const Record result{static_cast<u32>(encoded.size()),decoded,decoded,0};
    std::memcpy(completed.data()+i*sizeof(Record),&result,sizeof(result));
    std::ranges::copy(plain,completed.begin()+static_cast<std::ptrdiff_t>(output_at(layout,i)));
  }
  const auto accepted=batch.verify(completed);
  require(accepted.size()==2 && accepted[0].bytes==plain && accepted[1].key==frame.key,
      "verified exact native frames changed");
  for(const auto offset:{std::uint64_t{12},output_at(layout,0),output_at(layout,1)+decoded}) {
    auto changed=completed;changed[static_cast<std::size_t>(offset)]^=std::byte{1};
    refused([&]{(void)batch.verify(changed);},"changed completion metadata, byte or guard accepted");
  }
  refused([&]{(void)batch.verify(std::span(completed).first(completed.size()-1));},"short slab accepted");
  std::cout<<"native_lz4_decode framing=passed cpu_decoder=1 frames=2 checked_status_digest_and_guards=1\n";
}
}

int main() {
  try {exercise();}
  catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
