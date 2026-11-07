#pragma once
// Test-only, explicitly encoded fixture writer; not a product archive writer.
// The separate oracle test compares these bytes with pinned FlitzZip source.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#ifdef FLITZFS_TEST_HAS_ZSTD
#include <zstd.h>
#endif
#ifdef FLITZFS_TEST_HAS_ZLIB
#include <zlib.h>
#endif
namespace flzip_fixture {
using Bytes = std::vector<std::byte>;
inline void put(Bytes& b, std::uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) b.push_back(static_cast<std::byte>((value >> (8U*i)) & 255U));
}
inline void replace(Bytes& b, std::size_t at, std::uint64_t value, unsigned width) {
  if (at > b.size() || width > b.size() - at) throw std::runtime_error("fixture edit out of bounds");
  for (unsigned i = 0; i < width; ++i) b[at+i] = static_cast<std::byte>((value >> (8U*i)) & 255U);
}
// Bit-at-a-time reference independent of the production table implementation.
inline std::uint32_t crc(std::uint32_t prior, const Bytes& bytes) {
  auto value = ~prior;
  for (auto b : bytes) {
    value ^= std::to_integer<unsigned>(b);
    for (unsigned i = 0; i < 8; ++i) value = (value >> 1) ^ ((value & 1) ? 0xedb88320U : 0);
  }
  return ~value;
}
inline Bytes text(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()+s.size())};
}
struct Block { std::uint8_t kind = 0, fill = 0; Bytes plain, encoded; };
struct Record { std::string path; bool directory = false; std::vector<Block> blocks; };
inline Block raw(Bytes plain) { auto encoded = plain; return {0, 0, std::move(plain), std::move(encoded)}; }
inline Block fill(std::size_t n, std::uint8_t byte) { return {1, byte, Bytes(n, static_cast<std::byte>(byte)), {}}; }
inline Block pattern(std::size_t n, const std::string& motif = "abc") {
  auto bytes = text(motif); Bytes plain(n);
  for (std::size_t i=0;i<n;++i) plain[i]=bytes.at(i%bytes.size());
  return {3, 0, std::move(plain), std::move(bytes)};
}
inline Block compressed(std::uint8_t kind, Bytes plain) {
  Bytes out;
#ifdef FLITZFS_TEST_HAS_ZSTD
  if (kind == 8) {
    out.resize(ZSTD_compressBound(plain.size()));
    const auto size = ZSTD_compress(out.data(), out.size(), plain.data(), plain.size(), 1);
    if (ZSTD_isError(size)) throw std::runtime_error("fixture Zstandard encoding failed");
    out.resize(size);
  }
#endif
#ifdef FLITZFS_TEST_HAS_ZLIB
  if (kind == 2) {
    out.resize(compressBound(static_cast<uLong>(plain.size())));
    uLongf size = static_cast<uLongf>(out.size());
    if (compress2(reinterpret_cast<Bytef*>(out.data()), &size,
          reinterpret_cast<const Bytef*>(plain.data()), static_cast<uLong>(plain.size()), 1) != Z_OK)
      throw std::runtime_error("fixture Deflate encoding failed");
    out.resize(static_cast<std::size_t>(size));
  }
#endif
  if (out.empty() || out.size() >= plain.size()) throw std::runtime_error("fixture codec unavailable or not compressing");
  return {kind, 0, std::move(plain), std::move(out)};
}
struct Fixture {
  Bytes bytes;
  std::size_t index = 0;
  std::vector<std::size_t> record_fields, block_fields;
};
inline Fixture archive(const std::vector<Record>& records, std::uint32_t version = 1) {
  Fixture out;
  std::vector<std::uint64_t> starts, sizes;
  for (const auto& r : records) {
    starts.push_back(out.bytes.size());
    for (const auto& block : r.blocks) out.bytes.insert(out.bytes.end(), block.encoded.begin(), block.encoded.end());
    sizes.push_back(out.bytes.size() - starts.back());
  }
  out.index = out.bytes.size();
  put(out.bytes, 0x505a5553, 4); put(out.bytes, version, 4); put(out.bytes, records.size(), 4);
  for (std::size_t i=0;i<records.size();++i) {
    const auto& r=records[i];
    put(out.bytes,r.path.size(),2); const auto name=text(r.path);
    out.bytes.insert(out.bytes.end(),name.begin(),name.end());
    out.record_fields.push_back(out.bytes.size());
    put(out.bytes,r.directory?1:0,1);
    std::uint64_t logical=0; std::uint32_t checksum=0;
    for (const auto& block:r.blocks) { logical+=block.plain.size();checksum=crc(checksum,block.plain); }
    put(out.bytes,logical,8);put(out.bytes,starts[i],8);put(out.bytes,sizes[i],8);put(out.bytes,checksum,4);put(out.bytes,r.blocks.size(),4);
    std::uint64_t offset=0;
    for (const auto& block:r.blocks) {
      out.block_fields.push_back(out.bytes.size());
      put(out.bytes,block.kind,1);put(out.bytes,block.fill,1);put(out.bytes,block.plain.size(),4);
      put(out.bytes,block.kind==1?0:offset,8);put(out.bytes,block.encoded.size(),4);
      offset+=block.encoded.size();
    }
  }
  const auto index_size=out.bytes.size()-out.index;
  put(out.bytes,0x465a5553,4);put(out.bytes,version,4);put(out.bytes,out.index,8);put(out.bytes,index_size,8);
  return out;
}
inline Bytes plain(const Record& r) {
  Bytes out;for (const auto& b:r.blocks)out.insert(out.end(),b.plain.begin(),b.plain.end());return out;
}
} // namespace flzip_fixture
