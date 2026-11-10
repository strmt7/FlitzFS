// Hosted-only native object-store comparison. Timed backing bytes stay in RAM.
#include "flitzfs/object_store.hpp"
#ifdef FLITZFS_BOUNDED_DIGEST_COMPARISON
#include "hosted_digest_limits.hpp"
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
using Clock=std::chrono::steady_clock;
class Image final:public StoreByteIo {
 public:
  Bytes bytes;
  std::uint64_t extent() const override{return bytes.size();}
  StoreIoContract contract() const noexcept override{return {65536,true};}
  void admit_write(std::uint64_t n) override{if(n>kStoreMaxPhysicalBytes)throw std::runtime_error("image cap");}
  void flush() override{} // Immediately persistent abstract RAM; not physical I/O.
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    if(at>kStoreMaxPhysicalBytes || in.size()>kStoreMaxPhysicalBytes-at)throw std::runtime_error("write cap");
    if(at+in.size()>bytes.size())bytes.resize(static_cast<std::size_t>(at+in.size()));
    std::copy(in.begin(),in.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(at>bytes.size() || out.size()>bytes.size()-at)throw std::runtime_error("read cap");
    std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),out.size(),out.begin());
  }
};
std::vector<StoreMutation> corpus(std::string_view name) {
  constexpr std::size_t n=1024*1024;
  std::vector<StoreMutation> data;
  for(unsigned id=1;id<=4;++id) {
    Bytes b(n);std::uint64_t x=0xa4093822299f31d0ULL+id;
    for(std::size_t i=0;i<n;++i) {
      x^=x<<13;x^=x>>7;x^=x<<17;
      if(name=="zeros")b[i]=std::byte{0};
      else if(name=="uniform")b[i]=static_cast<std::byte>(31+id);
      else if(name=="motifs")b[i]=static_cast<std::byte>((i%251)*37+id);
      else if(name=="noise")b[i]=static_cast<std::byte>(x>>17);
      else if(name=="numeric") {
        // A repeated low-amplitude signal with slowly changing high bytes.
        const auto value=std::uint32_t((i/4+id*17)%4096);
        b[i]=static_cast<std::byte>(value>>(8*(i%4)));
      } else if(name=="text") {
        constexpr std::string_view s="native filesystem transaction object compression read write recovery generation inode extent\n";
        b[i]=i%4096<4000?static_cast<std::byte>(s[(i+id)%s.size()]):static_cast<std::byte>(x>>17);
      } else throw std::runtime_error("unknown bounded corpus");
    }
    data.push_back({id,false,std::move(b)});
  }
  return data;
}
StoreIdentity identity(){StoreIdentity id{};id[0]=std::byte{109};id[1]=std::byte{7};return id;}
double milliseconds(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
void hosted_only() {
  const char* a=std::getenv("GITHUB_ACTIONS"),*b=std::getenv("RUNNER_ENVIRONMENT");
  if(!a || !b || std::strcmp(a,"true") || std::strcmp(b,"github-hosted"))
    throw std::runtime_error("comparison requires a disposable GitHub-hosted VM");
}
void verify(const ObjectStoreSnapshot& s,const std::vector<StoreMutation>& data) {
  if(s.info().objects!=data.size())throw std::runtime_error("namespace parity");
  for(const auto& m:data)if(s.read(m.id)!=m.bytes)throw std::runtime_error("byte parity");
}
}
int main(int argc,char** argv) {
  try {
    hosted_only();
#ifdef FLITZFS_BOUNDED_DIGEST_COMPARISON
    hosted_digest_limits();
#endif
    if(argc!=4 && argc!=5)throw std::runtime_error("mode corpus action [image-path] required");
    const std::string mode(argv[1]),kind(argv[2]),action(argv[3]);
    auto data=corpus(kind);
    if(action=="verify-image") {
      if(argc!=5)throw std::runtime_error("image path required");
      auto reader=ObjectStoreSnapshot::open_file(argv[4]);verify(reader,data);
      std::cout<<"verified\n";return 0;
    }
    if(argc!=4 || (action!="measure" && action!="dump-image"))throw std::runtime_error("invalid comparison action");
    auto backing=std::make_shared<Image>();
    const auto begin=Clock::now();
#ifdef FLITZFS_CURRENT_STORE
    StoreCompressionOptions options;
    if(mode=="raw")options.mode=StoreCompressionMode::raw;
    else if(mode=="auto1")options.zstd_level=1;
    else if(mode=="auto6")options.zstd_level=6;
    else if(mode=="auto9")options.zstd_level=9;
    else throw std::runtime_error("unknown comparison policy");
    options.require_zstd=options.mode==StoreCompressionMode::automatic;
    auto writer=ObjectStoreWriter::create(backing,identity(),options);
#else
    if(mode!="baseline")throw std::runtime_error("baseline raw policy required");
    auto writer=ObjectStoreWriter::create(backing,identity());
#endif
    const auto initialized=Clock::now();
    const auto committed=writer.commit(data);
    const auto written=Clock::now();
    auto reopened=ObjectStoreSnapshot::open(backing);
    const auto recovered=Clock::now();
    verify(reopened,data);
    const auto read=Clock::now();
    if(action=="dump-image") {
#ifdef _WIN32
      if(_setmode(_fileno(stdout),_O_BINARY)==-1)throw std::runtime_error("binary image stdout mode");
#endif
      std::cout.write(reinterpret_cast<const char*>(backing->bytes.data()),static_cast<std::streamsize>(backing->bytes.size()));
      if(!std::cout)throw std::runtime_error("dump output");return 0;
    }
    std::cout<<"{\"logical_bytes\":"<<committed.logical_bytes
             <<",\"physical_bytes\":"<<committed.physical_end
#ifdef FLITZFS_CURRENT_STORE
             <<",\"active_payload_bytes\":"<<committed.stored_payload_bytes
             <<",\"zstd_runtime_version\":"<<store_zstd_version()
#endif
             <<",\"initialize_ms\":"<<milliseconds(initialized-begin)
             <<",\"write_ms\":"<<milliseconds(written-initialized)
             <<",\"reopen_verify_ms\":"<<milliseconds(recovered-written)
             <<",\"read_verify_ms\":"<<milliseconds(read-recovered)<<"}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
