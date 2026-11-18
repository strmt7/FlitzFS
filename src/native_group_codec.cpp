#include "native_group_codec.hpp"
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
NativeGroupMethod member_method(std::span<const std::byte> bytes,bool allow_anchor_copies) {
  if(bytes.size()<header || bytes.size()>=extent)fail();
  const auto version=get(bytes,8,4);
  if(version==1 && std::equal(aligned_magic.begin(),aligned_magic.end(),bytes.begin()))
    return NativeGroupMethod::aligned_patches;
  if(allow_anchor_copies && version==2 && std::equal(copy_magic.begin(),copy_magic.end(),bytes.begin()))
    return NativeGroupMethod::anchor_copies;
  fail();
}
void write_header(std::span<std::byte> bytes,NativeGroupMethod method,std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,std::uint32_t count) {
  const auto& magic=method==NativeGroupMethod::aligned_patches?aligned_magic:copy_magic;
  std::copy(magic.begin(),magic.end(),bytes.begin());
  put(bytes,8,static_cast<std::uint32_t>(method),4);
  put(bytes,12,extent,4);put(bytes,16,base_id,8);put(bytes,24,generation,8);
  const auto base_hash=record_digest(base),member_hash=record_digest(member);
  std::copy(base_hash.begin(),base_hash.end(),bytes.begin()+32);
  std::copy(member_hash.begin(),member_hash.end(),bytes.begin()+64);
  put(bytes,96,count,4);put(bytes,100,bytes.size()-header,4);
}
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

NativeGroupReference parse_native_group_member(std::span<const std::byte> bytes,bool allow_anchor_copies) {
  const auto method=member_method(bytes,allow_anchor_copies);
  if(get(bytes,12,4)!=extent || get(bytes,100,4)!=bytes.size()-header ||
      !std::all_of(bytes.begin()+104,bytes.begin()+128,[](auto value){return value==std::byte{0};}))fail();
  NativeGroupReference ref;ref.base_id=get(bytes,16,8);ref.base_generation=get(bytes,24,8);
  if(!base_id_valid(ref.base_id) || !ref.base_generation)fail();
  std::copy_n(bytes.begin()+32,32,ref.base_digest.begin());std::copy_n(bytes.begin()+64,32,ref.decoded_digest.begin());
  ref.member_bytes=static_cast<std::uint32_t>(bytes.size());ref.method=method;
  if(method==NativeGroupMethod::aligned_patches)patches(bytes,[](auto,auto){});
  else spans(bytes,[](auto,auto,auto){},[](auto,auto){});
  return ref;
}
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> bytes,
    std::span<const std::byte> base,bool allow_anchor_copies) {
  const auto ref=parse_native_group_member(bytes,allow_anchor_copies);
  if(base.size()!=extent || record_digest(base)!=ref.base_digest)fail();
  std::vector<std::byte> result;
  const auto literal=[&](auto offset,auto input){std::copy(input.begin(),input.end(),result.begin()+offset);};
  if(ref.method==NativeGroupMethod::aligned_patches) {
    result.assign(base.begin(),base.end());
    patches(bytes,literal);
  } else {
    result.resize(extent);
    spans(bytes,[&](auto output,auto source,auto length){
      std::copy_n(base.begin()+source,length,result.begin()+output);
    },literal);
  }
  if(record_digest(result)!=ref.decoded_digest)fail();return result;
}
void bind_native_group_member(std::span<std::byte> member,std::uint64_t base_id,
    std::uint64_t generation,bool allow_anchor_copies) {
  if(!base_id_valid(base_id) || !generation)fail();
  (void)member_method(member,allow_anchor_copies);
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

std::optional<std::vector<std::byte>> NativeGroupAnchorIndex::encode(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation,
    std::size_t exclusive_limit) const {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation ||
      exclusive_limit>extent)fail();
  if(exclusive_limit<=kNativeGroupCopyMinimumBytes)return {};
  std::vector<std::byte> result(header);
  std::size_t literal_start=0;
  std::uint32_t count=0;
  const auto flush_literal=[&](std::size_t end) {
    const auto length=end-literal_start;
    if(!length)return true;
    if(length+4>=exclusive_limit-result.size())return false;
    const auto output=result.size();
    result.resize(output+4+length);
    put(result,output,length,4);
    std::copy_n(member.begin()+literal_start,length,result.begin()+output+4);
    ++count;
    return true;
  };

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
    if(!flush_literal(offset) || 8>=exclusive_limit-result.size())return {};
    const auto output=result.size();
    result.resize(output+8);
    put(result,output,copy_flag|length,4);put(result,output+4,source,4);
    ++count;
    offset+=length;
    literal_start=offset;
    if(offset<=last)rolling=fingerprint(member.subspan(offset,window_bytes));
  }
  if(!flush_literal(extent))return {};
  write_header(result,NativeGroupMethod::anchor_copies,member,base,base_id,generation,count);
  return result;
}
} // namespace flitzfs::detail
