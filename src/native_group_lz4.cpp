#include "native_group_lz4.hpp"
#include <algorithm>
#include <stdexcept>

namespace flitzfs::detail {
namespace {
constexpr std::uint32_t extent=kNativeExtentBytes,half=extent/2;
constexpr std::uint32_t minimum_useful_copy=12;

bool read_length(UniformLz4Bytes payload,std::size_t& at,std::uint32_t& length) noexcept {
  if(length!=15)return true;
  unsigned next=255;
  while(next==255) {
    if(at==payload.size())return false;
    next=std::to_integer<unsigned>(payload[at++]);
    if(next>extent-length)return false;
    length+=next;
  }
  return true;
}

bool collect_ranges(UniformLz4Bytes payload,std::uint32_t member_half,std::uint32_t anchor_half,
    std::vector<NativeSharedBaseCopyRange>& ranges) {
  if(payload.empty())return true;
  if(payload.size()>=extent)return false;
  std::size_t at=0;
  std::uint32_t decoded=0;
  while(at<payload.size()) {
    const auto token=std::to_integer<unsigned>(payload[at++]);
    std::uint32_t literal=token>>4;
    if(!read_length(payload,at,literal) || literal>payload.size()-at || literal>extent-decoded)return false;
    decoded+=literal;at+=literal;
    if(at==payload.size())return decoded==extent;
    if(payload.size()-at<2)return false;
    const auto distance=std::to_integer<unsigned>(payload[at])|
        (std::to_integer<unsigned>(payload[at+1])<<8);
    at+=2;
    std::uint32_t length=token&15;
    if(!distance || distance>decoded || !read_length(payload,at,length) ||
        extent-decoded<4 || length>extent-decoded-4)return false;
    length+=4;
    const auto copy_start=(std::max)(decoded,half);
    const auto copy_source=copy_start-distance;
    if(copy_start<decoded+length && copy_source<half) {
      const auto available=(std::min)(decoded+length-copy_start,half-copy_source);
      if(available>=minimum_useful_copy)
        ranges.push_back({member_half*half+copy_start-half,anchor_half*half+copy_source,available});
    }
    decoded+=length;
  }
  return false;
}
}

PreparedUniformLz4 prepare_native_anchor_search(std::span<const std::byte> anchor,
    std::span<const std::byte> members,const UniformLz4Limits& limits) {
  if(anchor.size()!=extent || members.empty() || members.size()%extent ||
      !valid_uniform_lz4_limits(limits) || limits.chunk_bytes!=extent ||
      limits.input_bytes!=std::uint64_t(limits.chunks)*extent ||
      limits.chunks%kAnchorLz4BlocksPerMember ||
      members.size()/extent!=limits.chunks/kAnchorLz4BlocksPerMember)
    throw std::invalid_argument("invalid shared-base LZ4 search shape");
  std::vector<std::byte> input(static_cast<std::size_t>(limits.input_bytes));
  auto out=input.begin();
  for(std::size_t member=0;member<members.size();member+=extent)
    for(unsigned target=0;target<2;++target)
      for(unsigned base=0;base<2;++base) {
        out=std::copy_n(anchor.begin()+base*half,half,out);
        out=std::copy_n(members.begin()+member+target*half,half,out);
      }
  return PreparedUniformLz4::prepare(input,limits);
}

std::vector<NativeSharedBaseCopyRange> native_anchor_copy_ranges(
    std::span<const UniformLz4Bytes,kAnchorLz4BlocksPerMember> blocks) {
  std::vector<NativeSharedBaseCopyRange> ranges;
  for(std::uint32_t i=0;i<blocks.size();++i)
    if(!collect_ranges(blocks[i],i/2,i%2,ranges))return {};
  std::ranges::sort(ranges,[](const auto& a,const auto& b) {
    if(a.member_offset!=b.member_offset)return a.member_offset<b.member_offset;
    if(a.length!=b.length)return a.length>b.length;
    return a.base_offset<b.base_offset;
  });
  std::size_t count=0;
  for(auto range:ranges) {
    if(count) {
      auto& previous=ranges[count-1];
      const auto end=previous.member_offset+previous.length;
      if(range.member_offset<end) {
        const auto overlap=end-range.member_offset;
        if(overlap>=range.length)continue;
        range.member_offset+=overlap;range.base_offset+=overlap;range.length-=overlap;
      }
      if(range.member_offset==end && range.base_offset==previous.base_offset+previous.length) {
        previous.length+=range.length;
        continue;
      }
    }
    if(range.length>=minimum_useful_copy)ranges[count++]=range;
  }
  ranges.resize(count);
  return ranges;
}

} // namespace flitzfs::detail
