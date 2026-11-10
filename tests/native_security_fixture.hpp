#pragma once
#include "flitzfs/native_filesystem.hpp"
#include <algorithm>
namespace native_security_fixture {
using Bytes=flitzfs::NativeSecurityDescriptor;
inline void put(Bytes& out,std::size_t at,std::uint64_t value,unsigned count=4) {
  for(unsigned i=0;i<count;++i)out.at(at+i)=static_cast<std::byte>(value>>(8*i));
}
inline std::uint64_t get(const Bytes& in,std::size_t at,unsigned count=4) {
  std::uint64_t value=0;
  for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(in.at(at+i)))<<(8*i);
  return value;
}
// Self-relative descriptor: owner/group SIDs, one ordinary allow ACE for
// Everyone. Windows SDK tests independently validate these actual bytes.
inline Bytes descriptor(unsigned version=1) {
  Bytes out(72);put(out,0,1,1);put(out,2,0x8004,2);
  put(out,4,20);put(out,8,32);put(out,16,44);
  for(const auto at:{20U,32U,60U}) {
    put(out,at,1,1);put(out,at+1,1,1);put(out,at+7,at==60?1:5,1);
    put(out,at+8,at==20?17+version:at==32?32:0);
  }
  put(out,44,2,1);put(out,46,28,2);put(out,48,1,2);
  put(out,54,20,2);put(out,56,0x120089);
  return out;
}
inline Bytes null_dacl(bool present=true) {
  Bytes out(20);put(out,0,1,1);put(out,2,present?0x8004:0x8000,2);return out;
}
inline Bytes empty_dacl() {
  auto out=null_dacl();out.resize(28);put(out,16,20);put(out,20,2,1);put(out,22,8,2);return out;
}
}
