#include "extent_codec.hpp"
#include "extent_codec_fixture.hpp"
#include <iostream>
#include <limits>
#ifdef FLITZFS_NATIVE_ORACLE
bool reference_native_decode(std::uint8_t, std::uint8_t, std::span<const std::byte>, std::span<std::byte>);
#endif

namespace {
using namespace flitzfs::detail;
using namespace extent_fixture;
std::size_t checks=0;
void require(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F&& operation) {
  try { operation(); }
  catch(const ExtentCodecError&){++checks;return;}
  throw std::runtime_error("malformed extent payload accepted");
}
void reject_block(const Block& b) {
  rejects([&]{Bytes out(b.plain.size());decode_extent(b.kind,b.fill,b.encoded,out);});
}
void roundtrip(const Block& b) {
  Bytes guarded(b.plain.size()+32,std::byte{0xa7});
  auto output=std::span<std::byte>(guarded).subspan(16,b.plain.size());
  decode_extent(b.kind,b.fill,b.encoded,output);
  require(std::equal(output.begin(),output.end(),b.plain.begin(),b.plain.end()),"independent extent codec whole-block parity");
  require(std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==std::byte{0xa7};}) &&
      std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==std::byte{0xa7};}),"extent decoder changed guard bytes");
#ifdef FLITZFS_NATIVE_ORACLE
  if(b.kind!=0) {
    Bytes reference(b.plain.size());
    require(reference_native_decode(b.kind,b.fill,b.encoded,reference),"pinned mechanism reference rejected fixture");
    require(reference==b.plain,"independent pinned CPU decoding mismatch");
  }
#endif
}
void change(const Block& good,std::size_t pos,std::uint64_t value,unsigned width=1) {
  auto bad=good;replace(bad.encoded,pos,value,width);reject_block(bad);
}
}
int main() {
  try {
    const auto samples=native_samples();
    for(const auto& b:samples) roundtrip(b);
    for(const auto n:{4095U,4096U,4097U,8191U,8192U,8193U}) {
      roundtrip(prefix_sample(false,n));roundtrip(prefix_sample(true,n));
    }
    // Compound plaintext is an encoded original stage, not ordinary user data.
    roundtrip(compound(pattern(16384,std::string(256,'z')),true));
    for(unsigned width:{2U,4U,8U}) {
      roundtrip(byte_plane(width,prefix_sample(true,10000+width-1)));
      roundtrip(byte_plane(width,huffman(32768+width-1)));
      roundtrip(byte_plane(width,sparse(true)));
    }
    for(bool adaptive:{false,true}) {
      const auto b=prefix_sample(adaptive);
      const auto start=adaptive?84U:0U;
      change(b,start,1,4);change(b,start+4,0xffffffffU,4);
      change(b,start+8,1,4);change(b,start+12,0,4);
      auto broken=b;broken.encoded.resize(start+15);reject_block(broken);
      broken=b;broken.encoded.push_back(std::byte{0});reject_block(broken);
      broken=b;broken.encoded.pop_back();reject_block(broken);
      broken=b;replace(broken.encoded,start+4,0,4);reject_block(broken);
    }
    // Static high group allows only 84+0..171. Invalid code must not wrap a byte.
    auto high=prefix(Bytes(256,std::byte{0}));
    high.encoded[8]=std::byte{0xff};high.encoded[9]=std::byte{0xff};reject_block(high);
    auto tiny=prefix(Bytes(40,std::byte{0}));
    for(std::size_t n=0;n<tiny.encoded.size();++n) {
      auto broken=tiny;broken.encoded.resize(n);reject_block(broken);
    }
    const auto h=huffman();
    change(h,1,0);change(h,1,13);change(h,2,0);change(h,8192,1,4);
    change(h,8196,0xffffffffU,4);
    auto hshort=h;hshort.encoded.resize(8191);reject_block(hshort);
    // Valid complete two-leaf table with absent alphabet symbols.
    Bytes two_table;for(unsigned i=0;i<4096;++i){put(two_table,i%2?0x92:0x18,1);put(two_table,1,1);}
    const auto two_plain=pattern(16385,std::string("\x18\x92",2)).plain;
    auto two_stream=segments(two_plain,two_table,[](BitWriter& bits,unsigned c){bits.write(c==0x92?1:0,1);});
    roundtrip({10,0,two_plain,two_stream});
    for(bool long_motif:{false,true}) {
      const auto b=sparse(long_motif);const std::size_t period=long_motif?16385:3, patch=8+period;
      change(b,0,0,4);change(b,0,long_motif?16384:16385,4);change(b,4,0,4);change(b,4,0xffffffffU,4);
      change(b,patch,period-1,4);change(b,patch,b.plain.size(),4);
      change(b,patch+4,std::to_integer<unsigned>(b.encoded[8])); // redundant correction
      change(b,patch+5,period,4); // duplicate correction position
      auto broken=b;broken.encoded.pop_back();reject_block(broken);
      broken=b;broken.encoded.push_back(std::byte{0});reject_block(broken);
    }
    for(const auto& b:samples) if(b.kind>=11) {
      auto noncanonical=b;noncanonical.fill=1;reject_block(noncanonical);
      change(b,3,1);
      if(b.kind==11) {
        for(unsigned kind:{0U,2U,8U,11U,12U,13U,255U}){change(b,0,kind);change(b,1,kind);}
        change(b,4,0,4);change(b,4,b.encoded.size(),4);change(b,4,b.plain.size(),4);change(b,4,0xffffffffU,4);
        if(b.encoded[0]!=std::byte{1}) change(b,2,1);
      } else {
        for(unsigned width:{0U,1U,3U,16U,255U})change(b,0,width);
        if(b.kind==12) {
          for(unsigned kind:{0U,2U,8U,11U,12U,13U,255U}) change(b,1,kind);
          if(b.encoded[1]!=std::byte{1})change(b,2,1);
        } else {
          change(b,1,1);change(b,2,1);change(b,6,1,4); // fill may not carry bytes
          change(b,12,0xffffffffU,4);change(b,11,1); // raw plane fill must be zero
          for(unsigned kind:{2U,8U,11U,12U,13U,255U}) change(b,4,kind);
        }
      }
      auto broken=b;broken.encoded.push_back(std::byte{0});reject_block(broken);
      broken=b;broken.encoded.pop_back();reject_block(broken);
    }
#ifdef FLITZFS_TEST_HAS_LZ4
    require(extent_codec_available(6),"linked LZ4 capability missing");
    for(auto n:{65535U,65536U,65537U})roundtrip(dictionary(pattern(n,"bounded-LZ4!").plain));
    const auto d=dictionary(pattern(131089,"dictionary native").plain);
    change(d,0,1,4);change(d,4,0,4);change(d,4,0xffffffffU,4);change(d,8,1,4);
    auto bad=d;
    // A compressed representation requiring an external history cannot read it.
    bad=d;bad.encoded[16]=std::byte{0};bad.encoded[17]=std::byte{0xff};bad.encoded[18]=std::byte{0xff};reject_block(bad);
#else
    require(!extent_codec_available(6),"missing LZ4 reported available");
    for(auto kind:{6U,12U,13U}) {
      Block b{6,0,Bytes(256,std::byte{0}),Bytes(8,std::byte{0})};
      if(kind==12)b=byte_plane(2,b);
      if(kind==13)b=contexts({fill(256,0),b});
      bool unsupported=false;
      try{Bytes out(b.plain.size());decode_extent(b.kind,b.fill,b.encoded,out);}
      catch(const ExtentCodecError& e){unsupported=e.code()==ExtentCodecErrorCode::unsupported_codec;}
      require(unsupported,"missing nested dictionary dependency not explicit");
    }
#endif
    roundtrip(raw(text("filesystem extent bytes")));roundtrip(fill(4097,17));
    rejects([&]{Bytes out(kMaxExtentCodecBytes+1);decode_extent(4,0,{},out);});
    rejects([&]{Bytes out(32);decode_extent(255,0,{},out);});
    auto noncanonical=prefix_sample();noncanonical.fill=1;reject_block(noncanonical);
    require(!extent_codec_available(14) && !extent_codec_available(255),"unknown extent codec accepted");
    std::cout<<"PASS: "<<checks<<" native codec assertions; independent fixtures, exact bytes, guard bytes, framing rejection and bounded output.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"native codec failure after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
