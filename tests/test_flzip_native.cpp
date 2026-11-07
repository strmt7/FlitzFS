#include "flitzfs/flzip_reader.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flzip_native_fixture.hpp"
#include <iostream>
#include <limits>
#ifdef FLITZFS_NATIVE_ORACLE
bool reference_native_decode(std::uint8_t, std::uint8_t, std::span<const std::byte>, std::span<std::byte>);
#endif

namespace {
using namespace flzip_fixture;
std::size_t checks=0;
void require(bool value,const char* message) { ++checks;if(!value)throw std::runtime_error(message); }
template<class F> void rejects(F&& operation) {
  try { operation(); }
  catch(const flitzfs::FlzipError&){++checks;return;}
  catch(const flitzfs::CatalogError&){++checks;return;}
  throw std::runtime_error("malformed native archive accepted");
}
void reject_block(const Block& b) {
  const auto fixture=archive({{"native.bin",false,{b}}},minimum_version(b.kind));
  rejects([&]{(void)flitzfs::FlzipReader::from_bytes(fixture.bytes);});
}
void roundtrip(const Block& b) {
  const auto f=archive({{"Native/data.bin",false,{b}}},minimum_version(b.kind));
  auto reader=flitzfs::FlzipReader::from_bytes(f.bytes);
  require(reader.read("native/DATA.BIN",0,static_cast<std::uint32_t>(b.plain.size()))==b.plain,"native whole-block parity");
  for(const auto offset:{0U,1U,4093U,4096U,65533U,65536U}) if(offset<b.plain.size()) {
    const auto n=static_cast<std::uint32_t>((std::min)(std::size_t{21},b.plain.size()-offset));
    require(reader.read("Native/data.bin",offset,n)==Bytes(b.plain.begin()+offset,b.plain.begin()+offset+n),"native segment-boundary range parity");
  }
  require(reader.read("Native/data.bin",b.plain.size(),0).empty(),"native EOF");
  rejects([&]{reader.read("Native/data.bin",b.plain.size(),1);});
  reader.verify_all();
#ifdef FLITZFS_NATIVE_ORACLE
  Bytes reference(b.plain.size());
  require(reference_native_decode(b.kind,b.fill,b.encoded,reference),"pinned upstream decoder rejected valid fixture");
  require(reference==b.plain,"independent pinned CPU decoding mismatch");
#endif
  // Verify each introduced-version gate, including the composed families.
  const auto version=minimum_version(b.kind);
  if(version>1) rejects([&]{(void)flitzfs::FlzipReader::from_bytes(archive({{"n",false,{b}}},version-1).bytes);});
  // Bounded payload mutations: changed unused padding may remain valid, but
  // accepted content must agree with the original file CRC and expected bytes.
  for(std::size_t k=0;k<12 && !b.encoded.empty();++k) {
    auto bytes=f.bytes;
    const auto position=(k*131+7)%b.encoded.size();bytes[position]^=std::byte{0x80};
    try {auto mutated=flitzfs::FlzipReader::from_bytes(bytes);
      require(mutated.read("Native/data.bin",0,static_cast<std::uint32_t>(b.plain.size()))==b.plain,"mutated archive escaped original-byte check");}
    catch(const flitzfs::FlzipError&) {++checks;}
  }
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
    // Mixed-file reads cross both codec and entropy-segment boundaries.
    Record mixed{"mixed.bin",false,{raw(text("first")),prefix_sample(),sparse(),huffman(),fill(17,42)}};
    auto r=flitzfs::FlzipReader::from_bytes(archive({mixed},11).bytes);
    const auto expected=plain(mixed);
    require(r.read("mixed.bin",0,static_cast<std::uint32_t>(expected.size()))==expected,"mixed-native archive parity");
    require(r.read("mixed.bin",8999,64)==Bytes(expected.begin()+8999,expected.begin()+9063),"cross-codec range parity");
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
    require(flitzfs::flzip_codec_available(6),"linked LZ4 capability missing");
    for(auto n:{65535U,65536U,65537U})roundtrip(dictionary(pattern(n,"bounded-LZ4!").plain));
    const auto d=dictionary(pattern(131089,"dictionary native").plain);
    change(d,0,1,4);change(d,4,0,4);change(d,4,0xffffffffU,4);change(d,8,1,4);
    auto bad=d;bad.encoded.back()^=std::byte{0x80};reject_block(bad);
    // A compressed representation requiring an external history cannot read it.
    bad=d;bad.encoded[16]=std::byte{0};bad.encoded[17]=std::byte{0xff};bad.encoded[18]=std::byte{0xff};reject_block(bad);
#else
    require(!flitzfs::flzip_codec_available(6),"missing LZ4 reported available");
    for(auto kind:{6U,12U,13U}) {
      Block b{6,0,Bytes(256,std::byte{0}),Bytes(8,std::byte{0})};
      if(kind==12)b=byte_plane(2,b);
      if(kind==13)b=contexts({fill(256,0),b});
      bool unsupported=false;
      try{(void)flitzfs::FlzipReader::from_bytes(archive({{"x",false,{b}}},11).bytes);}
      catch(const flitzfs::FlzipError& e){unsupported=e.code()==flitzfs::FlzipErrorCode::unsupported_codec;}
      require(unsupported,"missing nested dictionary dependency not explicit");
    }
#endif
    // Header lengths cannot cause proportional allocation beyond fixed block caps.
    const auto b=prefix_sample();auto huge=archive({{"x",false,{b}}},2);
    replace(huge.bytes,huge.block_fields[0]+2,flitzfs::kMaxFlzipBlockBytes+1,4);
    rejects([&]{(void)flitzfs::FlzipReader::from_bytes(huge.bytes);});
    require(!flitzfs::flzip_codec_available(14) && !flitzfs::flzip_codec_available(255),"unknown native codec accepted");
    std::cout<<"PASS: "<<checks<<" native codec assertions; independent fixtures, exact bytes, framing rejection and bounded ranges.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"native codec failure after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
}
