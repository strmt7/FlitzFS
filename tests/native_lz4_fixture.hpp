#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
namespace native_lz4_fixture {
using Bytes=std::vector<std::byte>;
inline Bytes source(){
  Bytes prefix(8192),plain(65536);std::uint64_t x=0x679a210fed44c3abULL;
  for(auto& byte:prefix){x^=x<<13;x^=x>>7;x^=x<<17;byte=static_cast<std::byte>(x>>21);}
  for(std::size_t i=0;i<plain.size();++i)plain[i]=prefix[i%prefix.size()];return plain;
}
// Independent literal/match construction, rather than the production encoder.
// Two explicit matches and five final literals encode this seeded repeated data.
inline Bytes alternative_block(std::span<const std::byte> source){
  if(source.size()!=65536)throw std::runtime_error("alternative block extent");
  Bytes out;
  const auto extension=[&](std::size_t value){
    while(value>=255){out.push_back(std::byte{255});value-=255;}
    out.push_back(static_cast<std::byte>(value));
  };
  const auto match=[&](std::span<const std::byte> literals,std::size_t length){
    out.push_back(static_cast<std::byte>((std::min<std::size_t>(15,literals.size())<<4)|
        std::min<std::size_t>(15,length-4)));
    if(literals.size()>=15)extension(literals.size()-15);
    out.insert(out.end(),literals.begin(),literals.end());
    out.push_back(std::byte{0});out.push_back(std::byte{32}); // Offset8192.
    if(length>=19)extension(length-19);
  };
  match(std::span(source).first(8192),8192);
  match({},source.size()-16384-5);
  out.push_back(std::byte{0x50});
  out.insert(out.end(),source.end()-5,source.end());return out;
}
}
