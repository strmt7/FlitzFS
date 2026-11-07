// Test-only interoperability oracle, linked to the real pinned upstream writer.
#include "core/archive_index.hpp"
#include "flitzfs/flzip_reader.hpp"
#include "flzip_native_fixture.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace flzip_fixture;
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
void compare(const std::vector<Record>& records,std::uint32_t version) {
  const auto expected=archive(records,version);
  flitzzip::ArchiveIndex index;index.version=version;
  std::uint64_t archive_offset=0;
  for(const auto& record:records) {
    flitzzip::ArchiveEntry entry;entry.path=record.path;entry.directory=record.directory;
    entry.payload_offset=archive_offset;
    for(const auto& b:record.blocks) {
      entry.blocks.push_back({static_cast<flitzzip::BlockKind>(b.kind),b.fill,
          static_cast<std::uint32_t>(b.plain.size()),b.kind==1?0:entry.payload_size,
          static_cast<std::uint32_t>(b.encoded.size())});
      entry.crc32=crc(entry.crc32,b.plain);entry.uncompressed_size+=b.plain.size();entry.payload_size+=b.encoded.size();
    }
    archive_offset+=entry.payload_size;index.entries.push_back(std::move(entry));
  }
  std::ostringstream encoded(std::ios::binary);
  flitzzip::write_archive_index(encoded,index);
  const auto metadata=encoded.str();
  std::istringstream input(metadata,std::ios::binary);
  const auto decoded=flitzzip::read_archive_index(input);
  require(decoded.version==version && decoded.entries.size()==records.size(),"upstream index self-roundtrip");
  Bytes bytes(expected.bytes.begin(),expected.bytes.begin()+static_cast<std::ptrdiff_t>(expected.index));
  const auto meta=text(metadata);bytes.insert(bytes.end(),meta.begin(),meta.end());
  // Same upstream little-endian primitives used by the native archive footer writer.
  std::ostringstream footer(std::ios::binary);
  flitzzip::write_u32(footer,flitzzip::kFlitzZipFooterMagic);flitzzip::write_u32(footer,version);
  flitzzip::write_u64(footer,expected.index);flitzzip::write_u64(footer,metadata.size());
  const auto footer_bytes=text(footer.str());bytes.insert(bytes.end(),footer_bytes.begin(),footer_bytes.end());
  require(bytes==expected.bytes,"fixture writer disagrees with real FlitzZip serializer");
  auto reader=flitzfs::FlzipReader::from_bytes(bytes);
  for(const auto& record:records) {
    const auto entry=reader.stat(record.path);require(entry.directory==record.directory,"directory parity");
    if(!entry.directory) {
      const auto content=plain(record);
      require(reader.read(record.path,0,static_cast<std::uint32_t>(content.size()))==content,"FlitzZip serialized content mismatch");
    }
  }
}
}
int main() {
 try {
  for(std::uint32_t v=1;v<=11;++v) {
    compare({{"folder",true,{}},{"folder/raw.bin",false,{raw(text("123456789"))}},
        {"folder/fill.bin",false,{fill(4099,0xee)}},{"pattern.bin",false,{pattern(8197)}},
        {"empty.bin",false,{}}},v);
  }
  for(std::uint8_t codec:{2,8}) {
    require(flitzfs::flzip_codec_available(codec),"oracle lane requires both CPU codecs");
    const auto block=compressed(codec,pattern(9001,"reference writer and CPU codec").plain);
    compare({{"mixed.bin",false,{raw(text("prefix")),block,fill(37,19),pattern(4099),raw(text("suffix"))}}},codec==8?6:1);
  }
  for(const auto& block:flzip_fixture::native_samples()) {
    compare({{"native.bin",false,{block}}},flzip_fixture::minimum_version(block.kind));
  }
  std::cout<<"PASS: byte-exact pinned FlitzZip index/footer serializer parity; supported codec fixtures decode correctly.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
