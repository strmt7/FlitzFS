// Hosted-only CPU ordinary-file interruption probe. No mount, driver or GPU.
#include "flitzfs/object_store.hpp"
#include "flitzfs/read_only_file.hpp"
#include "store_file.hpp"
#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
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
  void event(const char* operation,std::uint64_t at=0,std::size_t bytes=0,std::uint64_t generation=0,
             std::string_view source_digest={},std::string_view state_digest={},bool compact=false) {
    require(++sequence_<=256,"event budget");
    std::cout<<"{\"event\":"<<sequence_<<",\"op\":\""<<operation<<"\",\"offset\":"<<at
             <<",\"bytes\":"<<bytes<<",\"generation\":"<<generation;
    if(!source_digest.empty())std::cout<<",\"source_digest\":\""<<source_digest<<"\"";
    if(!state_digest.empty())std::cout<<",\"state_digest\":\""<<state_digest<<"\"";
    if(compact)std::cout<<",\"root\":3,\"unit\":4096";
    std::cout<<"}\n"<<std::flush;
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

void replicated_repair_write(const fs::path& path,StoreCompressionOptions options) {
  auto backing=detail::new_store_file(path,{4096,true});
  auto trace=std::make_shared<Traced>(backing);
  {
    auto writer=ObjectStoreWriter::create(trace,id(),options);
    require(writer.commit_replicated(first()).generation==2,"replicated baseline generation");
  }
  std::array<std::byte,4096> locator{};backing->read_exact(0,locator);
  std::uint64_t catalog=0;
  for(unsigned i=0;i<8;++i)catalog|=std::uint64_t(std::to_integer<unsigned char>(locator[32+i]))<<(8*i);
  require(catalog>=2*kStoreCommitUnit && catalog%kStoreCommitUnit==0,"replica damage fixture bounds");
  // Lose only the owned image's selected catalog unit, after acknowledgement.
  // The peer catalog and both root locators remain intact.
  backing->write_exact(catalog,Bytes(65536));backing->flush();
  require(ObjectStoreSnapshot::open(trace).info().degraded_checkpoint,"catalog loss not reported");
  original_bytes(ObjectStoreSnapshot::open(trace),false);
  trace->event("ack",0,0,2);trace->armed=true;
  auto writer=ObjectStoreWriter::resume(trace,id(),2,options);
  trace->armed=false;
  require(writer.snapshot().info().generation==4 && !writer.snapshot().info().degraded_checkpoint,"replicated repair incomplete");
  original_bytes(writer.snapshot(),false);trace->event("ack",0,0,4);
  std::cout<<"{\"done\":true}\n"<<std::flush;
}

void native_file_write(const fs::path& path,StoreCompressionOptions options) {
  auto backing=detail::new_store_file(path,{4096,true});
  auto trace=std::make_shared<Traced>(backing);
  auto files=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(trace,id(),options));
  require(files.apply(native_fixture::baseline()).storage.generation==4,"native file baseline generation");
  native_fixture::expected(files.snapshot(),false);
  const auto stable=native_fixture::stable_record(ObjectStoreSnapshot::open(trace));
  require(stable.generation==3,"native reused fixture baseline generation");
  trace->event("ack",0,0,4);
  trace->armed=true;
  require(files.apply(native_fixture::update()).storage.generation==6,"native file update generation");
  trace->armed=false;native_fixture::expected(files.snapshot(),true);
  const auto reused=native_fixture::stable_record(ObjectStoreSnapshot::open(trace));
  require(reused.id==stable.id && reused.generation==stable.generation,"native process fixture rewrote reverted payload");
  trace->event("ack",0,0,6);
  std::cout<<"{\"done\":true}\n"<<std::flush;
}
void native_file_recover(const fs::path& path,std::uint64_t floor,StoreCompressionOptions options) {
  std::uint64_t generation=0,continued=0,sequence=0;bool changed=false;
  {
    auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
    generation=snapshot.storage_info().generation;
    require(generation>=floor && generation<=6 && snapshot.sequence()==(generation>4?3U:2U),"native file acknowledgement sequence");
    changed=snapshot.sequence()==3;native_fixture::expected(snapshot,changed);
    require(native_fixture::stable_record(ObjectStoreSnapshot::open_file(path)).generation==3,
        "native interrupted transaction replaced stable payload");
  }
  {
    auto backing=detail::reopen_store_file_for_hosted_test(path,{4096,true});
    auto files=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(backing,id(),generation,options));
    native_fixture::expected(files.snapshot(),changed);
    const std::array<NativeFileMutation,2> more{{native_fixture::file_create("continued"),native_fixture::file_write("continued",0,native_fixture::data(3073,91))}};
    const auto committed=files.apply(more);continued=committed.storage.generation;sequence=committed.sequence;
    native_fixture::expected(files.snapshot(),changed,true);
  }
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
  require(reopened.storage_info().generation==continued && reopened.sequence()==sequence,"native file continuation did not persist");
  native_fixture::expected(reopened,changed,true);
  require(native_fixture::stable_record(ObjectStoreSnapshot::open_file(path)).generation==3,
      "native recovery continuation replaced stable payload");
  std::cout<<"{\"recovered\":"<<generation<<",\"continued\":"<<continued<<"}\n"<<std::flush;
}


bool basic_phase(std::string_view phase) {
  if(phase.starts_with("dense-"))phase.remove_prefix(6);
  return phase=="basic-upgrade" || phase=="basic-update" || phase=="basic-compound";
}
NativeBasicMetadata basic_tuple(unsigned version,bool directory=false) {
  const std::uint64_t base=133700000000000000ULL+100*version;
  return {base+1,base+2,base+3,base+4,directory?0x10U:0x2022U};
}
std::string basic_state_digest(ObjectStoreSnapshot objects) {
  objects.verify_all();
  Bytes bytes;bytes.reserve(4*kNativeExtentBytes+64);
  const auto append=[&](std::uint64_t n) {
    for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<std::byte>(n>>(8*i)));
  };
  for(const auto& object:objects.list()) {
    auto data=objects.read(object.id);
    require(bytes.size()<=512*1024 && data.size()+16<=512*1024-bytes.size(),
        "basic state closure digest budget");
    append(object.id);append(data.size());bytes.insert(bytes.end(),data.begin(),data.end());
  }
  const auto digest=detail::record_digest(bytes);constexpr char digits[]="0123456789abcdef";
  std::string out;out.reserve(64);
  for(auto byte:digest){const auto n=std::to_integer<unsigned char>(byte);out+=digits[n>>4];out+=digits[n&15];}
  return out;
}
void basic_expected(const NativeFilesystemSnapshot& view,std::string_view phase,bool changed,bool continued=false) {
  require(!view.needs_session_recovery(),"basic phase unexpectedly persisted lifetime intent");
  const bool upgrade=phase=="basic-upgrade",compound=phase=="basic-compound";
  const auto name=changed && compound?"renamed":"f";
  require(view.stat(name).inode==2 && view.stat("keep").inode==3,"basic inode/namespace identity");
  auto expected=native_fixture::data(73,31);
  if(changed && compound) {
    const auto patch=native_fixture::data(19,33);std::copy(patch.begin(),patch.end(),expected.begin()+7);
  }
  auto keep=native_fixture::data(91,32);
  if(continued){const auto patch=native_fixture::data(7,35);std::copy(patch.begin(),patch.end(),keep.begin()+3);}
  require(view.read_inode(2,0,100)==expected && view.read("keep",0,100)==keep,
      "basic acknowledged data changed/mixed");
  require(changed?view.basic_metadata(2)==basic_tuple(2):
      (upgrade?!view.basic_metadata(2):view.basic_metadata(2)==basic_tuple(1)),
      "basic recovered tuple mixed with data/namespace");
  require(view.basic_metadata(1)==(upgrade?std::optional<NativeBasicMetadata>{}:
      std::optional<NativeBasicMetadata>{basic_tuple(1,true)}),"basic acknowledged root tuple lost");
  require(view.basic_metadata(3)==(continued?std::optional<NativeBasicMetadata>{basic_tuple(9)}:
      (upgrade?std::optional<NativeBasicMetadata>{}:std::optional<NativeBasicMetadata>{basic_tuple(1)})),
      "basic acknowledged keep tuple lost");
  require(view.list().size()==(changed && compound?3U:2U),"partial stamped creation exposed");
  if(changed && compound)
    require(view.stat("new").inode==4 && view.read("new",0,100)==native_fixture::data(31,34) &&
        view.basic_metadata(4)==basic_tuple(3),"new file/data/basic tuple not atomic");
  require(view.sequence()==(changed?3U:2U)+(continued?1U:0U),"basic recovered namespace sequence");
}
// Inspect the exact root selected by an independently admitted snapshot. The
// library has already verified its full object graph; bind the stored geometry
// as well, rather than labelling a fixed-layout run "compact".
void compact_root_bytes(std::span<const std::byte,4096> root,std::uint64_t generation) {
  const auto number=[&](std::size_t at,unsigned count) {
    std::uint64_t value=0;
    for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(root[at+i]))<<(8*i);
    return value;
  };
  const auto digest=detail::record_digest(std::span<const std::byte>(root).first(4064));
  require(std::equal(digest.begin(),digest.end(),root.begin()+4064) &&
      number(16,8)==generation && number(8,4)==3 && number(112,4)==4096,
      "compact process campaign did not admit the selected root-3/4096 geometry");
}
void compact_root(const StoreByteIo& backing,std::uint64_t generation) {
  std::array<std::byte,4096> root{};
  backing.read_exact((generation%2)*kStoreCommitUnit,root);
  compact_root_bytes(root,generation);
}
void compact_root(const fs::path& path,std::uint64_t generation) {
  auto file=ReadOnlyFile::open(path);std::array<std::byte,4096> root{};
  file.read_exact_at((generation%2)*kStoreCommitUnit,root.data(),root.size());
  compact_root_bytes(root,generation);
}
void basic_write(const fs::path& path,std::string_view phase,StoreCompressionOptions options,bool compact=false) {
  auto backing=detail::new_store_file(path,{4096,true});auto trace=std::make_shared<Traced>(backing);
  auto files=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(trace,id(),options,StoreCheckpointOptions{compact}));
  const std::array<NativeFileMutation,4> initial{{native_fixture::file_create("f"),
      native_fixture::file_write("f",0,native_fixture::data(73,31)),native_fixture::file_create("keep"),
      native_fixture::file_write("keep",0,native_fixture::data(91,32))}};
  const std::array<NativeBasicMutation,3> initial_basic{{{1,0,basic_tuple(1,true)},
      {2,0,basic_tuple(1)},{3,0,basic_tuple(1)}}};
  const auto before=phase=="basic-upgrade"?files.apply(initial):files.apply_with_basic(initial,initial_basic);
  require(before.storage.generation==4,"basic baseline generation");
  basic_expected(files.snapshot(),phase,false);
  if(compact)compact_root(*backing,4);
  trace->event("ack",0,0,4,{},basic_state_digest(ObjectStoreSnapshot::open(trace)),compact);trace->armed=true;
  NativeFsCommit changed;
  if(phase=="basic-compound") {
    const std::array<NativeFileMutation,4> operations{{native_fixture::file_write("f",7,native_fixture::data(19,33)),
        native_fixture::file_rename("f","renamed"),native_fixture::file_create("new"),
        native_fixture::file_write("new",0,native_fixture::data(31,34))}};
    const std::array<NativeBasicMutation,2> metadata{{{2,0,basic_tuple(2)},{0,1,basic_tuple(3)}}};
    changed=files.apply_with_basic(operations,metadata);
  } else changed=files.set_basic_metadata(2,basic_tuple(2));
  trace->armed=false;require(changed.storage.generation==6,"basic candidate generation");
  basic_expected(files.snapshot(),phase,true);
  if(compact)compact_root(*backing,6);
  trace->event("ack",0,0,6,{},basic_state_digest(ObjectStoreSnapshot::open(trace)),compact);
  std::cout<<"{\"done\":true}\n"<<std::flush;
}
void basic_recover(const fs::path& path,std::string_view phase,std::uint64_t floor,StoreCompressionOptions options,bool compact=false) {
  std::uint64_t generation=0,continued=0;bool changed=false;
  {
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
    generation=view.storage_info().generation;
    require(generation>=floor && generation<=6 && view.sequence()==(generation>4?3U:2U),
        "basic acknowledged generation/sequence lost");
    changed=view.sequence()==3;basic_expected(view,phase,changed);
  }
  if(compact)compact_root(path,generation);
  const auto recovered_digest=basic_state_digest(ObjectStoreSnapshot::open_file(path));
  {
    auto backing=detail::reopen_store_file_for_hosted_test(path,{4096,true});
    auto files=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(backing,id(),generation,options));
    basic_expected(files.snapshot(),phase,changed);
    const auto operation=native_fixture::file_write("keep",3,native_fixture::data(7,35));
    const NativeBasicMutation metadata{3,0,basic_tuple(9)};
    const auto result=files.apply_with_basic({&operation,1},{&metadata,1});
    continued=result.storage.generation;
  }
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
  require(view.storage_info().generation==continued && continued>generation,"basic continuation publication");
  basic_expected(view,phase,changed,true);
  if(compact)compact_root(path,continued);
  const auto continuation_digest=basic_state_digest(ObjectStoreSnapshot::open_file(path));
  require(continuation_digest!=recovered_digest,"basic continuation did not change verified closure");
  std::cout<<"{\"recovered\":"<<generation<<",\"continued\":"<<continued
      <<",\"state_digest\":\""<<recovered_digest<<"\",\"continued_digest\":\""<<continuation_digest<<"\"";
  if(compact)std::cout<<",\"root\":3,\"unit\":4096";
  std::cout<<"}\n"<<std::flush;
}

StoreIdentity copy_identity(bool retry=false){auto value=id();value[2]=retry?std::byte{22}:std::byte{21};return value;}
std::string file_digest(const fs::path& path) {
  auto file=ReadOnlyFile::open(path);require(file.size()<=2*1024*1024,"source digest bound");
  Bytes bytes(static_cast<std::size_t>(file.size()));file.read_exact_at(0,bytes.data(),bytes.size());
  const auto digest=detail::record_digest(bytes);constexpr char hex[]="0123456789abcdef";
  std::string out;out.reserve(64);
  for(auto byte:digest){const auto n=std::to_integer<unsigned char>(byte);out+=hex[n>>4];out+=hex[n&15];}
  return out;
}
bool has_root(const fs::path& path) {
  auto file=ReadOnlyFile::open(path);
  for(unsigned slot=0;slot<2;++slot) {
    const auto at=slot*kStoreCommitUnit;
    if(at>file.size() || file.size()-at<4096)continue;
    std::array<std::byte,4096> bytes{};file.read_exact_at(at,bytes.data(),bytes.size());
    const auto digest=detail::record_digest(std::span<const std::byte>(bytes).first(4064));
    if(std::equal(digest.begin(),digest.end(),bytes.begin()+4064))return true;
  }
  return false;
}
void native_compact_write(const fs::path& root,StoreCompressionOptions options) {
  const auto path=root/"image.fltx";
  {
    auto files=NativeFilesystemWriter::initialize(ObjectStoreWriter::create_file(path,id(),{4096,true},options));
    files.apply(native_fixture::baseline());files.apply(native_fixture::update());
    native_fixture::expected(files.snapshot(),true);
  }
  auto source=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
  const auto digest=file_digest(path);
  auto trace=std::make_shared<Traced>(detail::new_store_file(root/"copy.fltx",{4096,true}));
  trace->event("ack",0,0,6,digest);trace->armed=true;
  auto copy=NativeFilesystemWriter::compact(source,trace,copy_identity(),options);
  trace->armed=false;
  require(copy.snapshot().storage_info().generation==8 && copy.snapshot().sequence()==3,"compacted publication generation");
  native_fixture::expected(copy.snapshot(),true);
  require(file_digest(path)==digest,"compaction changed source bytes");
  trace->event("ack",0,0,8,digest);
  std::cout<<"{\"done\":true}\n"<<std::flush;
}
void native_compact_recover(const fs::path& root,std::uint64_t floor,
    const std::string& expected_digest,StoreCompressionOptions options) {
  const auto path=root/"image.fltx",copied=root/"copy.fltx",retry=root/"retry.fltx";
  require(file_digest(path)==expected_digest,"acknowledged source bytes changed during compaction");
  auto source=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(path));
  require(source.identity()==id() && source.storage_info().generation==6 && source.sequence()==3,"compaction source baseline");
  native_fixture::expected(source,true);
  const bool published=has_root(copied);
  require(published || floor==6,"acknowledged compacted destination lost");
  std::uint64_t recovered=6,continued=0;StoreIdentity target_id=copy_identity(!published);
  const auto target=published?copied:retry;
  const std::array<NativeFileMutation,2> more{{native_fixture::file_create("continued"),native_fixture::file_write("continued",0,native_fixture::data(3073,91))}};
  if(published) {
    {
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(copied));
      recovered=snapshot.storage_info().generation;
      require(snapshot.identity()==target_id && snapshot.sequence()==3 && recovered>=7 && recovered<=8 && recovered>=floor,
              "partial compacted destination");
      native_fixture::expected(snapshot,true);
    }
    auto files=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(
        detail::reopen_store_file_for_hosted_test(copied,{4096,true}),target_id,recovered,options));
    continued=files.apply(more).storage.generation;native_fixture::expected(files.snapshot(),true,true);
  } else {
    // Never truncate/reuse the partial destination. Start a separately owned
    // NEW path/identity from the still-byte-exact acknowledged source.
    auto files=NativeFilesystemWriter::compact(source,detail::new_store_file(retry,{4096,true}),target_id,options);
    continued=files.apply(more).storage.generation;native_fixture::expected(files.snapshot(),true,true);
  }
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(target));
  require(reopened.identity()==target_id && reopened.storage_info().generation==continued && reopened.sequence()==4,
          "compacted continuation did not persist");
  native_fixture::expected(reopened,true,true);
  require(file_digest(path)==expected_digest,"recovery modified compaction source");
  std::cout<<"{\"recovered\":"<<recovered<<",\"continued\":"<<continued<<"}\n"<<std::flush;
}

void recover_probe(const fs::path& path,bool repair,std::uint64_t floor,StoreCompressionOptions options,bool replicated=false) {
  std::uint64_t generation=0,continued=0;bool changed=false;
  {
    auto snapshot=ObjectStoreSnapshot::open_file(path);generation=snapshot.info().generation;
    require(generation>=floor && generation<=(replicated?4U:2U),"acknowledged generation lost or future generation fabricated");
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
    require(argc==7 || argc==8,"probe role, parent, token, policy, phase, floor and optional source digest required");
    const std::string role(argv[1]),token(argv[3]),policy(argv[4]),phase(argv[5]),floor_text(argv[6]);
    require((role=="write"||role=="recover") && (policy=="raw"||policy=="auto") &&
            (phase=="mutation"||phase=="repair"||phase=="files"||phase=="compact"||phase=="checkpoint"||basic_phase(phase)),"invalid probe scope");
    require(argc==((phase=="compact" && role=="recover")?8:7),"invalid source digest argument scope");
    const std::string digest=argc==8?argv[7]:"";
    if(argc==8)require(digest.size()==64 && std::all_of(digest.begin(),digest.end(),
        [](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}),"invalid source digest");
    std::uint64_t floor=0;const auto parsed=std::from_chars(floor_text.data(),floor_text.data()+floor_text.size(),floor);
    require(parsed.ec==std::errc{} && parsed.ptr==floor_text.data()+floor_text.size() &&
            (role=="write"?floor==0:(phase=="compact"?(floor==6||floor==8):(phase=="files"||basic_phase(phase)?(floor==4||floor==6):(phase=="checkpoint"?(floor==2||floor==4):(floor>=1&&floor<=2))))),"invalid acknowledgement floor");
    // Controller sees resource admission before allowing filesystem/corpus work.
    std::cout<<"{\"ready\":true}\n"<<std::flush;advance();
    const fs::path root(argv[2]);owned_parent(root,token);
    StoreCompressionOptions options;options.mode=policy=="raw"?StoreCompressionMode::raw:StoreCompressionMode::automatic;
    options.require_zstd=policy=="auto";
    if(basic_phase(phase)) {
      const bool compact=phase.starts_with("dense-");
      const auto kind=compact?phase.substr(6):phase;
      if(role=="write")basic_write(root/"image.fltx",kind,options,compact);
      else basic_recover(root/"image.fltx",kind,floor,options,compact);
    } else if(phase=="checkpoint") {
      if(role=="write")replicated_repair_write(root/"image.fltx",options);
      else recover_probe(root/"image.fltx",true,floor,options,true);
    } else if(phase=="compact") {
      if(role=="write")native_compact_write(root,options);
      else native_compact_recover(root,floor,digest,options);
    } else if(phase=="files") {
      if(role=="write")native_file_write(root/"image.fltx",options);
      else native_file_recover(root/"image.fltx",floor,options);
    } else if(role=="write")write_probe(root/"image.fltx",phase=="repair",options);
    else recover_probe(root/"image.fltx",phase=="repair",floor,options);
    return 0;
  }catch(const std::exception& e){std::cerr<<"probe error: "<<e.what()<<'\n';return 1;}
}
