// Hosted-only CPU ordinary-file interruption probe. No mount, driver or GPU.
#include "flitzfs/object_store.hpp"
#include "flitzfs/read_only_file.hpp"
#include "store_file.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#endif
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
namespace fs=std::filesystem;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void hosted() {
  const char* a=std::getenv("GITHUB_ACTIONS"),*b=std::getenv("RUNNER_ENVIRONMENT");
  require(a && b && std::string(a)=="true" && std::string(b)=="github-hosted","disposable hosted VM required");
}
void contain() {
#ifdef _WIN32
  // Keep this handle until process exit. Closing an assigned kill-on-close
  // job in a normal stack destructor would terminate the process prematurely.
  const auto job=CreateJobObjectW(nullptr,nullptr);require(job!=nullptr,"job creation");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY|JOB_OBJECT_LIMIT_JOB_MEMORY;
  limits.BasicLimitInformation.ActiveProcessLimit=1;
  limits.ProcessMemoryLimit=768ULL*1024*1024;limits.JobMemoryLimit=768ULL*1024*1024;
  if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
    CloseHandle(job);throw std::runtime_error("job memory/process limits");
  }
  JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
  cpu.ControlFlags=JOB_OBJECT_CPU_RATE_CONTROL_ENABLE|JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;cpu.CpuRate=2000;
  if(!SetInformationJobObject(job,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu)) ||
      !AssignProcessToJobObject(job,GetCurrentProcess())) {
    CloseHandle(job);throw std::runtime_error("job CPU/assignment limits");
  }
#else
  const rlimit memory{512ULL*1024*1024,512ULL*1024*1024},cpu{15,15},core{0,0},file{2ULL*1024*1024,2ULL*1024*1024};
  require(::setrlimit(RLIMIT_AS,&memory)==0 && ::setrlimit(RLIMIT_CPU,&cpu)==0 &&
          ::setrlimit(RLIMIT_CORE,&core)==0 && ::setrlimit(RLIMIT_FSIZE,&file)==0,"process resource limits");
#endif
}
void advance(){char command=0;require(bool(std::cin.get(command)) && command=='c',"missing controller continuation");}
void owned_parent(const fs::path& root,const std::string& token) {
  require(root.is_absolute() && root.filename().string().rfind("flitzfs-store-crash-",0)==0 &&
          fs::is_directory(fs::symlink_status(root)),"not an owned probe directory");
  require(token.size()==32 && std::all_of(token.begin(),token.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid ownership token");
  auto marker=ReadOnlyFile::open(root/".owner");require(marker.size()==32,"owner marker bound");
  std::array<char,32> data{};marker.read_exact_at(0,data.data(),data.size());
  require(std::string(data.data(),data.size())==token,"owner marker mismatch");
}
StoreIdentity id(){StoreIdentity identity{};identity[0]=std::byte{127};identity[1]=std::byte{83};return identity;}
Bytes data(std::size_t n,unsigned seed,bool structured=false) {
  Bytes out(n);std::uint64_t x=0x082efa98ec4e6c89ULL+seed;
  for(std::size_t i=0;i<n;++i){x^=x<<13;x^=x>>7;x^=x<<17;out[i]=static_cast<std::byte>(x>>21);}
  if(structured)for(std::size_t i=16384;i<n;++i)out[i]=out[i%16384];
  return out;
}
std::array<StoreMutation,2> first(){return {StoreMutation{1,false,Bytes(8193)},StoreMutation{2,false,data(131073,17,true)}};}
std::array<StoreMutation,3> next(){return {StoreMutation{1,false,Bytes(65537,std::byte{71})},StoreMutation{2,true,{}},StoreMutation{3,false,data(131073,23,true)}};}
void original_bytes(const ObjectStoreSnapshot& snapshot,bool changed,bool continued=false) {
  require(snapshot.identity()==id(),"image identity changed");
  require(snapshot.list().size()==(continued?3U:2U),"partial transaction namespace");
  require(snapshot.read(1)==(changed?Bytes(65537,std::byte{71}):Bytes(8193)),"object one mismatch");
  require(snapshot.read(changed?3:2)==data(131073,changed?23:17,true),"other object mismatch");
  bool missing=false;try{(void)snapshot.read(changed?2:3);}catch(const std::exception&){missing=true;}
  require(missing,"deleted/new object escaped atomic namespace");
  if(continued)require(snapshot.read(99)==data(3073,91),"continuation payload mismatch");
}
class Traced final:public StoreByteIo {
  std::shared_ptr<StoreByteIo> inner_;
  unsigned sequence_=0;
 public:
  bool armed=false;
  explicit Traced(std::shared_ptr<StoreByteIo> inner):inner_(std::move(inner)){}
  void event(const char* operation,std::uint64_t at=0,std::size_t bytes=0,std::uint64_t generation=0) {
    require(++sequence_<=128,"event budget");
    std::cout<<"{\"event\":"<<sequence_<<",\"op\":\""<<operation<<"\",\"offset\":"<<at
             <<",\"bytes\":"<<bytes<<",\"generation\":"<<generation<<"}\n"<<std::flush;
    require(bool(std::cout),"event output");advance();
  }
  std::uint64_t extent() const override{return inner_->extent();}
  StoreIoContract contract() const noexcept override{return inner_->contract();}
  void admit_write(std::uint64_t n) override{inner_->admit_write(n);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{inner_->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    if(armed)event("write-before",at,in.size());
    inner_->write_exact(at,in);
    if(armed)event("write-after",at,in.size());
  }
  void flush() override {
    if(armed)event("flush-before");
    inner_->flush();
    if(armed)event("flush-after");
  }
};
void write_probe(const fs::path& path,bool repair,StoreCompressionOptions options) {
  auto backing=detail::new_store_file(path,{4096,true});
  auto trace=std::make_shared<Traced>(backing);
  if(repair) {
    {auto writer=ObjectStoreWriter::create(trace,id(),options);writer.commit(first());}
    // Seed only the owned image's obsolete root zone. The acknowledged root in
    // the other failure unit and all immutable data remain intact.
    backing->write_exact(0,Bytes(65536));backing->flush();
    trace->event("ack",0,0,1);trace->armed=true;
    auto writer=ObjectStoreWriter::resume(trace,id(),1,options);
    trace->armed=false;
    require(writer.snapshot().info().generation==2,"repair generation");
    original_bytes(writer.snapshot(),false);
    trace->event("ack",0,0,2);
  } else {
    auto writer=ObjectStoreWriter::create(trace,id(),options);writer.commit(first());
    trace->event("ack",0,0,1);trace->armed=true;
    require(writer.commit(next()).generation==2,"mutation generation");
    trace->armed=false;original_bytes(writer.snapshot(),true);trace->event("ack",0,0,2);
  }
  std::cout<<"{\"done\":true}\n"<<std::flush;
}
void recover_probe(const fs::path& path,bool repair,std::uint64_t floor,StoreCompressionOptions options) {
  std::uint64_t generation=0,continued=0;bool changed=false;
  {
    auto snapshot=ObjectStoreSnapshot::open_file(path);generation=snapshot.info().generation;
    require(generation>=floor && generation<=2,"acknowledged generation lost or future generation fabricated");
    changed=!repair && generation==2;original_bytes(snapshot,changed);
  }
  {
    auto backing=detail::reopen_store_file_for_hosted_test(path,{4096,true});
    auto writer=ObjectStoreWriter::resume(backing,id(),generation,options);
    original_bytes(writer.snapshot(),changed);
    const StoreMutation added{99,false,data(3073,91)};
    continued=writer.commit({&added,1}).generation;
    original_bytes(writer.snapshot(),changed,true);
  }
  auto independently_reopened=ObjectStoreSnapshot::open_file(path);
  require(independently_reopened.info().generation==continued,"continued generation did not persist");
  original_bytes(independently_reopened,changed,true);
  std::cout<<"{\"recovered\":"<<generation<<",\"continued\":"<<continued<<"}\n"<<std::flush;
}
}
int main(int argc,char** argv) {
  try {
    hosted();contain();
    require(argc==7,"probe role, parent, token, policy, phase, floor required");
    const std::string role(argv[1]),token(argv[3]),policy(argv[4]),phase(argv[5]),floor_text(argv[6]);
    require((role=="write"||role=="recover") && (policy=="raw"||policy=="auto") &&
            (phase=="mutation"||phase=="repair"),"invalid probe scope");
    std::uint64_t floor=0;const auto parsed=std::from_chars(floor_text.data(),floor_text.data()+floor_text.size(),floor);
    require(parsed.ec==std::errc{} && parsed.ptr==floor_text.data()+floor_text.size() &&
            (role=="write"?floor==0:(floor>=1&&floor<=2)),"invalid acknowledgement floor");
    // Controller sees resource admission before allowing filesystem/corpus work.
    std::cout<<"{\"ready\":true}\n"<<std::flush;advance();
    const fs::path root(argv[2]);owned_parent(root,token);
    StoreCompressionOptions options;options.mode=policy=="raw"?StoreCompressionMode::raw:StoreCompressionMode::automatic;
    options.require_zstd=policy=="auto";
    if(role=="write")write_probe(root/"image.fltx",phase=="repair",options);
    else recover_probe(root/"image.fltx",phase=="repair",floor,options);
    return 0;
  }catch(const std::exception& e){std::cerr<<"probe error: "<<e.what()<<'\n';return 1;}
}
