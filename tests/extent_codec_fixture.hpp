#pragma once
// Independent test encoders for compression mechanisms. No archive framing.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <array>
#include <span>
#ifdef FLITZFS_TEST_HAS_LZ4
#include <lz4.h>
#endif
namespace extent_fixture {
using Bytes = std::vector<std::byte>;
inline void put(Bytes& b, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) b.push_back(static_cast<std::byte>((value >> (8U*i)) & 255U));
}
inline void replace(Bytes& b, std::size_t at, std::uint64_t value, unsigned width) {
  if (at > b.size() || width > b.size() - at) throw std::runtime_error("fixture edit out of bounds");
  for (unsigned i = 0; i < width; ++i) b[at+i] = static_cast<std::byte>((value >> (8U*i)) & 255U);
}
inline Bytes text(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()+s.size())};
}
struct Block { std::uint8_t kind = 0, fill = 0; Bytes plain, encoded; };
inline Block raw(Bytes plain) { auto encoded = plain; return {0, 0, std::move(plain), std::move(encoded)}; }
inline Block fill(std::size_t n, std::uint8_t byte) { return {1, byte, Bytes(n, static_cast<std::byte>(byte)), {}}; }
inline Block pattern(std::size_t n, const std::string& motif = "abc") {
  auto bytes = text(motif); Bytes plain(n);
  for (std::size_t i=0;i<n;++i) plain[i]=bytes.at(i%bytes.size());
  return {3, 0, std::move(plain), std::move(bytes)};
}
inline Bytes prefix_book() {
  Bytes book(84);
  for (std::size_t i=0; i<book.size(); ++i) book[i]=static_cast<std::byte>((i*73+19)%256);
  return book;
}
struct BitWriter {
  Bytes bytes;
  std::size_t position=0;
  void write(unsigned value, unsigned width) {
    for (unsigned i=0; i<width; ++i,++position) {
      if ((position%8)==0) bytes.push_back(std::byte{0});
      bytes.back() |= static_cast<std::byte>(((value>>i)&1U)<<(position%8));
    }
  }
};
// Each independent 4096-byte segment is byte-aligned and carries its own offsets.
template<class Encode> inline Bytes segments(const Bytes& plain, const Bytes& header, Encode encode) {
  Bytes stream, table; put(table,0,4);
  for (std::size_t start=0; start<plain.size(); start+=4096) {
    BitWriter bits;
    for (std::size_t i=start; i<(std::min)(start+4096,plain.size()); ++i)
      encode(bits,std::to_integer<unsigned>(plain[i]));
    stream.insert(stream.end(),bits.bytes.begin(),bits.bytes.end()); put(table,stream.size(),4);
  }
  Bytes out=header; out.insert(out.end(),table.begin(),table.end());
  out.insert(out.end(),stream.begin(),stream.end()); return out;
}
inline Block prefix(Bytes plain, bool adaptive=false) {
  const auto book=prefix_book();
  auto out=segments(plain,adaptive?book:Bytes{},[&](BitWriter& bits,unsigned byte) {
    unsigned rank=byte;
    if (adaptive) rank=static_cast<unsigned>(std::find(book.begin(),book.end(),static_cast<std::byte>(byte))-book.begin());
    if (rank<4) { bits.write(0,1); bits.write(rank,2); }
    else if (rank<20) { bits.write(1,2); bits.write(rank-4,4); }
    else if (rank<84) { bits.write(3,3); bits.write(rank-20,6); }
    else { bits.write(7,3); bits.write(adaptive?byte:byte-84,8); }
  });
  return {static_cast<std::uint8_t>(adaptive?5:4),0,std::move(plain),std::move(out)};
}
inline Block prefix_sample(bool adaptive=false,std::size_t n=9001) {
  const auto book=prefix_book(); Bytes plain(n);
  for (std::size_t i=0; i<n; ++i) {
    unsigned value=static_cast<unsigned>(i%4);
    if (i%43==0) value=4+static_cast<unsigned>(i%16);
    if (i%71==0) value=20+static_cast<unsigned>(i%64);
    if (i%97==0) value=84+static_cast<unsigned>(i%172);
    plain[i]=adaptive && value<84?book[value]:static_cast<std::byte>(value);
  }
  return prefix(std::move(plain),adaptive);
}
// An intentionally unbalanced complete tree exercises code widths 1 through 12.
inline Block huffman(std::size_t n=32777) {
  Bytes lookup;
  for (unsigned slot=0; slot<4096; ++slot) {
    unsigned trailing=0;
    while (trailing<12 && (slot&(1U<<trailing))) ++trailing;
    put(lookup,trailing,1); put(lookup,(std::min)(trailing+1,12U),1);
  }
  Bytes plain(n);
  for (std::size_t i=0; i<n; ++i) plain[i]=static_cast<std::byte>(i%29?i%2:i%13);
  auto out=segments(plain,lookup,[](BitWriter& bits,unsigned symbol) {
    const auto width=(std::min)(symbol+1,12U);
    bits.write(symbol==12?4095U:(1U<<symbol)-1,width);
  });
  return {10,0,std::move(plain),std::move(out)};
}
inline Block sparse(bool long_motif=false) {
  const std::size_t period=long_motif?16385:3, n=period*2+43;
  Bytes motif(period), plain(n), encoded;
  for (std::size_t i=0; i<period; ++i) motif[i]=static_cast<std::byte>((i*37+11)%256);
  for (std::size_t i=0; i<n; ++i) plain[i]=motif[i%period];
  const std::array<std::size_t,3> positions{period,period*2-1,n-1};
  put(encoded,period,4); put(encoded,positions.size(),4);
  encoded.insert(encoded.end(),motif.begin(),motif.end());
  for (auto position:positions) {
    plain[position]^=std::byte{0xa5}; put(encoded,position,4); encoded.push_back(plain[position]);
  }
  return {static_cast<std::uint8_t>(long_motif?9:7),0,std::move(plain),std::move(encoded)};
}
inline Block dictionary(Bytes plain) {
  Bytes table, stream; put(table,0,4);
#ifdef FLITZFS_TEST_HAS_LZ4
  for (std::size_t start=0; start<plain.size(); start+=65536) {
    const int count=static_cast<int>((std::min)(std::size_t{65536},plain.size()-start));
    Bytes encoded(static_cast<std::size_t>(LZ4_compressBound(count)));
    const int n=LZ4_compress_default(reinterpret_cast<const char*>(plain.data()+start),
        reinterpret_cast<char*>(encoded.data()),count,static_cast<int>(encoded.size()));
    if (n<=0) throw std::runtime_error("LZ4 fixture encode failed");
    stream.insert(stream.end(),encoded.begin(),encoded.begin()+n); put(table,stream.size(),4);
  }
#else
  throw std::runtime_error("LZ4 fixture encoder unavailable");
#endif
  table.insert(table.end(),stream.begin(),stream.end());
  return {6,0,std::move(plain),std::move(table)};
}
inline Block compound(Block original, bool use_fill=false) {
  Block secondary;
  if (use_fill) {
    if (original.encoded.empty() || !std::all_of(original.encoded.begin(),original.encoded.end(),
        [&](std::byte b){return b==original.encoded[0];})) throw std::runtime_error("nonuniform compound fixture");
    secondary=fill(original.encoded.size(),std::to_integer<std::uint8_t>(original.encoded[0]));
  } else secondary=prefix(original.encoded);
  Bytes out; put(out,secondary.kind,1); put(out,original.kind,1); put(out,secondary.fill,1);
  put(out,0,1); put(out,original.encoded.size(),4);
  out.insert(out.end(),secondary.encoded.begin(),secondary.encoded.end());
  if (out.size()>=original.encoded.size()) throw std::runtime_error("compound fixture not smaller");
  return {11,0,std::move(original.plain),std::move(out)};
}
inline Bytes unshuffle(const Bytes& planes,unsigned width) {
  Bytes out(planes.size());const auto rows=out.size()/width;
  for (std::size_t row=0; row<rows; ++row)
    for (unsigned plane=0; plane<width; ++plane) out[row*width+plane]=planes[plane*rows+row];
  std::copy(planes.begin()+static_cast<std::ptrdiff_t>(rows*width),planes.end(),
      out.begin()+static_cast<std::ptrdiff_t>(rows*width));
  return out;
}
inline Block byte_plane(unsigned width, Block inner) {
  Bytes out; put(out,width,1); put(out,inner.kind,1); put(out,inner.fill,1); put(out,0,1);
  out.insert(out.end(),inner.encoded.begin(),inner.encoded.end());
  return {12,0,unshuffle(inner.plain,width),std::move(out)};
}
inline Block contexts(const std::vector<Block>& children) {
  const auto width=static_cast<unsigned>(children.size());
  Bytes out, encoded, decoded;
  put(out,width,1); put(out,0,3);
  for (const auto& child:children) {
    put(out,child.kind,1);put(out,child.fill,1);put(out,child.encoded.size(),4);
    encoded.insert(encoded.end(),child.encoded.begin(),child.encoded.end());
    decoded.insert(decoded.end(),child.plain.begin(),child.plain.end());
  }
  out.insert(out.end(),encoded.begin(),encoded.end());
  return {13,0,unshuffle(decoded,width),std::move(out)};
}
// All new native families, with intentionally small, deterministic data.
inline std::vector<Block> native_samples() {
  std::vector<Block> blocks{prefix_sample(),prefix_sample(true),sparse(),sparse(true),huffman(),
      compound(prefix(Bytes(12001,std::byte{0}))),compound(pattern(5000,std::string(64,'x')),true)};
  for (unsigned width:{2U,4U,8U}) {
    blocks.push_back(byte_plane(width,prefix_sample(false,8192+width-1)));
    blocks.push_back(byte_plane(width,fill(8192+width-1,0xab)));
    std::vector<Block> planes;
    for (unsigned p=0;p<width;++p) {
      const std::size_t n=4096+(p+1==width?width-1:0);
      if (p==0) planes.push_back(fill(n,0x91));
      else if (p==1) planes.push_back(raw(pattern(n,"random-like plane").plain));
      else if (p%3==0) planes.push_back(prefix_sample(true,n));
      else if (p%3==1) planes.push_back(prefix_sample(false,n));
      else planes.push_back(pattern(n,"plane"));
    }
    blocks.push_back(contexts(planes));
  }
#ifdef FLITZFS_TEST_HAS_LZ4
  blocks.push_back(dictionary(pattern(131089,"dictionary/segment/sample/").plain));
  blocks.push_back(byte_plane(4,dictionary(pattern(8195,"dictionary plane").plain)));
#endif
  return blocks;
}
} // namespace extent_fixture
