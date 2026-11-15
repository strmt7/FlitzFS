#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/paged_store.hpp"
#include <iostream>
#include <optional>
#include <new>

namespace {
using namespace flitzfs;
using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> void rejects(F&& f,const char* why) {
  ++checks;try{f();}catch(const NativeFsError&){return;}throw std::runtime_error(why);
}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram) {
  return NativeFilesystemWriter::initialize(PagedObjectStoreWriter::create(
      ram,identity(),limits,{StoreCompressionMode::raw,1,false}));
}
NativeFilesystemSnapshot view(const std::shared_ptr<Ram>& ram) {
  return NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,limits));
}
NativeFilesystemWriter resume(const std::shared_ptr<Ram>& ram,bool closed=false) {
  const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
  auto objects=PagedObjectStoreWriter::resume(ram,identity(),generation,limits,{StoreCompressionMode::raw,1,false});
  return closed?NativeFilesystemWriter::resume_closed_session(std::move(objects)):
      NativeFilesystemWriter::resume(std::move(objects));
}
void equivalent(const NativeFilesystemSnapshot& a,const NativeFilesystemSnapshot& b,std::string_view dir="") {
  require(a.sequence()==b.sequence() && a.identity()==b.identity(),"native sequence/identity parity");
  require(a.workload_info()==b.workload_info(),"workload parity");
  const auto left=a.list(dir),right=b.list(dir);
  require(left.size()==right.size(),"directory size parity");
  for(std::size_t i=0;i<left.size();++i) {
    const auto& x=left[i];const auto& y=right[i];
    require(x.inode==y.inode && x.parent==y.parent && x.path==y.path &&
        x.logical_bytes==y.logical_bytes && x.directory==y.directory &&
        x.created_sequence==y.created_sequence && x.modified_sequence==y.modified_sequence,"inode parity");
    require(a.basic_metadata(x.inode)==b.basic_metadata(y.inode),"basic metadata parity");
    require(a.security_descriptor(x.inode)==b.security_descriptor(y.inode),"security metadata parity");
    require(a.allocation_info(x.inode).reserved_bytes==b.allocation_info(y.inode).reserved_bytes,"allocation parity");
    if(x.directory)equivalent(a,b,x.path);
    else require(a.read_inode(x.inode,0,kNativeMaxReadBytes)==b.read_inode(y.inode,0,kNativeMaxReadBytes),"file bytes parity");
  }
}
void shared_engine() {
  auto a=std::make_shared<Ram>(),b=std::make_shared<Ram>();a->failure_unit=b->failure_unit=4096;
  auto flat=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(a,identity(),{StoreCompressionMode::raw,1,false}));
  auto paged=fresh(b);
  auto both=[&](std::vector<NativeFileMutation> batch) {
    const auto x=flat.apply(batch),y=paged.apply(batch);
    require(x.sequence==y.sequence && x.created_inodes==y.created_inodes,"mutation parity");
    equivalent(flat.snapshot(),paged.snapshot());
  };
  flat.enable_workload_accounting();paged.enable_workload_accounting();
  both({file_create("D",true)});
  for(unsigned i=0;i<8;++i)both({file_create("D/f"+std::to_string(i)),file_write("D/f"+std::to_string(i),0,data(4097+i,i))});
  auto before=paged.snapshot();const auto before_bytes=before.read("D/f0",0,100000);
  std::uint64_t random=0x894abcfe123ULL;
  for(unsigned i=0;i<96;++i) {
    random^=random<<13;random^=random>>7;random^=random<<17;
    const auto path="D/f"+std::to_string(random%8);
    if(i%3==0)both({file_truncate(path,random%131073)});
    else both({file_write(path,random%98304,data(1+random%2049,i))});
  }
  both({file_rename("D/f0","D/Renamed"),file_create("D/f0"),file_write("D/f0",0,data(19,73))});
  require(before.read("D/f0",0,100000)==before_bytes,"paged old snapshot changed");
  NativeSecurityDescriptor sd(20);sd[0]=std::byte{1};sd[3]=std::byte{128};
  const NativeBasicMetadata basic{1,2,3,4,32};
  const auto x=flat.create_inode_result("secured",false,basic,sd);
  const auto y=paged.create_inode_result("secured",false,basic,sd);
  require(x.info.inode==y.info.inode,"secured creation identity");
  flat.reserve_inode_result(x.info.inode,65536);paged.reserve_inode_result(y.info.inode,65536);
  equivalent(flat.snapshot(),paged.snapshot());
  const auto writes=b->writes,flushes=b->flushes;const auto generation=paged.snapshot().storage_info().generation;
  paged.flush();require(b->writes==writes && b->flushes==flushes+1 &&
      paged.snapshot().storage_info().generation==generation,"paged native Flush published/wrote or missed barrier");
  auto target=std::make_shared<Ram>();auto new_id=identity();new_id[1]=std::byte{77};
  rejects([&]{(void)NativeFilesystemWriter::compact(paged.snapshot(),target,new_id);},"flat compactor accepted paged source");
  require(target->extent()==0 && !paged.poisoned(),"unsupported compaction touched target/source");
  view(b).verify_all();
}
void sharing_and_sessions() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  {
    auto writer=fresh(ram);const auto bytes=data(kNativeExtentBytes,31);
    writer.apply(std::array{file_create("a"),file_write("a",0,bytes),file_create("b"),file_write("b",0,bytes)});
    const auto prior=writer.snapshot();const auto objects=prior.storage_info().objects;
    writer.coalesce_equal_extents();auto shared=writer.snapshot();
    require(shared.storage_info().objects==objects-1,"paged native exact sharing absent");
    file_apply(writer,file_write("a",7,data(3,99)));
    require(writer.snapshot().read("b",0,kNativeExtentBytes)==bytes && shared.read("a",0,kNativeExtentBytes)==bytes,
        "paged shared write damaged another name/snapshot");
    auto session=NativeFileSession::create(std::move(writer));
    const auto handle=session.open("a",{true,true,true});
    const auto other=session.open("a",{true,true,true});
    session.rename(handle,"moved");
    require(session.info(other).info.path=="moved","paged session rename lost second handle");
    session.set_delete(handle,true);session.cleanup(handle,true);session.close(handle);
    require(!session.read(other,0,100).empty(),"paged detached file lost live bytes");
    session.cleanup(other,false);session.close(other);
    require(session.stats().contexts==0 && session.snapshot().list().size()==1,"paged session retirement leaked inode");
    session.flush();
  }
  auto writer=resume(ram);file_apply(writer,file_create("after-restart"));writer.snapshot().verify_all();
}
void closed_session_and_foreign_objects() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  {
    auto writer=fresh(ram);file_apply(writer,file_create("pending"));
    writer.set_delete_pending(writer.snapshot().stat("pending").inode,true);
  }
  require(view(ram).needs_session_recovery(),"paged pending intent not persisted");
  const auto before=ram->live;
  rejects([&]{auto writer=resume(ram);},"ordinary paged resume accepted unretired contexts");
  require(ram->live==before,"refused native resume changed paged bytes");
  {
    auto writer=resume(ram,true);
    require(writer.snapshot().list().empty() && !writer.snapshot().needs_session_recovery(),"closed session recovery incomplete");
  }
  {
    auto objects=PagedObjectStoreWriter::resume(ram,identity(),view(ram).storage_info().generation,limits);
    const StoreMutation foreign{999,false,data(3,1)};objects.commit({&foreign,1});
  }
  rejects([&]{(void)view(ram);},"foreign paged object exposed as native volume");
}
void failure_matrix(std::uint32_t unit) {
  auto source=std::make_shared<Ram>();source->failure_unit=unit;
  {auto writer=fresh(source);writer.apply(baseline());}
  const auto initial=source->stable;
  auto probe=image(initial,unit);std::size_t operations=0;
  {auto writer=resume(probe);const auto start=probe->calls;writer.apply(update());operations=probe->calls-start;}
  require(operations>3 && operations<256,"unexpected paged native fault count");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<operations;++cut) {
    auto ram=image(initial,unit);
    {
      auto writer=resume(ram);auto prior=writer.snapshot();ram->failure=mode;ram->fail_at=ram->calls+cut;
      rejects([&]{writer.apply(update());},"paged native injected fault acknowledged");
      require(writer.poisoned(),"paged native failed commit not fenced");
      rejects([&]{file_apply(writer,file_create("forbidden-retry"));},"paged native retried after indeterminate publication");
      require(prior.read("stable.bin",0,100)==data(31,18),"paged fault damaged acknowledged immutable extent");
    }
    ram->live=ram->stable;ram->fail_at=(std::numeric_limits<std::size_t>::max)();
    auto recovered=view(ram);require(recovered.sequence()==2 || recovered.sequence()==3,"partial native paged sequence");
    const auto updated=recovered.sequence()==3;expected(recovered,updated);++checks;
    auto writer=resume(ram);
    writer.apply(std::array{file_create("continued"),file_write("continued",0,data(3073,91))});
    expected(writer.snapshot(),updated,true);++checks;++faults;
  }
}
void flush_failures() {
  for(unsigned failure=0;failure<2;++failure) {
    auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
    writer.apply(baseline());const auto writes=ram->writes;
    if(failure==0)ram->fail_at=ram->calls;
    else {ram->corrupt_read_offset=0;ram->corrupt_read_number=2;ram->matching_reads=0;}
    rejects([&]{writer.flush();},"paged native Flush fault accepted");
    require(writer.poisoned() && ram->writes==writes,"paged Flush failure not fenced or rewrote data");
    require(failure==0 || ram->matching_reads==2,"post-barrier root corruption not exercised");
  }
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
  const auto flushes=ram->flushes;ram->failure_unit=65536;
  rejects([&]{writer.flush();},"changed backend failure unit accepted");
  require(writer.poisoned() && ram->flushes==flushes,"changed contract reached flush barrier");
}
class ObservedFlush final:public StoreByteIo {
 public:
  Ram ram;
  bool armed=false,after=false,preflight_allocation=false,barrier_allocation=false,
      postflight_allocation=false,extend_at_barrier=false;
  unsigned barriers=0;
  StoreIoContract contract() const noexcept override{return ram.contract();}
  std::uint64_t extent() const override{return ram.extent();}
  void admit_write(std::uint64_t bytes) override{ram.admit_write(bytes);}
  void read_exact(std::uint64_t at,std::span<std::byte> bytes) const override {
    if(armed && (preflight_allocation || (after && postflight_allocation)))throw std::bad_alloc();
    ram.read_exact(at,bytes);
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override{ram.write_exact(at,bytes);}
  void flush() override {
    if(armed) {++barriers;if(barrier_allocation)throw std::bad_alloc();}
    ram.flush();
    if(armed) {after=true;if(extend_at_barrier)ram.live.push_back(std::byte{0});}
  }
};
void paged_barrier_pressure() {
  for(unsigned mode=0;mode<4;++mode) {
    auto io=std::make_shared<ObservedFlush>();io->ram.failure_unit=4096;
    auto writer=PagedObjectStoreWriter::create(io,identity(),limits);
    const auto initial=writer.snapshot().info();const auto writes=io->ram.writes;
    io->armed=true;io->preflight_allocation=mode==0;io->barrier_allocation=mode==1;
    io->postflight_allocation=mode==2;io->extend_at_barrier=mode==3;
    bool rejected=false;
    try{writer.flush_checkpoint();}catch(const std::exception&){rejected=true;}
    require(rejected && io->ram.writes==writes,"paged barrier pressure/tail change accepted or wrote bytes");
    require(writer.poisoned()==(mode!=0) && io->barriers==(mode==0?0U:1U),"paged barrier allocation phase/retirement contract");
    if(mode==0) {
      io->preflight_allocation=false;const auto result=writer.flush_checkpoint();
      require(result.generation==initial.generation && io->barriers==1 && !writer.poisoned(),
          "preflight allocation refusal prevented healthy continuation");
    }
  }
}
}
int main() {try {
  shared_engine();sharing_and_sessions();closed_session_and_foreign_objects();
  failure_matrix(4096);failure_matrix(65536);flush_failures();paged_barrier_pressure();
  std::cout<<"PASS: "<<checks<<" native paged-backend checks; "<<faults
      <<" injected write/barrier cases; memory CPU only, namespace still bounded.\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
