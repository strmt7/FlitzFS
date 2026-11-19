#include "native_group_codec.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <stdexcept>
namespace flitzfs::detail {
namespace {
constexpr std::size_t extent=65536,header=128;
constexpr std::array<std::byte,8> aligned_magic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},
    std::byte{'G'},std::byte{'M'},std::byte{'0'},std::byte{'1'}};
constexpr auto copy_magic=[] {auto value=aligned_magic;value.back()=std::byte{'2'};return value;}();
constexpr auto xor_magic=[] {auto value=aligned_magic;value.back()=std::byte{'3'};return value;}();
constexpr auto delta_magic=[] {auto value=aligned_magic;value.back()=std::byte{'4'};return value;}();
constexpr auto sparse_xor_magic=[] {auto value=aligned_magic;value.back()=std::byte{'5'};return value;}();
constexpr std::array member_magics{aligned_magic,copy_magic,xor_magic,delta_magic,sparse_xor_magic};
constexpr std::size_t xor_tile_bytes=256,xor_plane_bytes=xor_tile_bytes/8,xor_tiles=extent/xor_tile_bytes;
using anchor_match::sparse_plane_max;
using anchor_match::sparse_plane_inverse;
static_assert(extent%xor_tile_bytes==0 && xor_tile_bytes%8==0 &&
    extent==anchor_match::extent_bytes && xor_tile_bytes==anchor_match::xor_tile_bytes);
static_assert(xor_tile_bytes==256 && sparse_plane_max<sparse_plane_inverse);
constexpr std::size_t window_bytes=16,anchor_stride=16;
constexpr std::uint32_t copy_flag=1U<<31;
constexpr std::uint64_t multiplier=0x100000001B3ULL;
constexpr auto outgoing_weight=[] {
  std::uint64_t result=1;
  for(std::size_t i=1;i<window_bytes;++i)result*=multiplier;
  return result;
}();
static_assert(header+8==kNativeGroupCopyMinimumBytes);

[[noreturn]]void fail(){throw std::runtime_error("invalid shared-base member");}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,unsigned count) {
  if(at>bytes.size() || count>bytes.size()-at)fail();std::uint64_t value=0;
  for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(i*8);return value;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned count) {
  for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::byte>(value>>(i*8));
}
bool base_id_valid(std::uint64_t id){return id>kNativeGroupBaseBit && id<2*kNativeGroupBaseBit;}
NativeGroupMethod member_method(std::span<const std::byte> bytes,NativeGroupMethod maximum_method) {
  if(bytes.size()<header || bytes.size()>=extent)fail();
  const auto version=get(bytes,8,4);
  if(!version || version>static_cast<std::uint32_t>(maximum_method) || version>member_magics.size() ||
      !std::ranges::equal(bytes.first(8),member_magics[static_cast<std::size_t>(version-1)]))
    fail();
  return static_cast<NativeGroupMethod>(version);
}
void write_header(std::span<std::byte> bytes,NativeGroupMethod method,std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,std::uint32_t count) {
  const auto& magic=member_magics[static_cast<std::size_t>(method)-1];
  std::copy(magic.begin(),magic.end(),bytes.begin());
  put(bytes,8,static_cast<std::uint32_t>(method),4);
  put(bytes,12,extent,4);put(bytes,16,base_id,8);put(bytes,24,generation,8);
  const auto base_hash=record_digest(base),member_hash=record_digest(member);
  std::copy(base_hash.begin(),base_hash.end(),bytes.begin()+32);
  std::copy(member_hash.begin(),member_hash.end(),bytes.begin()+64);
  put(bytes,96,count,4);put(bytes,100,bytes.size()-header,4);
}
class AnchorCopyFrame {
 public:
  AnchorCopyFrame(std::span<const std::byte> member,std::size_t limit):member_(member),limit_(limit),bytes_(header){}
  bool append_copy(std::size_t offset,std::size_t source,std::size_t length) {
    if(!literal_until(offset) || 8>=limit_-bytes_.size())return false;
    const auto next=bytes_.size();bytes_.resize(next+8);
    put(bytes_,next,copy_flag|length,4);put(bytes_,next+4,source,4);
    ++count_;end_=offset+length;
    return true;
  }
  std::optional<std::vector<std::byte>> finish(std::span<const std::byte> base,std::uint64_t id,std::uint64_t generation) {
    if(!literal_until(extent))return {};
    write_header(bytes_,NativeGroupMethod::anchor_copies,member_,base,id,generation,count_);
    return std::move(bytes_);
  }
 private:
  bool literal_until(std::size_t end) {
    const auto length=end-end_;
    if(!length)return true;
    if(length+4>=limit_-bytes_.size())return false;
    const auto next=bytes_.size();bytes_.resize(next+4+length);
    put(bytes_,next,length,4);
    std::copy_n(member_.begin()+end_,length,bytes_.begin()+next+4);
    ++count_;end_=end;
    return true;
  }
  std::span<const std::byte> member_;
  std::size_t limit_,end_=0;
  std::vector<std::byte> bytes_;
  std::uint32_t count_=0;
};
template<class Apply>void patches(std::span<const std::byte> bytes,Apply apply) {
  std::size_t at=header,end=0;const auto count=get(bytes,96,4);
  if(count>(bytes.size()-header)/9)fail();
  for(std::uint64_t i=0;i<count;++i) {
    const auto offset=get(bytes,at,4),length=get(bytes,at+4,4);at+=8;
    if(!length || offset<end || offset>extent || length>extent-offset || length>bytes.size()-at)fail();
    apply(static_cast<std::size_t>(offset),bytes.subspan(at,static_cast<std::size_t>(length)));
    end=static_cast<std::size_t>(offset+length);at+=static_cast<std::size_t>(length);
  }
  if(at!=bytes.size())fail();
}

template<class Copy,class Literal>
void spans(std::span<const std::byte> bytes,Copy copy,Literal literal) {
  std::size_t input=header,output=0;
  const auto count=get(bytes,96,4);
  if(!count || count>(bytes.size()-header)/5)fail();
  for(std::uint64_t i=0;i<count;++i) {
    const auto control=static_cast<std::uint32_t>(get(bytes,input,4));
    input+=4;
    const std::size_t length=control&~copy_flag;
    if(!length || length>extent-output)fail();
    if(control&copy_flag) {
      const auto source=static_cast<std::size_t>(get(bytes,input,4));
      input+=4;
      if(source>=extent || length>extent-source)fail();
      copy(output,source,length);
    } else {
      if(length>bytes.size()-input)fail();
      literal(output,bytes.subspan(input,length));
      input+=length;
    }
    output+=length;
  }
  if(output!=extent || input!=bytes.size())fail();
}

template<class Apply>void packed_xor_tiles(std::span<const std::byte> body,Apply apply) {
  std::size_t input=0;
  bool changed=false;
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    if(input==body.size())fail();
    const auto mask=std::to_integer<unsigned>(body[input++]);
    const auto payload=static_cast<std::size_t>(std::popcount(mask))*xor_plane_bytes;
    if(payload>body.size()-input)fail();
    const auto planes=body.subspan(input,payload);
    for(std::size_t at=0;at<planes.size();at+=xor_plane_bytes) {
      const auto plane=planes.subspan(at,xor_plane_bytes);
      if(std::ranges::all_of(plane,[](auto packed){return packed==std::byte{};}))fail();
    }
    apply(tile,mask,planes);
    input+=payload;
    changed=changed || mask!=0;
  }
  if(input!=body.size() || !changed)fail();
}

void apply_dense_xor_plane(std::span<std::byte> tile,unsigned bit,std::span<const std::byte> plane) {
  std::size_t at=0;
  for(const auto packed:plane) {
    const auto value=std::to_integer<unsigned>(packed);
    for(unsigned i=0;i<8;++i)
      tile[at+i]^=static_cast<std::byte>(((value>>i)&1U)<<bit);
    at+=8;
  }
}
void apply_packed_xor(std::span<const std::byte> body,std::span<std::byte> result) {
  packed_xor_tiles(body,[&](std::size_t tile,unsigned mask,std::span<const std::byte> planes) {
    const auto output=result.subspan(tile*xor_tile_bytes,xor_tile_bytes);
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      apply_dense_xor_plane(output,bit,planes.first(xor_plane_bytes));
      planes=planes.subspan(xor_plane_bytes);
    }
  });
}

unsigned plane_ones(std::span<const std::byte> plane) noexcept {
  unsigned count=0;
  for(const auto byte:plane)count+=std::popcount(std::to_integer<unsigned>(byte));
  return count;
}

template<class Dense,class Sparse>
std::size_t sparse_xor_planes(std::span<const std::byte> body,Dense dense,Sparse sparse) {
  std::size_t planes=0;
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    if(body.empty())fail();
    const auto mask=std::to_integer<unsigned>(body.front());
    body=body.subspan(1);
    if(!mask)continue;
    if(body.empty())fail();
    const auto compact=std::to_integer<unsigned>(body.front());
    body=body.subspan(1);
    if(compact&~mask)fail();
    planes+=static_cast<std::size_t>(std::popcount(mask));
    const auto output=tile*xor_tile_bytes;
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      if(!(compact&(1U<<bit))) {
        if(body.size()<xor_plane_bytes)fail();
        const auto plane=body.first(xor_plane_bytes);
        const auto count=plane_ones(plane);
        if(count<=sparse_plane_max || xor_tile_bytes-count<=sparse_plane_max)fail();
        dense(output,bit,plane);
        body=body.subspan(xor_plane_bytes);
      }else {
        if(body.empty())fail();
        const auto descriptor=std::to_integer<unsigned>(body.front());
        const auto count=descriptor&~sparse_plane_inverse;
        const bool invert=(descriptor&sparse_plane_inverse)!=0;
        body=body.subspan(1);
        if(count>sparse_plane_max || (!count && !invert) || count>body.size())fail();
        const auto positions=body.first(count);
        if(std::adjacent_find(positions.begin(),positions.end(),std::greater_equal<>{})!=positions.end())fail();
        sparse(output,bit,invert,positions);
        body=body.subspan(count);
      }
    }
  }
  if(!planes || !body.empty())fail();
  return planes;
}

std::size_t apply_sparse_xor(std::span<const std::byte> body,std::span<std::byte> result) {
  return sparse_xor_planes(body,[&](std::size_t at,unsigned bit,std::span<const std::byte> plane) {
    apply_dense_xor_plane(result.subspan(at,xor_tile_bytes),bit,plane);
  },[&](std::size_t at,unsigned bit,bool invert,std::span<const std::byte> positions) {
    const auto mask=static_cast<std::byte>(1U<<bit);
    if(invert)for(auto& value:result.subspan(at,xor_tile_bytes))value^=mask;
    for(const auto pos:positions)result[at+std::to_integer<unsigned>(pos)]^=mask;
  });
}

using XorDifferences=std::array<unsigned char,xor_tile_bytes>;
using XorPlane=std::array<std::byte,xor_plane_bytes>;
unsigned xor_differences(std::span<const std::byte> member,std::span<const std::byte> base,
    std::size_t at,XorDifferences& differences) {
  unsigned mask=0;
  for(std::size_t i=0;i<differences.size();++i) {
    differences[i]=std::to_integer<unsigned char>(member[at+i]^base[at+i]);
    mask|=differences[i];
  }
  return mask;
}

void xor_plane(const XorDifferences& differences,unsigned bit,XorPlane& plane) {
  for(std::size_t byte=0;byte<plane.size();++byte) {
    unsigned packed=0;
    for(unsigned i=0;i<8;++i)packed|=((differences[byte*8+i]>>bit)&1U)<<i;
    plane[byte]=static_cast<std::byte>(packed);
  }
}

std::size_t sparse_xor_plane_bytes(std::size_t ones) noexcept {
  return (std::min)({ones+1,xor_tile_bytes-ones+1,xor_plane_bytes});
}

template<class Plane>
bool append_sparse_xor_tile(std::vector<std::byte>& frame,std::size_t limit,
    std::size_t tile,unsigned mask,Plane get_plane) {
  const auto bound=limit-(xor_tiles-1-tile);
  const auto prefix=mask?2U:1U;
  if(frame.size()>=bound || prefix>=bound-frame.size())return false;
  frame.push_back(static_cast<std::byte>(mask));
  if(!mask)return true;
  const auto sparse_at=frame.size();
  frame.push_back(std::byte{});
  for(unsigned bit=0;bit<8;++bit) {
    if(!(mask&(1U<<bit)))continue;
    const std::span<const std::byte> plane=get_plane(bit);
    const auto ones=plane_ones(plane);
    const bool invert=ones>xor_tile_bytes/2;
    const auto count=invert?xor_tile_bytes-ones:ones;
    const bool compact=count<=sparse_plane_max;
    const auto bytes=sparse_xor_plane_bytes(ones);
    if(bytes>=bound-frame.size())return false;
    if(!compact) {
      frame.insert(frame.end(),plane.begin(),plane.end());
      continue;
    }
    frame[sparse_at]|=static_cast<std::byte>(1U<<bit);
    frame.push_back(static_cast<std::byte>(count|(invert?sparse_plane_inverse:0U)));
    for(std::size_t byte=0;byte<plane.size();++byte) {
      auto positions=std::to_integer<unsigned>(plane[byte])^(invert?255U:0U);
      while(positions) {
        frame.push_back(static_cast<std::byte>(byte*8+std::countr_zero(positions)));
        positions&=positions-1;
      }
    }
  }
  return true;
}

template<class Word>Word load_little_endian(const std::byte* data) noexcept {
  Word value{};
  if constexpr(std::endian::native==std::endian::little) {
    std::memcpy(&value,data,sizeof(value));
  } else {
    std::uint64_t widened=0;
    for(std::size_t i=0;i<sizeof(value);++i)widened|=std::to_integer<std::uint64_t>(data[i])<<(8*i);
    value=static_cast<Word>(widened);
  }
  return value;
}
template<class Word>void store_little_endian(std::byte* data,Word value) noexcept {
  if constexpr(std::endian::native==std::endian::little) {
    std::memcpy(data,&value,sizeof(value));
  } else {
    for(std::size_t i=0;i<sizeof(value);++i)data[i]=static_cast<std::byte>(value>>(8*i));
  }
}
template<class Word>Word modular_difference(const std::byte* member,const std::byte* base) noexcept {
  return static_cast<Word>(std::uint64_t{load_little_endian<Word>(member)}-load_little_endian<Word>(base));
}
template<class Word>Word zigzag_residual(const std::byte* member,const std::byte* base,std::uint64_t bias) noexcept {
  const auto delta=static_cast<Word>(std::uint64_t{modular_difference<Word>(member,base)}-bias);
  const auto widened=static_cast<std::uint64_t>(delta);
  return static_cast<Word>((widened<<1)^(std::uint64_t{0}-(widened>>(8*sizeof(Word)-1))));
}
struct DeltaChoice {
  std::size_t bytes,word_bytes=0;
  std::uint64_t bias=0;
};
bool valid_delta_parameters(std::size_t width,std::uint64_t bias) noexcept {
  return (width==1 || width==2 || width==4 || width==8) &&
      (width==8 || (bias>>(8*width))==0);
}
template<class Word>void consider_delta(std::span<const std::byte> member,std::span<const std::byte> base,
    DeltaChoice& best) {
  if(best.bytes<=header)return;
  constexpr auto plane_bytes=xor_tile_bytes/(8*sizeof(Word));
  const auto first=modular_difference<Word>(member.data(),base.data());
  const auto consider=[&](std::uint64_t bias) {
    auto bytes=header+xor_tiles;
    for(std::size_t offset=0;offset<extent;offset+=xor_tile_bytes) {
      Word combined{};
      for(std::size_t at=offset;at<offset+xor_tile_bytes;at+=sizeof(Word))
        combined|=zigzag_residual<Word>(member.data()+at,base.data()+at,bias);
      if(!combined)continue;
      bytes+=std::bit_width(combined)*plane_bytes;
      if(bytes>=best.bytes)return;
    }
    if(bytes==header+xor_tiles) {
      if(!bias)return;
      bytes=header;
    }
    if(bytes<best.bytes)best={bytes,sizeof(Word),bias};
  };
  consider(first);
  if(first && best.bytes>header+xor_tiles)consider(0);
  // An extra bias differs from the first word's bias and therefore needs
  // a residual body. The existing choices already cover exact shifts.
  if(best.bytes<=header+xor_tiles)return;
  Word samples[anchor_match::delta_bias_samples],biases[anchor_match::delta_bias_choices];
  for(std::uint32_t i=0;i<anchor_match::delta_bias_samples;++i) {
    const auto offset=anchor_match::modular_bias_sample_offset(i);
    samples[i]=modular_difference<Word>(member.data()+offset,base.data()+offset);
  }
  const auto count=anchor_match::modular_bias_candidates(first,samples,biases);
  for(auto i=first?2U:1U;i<count;++i)consider(biases[i]);
}
template<class Word>void append_delta_tiles(std::vector<std::byte>& frame,std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t bias) {
  constexpr auto words=xor_tile_bytes/sizeof(Word);
  std::array<Word,words> residuals{};
  for(std::size_t tile=0;tile<extent;tile+=xor_tile_bytes) {
    Word combined{};
    for(std::size_t i=0;i<words;++i) {
      const auto offset=tile+i*sizeof(Word);
      residuals[i]=zigzag_residual<Word>(member.data()+offset,base.data()+offset,bias);
      combined|=residuals[i];
    }
    const auto bits=static_cast<unsigned>(std::bit_width(combined));
    frame.push_back(static_cast<std::byte>(bits));
    for(unsigned bit=0;bit<bits;++bit) {
      for(std::size_t i=0;i<words;i+=8) {
        unsigned packed=0;
        for(unsigned j=0;j<8;++j)packed|=static_cast<unsigned>((residuals[i+j]>>bit)&1U)<<j;
        frame.push_back(static_cast<std::byte>(packed));
      }
    }
  }
}

DeltaChoice delta_parameters(std::span<const std::byte> frame) {
  const auto width=static_cast<std::size_t>(get(frame,104,1));
  const auto bias=get(frame,112,8);
  if(!valid_delta_parameters(width,bias) ||
      !std::ranges::all_of(frame.subspan(105,7),[](auto value){return value==std::byte{};}) ||
      get(frame,120,8) || get(frame,96,4)!=(frame.size()==header?0:xor_tiles) ||
      (frame.size()==header && !bias))fail();
  return {frame.size(),width,bias};
}
template<class Apply>void visit_delta_tiles(std::span<const std::byte> body,std::size_t word_bytes,Apply apply) {
  const auto plane_bytes=xor_tile_bytes/(8*word_bytes);
  std::size_t input=0;
  bool changed=false;
  for(std::size_t tile=0;tile<extent;tile+=xor_tile_bytes) {
    if(input==body.size())fail();
    const auto bits=std::to_integer<unsigned>(body[input++]);
    if(bits>8*word_bytes || bits*plane_bytes>body.size()-input)fail();
    const auto planes=body.subspan(input,bits*plane_bytes);
    if(bits && std::ranges::all_of(planes.last(plane_bytes),[](auto value){return value==std::byte{};}))fail();
    apply(tile,bits,planes);
    input+=planes.size();
    changed=changed || bits!=0;
  }
  if(input!=body.size() || !changed)fail();
}
template<class Word>void unpack_delta(std::span<const std::byte> body,std::uint64_t bias,std::span<std::byte> result) {
  constexpr auto words=xor_tile_bytes/sizeof(Word),plane_bytes=words/8;
  const auto add_residual=[&](std::size_t offset,std::uint64_t encoded) {
    const auto delta=(encoded>>1)^(std::uint64_t{0}-(encoded&1U));
    auto* target=result.data()+offset;
    const auto word=static_cast<Word>(std::uint64_t{load_little_endian<Word>(target)}+bias+delta);
    store_little_endian(target,word);
  };
  if(body.empty()) {
    for(std::size_t at=0;at<extent;at+=sizeof(Word))add_residual(at,0);
    return;
  }
  visit_delta_tiles(body,sizeof(Word),[&](std::size_t tile,unsigned bits,std::span<const std::byte> planes) {
    std::array<Word,words> residuals{};
    for(unsigned bit=0;bit<bits;++bit) {
      for(std::size_t i=0;i<plane_bytes;++i) {
        const auto packed=std::to_integer<unsigned>(planes[bit*plane_bytes+i]);
        for(unsigned j=0;j<8;++j)
          residuals[i*8+j]|=static_cast<Word>(std::uint64_t{(packed>>j)&1U}<<bit);
      }
    }
    for(std::size_t i=0;i<words;++i)add_residual(tile+i*sizeof(Word),residuals[i]);
  });
}
void apply_modular_delta(std::span<const std::byte> body,const DeltaChoice& choice,std::span<std::byte> result) {
  switch(choice.word_bytes) {
    case 1: unpack_delta<std::uint8_t>(body,choice.bias,result);break;
    case 2: unpack_delta<std::uint16_t>(body,choice.bias,result);break;
    case 4: unpack_delta<std::uint32_t>(body,choice.bias,result);break;
    case 8: unpack_delta<std::uint64_t>(body,choice.bias,result);break;
  }
}
template<class Word>std::uint64_t delta_signature(std::span<const std::byte> bytes) {
  constexpr std::size_t samples=16,window=64;
  std::array<std::byte,samples*window> normalized{};
  const auto first=load_little_endian<Word>(bytes.data());
  bool varied=false;
  for(std::size_t i=0;i<samples;++i) {
    for(std::size_t at=0;at<window;at+=sizeof(Word)) {
      const auto value=load_little_endian<Word>(bytes.data()+i*(extent/samples)+at);
      const auto delta=static_cast<Word>(std::uint64_t{value}-first);
      varied=varied || delta!=0;
      store_little_endian(normalized.data()+i*window+at,delta);
    }
  }
  if(!varied)return 0;
  const auto digest=record_digest(normalized);
  return get(digest,0,8);
}

std::uint64_t fingerprint(std::span<const std::byte> window) noexcept {
  std::uint64_t value=0;
  for(const auto byte:window)value=value*multiplier+std::to_integer<unsigned>(byte);
  return value;
}
std::uint64_t roll(std::uint64_t value,std::byte outgoing,std::byte incoming) noexcept {
  return (value-std::to_integer<unsigned>(outgoing)*outgoing_weight)*multiplier+
      std::to_integer<unsigned>(incoming);
}
// This mixer only controls candidate placement. Every copy is byte-compared.
std::uint64_t mix(std::uint64_t value) noexcept {
  value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
  value=(value^(value>>27))*0x94d049bb133111ebULL;
  return value^(value>>31);
}
}

NativeGroupReference parse_native_group_member(std::span<const std::byte> bytes,NativeGroupMethod maximum_method) {
  const auto method=member_method(bytes,maximum_method);
  if(get(bytes,12,4)!=extent || get(bytes,100,4)!=bytes.size()-header ||
      (method!=NativeGroupMethod::modular_delta &&
       !std::all_of(bytes.begin()+104,bytes.begin()+128,[](auto value){return value==std::byte{};})))fail();
  NativeGroupReference ref;ref.base_id=get(bytes,16,8);ref.base_generation=get(bytes,24,8);
  if(!base_id_valid(ref.base_id) || !ref.base_generation)fail();
  std::copy_n(bytes.begin()+32,32,ref.base_digest.begin());std::copy_n(bytes.begin()+64,32,ref.decoded_digest.begin());
  ref.member_bytes=static_cast<std::uint32_t>(bytes.size());ref.method=method;
  switch(method) {
    case NativeGroupMethod::aligned_patches:patches(bytes,[](auto,auto){});break;
    case NativeGroupMethod::anchor_copies:spans(bytes,[](auto,auto,auto){},[](auto,auto){});break;
    case NativeGroupMethod::packed_xor:
      if(get(bytes,96,4)!=xor_tiles)fail();
      packed_xor_tiles(bytes.subspan(header),[](auto,auto,auto){});
      break;
    case NativeGroupMethod::modular_delta: {
      const auto choice=delta_parameters(bytes);
      if(bytes.size()>header)visit_delta_tiles(bytes.subspan(header),choice.word_bytes,[](auto,auto,auto){});
      break;
    }
    case NativeGroupMethod::sparse_xor:
      if(get(bytes,96,4)!=xor_tiles)fail();
      sparse_xor_planes(bytes.subspan(header),[](auto,auto,auto){},[](auto,auto,auto,auto){});
      break;
  }
  return ref;
}
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> bytes,
    std::span<const std::byte> base,NativeGroupMethod maximum_method) {
  const auto ref=parse_native_group_member(bytes,maximum_method);
  if(base.size()!=extent || record_digest(base)!=ref.base_digest)fail();
  std::vector<std::byte> result;
  const auto literal=[&](auto offset,auto input){std::copy(input.begin(),input.end(),result.begin()+offset);};
  if(ref.method==NativeGroupMethod::anchor_copies) {
    result.resize(extent);
    spans(bytes,[&](auto output,auto source,auto length){
      std::copy_n(base.begin()+source,length,result.begin()+output);
    },literal);
  } else {
    result.assign(base.begin(),base.end());
    if(ref.method==NativeGroupMethod::aligned_patches)patches(bytes,literal);
    else if(ref.method==NativeGroupMethod::packed_xor)apply_packed_xor(bytes.subspan(header),result);
    else if(ref.method==NativeGroupMethod::sparse_xor)apply_sparse_xor(bytes.subspan(header),result);
    else apply_modular_delta(bytes.subspan(header),delta_parameters(bytes),result);
  }
  if(record_digest(result)!=ref.decoded_digest)fail();return result;
}
void bind_native_group_member(std::span<std::byte> member,std::uint64_t base_id,
    std::uint64_t generation,NativeGroupMethod maximum_method) {
  if(!base_id_valid(base_id) || !generation)fail();
  (void)member_method(member,maximum_method);
  put(member,16,base_id,8);
  put(member,24,generation,8);
}
std::optional<std::vector<std::byte>> encode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation)fail();
  std::vector<std::byte> result(header);
  std::uint32_t count=0;
  for(std::size_t at=0;at<extent;) {
    while(at<extent && member[at]==base[at])++at;
    if(at==extent)break;
    const auto first=at;auto last=at;++at;
    while(at<extent) {
      if(member[at]!=base[at])last=at;
      else if(at-last>8)break;
      ++at;
    }
    const auto length=last-first+1;
    if(8+length>=extent-result.size())return std::nullopt;
    const auto end=result.size();result.resize(end+8+length);put(result,end,first,4);put(result,end+4,length,4);
    std::copy_n(member.begin()+first,length,result.begin()+end+8);++count;at=last+1;
  }
  write_header(result,NativeGroupMethod::aligned_patches,member,base,base_id,generation,count);
  return result;
}

std::optional<std::vector<std::byte>> encode_native_group_xor(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,std::size_t exclusive_limit) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(exclusive_limit<=header+xor_tiles+xor_plane_bytes)return {};
  std::vector<std::byte> frame(header);
  XorDifferences differences{};
  XorPlane plane{};
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    const auto mask=xor_differences(member,base,tile*xor_tile_bytes,differences);
    const auto payload=static_cast<std::size_t>(std::popcount(mask))*xor_plane_bytes;
    // Charge one mask byte for every remaining tile before retaining any planes.
    if(payload+xor_tiles-tile>=exclusive_limit-frame.size())return {};
    frame.push_back(static_cast<std::byte>(mask));
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      xor_plane(differences,bit,plane);
      frame.insert(frame.end(),plane.begin(),plane.end());
    }
  }
  if(frame.size()==header+xor_tiles)return {};
  write_header(frame,NativeGroupMethod::packed_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  return frame;
}

std::optional<std::vector<std::byte>> encode_native_group_sparse_xor(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,std::size_t exclusive_limit) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(exclusive_limit<=header+xor_tiles+2)return {};
  std::vector<std::byte> frame(header);
  XorDifferences differences{};
  XorPlane plane{};
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    const auto mask=xor_differences(member,base,tile*xor_tile_bytes,differences);
    if(!append_sparse_xor_tile(frame,exclusive_limit,tile,mask,[&](unsigned bit) {
      xor_plane(differences,bit,plane);
      return std::span<const std::byte>(plane);
    }))return {};
  }
  if(frame.size()==header+xor_tiles)return {};
  write_header(frame,NativeGroupMethod::sparse_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  return frame;
}

std::size_t price_native_group_xor(std::span<const std::byte> member,std::span<const std::byte> base) {
  if(member.size()!=extent || base.size()!=extent)fail();
  constexpr auto masks=header+xor_tiles;
  std::size_t dense=masks,sparse=masks;
  std::array<std::uint64_t,xor_tile_bytes/sizeof(std::uint64_t)> differences;
  for(std::size_t tile=0;tile<extent;tile+=xor_tile_bytes) {
    std::uint64_t changed=0;
    for(std::size_t word=0;word<differences.size();++word) {
      const auto offset=tile+word*sizeof(std::uint64_t);
      std::uint64_t a=0,b=0;
      std::memcpy(&a,member.data()+offset,sizeof(a));
      std::memcpy(&b,base.data()+offset,sizeof(b));
      differences[word]=a^b;
      changed|=differences[word];
    }
    if(!changed)continue;
    changed|=changed>>32;
    changed|=changed>>16;
    changed|=changed>>8;
    auto planes=static_cast<unsigned>(changed&255U);
    dense+=static_cast<std::size_t>(std::popcount(planes))*xor_plane_bytes;
    ++sparse; // A changed tile carries a second mask for the sparse planes.
    while(planes) {
      const auto plane=0x0101010101010101ULL<<std::countr_zero(planes);
      std::size_t ones=0;
      for(const auto word:differences)ones+=static_cast<std::size_t>(std::popcount(word&plane));
      sparse+=sparse_xor_plane_bytes(ones);
      planes&=planes-1;
    }
    if(dense>=extent && sparse>=extent)return extent;
  }
  return dense==masks?header:(std::min)({dense,sparse,extent});
}

std::optional<std::vector<std::byte>> encode_native_group_delta(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,std::size_t exclusive_limit) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation || exclusive_limit>extent)fail();
  DeltaChoice best{exclusive_limit};
  consider_delta<std::uint8_t>(member,base,best);
  consider_delta<std::uint16_t>(member,base,best);
  consider_delta<std::uint32_t>(member,base,best);
  consider_delta<std::uint64_t>(member,base,best);
  if(!best.word_bytes)return {};
  std::vector<std::byte> frame(header);
  frame.reserve(best.bytes);
  if(best.bytes>header) {
    switch(best.word_bytes) {
      case 1: append_delta_tiles<std::uint8_t>(frame,member,base,best.bias);break;
      case 2: append_delta_tiles<std::uint16_t>(frame,member,base,best.bias);break;
      case 4: append_delta_tiles<std::uint32_t>(frame,member,base,best.bias);break;
      case 8: append_delta_tiles<std::uint64_t>(frame,member,base,best.bias);break;
    }
  }
  if(frame.size()!=best.bytes)fail();
  write_header(frame,NativeGroupMethod::modular_delta,member,base,base_id,generation,
      frame.size()==header?0:static_cast<std::uint32_t>(xor_tiles));
  frame[104]=static_cast<std::byte>(best.word_bytes);
  put(frame,112,best.bias,8);
  return frame;
}

NativeGroupDeltaSignatures native_group_delta_signatures(std::span<const std::byte> bytes) {
  if(bytes.size()!=extent)fail();
  return {delta_signature<std::uint8_t>(bytes),delta_signature<std::uint16_t>(bytes),
      delta_signature<std::uint32_t>(bytes),delta_signature<std::uint64_t>(bytes)};
}

std::optional<CollectiveGroupBody> parse_collective_group_body(std::span<const std::byte> frame) noexcept {
  using namespace anchor_match;
  if(frame.size()<=collective_body_tag_bytes)return {};
  const auto tag=static_cast<CollectiveBody>(get(frame,0,collective_body_tag_bytes));
  const auto bytes=frame.subspan(collective_body_tag_bytes);
  switch(tag) {
    case CollectiveBody::sparse_xor:
      if(bytes.size()>=sparse_xor_min_body_bytes && bytes.size()<=sparse_xor_max_body_bytes)
        return CollectiveGroupBody{NativePackedGroupMethod::sparse_xor,bytes};
      break;
    case CollectiveBody::modular_delta:
      if(bytes.size()>=delta_parameters_bytes && bytes.size()<=delta_max_payload_bytes)
        return CollectiveGroupBody{NativePackedGroupMethod::modular_delta,bytes};
      break;
  }
  return {};
}

NativeGroupPackedCandidate check_native_group_delta_body(std::span<const std::byte> payload,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t generation,std::size_t exclusive_limit) {
  using Check=NativePackedGroupCheck;
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(payload.empty())return {};
  constexpr auto metadata=anchor_match::delta_parameters_bytes;
  static_assert(anchor_match::delta_max_payload_bytes==extent-header+metadata-1);
  if(payload.size()<metadata || payload.size()>anchor_match::delta_max_payload_bytes)return {{},Check::invalid};
  const auto width=static_cast<std::size_t>(get(payload,0,4));
  const auto bias=get(payload,4,8);
  const auto body=payload.subspan(metadata);
  if(!valid_delta_parameters(width,bias) || (body.empty() && !bias))return {{},Check::invalid};
  std::vector<std::byte> decoded(base.begin(),base.end());
  try {
    apply_modular_delta(body,{header+body.size(),width,bias},decoded);
  }catch(const std::runtime_error&) {return {{},Check::invalid};}
  if(!std::ranges::equal(decoded,member))return {{},Check::invalid};
  if(header+body.size()>=exclusive_limit)return {{},Check::verified};
  std::vector<std::byte> frame(header+body.size());
  std::ranges::copy(body,frame.begin()+header);
  write_header(frame,NativeGroupMethod::modular_delta,member,base,base_id,generation,
      body.empty()?0:static_cast<std::uint32_t>(xor_tiles));
  frame[104]=static_cast<std::byte>(width);
  put(frame,112,bias,8);
  return {std::move(frame),Check::verified,NativeGroupMethod::modular_delta};
}

NativeGroupPackedCandidate check_native_group_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t generation,std::size_t exclusive_limit,bool include_sparse) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(body.empty())return {};
  constexpr auto maximum_body=xor_tiles*(1+8*xor_plane_bytes);
  if(body.size()>maximum_body)return {{},NativePackedXorCheck::invalid};
  std::vector<std::byte> decoded(base.begin(),base.end());
  try {
    apply_packed_xor(body,decoded);
  }catch(const std::runtime_error&) {
    return {{},NativePackedXorCheck::invalid};
  }
  if(!std::ranges::equal(decoded,member))return {{},NativePackedXorCheck::invalid};
  // Reuse the verified dense bit planes. The final sparse frame is separately
  // decoded and compared before the indexed writer can publish it.
  const auto sparse_limit=(std::min)(exclusive_limit,header+body.size());
  if(include_sparse && sparse_limit>header+xor_tiles+2) {
    std::vector<std::byte> sparse(header);
    bool fits=true;
    packed_xor_tiles(body,[&](std::size_t tile,unsigned mask,std::span<const std::byte> planes) {
      if(!fits)return;
      fits=append_sparse_xor_tile(sparse,sparse_limit,tile,mask,[&](unsigned) {
        const auto plane=planes.first(xor_plane_bytes);
        planes=planes.subspan(xor_plane_bytes);
        return plane;
      });
    });
    if(fits) {
      write_header(sparse,NativeGroupMethod::sparse_xor,member,base,base_id,generation,
          static_cast<std::uint32_t>(xor_tiles));
      return {std::move(sparse),NativePackedXorCheck::verified,NativeGroupMethod::sparse_xor};
    }
  }
  if(header+body.size()>=exclusive_limit)return {{},NativePackedXorCheck::verified};
  std::vector<std::byte> frame(header+body.size());
  std::ranges::copy(body,frame.begin()+header);
  write_header(frame,NativeGroupMethod::packed_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  return {std::move(frame),NativePackedXorCheck::verified};
}

NativeGroupPackedCandidate check_native_group_sparse_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t generation,std::size_t exclusive_limit) {
  using Check=NativePackedGroupCheck;
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(body.empty())return {};
  if(body.size()>anchor_match::sparse_xor_max_body_bytes)return {{},Check::invalid};
  std::vector<std::byte> decoded(base.begin(),base.end());
  std::size_t planes=0;
  try {
    planes=apply_sparse_xor(body,decoded);
  }catch(const std::runtime_error&) {
    return {{},Check::invalid};
  }
  if(!std::ranges::equal(decoded,member))return {{},Check::invalid};
  // The dense format prices every changed plane equally. Preserve it on ties
  // and only encode it on CPU when this sparse candidate cannot be selected.
  if(header+body.size()>=exclusive_limit || body.size()>=xor_tiles+planes*xor_plane_bytes)
    return {{},Check::verified};
  std::vector<std::byte> frame(header+body.size());
  std::ranges::copy(body,frame.begin()+header);
  write_header(frame,NativeGroupMethod::sparse_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  return {std::move(frame),Check::verified,NativeGroupMethod::sparse_xor};
}

std::array<std::uint64_t,kNativeGroupShiftFeatures> native_group_shift_features(std::span<const std::byte> bytes) noexcept {
  while(!bytes.empty() && bytes.back()==std::byte{})bytes=bytes.first(bytes.size()-1);
  std::array<std::uint64_t,kNativeGroupShiftFeatures> features{};
  if(bytes.size()<window_bytes)return features;
  std::array<std::uint64_t,kNativeGroupShiftFeatures> minima{};
  auto rolling=fingerprint(bytes.first(window_bytes));
  const auto last=bytes.size()-window_bytes;
  for(std::size_t offset=0;;++offset) {
    if(rolling) {
      const auto value=mix(rolling);
      auto& minimum=minima[value%minima.size()];
      if(!minimum || value<minimum)minimum=value;
    }
    if(offset==last)break;
    rolling=roll(rolling,bytes[offset],bytes[offset+window_bytes]);
  }
  // Require two distinct buckets so identical padding or one repeated word
  // cannot create an offset-independent candidate by itself.
  for(std::size_t i=0;i<features.size();++i) {
    const auto next=minima[(i+1)%minima.size()];
    if(minima[i] && next)features[i]=mix(minima[i]^std::rotl(next,29));
  }
  return features;
}

NativeGroupAnchorIndex::NativeGroupAnchorIndex(std::span<const std::byte> base) {
  if(base.size()!=extent)fail();
  static_assert(extent/anchor_stride*sizeof(Entry)<=kNativeGroupAnchorIndexBytes);
  entries_.reserve(extent/anchor_stride);
  for(std::uint32_t offset=0;offset<=extent-window_bytes;offset+=anchor_stride)
    entries_.emplace_back(fingerprint(base.subspan(offset,window_bytes)),offset);
  std::ranges::sort(entries_);
}

std::optional<std::vector<std::byte>> encode_native_group_copies(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,
    std::span<const NativeSharedBaseCopyRange> copies,std::size_t exclusive_limit) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(copies.empty() || exclusive_limit<=kNativeGroupCopyMinimumBytes ||
      copies.size()>(exclusive_limit-header)/8)return {};
  AnchorCopyFrame frame(member,exclusive_limit);
  std::size_t end=0;
  for(const auto& copy:copies) {
    const auto offset=copy.member_offset,source=copy.base_offset,length=copy.length;
    if(offset<end || offset>=extent || source>=extent || !length || length>extent-offset ||
        length>extent-source || !std::ranges::equal(member.subspan(offset,length),base.subspan(source,length)) ||
        !frame.append_copy(offset,source,length))return {};
    end=offset+length;
  }
  return frame.finish(base,base_id,generation);
}

std::optional<std::vector<std::byte>> NativeGroupAnchorIndex::encode(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,
    std::size_t exclusive_limit) const {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(exclusive_limit<=kNativeGroupCopyMinimumBytes)return {};
  AnchorCopyFrame frame(member,exclusive_limit);

  const auto last=extent-window_bytes;
  auto rolling=fingerprint(member.first(window_bytes));
  for(std::size_t offset=0;offset<=last;) {
    const auto first=std::ranges::lower_bound(entries_,Entry{rolling,0});
    std::size_t source=0,length=0;
    if(first!=entries_.end() && first->first==rolling) {
      const auto near=std::lower_bound(first,entries_.end(),Entry{rolling,static_cast<std::uint32_t>(offset)});
      const auto earlier=near==first?entries_.end():near-1;
      for(const auto candidate:{first,near,earlier}) {
        if(candidate==entries_.end() || candidate->first!=rolling)continue;
        const std::size_t start=candidate->second;
        const auto capacity=(std::min)(extent-offset,extent-start);
        if(capacity<=length)continue;
        const auto left=base.begin()+start;
        const auto matched=static_cast<std::size_t>(std::mismatch(left,left+capacity,member.begin()+offset).first-left);
        if(matched>=window_bytes && matched>length){source=start;length=matched;}
      }
    }
    if(!length) {
      if(offset==last)break;
      rolling=roll(rolling,member[offset],member[offset+window_bytes]);
      ++offset;
      continue;
    }
    if(!frame.append_copy(offset,source,length))return {};
    offset+=length;
    if(offset<=last)rolling=fingerprint(member.subspan(offset,window_bytes));
  }
  return frame.finish(base,base_id,generation);
}
} // namespace flitzfs::detail
