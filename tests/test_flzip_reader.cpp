#include "flitzfs/flzip_reader.hpp"
#include "flitzfs/catalog_error.hpp"
#include "flitzfs/projection.hpp"
#include "flzip_test_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace flzip_fixture;
using flitzfs::FlzipReader;
void require(bool ok,const char* why) {if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F&& f) {
  try { f(); }
  catch(const flitzfs::FlzipError&) {return;}
  catch(const flitzfs::CatalogError&) {return;}
  throw std::runtime_error("invalid archive/range accepted");
}
struct Temp {
  std::filesystem::path root;
  Temp() {
    root=std::filesystem::temp_directory_path()/("flitzfs-flzip-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(std::filesystem::create_directory(root),"new fixture directory required");
  }
  ~Temp(){std::error_code ignored; for(const auto* n:{"fixture.flzip","link.flzip"})std::filesystem::remove(root/n,ignored);std::filesystem::remove(root,ignored);}
};
void write(const std::filesystem::path& p,const Bytes& bytes) {
  std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));require(bool(f),"fixture write failed");
}
}
int main() {
 try {
  require(crc(0,text("123456789"))==0xcbf43926U,"independent CRC vector mismatch");
  Record mixed{"Data/Mixed.bin",false,{raw(text("123456789")),fill(4100,7),pattern(9001),raw(text("tail"))}};
  const auto fixture=archive({mixed,{"Data",true,{}},{"Empty",true,{}},{"zero.bin",false,{}}});
  const auto original=plain(mixed);
  auto r=FlzipReader::from_bytes(fixture.bytes);
  require(r.info().version==1 && r.info().files==2 && r.info().blocks==4,"archive summary");
  require(r.stat("data/mIXED.BIN").logical_bytes==original.size(),"case-insensitive stat");
  require(r.list("").size()==3 && r.list("EMPTY").empty(),"explicit/implicit directories");
  require(r.read("data/mixed.bin",0,static_cast<std::uint32_t>(original.size()))==original,"whole-file parity");
  for(std::uint64_t offset:{0ULL,7ULL,4107ULL,4109ULL,13108ULL}) {
    const auto n=static_cast<std::uint32_t>((std::min)(std::uint64_t{8},original.size()-offset));
    const Bytes expected(original.begin()+static_cast<std::ptrdiff_t>(offset),original.begin()+static_cast<std::ptrdiff_t>(offset+n));
    require(r.read("Data/Mixed.bin",offset,n)==expected,"cross-block random-read parity");
  }
  require(r.read("zero.bin",0,0).empty(),"empty file");
  require(r.read("data/mixed.bin",original.size(),0).empty(),"zero-byte exact EOF");
  rejects([&]{r.read("data/mixed.bin",original.size(),1);});
  rejects([&]{r.read("data/mixed.bin",(std::numeric_limits<std::uint64_t>::max)(),1);});
  rejects([&]{r.read("data/mixed.bin",0,flitzfs::kMaxSingleReadBytes+1);});
  rejects([&]{r.read("Data",0,0);});rejects([&]{r.list("zero.bin");});
  rejects([&]{r.stat("../zero.bin");});rejects([&]{r.stat("missing");});
  rejects([&]{FlzipReader::from_bytes(fixture.bytes,original.size()-1);});
  r.verify_all();
  // Source memory and source-file changes must not alter a verified snapshot.
  auto mutable_bytes=fixture.bytes;auto stable=FlzipReader::from_bytes(mutable_bytes);
  mutable_bytes[0]^=std::byte{1}; require(stable.read(mixed.path,0,9)==text("123456789"),"snapshot aliases caller bytes");
  Temp temp;const auto disk=temp.root/"fixture.flzip";write(disk,fixture.bytes);
  auto saved=FlzipReader::open(disk);write(disk,mutable_bytes);
  require(saved.read(mixed.path,0,9)==text("123456789"),"snapshot follows backing mutation");
#ifndef _WIN32
  std::filesystem::create_symlink(disk,temp.root/"link.flzip");
  bool denied=false;try{(void)FlzipReader::open(temp.root/"link.flzip");}catch(const std::exception&){denied=true;}
  require(denied,"final symlink accepted");
#endif
  write(disk,fixture.bytes);
  auto catalog=flitzfs::ReadOnlyCatalog::open_flzip(disk);
  require(catalog.stat(mixed.path).logical_bytes==original.size(),"catalog FLZIP stat");
  require(catalog.list("Empty").empty(),"catalog empty directory");
  rejects([&]{catalog.verify_all(original.size()-1);});
  catalog.verify_all();
  std::atomic<unsigned> failures{0};
  auto concurrent=[&]{try{for(unsigned i=0;i<8;++i) if(catalog.read(mixed.path,7,32)!=Bytes(original.begin()+7,original.begin()+39))++failures;}catch(...){++failures;}};
  std::thread a(concurrent),b(concurrent);a.join();b.join();require(failures==0,"catalog synchronized FLZIP reads");
  auto projection=flitzfs::ReadOnlyProjection::open_flzip(disk);
  require(projection.stat(L"\\Data\\Mixed.bin").logical_bytes==original.size(),"FLZIP projection stat");
  require(projection.read(L"\\Data\\Mixed.bin",original.size()-2,9)==Bytes(original.end()-2,original.end()),"FLZIP projection short read");
  require(projection.read(L"\\zero.bin",0,4).empty(),"FLZIP projection EOF");
  rejects([&]{projection.read(L"\\Data\\Mixed.bin",0,flitzfs::kMaxSingleReadBytes+1);});
  for(std::uint32_t v=1;v<=11;++v) {
    auto f=archive({mixed},v);auto reader=FlzipReader::from_bytes(f.bytes);
    require(reader.read(mixed.path,0,9)==text("123456789"),"version framing mismatch");
  }
  for(auto version:{0U,12U,0xffffffffU})rejects([&]{FlzipReader::from_bytes(archive({mixed},version).bytes);});
  for(const auto* name:{"../escape","/absolute","C:/drive","x:stream","NUL.txt","x./a","x\\b","x?y","x/y ","NUL .txt"})
    rejects([&]{FlzipReader::from_bytes(archive({{name,false,{}}}).bytes);});
  for(const auto& records:std::vector<std::vector<Record>>{
      {{"a",false,{}},{"A",false,{}}},{{"a",false,{}},{"a/b",false,{}}},
      {{"a/b",false,{}},{"a",false,{}}},{{"a",true,{}},{"A",true,{}}}})
    rejects([&]{FlzipReader::from_bytes(archive(records).bytes);});
  auto mutate=[&](std::size_t pos,std::uint64_t value,unsigned width=1) {
    auto bytes=fixture.bytes;replace(bytes,pos,value,width);rejects([&]{FlzipReader::from_bytes(bytes);});
  };
  const auto end=fixture.bytes.size(),record=fixture.record_fields[0],block=fixture.block_fields[0];
  mutate(end-24,0);mutate(end-20,2,4);mutate(end-16,~std::uint64_t{0},8);mutate(end-8,~std::uint64_t{0},8);
  mutate(fixture.index,0);mutate(fixture.index+8,0xffffffffU,4);
  mutate(record,2);mutate(record+1,flitzfs::kMaxFlzipLogicalBytes+1,8);
  mutate(record+9,fixture.index,8);mutate(record+17,~std::uint64_t{0},8);mutate(record+25,0,4);mutate(record+29,0xffffffffU,4);
  mutate(block,255);mutate(block+2,0,4);mutate(block+2,flitzfs::kMaxFlzipBlockBytes+1,4);
  mutate(block+6,~std::uint64_t{0},8);mutate(block+14,8,4);
  mutate(fixture.block_fields[1]+14,1,4);mutate(fixture.block_fields[2]+14,1,4);
  // Every proper truncation of a small fixture must fail, without quadratic large-fixture work.
  const auto tiny=archive({{"tiny.bin",false,{raw(text("123456789"))}}});
  for(std::size_t n=0;n<tiny.bytes.size();++n) rejects([&]{FlzipReader::from_bytes(std::span<const std::byte>(tiny.bytes).first(n));});
  for(std::size_t i=0;i<tiny.bytes.size();++i) {
    auto bytes=tiny.bytes;bytes[i]^=std::byte{0x80};
    try {auto changed=FlzipReader::from_bytes(bytes);changed.verify_all();}
    catch(const flitzfs::FlzipError&){}catch(const flitzfs::CatalogError&){}
  }
  // Missing optional codecs must never silently fall back to a different codec.
  for(std::uint8_t kind:{2,6,8}) {
    if(flitzfs::flzip_codec_available(kind)) continue;
    auto f=archive({mixed},11);replace(f.bytes,f.block_fields[0],kind,1);
    bool unsupported=false;try{(void)FlzipReader::from_bytes(f.bytes);}
    catch(const flitzfs::FlzipError& e){unsupported=e.code()==flitzfs::FlzipErrorCode::unsupported_codec;}
    require(unsupported,"unsupported codec not distinguished");
  }
  for(std::uint8_t codec:{2,8}) {
    if(!flitzfs::flzip_codec_available(codec)) {
      auto f=archive({mixed},11);replace(f.bytes,f.block_fields[0],codec,1);
      rejects([&]{FlzipReader::from_bytes(f.bytes);});continue;
    }
    const auto b=compressed(codec,pattern(8193,"CPU codec contract!").plain);
    auto f=archive({{"compressed.bin",false,{b}}},codec==8?6:1);
    auto reader=FlzipReader::from_bytes(f.bytes);
    require(reader.read("compressed.bin",0,static_cast<std::uint32_t>(b.plain.size()))==b.plain,"CPU codec round trip");
    auto check_bad=[&](Block broken) {auto malformed=archive({{"compressed.bin",false,{broken}}},codec==8?6:1);rejects([&]{FlzipReader::from_bytes(malformed.bytes);});};
    auto broken=b;broken.encoded.push_back(std::byte{0});check_bad(broken);
    broken=b;broken.encoded.insert(broken.encoded.end(),b.encoded.begin(),b.encoded.end());check_bad(broken);
    broken=b;broken.encoded.pop_back();check_bad(broken);
    broken=b;broken.plain.push_back(std::byte{0});check_bad(broken);
    broken=b;broken.encoded[0]^=std::byte{0x40};check_bad(broken);
    if(codec==8)rejects([&]{FlzipReader::from_bytes(archive({{"bad-version",false,{b}}},5).bytes);});
  }
  // Verification covers unrequested files, not just the first requested range.
  mutate(fixture.record_fields[3]+25,7,4);
  auto overlap=archive({{"a.bin",false,{raw(text("abc"))}},{"b.bin",false,{raw(text("abc"))}}});
  replace(overlap.bytes,overlap.record_fields[1]+9,0,8);
  rejects([&]{FlzipReader::from_bytes(overlap.bytes);});
  auto motif=pattern(4099,std::string(300,'p'));
  rejects([&]{FlzipReader::from_bytes(archive({{"x.bin",false,{motif}}},3).bytes);});
  (void)FlzipReader::from_bytes(archive({{"x.bin",false,{motif}}},4).bytes);
  auto excessive=archive({{"x.bin",false,std::vector<Block>(8193,fill(1,7))}});
  rejects([&]{FlzipReader::from_bytes(excessive.bytes);});
  auto extra_index=tiny.bytes;extra_index.insert(extra_index.end()-24,std::byte{0});
  replace(extra_index,extra_index.size()-8,tiny.bytes.size()-24-tiny.index+1,8);
  rejects([&]{FlzipReader::from_bytes(extra_index);});
#ifdef FLITZFS_TEST_HAS_ZSTD
  {
    auto encoded=compressed(8,pattern(8193).plain);
    struct Context {ZSTD_CCtx* value=ZSTD_createCCtx();~Context(){ZSTD_freeCCtx(value);}} context;
    require(context.value!=nullptr,"Zstandard test context allocation");
    require(!ZSTD_isError(ZSTD_CCtx_setParameter(context.value,ZSTD_c_contentSizeFlag,0)),"content-size parameter");
    encoded.encoded.resize(ZSTD_compressBound(encoded.plain.size()));
    const auto n=ZSTD_compress2(context.value,encoded.encoded.data(),encoded.encoded.size(),encoded.plain.data(),encoded.plain.size());
    require(!ZSTD_isError(n),"unknown-content-size frame generation");encoded.encoded.resize(n);
    rejects([&]{FlzipReader::from_bytes(archive({{"unknown.bin",false,{encoded}}},6).bytes);});
  }
#endif
  // More than 16 files or 256 records must be rejected before exposing metadata.
  std::vector<Record> records;for(unsigned i=0;i<17;++i)records.push_back({std::to_string(i)+".bin",false,{}});
  rejects([&]{FlzipReader::from_bytes(archive(records).bytes);});
  records.clear();for(unsigned i=0;i<257;++i)records.push_back({"dir"+std::to_string(i),true,{}});
  rejects([&]{FlzipReader::from_bytes(archive(records).bytes);});
  auto maxblock=fill(flitzfs::kMaxFlzipBlockBytes,19);
  auto maxreader=FlzipReader::from_bytes(archive({{"max.bin",false,{maxblock}}}).bytes);
  require(maxreader.read("max.bin",flitzfs::kMaxFlzipBlockBytes-1,1)==Bytes{std::byte{19}},"maximum block boundary");
  std::cout<<"Bounded FLZIP CPU snapshot, integrity, codec, namespace and random-read tests passed.\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
