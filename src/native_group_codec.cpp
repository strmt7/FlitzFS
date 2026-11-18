#include "native_group_codec.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
namespace flitzfs::detail {
namespace {
constexpr std::size_t extent=65536,header=128;
constexpr std::array<std::byte,8> magic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},
    std::byte{'G'},std::byte{'M'},std::byte{'0'},std::byte{'1'}};
[[noreturn]]void fail(){throw std::runtime_error("invalid shared-base member");}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,unsigned count) {
  if(at>bytes.size() || count>bytes.size()-at)fail();std::uint64_t value=0;
  for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(i*8);return value;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,unsigned count) {
  for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::byte>(value>>(i*8));
}
bool base_id_valid(std::uint64_t id){return id>kNativeGroupBaseBit && id<2*kNativeGroupBaseBit;}
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
}
NativeGroupReference parse_native_group_member(std::span<const std::byte> bytes) {
  if(bytes.size()<header || bytes.size()>=extent || !std::equal(magic.begin(),magic.end(),bytes.begin()) ||
      get(bytes,8,4)!=1 || get(bytes,12,4)!=extent || get(bytes,100,4)!=bytes.size()-header ||
      !std::all_of(bytes.begin()+104,bytes.begin()+128,[](auto value){return value==std::byte{0};}))fail();
  NativeGroupReference ref;ref.base_id=get(bytes,16,8);ref.base_generation=get(bytes,24,8);
  if(!base_id_valid(ref.base_id) || !ref.base_generation)fail();
  std::copy_n(bytes.begin()+32,32,ref.base_digest.begin());std::copy_n(bytes.begin()+64,32,ref.decoded_digest.begin());
  ref.member_bytes=static_cast<std::uint32_t>(bytes.size());patches(bytes,[](auto,auto){});return ref;
}
std::vector<std::byte> decode_native_group_member(std::span<const std::byte> bytes,std::span<const std::byte> base) {
  const auto ref=parse_native_group_member(bytes);
  if(base.size()!=extent || record_digest(base)!=ref.base_digest)fail();
  std::vector<std::byte> result(base.begin(),base.end());
  patches(bytes,[&](std::size_t offset,auto literal){std::copy(literal.begin(),literal.end(),result.begin()+offset);});
  if(record_digest(result)!=ref.decoded_digest)fail();return result;
}
void bind_native_group_member(std::span<std::byte> member,std::uint64_t base_id,std::uint64_t generation) {
  if(member.size()<header || member.size()>=extent || !base_id_valid(base_id) || !generation ||
      !std::equal(magic.begin(),magic.end(),member.begin()))fail();
  put(member,16,base_id,8);
  put(member,24,generation,8);
}
std::optional<std::vector<std::byte>> encode_native_group_member(std::span<const std::byte> member,
    std::span<const std::byte> base,std::uint64_t base_id,std::uint64_t generation) {
  if(member.size()!=extent || base.size()!=extent || !base_id_valid(base_id) || !generation)fail();
  std::vector<std::byte> result(header);std::copy(magic.begin(),magic.end(),result.begin());
  put(result,8,1,4);put(result,12,extent,4);put(result,16,base_id,8);put(result,24,generation,8);
  const auto base_hash=record_digest(base),member_hash=record_digest(member);
  std::copy(base_hash.begin(),base_hash.end(),result.begin()+32);std::copy(member_hash.begin(),member_hash.end(),result.begin()+64);
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
  put(result,96,count,4);put(result,100,result.size()-header,4);return result;
}
} // namespace flitzfs::detail
