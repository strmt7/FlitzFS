#include "flitzfs/native_filesystem.hpp"

namespace flitzfs {
// No pointer casts, allocation, Windows call or policy interpretation. Bounds
// precede every read. ACE payloads stay opaque; framing is NOT authorization
// or a substitute for Windows descriptor/ACL validation at the OS boundary.
bool native_security_descriptor_framed(std::span<const std::byte> bytes) noexcept {
  if(bytes.size()<20 || bytes.size()>kNativeMaxSecurityBytes || bytes.size()%4)return false;
  const auto number=[&](std::size_t at,unsigned count) {
    std::uint32_t value=0;
    for(unsigned i=0;i<count;++i)value|=std::uint32_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
    return value;
  };
  const auto control=number(2,2);
  if(number(0,1)!=1 || !(control&0x8000U) || (control&0xc0U) ||
      (!(control&0x4000U) && number(1,1)))return false;
  for(const auto at:{4U,8U}) {
    const auto offset=number(at,4);
    if(!offset)continue;
    if(offset<20 || offset%4 || offset>bytes.size() || bytes.size()-offset<8)return false;
    const auto count=number(offset+1,1);
    if(number(offset,1)!=1 || count>15 || 8+4*count>bytes.size()-offset)return false;
  }
  for(const auto at:{12U,16U}) {
    const auto offset=number(at,4);
    const auto present=control&(at==12?0x10U:0x4U);
    if(!present && offset)return false;
    if(!offset)continue; // Preserve the distinction between NULL and absent ACL.
    if(offset<20 || offset%4 || offset>bytes.size() || bytes.size()-offset<8)return false;
    const auto revision=number(offset,1),size=number(offset+2,2),count=number(offset+4,2);
    if(revision<2 || revision>4 || size<8 || size%4 || size>bytes.size()-offset ||
        number(offset+1,1) || number(offset+6,2) || count>(size-8)/4)return false;
    std::size_t cursor=offset+8,end=offset+size;
    for(std::uint32_t i=0;i<count;++i) {
      if(end-cursor<4)return false;
      const auto length=number(cursor+2,2);
      if(length<4 || length%4 || length>end-cursor)return false;
      cursor+=length;
    }
    // Unused ACL capacity is legal opaque data. Never reorder or rewrite ACEs.
  }
  return true;
}
}
