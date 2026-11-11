#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace flitzfs {
// Private metadata frames, never decoder candidates or stored filesystem bytes.
struct HipIdentityRequest {
  std::uint64_t request_id=0;
  std::uint32_t ordinal=0,node_mask=0;
  std::array<std::byte,8> luid{};
};
struct HipIdentityObservation {
  std::uint64_t request_id=0;
  std::uint32_t ordinal=0,node_mask=0;
  std::array<std::byte,8> luid{};
};
namespace hip_identity_detail {
inline constexpr std::array<std::byte,8> magic{
    std::byte{'F'},std::byte{'L'},std::byte{'H'},std::byte{'I'},
    std::byte{'D'},std::byte{'0'},std::byte{'0'},std::byte{'1'}};
using Frame=std::array<std::byte,40>;
inline void require(bool ok){if(!ok)throw std::invalid_argument("invalid HIP identity metadata frame");}
inline void put(Frame& frame,std::size_t at,std::uint64_t value,unsigned bytes) {
  for(unsigned i=0;i<bytes;++i)frame[at+i]=static_cast<std::byte>(value>>(8*i));
}
inline std::uint64_t get(std::span<const std::byte> frame,std::size_t at,unsigned bytes) {
  std::uint64_t value=0;
  for(unsigned i=0;i<bytes;++i)value|=std::uint64_t(std::to_integer<unsigned>(frame[at+i]))<<(8*i);
  return value;
}
inline void request_valid(const HipIdentityRequest& value) {
  require(value.request_id && value.ordinal<64 && value.node_mask &&
      !(value.node_mask&(value.node_mask-1)) &&
      std::any_of(value.luid.begin(),value.luid.end(),[](auto b){return b!=std::byte{0};}));
}
inline Frame prefix(unsigned kind,std::uint64_t request_id,std::uint32_t ordinal) {
  require(request_id && ordinal<64);
  Frame frame{};std::copy(magic.begin(),magic.end(),frame.begin());
  put(frame,8,kind,4);put(frame,16,request_id,8);put(frame,24,ordinal,4);return frame;
}
inline void header(std::span<const std::byte> frame,unsigned kind) {
  require(frame.size()==40 && std::equal(magic.begin(),magic.end(),frame.begin()) &&
      get(frame,8,4)==kind && get(frame,12,4)==0 && get(frame,16,8)!=0 && get(frame,24,4)<64);
}
}
inline hip_identity_detail::Frame encode_hip_identity_request(const HipIdentityRequest& value) {
  hip_identity_detail::request_valid(value);
  auto frame=hip_identity_detail::prefix(1,value.request_id,value.ordinal);
  hip_identity_detail::put(frame,28,value.node_mask,4);
  std::copy(value.luid.begin(),value.luid.end(),frame.begin()+32);return frame;
}
inline HipIdentityRequest decode_hip_identity_request(std::span<const std::byte> frame) {
  using namespace hip_identity_detail;
  header(frame,1);
  HipIdentityRequest value{get(frame,16,8),static_cast<std::uint32_t>(get(frame,24,4)),
      static_cast<std::uint32_t>(get(frame,28,4)),{}};
  std::copy_n(frame.begin()+32,8,value.luid.begin());request_valid(value);return value;
}
inline hip_identity_detail::Frame encode_hip_identity_reply(const HipIdentityObservation& value) {
  auto frame=hip_identity_detail::prefix(2,value.request_id,value.ordinal);
  hip_identity_detail::put(frame,28,value.node_mask,4);
  std::copy(value.luid.begin(),value.luid.end(),frame.begin()+32);return frame;
}
inline HipIdentityObservation decode_hip_identity_reply(std::span<const std::byte> frame,
    const HipIdentityRequest& expected) {
  using namespace hip_identity_detail;
  request_valid(expected);header(frame,2);
  require(get(frame,16,8)==expected.request_id && get(frame,24,4)==expected.ordinal);
  HipIdentityObservation value{expected.request_id,expected.ordinal,
      static_cast<std::uint32_t>(get(frame,28,4)),{}};
  // Zero/multibit masks and a different LUID are observations, not compute consent.
  std::copy_n(frame.begin()+32,8,value.luid.begin());return value;
}
}
