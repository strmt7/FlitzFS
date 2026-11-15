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
#ifdef FLITZFS_TEST_INODE_RECORDS
  return NativeFilesystemWriter::initialize_inode_records(PagedObjectStoreWriter::create(
      ram,identity(),limits,{StoreCompressionMode::raw,1,false}));
#else
  return NativeFilesystemWriter::initialize(PagedObjectStoreWriter::create(
      ram,identity(),limits,{StoreCompressionMode::raw,1,false}));
#endif
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
  auto reference=[&] {
    auto flat=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(a,identity(),{StoreCompressionMode::raw,1,false}));
#ifdef FLITZFS_TEST_INODE_RECORDS
    // Match the new layout's initial feature state without adding a logical
    // user transaction. Retire the writer, extend the sole legacy root frame
    // to canonical format 7, then admit it through the independent flat reader.
    {auto retired=std::move(flat);}
    const auto snapshot=ObjectStoreSnapshot::open(a);auto metadata=snapshot.read(1);
    require(metadata.size()==128,"reference initial root framing");metadata.resize(168);
    metadata[7]=std::byte{'7'};metadata[8]=std::byte{7};
    auto objects=ObjectStoreWriter::resume(a,identity(),snapshot.info().generation,{StoreCompressionMode::raw,1,false});
    const StoreMutation upgraded{1,false,std::move(metadata)};objects.commit_replicated({&upgraded,1});
    return NativeFilesystemWriter::resume(std::move(objects));
#else
    return flat;
#endif
  };
  auto flat=reference();
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
#ifdef FLITZFS_TEST_INODE_RECORDS
constexpr auto inode_bit=1ULL<<63;
void inode_records_and_incremental_writes() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
  for(unsigned start=0;start<128;start+=16) {
    std::vector<NativeFileMutation> batch;
    for(unsigned i=start;i<start+16;++i)batch.push_back(file_create("f"+std::to_string(i)));
    writer.apply(batch);
  }
  const auto prior=PagedObjectStoreSnapshot::open(ram,limits);
  const auto records=prior.list_page(0,512);const auto whole_metadata=prior.info().logical_bytes;
  require(prior.read(1).size()==64 && records.size()==130,"inode-record fixed-root/inventory framing");
  const auto inode=writer.snapshot().stat("f0").inode;const auto writes=ram->write_bytes;
  file_apply(writer,file_write("f0",0,Bytes{std::byte{99}}));
  const auto current=PagedObjectStoreSnapshot::open(ram,limits);
  unsigned changed=0;std::uint64_t metadata_bytes=current.stat(1)->logical_bytes;
  for(const auto& before:records)if(before.id&inode_bit) {
    const auto after=current.stat(before.id);
    require(after.has_value(),"unrelated inode disappeared");
    if(before.generation!=after->generation) {
      require(before.id==(inode_bit|inode),"unrelated inode metadata rewritten");
      ++changed;metadata_bytes+=after->logical_bytes;
    }
  }
  require(changed==1 && metadata_bytes<whole_metadata,"single-file write republished whole namespace");
  require(prior.read(inode_bit|inode)!=current.read(inode_bit|inode),"retained inode record did not remain distinct");
  require(view(ram).read("f0",0,1)==Bytes{std::byte{99}},"independent inode-record file bytes");
  std::cout<<"inode_records metadata_input_bytes="<<metadata_bytes<<" whole_namespace_bytes="<<whole_metadata
      <<" changed_inode_records="<<changed<<" backend_write_bytes="<<ram->write_bytes-writes<<'\n';
}
void inode_record_capacity_and_large_delete() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  {
    auto writer=fresh(ram);
    for(unsigned start=0;start<255;start+=8) {
      std::vector<NativeFileMutation> batch;
      for(unsigned i=start;i<(std::min)(255U,start+8);++i) {
        const auto name="f"+std::to_string(i);
        batch.push_back(file_create(name));batch.push_back(file_write(name,0,Bytes{std::byte{42}}));
      }
      writer.apply(batch);
    }
    auto snapshot=writer.snapshot();
    require(snapshot.list().size()==255 && snapshot.storage_info().objects==512,
        "inode metadata consumed the promised file/data inventory");
    rejects([&]{file_apply(writer,file_create("over-node-limit"));},"existing node bound silently changed");
    for(unsigned i=0;i<255;++i)require(snapshot.read("f"+std::to_string(i),0,2)==Bytes{std::byte{42}},"capacity file bytes");
  }
  // A different volume has the maximum 255 data extents in ONE file. Truncate
  // requires 255 deletions plus the fixed root and changed inode in ONE commit.
  ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
  file_apply(writer,file_create("large"));
  for(unsigned start=0;start<255;start+=16) {
    const auto extents=(std::min)(16U,255U-start);
    file_apply(writer,file_write("large",std::uint64_t(start)*kNativeExtentBytes,
        Bytes(std::size_t(extents)*kNativeExtentBytes,std::byte{37})));
  }
  auto before=writer.snapshot();require(before.storage_info().objects==258,"large extent inventory");
  const auto baseline=ram->stable;auto probe=image(baseline,4096);std::size_t cuts=0;
  {auto trial=resume(probe);const auto start=probe->calls;file_apply(trial,file_truncate("large",0));cuts=probe->calls-start;}
  require(cuts>3 && cuts<128,"large deletion fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto failed=image(baseline,4096);
    {
      auto trial=resume(failed);failed->failure=mode;failed->fail_at=failed->calls+cut;
      rejects([&]{file_apply(trial,file_truncate("large",0));},"large inode/data deletion fault acknowledged");
      require(trial.poisoned(),"large deletion indeterminate writer not fenced");
    }
    failed->live=failed->stable;failed->fail_at=(std::numeric_limits<std::size_t>::max)();
    const auto recovered=view(failed);const auto size=recovered.stat("large").logical_bytes;
    require((size==0 && recovered.storage_info().objects==3) ||
        (size==std::uint64_t(255)*kNativeExtentBytes && recovered.storage_info().objects==258),
        "large deletion published a partial metadata/data set");
    if(size)require(recovered.read("large",size-13,13)==Bytes(13,std::byte{37}),"large deletion recovery lost acknowledged tail");
    auto continued=resume(failed);file_apply(continued,file_write("large",0,Bytes{std::byte{43}}));
    require(view(failed).read("large",0,1)==Bytes{std::byte{43}},"large deletion recovery could not continue");
    ++faults;
  }
  std::cout<<"inode_records large_delete_operations=257 fault_cuts="<<cuts<<" modes=3\n";
  file_apply(writer,file_truncate("large",0));
  require(writer.snapshot().storage_info().objects==3 && writer.snapshot().read("large",0,1).empty(),
      "257-operation native metadata/data deletion was not atomic");
  require(before.read("large",std::uint64_t(255)*kNativeExtentBytes-13,13)==Bytes(13,std::byte{37}),
      "large deletion damaged retained bytes");
  view(ram).verify_all();
}
void forged_inode_records() {
  auto original=std::make_shared<Ram>();original->failure_unit=4096;
  std::uint64_t inode=0;
  {auto writer=fresh(original);writer.apply(std::array{file_create("a"),file_write("a",0,data(17,91))});
    inode=writer.snapshot().stat("a").inode;}
  for(unsigned variant=0;variant<9;++variant) {
    auto ram=image(original->stable,4096);
    {
      auto snapshot=PagedObjectStoreSnapshot::open(ram,limits);
      auto objects=PagedObjectStoreWriter::resume(ram,identity(),snapshot.info().generation,limits,{StoreCompressionMode::raw,1,false});
      auto record=snapshot.read(inode_bit|inode);std::uint64_t key=inode_bit|inode;bool erase=false;
      if(variant==0)record.push_back(std::byte{0}); // Cannot leak bytes into the next inode frame.
      if(variant==1)record[0]^=std::byte{1}; // Logical inode must match its index key.
      if(variant==2)record[50]=std::byte{0}; // False extent count must not reframe adjacent records.
      if(variant==3)erase=true;
      if(variant==4)key=inode_bit|999; // Extra metadata despite otherwise intact current records.
      if(variant==5){key=1;record=snapshot.read(1);record[8]=std::byte{2};}
      if(variant==6){key=1;record=snapshot.read(1);record[12]=std::byte{255};record[13]=std::byte{255};}
      if(variant==7){key=1;record=snapshot.read(1);record[40]^=std::byte{1};}
      if(variant==8)record[88]=std::byte{20}; // Security bytes cannot consume another frame.
      if(erase)record.clear();const StoreMutation forged{key,erase,std::move(record)};
      objects.commit({&forged,1});
    }
    const auto calls=ram->calls;const auto bytes=ram->live;
    rejects([&]{(void)view(ram);},"intact forged inode record exposed files");
    require(ram->calls==calls && ram->live==bytes,"malformed inode reader wrote backing bytes");
  }
}
#endif
}
int main() {try {
  shared_engine();sharing_and_sessions();closed_session_and_foreign_objects();
  failure_matrix(4096);failure_matrix(65536);flush_failures();paged_barrier_pressure();
#ifdef FLITZFS_TEST_INODE_RECORDS
  inode_records_and_incremental_writes();inode_record_capacity_and_large_delete();forged_inode_records();
#endif
  std::cout<<"PASS: "<<checks<<" native paged-backend checks; "<<faults
      <<" injected write/barrier cases; memory CPU only, namespace still bounded.\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
