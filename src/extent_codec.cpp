#include "extent_codec.hpp"
#include "repeat_bytes.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#ifdef FLITZFS_HAS_LZ4
#include <lz4.h>
static_assert(LZ4_VERSION_NUMBER >= 10904, "LZ4 1.9.4 or newer is required");
#endif

// Compression mechanism provenance: FlitzZip 4e6a1bb, independently implemented.
// Internal extent experiments only; no archive framing, paths or compatibility.
// Limits and source/reference evidence: docs/EXTENT_CODECS.md.
namespace flitzfs::detail {
namespace {
using Input = std::span<const std::byte>;
using Output = std::span<std::byte>;
[[noreturn]] void invalid(const char* message) { throw ExtentCodecError(ExtentCodecErrorCode::corrupt, message); }
unsigned byte(Input s, std::size_t i) { return std::to_integer<unsigned>(s[i]); }
std::uint32_t u32(Input s, std::size_t at) {
  if (at>s.size() || s.size()-at<4) invalid("native field is truncated");
  std::uint32_t n=0;
  for (unsigned i=0; i<4; ++i) n |= std::uint32_t(byte(s,at+i))<<(8*i);
  return n;
}
bool plain_kind(unsigned kind) noexcept {
  return (kind>=3 && kind<=7) || kind==9 || kind==10;
}
bool plane_width(unsigned width) noexcept { return width==2 || width==4 || width==8; }
struct Bits {
  Input bytes;
  std::size_t position=0;
  unsigned peek(unsigned width) const {
    unsigned result=0;
    for (unsigned i=0; i<width && position+i<bytes.size()*8; ++i)
      result |= ((byte(bytes,(position+i)/8)>>((position+i)%8))&1U)<<i;
    return result;
  }
  unsigned take(unsigned width) {
    if (width>bytes.size()*8-position) invalid("native prefix code is truncated");
    const auto result=peek(width);position+=width;return result;
  }
};
// Validate every segment boundary before decoding. No allocation from table data.
template<class Decode> void segmented(Input payload, Output output,
    std::size_t header_bytes, std::size_t segment_size, Decode decode) {
  const auto count=(output.size()+segment_size-1)/segment_size;
  const auto table_bytes=(count+1)*4;
  if (header_bytes>payload.size() || table_bytes>=payload.size()-header_bytes)
    invalid("native segment header is truncated");
  const auto table=payload.subspan(header_bytes,table_bytes);
  const auto stream=payload.subspan(header_bytes+table_bytes);
  if (u32(table,0)!=0) invalid("native offsets must start at zero");
  std::size_t previous=0;
  for (std::size_t i=1;i<=count;++i) {
    const auto next=u32(table,i*4);
    if (next<previous || next>stream.size()) invalid("native offsets are not bounded and monotonic");
    previous=next;
  }
  if (previous!=stream.size()) invalid("native offsets leave trailing payload");
  previous=0;
  for (std::size_t i=0;i<count;++i) {
    const auto next=u32(table,(i+1)*4);
    const auto start=i*segment_size;
    decode(stream.subspan(previous,next-previous),output.subspan(start,(std::min)(segment_size,output.size()-start)));
    previous=next;
  }
}
void entropy(unsigned kind, Input input, Output output) {
  if (kind==10) {
    if (input.size()<8192) invalid("native Huffman lookup is truncated");
    struct Leaf { unsigned width=0, prefix=0, count=0; };
    std::array<Leaf,256> leaves{};
    for (unsigned slot=0;slot<4096;++slot) {
      const auto symbol=byte(input,slot*2), width=byte(input,slot*2+1);
      if (!width || width>12) invalid("native Huffman width is invalid");
      const auto prefix=slot&((1U<<width)-1);
      auto& leaf=leaves[symbol];
      if (!leaf.count) { leaf.width=width;leaf.prefix=prefix; }
      if (leaf.width!=width || leaf.prefix!=prefix) invalid("native Huffman prefixes conflict");
      ++leaf.count;
    }
    for (const auto& leaf:leaves)
      if (leaf.count && leaf.count!=(4096U>>leaf.width)) invalid("native Huffman lookup is incomplete");
  }
  const std::size_t header=kind==5?84:(kind==10?8192:0);
  segmented(input,output,header,4096,[&](Input source,Output destination) {
    Bits bits{source};
    for (auto& value:destination) {
      if (kind==10) {
        const auto slot=bits.peek(12), width=byte(input,slot*2+1);
        (void)bits.take(width);value=input[slot*2];continue;
      }
      unsigned rank=0;
      if (!bits.take(1)) rank=bits.take(2);
      else if (!bits.take(1)) rank=4+bits.take(4);
      else if (!bits.take(1)) rank=20+bits.take(6);
      else {
        const auto literal=bits.take(8);
        if (kind==4 && literal>171) invalid("static prefix high group exceeds byte range");
        value=static_cast<std::byte>(kind==4?84+literal:literal);continue;
      }
      value=kind==5?input[rank]:static_cast<std::byte>(rank);
    }
    // Experimental payloads permit unused segment padding; enclosing records must validate decoded bytes.
  });
}
void repeated(Input motif,Output output) {
  repeat_bytes(motif,output);
}
void sparse(unsigned kind,Input source,Output output) {
  const auto period=u32(source,0), count=u32(source,4);
  const unsigned minimum=kind==9?16385:2, maximum=kind==9?1048576:16384;
  if (period<minimum || period>maximum || period>=output.size() || !count || period>source.size()-8)
    invalid("native sparse header is invalid");
  const auto remainder=source.size()-8-period;
  if (remainder%5 || count!=remainder/5 || count>output.size()-period)
    invalid("native sparse correction count is invalid");
  const auto motif=source.subspan(8,period), patches=source.subspan(8+period);
  std::uint32_t previous=period-1;
  for (std::size_t i=0;i<count;++i) {
    const auto position=u32(patches,i*5);
    if (position<=previous || position>=output.size() || patches[i*5+4]==motif[position%period])
      invalid("native sparse correction is noncanonical");
    previous=position;
  }
  repeated(motif,output);
  for (std::size_t i=0;i<count;++i) output[u32(patches,i*5)]=patches[i*5+4];
}
// Plain stages cannot call the composition dispatcher, so recursion is impossible.
void plain(unsigned kind,unsigned fill,Input input,Output output) {
  if (output.empty() || output.size()>kMaxExtentCodecBytes) invalid("native output exceeds block bounds");
  if (kind==1) {
    if (!input.empty()) invalid("native fill stage has payload");
    std::fill(output.begin(),output.end(),static_cast<std::byte>(fill));return;
  }
  if (!plain_kind(kind)) invalid("unsupported or recursive native stage");
  if (input.empty() || input.size()>=output.size()) invalid("native payload must be smaller than output");
  switch (kind) {
    case 3:
      if (input.size()<2 || input.size()>16384) invalid("native motif length is invalid");
      repeated(input,output);return;
    case 4: case 5: case 10: entropy(kind,input,output);return;
    case 7: case 9: sparse(kind,input,output);return;
    case 6:
#ifdef FLITZFS_HAS_LZ4
      segmented(input,output,0,65536,[](Input source,Output destination) {
        if (source.empty() || source.size()>destination.size()+destination.size()/255+16)
          invalid("native dictionary segment exceeds capacity");
        const int n=LZ4_decompress_safe(reinterpret_cast<const char*>(source.data()),
            reinterpret_cast<char*>(destination.data()),static_cast<int>(source.size()),static_cast<int>(destination.size()));
        if (n!=static_cast<int>(destination.size())) invalid("native dictionary decoding failed");
      });return;
#else
      throw ExtentCodecError(ExtentCodecErrorCode::unsupported_codec,"native dictionary requires optional LZ4");
#endif
    default: invalid("unknown native codec");
  }
}
void inverse_planes(Input transformed,Output output,unsigned width) {
  const auto rows=output.size()/width;
  for (std::size_t row=0;row<rows;++row)
    for (unsigned p=0;p<width;++p) output[row*width+p]=transformed[p*rows+row];
  std::copy(transformed.begin()+static_cast<std::ptrdiff_t>(rows*width),transformed.end(),
      output.begin()+static_cast<std::ptrdiff_t>(rows*width));
}
} // namespace

bool extent_codec_available(std::uint8_t kind) noexcept {
  if (kind==0 || kind==1 || kind==3 || kind==4 || kind==5 || kind==7 || (kind>=9 && kind<=13)) return true;
#ifdef FLITZFS_HAS_LZ4
  if (kind==6) return true;
#endif
  return false;
}
void decode_extent(std::uint8_t kind,std::uint8_t fill,Input input,Output output) {
  if (output.empty() || output.size()>kMaxExtentCodecBytes || input.size()>kMaxExtentCodecBytes)
    invalid("extent codec block exceeds bounds");
  if (kind==0) {
    if(fill || input.size()!=output.size()) invalid("raw extent framing");
    std::copy(input.begin(),input.end(),output.begin());return;
  }
  if(kind==1) {plain(kind,fill,input,output);return;}
  if(fill || input.empty() || input.size()>=output.size()) invalid("compressed extent framing");
  if (plain_kind(kind)) { plain(kind,fill,input,output);return; }
  if (fill || input.size()<4) invalid("native composition header is invalid");
  if (kind==11) {
    if (input.size()<8) invalid("compound header is truncated");
    const auto secondary=byte(input,0), original=byte(input,1), secondary_fill=byte(input,2);
    const auto intermediate=u32(input,4);
    if ((secondary!=1 && !plain_kind(secondary)) || !plain_kind(original) || byte(input,3) ||
        (secondary!=1 && secondary_fill) || input.size()>=intermediate || intermediate>=output.size() ||
        (secondary==1 && input.size()!=8)) invalid("compound stage framing is invalid");
    std::vector<std::byte> temporary(intermediate);
    plain(secondary,secondary_fill,input.subspan(8),temporary);
    plain(original,0,temporary,output);return;
  }
  if (kind!=12 && kind!=13) invalid("unknown native composition");
  const auto width=byte(input,0);
  if (!plane_width(width) || output.size()<width || byte(input,3)) invalid("byte-plane header is invalid");
  if (kind==12) {
    const auto inner=byte(input,1), inner_fill=byte(input,2);
    if ((inner!=1 && !plain_kind(inner)) || (inner!=1 && inner_fill) ||
        (inner==1 && input.size()!=4)) invalid("byte-plane inner stage is invalid");
    std::vector<std::byte> temporary(output.size());
    plain(inner,inner_fill,input.subspan(4),temporary);
    inverse_planes(temporary,output,width);return;
  }
  const std::size_t header=4+width*6;
  if (byte(input,1) || byte(input,2) || input.size()<header) invalid("byte-plane context table is invalid");
  struct Stage { unsigned kind=0,fill=0;std::size_t encoded=0,decoded=0,offset=0; };
  std::array<Stage,8> stages{};
  std::size_t cursor=header;
  const auto rows=output.size()/width;
  for (unsigned p=0;p<width;++p) {
    auto& s=stages[p];s.kind=byte(input,4+p*6);s.fill=byte(input,5+p*6);
    s.encoded=u32(input,6+p*6);s.decoded=rows+(p+1==width?output.size()%width:0);s.offset=cursor;
    const bool extent=s.kind==1?s.encoded==0:(s.kind==0?s.encoded==s.decoded:
        plain_kind(s.kind) && s.encoded>0 && s.encoded<s.decoded);
    if (!extent || (s.kind!=1 && s.fill) || s.encoded>input.size()-cursor)
      invalid("byte-plane context kind or extent is invalid");
    cursor+=s.encoded;
  }
  if (cursor!=input.size()) invalid("byte-plane contexts have trailing payload");
  std::vector<std::byte> temporary(output.size());cursor=0;
  for (unsigned p=0;p<width;++p) {
    const auto& s=stages[p];auto out=Output(temporary).subspan(cursor,s.decoded);
    const auto source=input.subspan(s.offset,s.encoded);
    if (s.kind==0) std::memcpy(out.data(),source.data(),out.size());
    else plain(s.kind,s.fill,source,out);
    cursor+=s.decoded;
  }
  inverse_planes(temporary,output,width);
}
} // namespace flitzfs::detail
