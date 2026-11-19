#include "native_group_codec.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
namespace flitzfs::detail {
namespace {
constexpr std::size_t extent=65536,header=128;
constexpr std::array<std::byte,8> aligned_magic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},
    std::byte{'G'},std::byte{'M'},std::byte{'0'},std::byte{'1'}};
constexpr auto copy_magic=[] {auto value=aligned_magic;value.back()=std::byte{'2'};return value;}();
constexpr auto xor_magic=[] {auto value=aligned_magic;value.back()=std::byte{'3'};return value;}();
constexpr std::array member_magics{aligned_magic,copy_magic,xor_magic};
constexpr std::size_t xor_tile_bytes=256,xor_plane_bytes=xor_tile_bytes/8,xor_tiles=extent/xor_tile_bytes;
static_assert(extent%xor_tile_bytes==0 && xor_tile_bytes%8==0 &&
    extent==anchor_match::extent_bytes && xor_tile_bytes==anchor_match::xor_tile_bytes);
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

template<class Apply>void packed_xor_planes(std::span<const std::byte> frame,Apply apply) {
  if(get(frame,96,4)!=xor_tiles)fail();
  std::size_t input=header;
  bool changed=false;
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    if(input==frame.size())fail();
    const auto mask=std::to_integer<unsigned>(frame[input++]);
    const auto payload=static_cast<std::size_t>(std::popcount(mask))*xor_plane_bytes;
    if(payload>frame.size()-input)fail();
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      const auto plane=frame.subspan(input,xor_plane_bytes);
      if(std::ranges::all_of(plane,[](auto packed){return packed==std::byte{};}))fail();
      apply(tile*xor_tile_bytes,bit,plane);
      input+=xor_plane_bytes;
      changed=true;
    }
  }
  if(input!=frame.size() || !changed)fail();
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
      !std::all_of(bytes.begin()+104,bytes.begin()+128,[](auto value){return value==std::byte{0};}))fail();
  NativeGroupReference ref;ref.base_id=get(bytes,16,8);ref.base_generation=get(bytes,24,8);
  if(!base_id_valid(ref.base_id) || !ref.base_generation)fail();
  std::copy_n(bytes.begin()+32,32,ref.base_digest.begin());std::copy_n(bytes.begin()+64,32,ref.decoded_digest.begin());
  ref.member_bytes=static_cast<std::uint32_t>(bytes.size());ref.method=method;
  switch(method) {
    case NativeGroupMethod::aligned_patches:patches(bytes,[](auto,auto){});break;
    case NativeGroupMethod::anchor_copies:spans(bytes,[](auto,auto,auto){},[](auto,auto){});break;
    case NativeGroupMethod::packed_xor:packed_xor_planes(bytes,[](auto,auto,auto){});break;
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
    else packed_xor_planes(bytes,[&](std::size_t output,unsigned bit,std::span<const std::byte> plane) {
      for(const auto packed:plane) {
        const auto value=std::to_integer<unsigned>(packed);
        for(unsigned i=0;i<8;++i)
          result[output+i]^=static_cast<std::byte>(((value>>i)&1U)<<bit);
        output+=8;
      }
    });
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
  std::array<unsigned char,xor_tile_bytes> differences{};
  for(std::size_t tile=0;tile<xor_tiles;++tile) {
    const auto output=tile*xor_tile_bytes;
    unsigned mask=0;
    for(std::size_t i=0;i<xor_tile_bytes;++i) {
      differences[i]=std::to_integer<unsigned char>(member[output+i]^base[output+i]);
      mask|=differences[i];
    }
    const auto payload=static_cast<std::size_t>(std::popcount(mask))*xor_plane_bytes;
    // Charge one mask byte for every remaining tile before retaining any planes.
    if(payload+xor_tiles-tile>=exclusive_limit-frame.size())return {};
    frame.push_back(static_cast<std::byte>(mask));
    for(unsigned bit=0;bit<8;++bit) {
      if(!(mask&(1U<<bit)))continue;
      for(std::size_t first=0;first<xor_tile_bytes;first+=8) {
        unsigned packed=0;
        for(unsigned i=0;i<8;++i)
          packed|=((differences[first+i]>>bit)&1U)<<i;
        frame.push_back(static_cast<std::byte>(packed));
      }
    }
  }
  if(frame.size()==header+xor_tiles)return {};
  write_header(frame,NativeGroupMethod::packed_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  return frame;
}

std::optional<std::vector<std::byte>> accept_native_group_xor_body(std::span<const std::byte> body,
    std::span<const std::byte> member,std::span<const std::byte> base,std::uint64_t base_id,
    std::uint64_t generation,std::size_t exclusive_limit) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(body.empty() || exclusive_limit<=header || body.size()>=exclusive_limit-header)return {};
  std::vector<std::byte> frame(header);
  frame.insert(frame.end(),body.begin(),body.end());
  write_header(frame,NativeGroupMethod::packed_xor,member,base,base_id,generation,static_cast<std::uint32_t>(xor_tiles));
  try {
    if(std::ranges::equal(decode_native_group_member(frame,base,NativeGroupMethod::packed_xor),member))
      return frame;
  }catch(const std::runtime_error&) {
    // Untrusted advice can fail framing or readback without fencing storage.
  }
  return {};
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
