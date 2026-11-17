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
#ifdef FLITZFS_TEST_DIRECTORY_INDEX
  return NativeFilesystemWriter::initialize_indexed_names(PagedObjectStoreWriter::create(
      ram,identity(),limits,{StoreCompressionMode::raw,1,false}));
#elif defined(FLITZFS_TEST_INODE_RECORDS)
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
void equivalent(const NativeFilesystemSnapshot& a,const NativeFilesystemSnapshot& b,std::string_view dir="",bool same_identity=true) {
  require(a.sequence()==b.sequence() && (!same_identity || a.identity()==b.identity()),"native sequence/identity parity");
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
    if(x.directory)equivalent(a,b,x.path,same_identity);
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
  rejects([&]{(void)NativeFilesystemWriter::compact(paged.snapshot(),target,new_id);},"live writer compaction accepted");
  require(target->extent()==0 && !paged.poisoned(),"live-source refusal touched target/source");
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
  auto candidate=std::make_shared<Ram>();auto candidate_id=identity();candidate_id[7]=std::byte{17};
  rejects([&]{NativeFilesystemWriter::compact(view(ram),candidate,candidate_id);},"pending native contexts copied");
  require(candidate->extent()==0,"pending context copy touched target");
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
StoreIdentity next_identity(unsigned value=71) {auto id=identity();id[7]=static_cast<std::byte>(value);return id;}
void maintenance_success_and_admission() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  std::optional<NativeFilesystemSnapshot> stale;
  {
    auto writer=fresh(ram);
    writer.apply(std::array{file_create("d",true),file_create("d/a"),
        file_write("d/a",0,Bytes(2*kNativeExtentBytes,std::byte{42})),file_create("b"),file_write("b",0,data(19,8))});
    NativeBasicMetadata basic;basic.attributes=0x20;basic.creation_time=1234;basic.last_write_time=5678;
    auto a=writer.snapshot().stat("d/a").inode;
    writer.set_basic_metadata(a,basic);
    Bytes descriptor(20);descriptor[0]=std::byte{1};descriptor[3]=std::byte{0x80};
    const auto secure=writer.create_inode_result("secure",false,basic,descriptor).info.inode;
    writer.reserve_inode_result(secure,kNativeExtentBytes);
    stale=writer.snapshot();
    for(unsigned i=0;i<16;++i)file_apply(writer,file_write("b",0,data(19,i)));
    auto target=std::make_shared<Ram>();target->failure_unit=4096;
    rejects([&]{NativeFilesystemWriter::compact(writer.snapshot(),target,next_identity());},"live source writer copied");
    require(target->extent()==0,"live source refusal wrote target");
  }
  auto source=view(ram);const auto original=ram->live;const auto source_calls=ram->calls;
  auto refused=std::make_shared<Ram>();refused->failure_unit=4096;
  rejects([&]{NativeFilesystemWriter::compact(*stale,refused,next_identity());},"stale native snapshot copied");
  require(refused->extent()==0,"stale source wrote target");
  rejects([&]{NativeFilesystemWriter::compact(source,refused,source.identity());},"same identity clone admitted");
  rejects([&]{NativeFilesystemWriter::compact(source,ram,next_identity());},"aliased source target admitted");
  rejects([&]{NativeFilesystemWriter::compact(source,refused,next_identity(),{}, {true});},"flat metadata option admitted for paged output");
  require(refused->extent()==0 && ram->live==original && ram->calls==source_calls,"maintenance admission modified source/target");
  auto compacted=std::make_shared<Ram>(),compressed=std::make_shared<Ram>();
  compacted->failure_unit=compressed->failure_unit=4096;
  std::uint64_t compact_size=0,compressed_size=0;
  {
    auto copy=NativeFilesystemWriter::compact(source,compacted,next_identity(),{StoreCompressionMode::automatic,1,false});
    auto encoded=NativeFilesystemWriter::recompress(source,compressed,next_identity(),{StoreCompressionMode::automatic,1,false});
    equivalent(source,copy.snapshot(),"",false);equivalent(source,encoded.snapshot(),"",false);
    require(copy.snapshot().identity()==next_identity() && encoded.snapshot().identity()==next_identity(),"namespace identity not rebound");
    require(copy.snapshot().storage_info().generation==source.storage_info().generation+2,"copy generation did not advance by two");
    require(compacted->extent()<ram->extent(),"history was not reclaimed by copy");
    require(encoded.snapshot().storage_info().stored_payload_bytes<copy.snapshot().storage_info().stored_payload_bytes &&
        compressed->extent()<compacted->extent(),"pattern recompression did not reduce complete new image");
    compact_size=compacted->extent();compressed_size=compressed->extent();
    const auto old=PagedObjectStoreSnapshot::open(ram,limits),fresh_copy=PagedObjectStoreSnapshot::open(compacted,limits);
    for(const auto& entry:old.list_page(0,768)) {
      const auto copied=fresh_copy.stat(entry.id);require(copied && copied->generation==entry.generation,"object generation changed on maintenance");
      if(entry.id!=1)require(copied->stored_payload_bytes==entry.stored_payload_bytes &&
          fresh_copy.read(entry.id)==old.read(entry.id),"compact re-encoded a non-root object");
    }
    file_apply(copy,file_write("d/a",0,Bytes{std::byte{99}}));
    require(source.read("d/a",0,1)==Bytes{std::byte{42}} && encoded.snapshot().read("d/a",0,1)==Bytes{std::byte{42}},
        "new volume write damaged source/other candidate");
  }
  auto encoded_source=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(compressed,limits));
  auto preserved=std::make_shared<Ram>(),restored=std::make_shared<Ram>();preserved->failure_unit=restored->failure_unit=4096;
  {
    auto copy=NativeFilesystemWriter::compact(encoded_source,preserved,next_identity(72),{StoreCompressionMode::raw,1,false});
    equivalent(encoded_source,copy.snapshot(),"",false);
    const auto left=PagedObjectStoreSnapshot::open(compressed,limits),right=PagedObjectStoreSnapshot::open(preserved,limits);
    for(const auto& entry:left.list_page(0,768))if(entry.id!=1)
      require(right.stat(entry.id)->stored_payload_bytes==entry.stored_payload_bytes,"raw future policy expanded compacted encoded payload");
    auto raw=NativeFilesystemWriter::recompress(encoded_source,restored,next_identity(73),{StoreCompressionMode::raw,1,false});
    equivalent(encoded_source,raw.snapshot(),"",false);
    require(raw.snapshot().storage_info().stored_payload_bytes>encoded_source.storage_info().stored_payload_bytes,
        "raw restoration did not replace compressed payload encoding");
  }
  require(ram->live==original && ram->calls==source_calls,"maintenance changed source bytes or flushed source");
  std::cout<<"native_paged_maintenance source_image_bytes="<<ram->extent()<<" compact_image_bytes="<<compact_size
      <<" recompressed_image_bytes="<<compressed_size<<'\n';
}
void maintenance_faults(std::uint32_t unit) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
  {auto writer=fresh(ram);writer.apply(baseline());}
  auto source=view(ram);const auto original=ram->live;const auto original_calls=ram->calls;
  for(bool reencode:{false,true}) {
    auto invoke=[&](const std::shared_ptr<Ram>& target) {
      return reencode?NativeFilesystemWriter::recompress(source,target,next_identity(),{StoreCompressionMode::automatic,1,false}):
          NativeFilesystemWriter::compact(source,target,next_identity(),{StoreCompressionMode::raw,1,false});
    };
    auto probe=std::make_shared<Ram>();probe->failure_unit=unit;
    {auto copy=invoke(probe);equivalent(source,copy.snapshot(),"",false);}
    const auto cuts=probe->calls;require(cuts>3 && cuts<160,"maintenance fault envelope");
    if(!reencode) {
      std::uint64_t primary=0;for(unsigned i=0;i<8;++i)
        primary|=std::uint64_t(std::to_integer<unsigned char>(probe->stable[104+i]))<<(8*i);
      require(primary && primary<probe->extent(),"copy root-page fixture bounds");
      for(const auto offset:{std::uint64_t{0},primary}) {
        auto unreadable=std::make_shared<Ram>();unreadable->failure_unit=unit;
        unreadable->corrupt_read_offset=offset;unreadable->corrupt_read_number=1;
        rejects([&]{invoke(unreadable);},"unverified/degraded new root acknowledged");
        require(ram->live==original && ram->calls==original_calls,"new-root corruption modified source");
      }
    }
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
      auto failed=std::make_shared<Ram>();failed->failure_unit=unit;failed->failure=mode;failed->fail_at=cut;
      rejects([&]{invoke(failed);},"faulted new-image copy acknowledged");
      require(ram->live==original && ram->calls==original_calls,"failed copy changed/flushed source");
      failed->live=failed->stable;failed->fail_at=(std::numeric_limits<std::size_t>::max)();
      std::optional<PagedObjectStoreSnapshot> exposed;
      try{exposed=PagedObjectStoreSnapshot::open(failed,limits);}catch(const std::runtime_error&){}
      if(exposed) {
        auto recovered=NativeFilesystemSnapshot::open(std::move(*exposed));equivalent(source,recovered,"",false);
        const auto generation=recovered.storage_info().generation;
        auto objects=PagedObjectStoreWriter::resume(failed,next_identity(),generation,limits);
        auto writer=NativeFilesystemWriter::resume(std::move(objects));file_apply(writer,file_create("continued"));
        require(writer.snapshot().stat("continued").inode!=0,"published failed candidate cannot recover");
      }
      ++faults;
    }
    std::cout<<"native_paged_maintenance unit="<<unit<<" reencode="<<reencode<<" fault_cuts="<<cuts<<" modes=3\n";
  }
  auto corrupt=std::make_shared<Ram>();corrupt->failure_unit=unit;
  corrupt->corrupt_read_offset=2*kStoreCommitUnit+128;corrupt->corrupt_read_number=1;
  rejects([&]{NativeFilesystemWriter::compact(source,corrupt,next_identity());},"corrupted copy readback published");
  require(std::all_of(corrupt->live.begin(),corrupt->live.begin()+2*kStoreCommitUnit,
      [](auto byte){return byte==std::byte{0};}),"bad target had a published root");
  require(ram->live==original && ram->calls==original_calls,"corrupt candidate touched source");
  Bytes known(kNativeExtentBytes);const auto prefix=data(31,18);std::copy(prefix.begin(),prefix.end(),known.begin());
  const auto where=std::search(ram->live.begin(),ram->live.end(),known.begin(),known.end());
  require(where!=ram->live.end() && std::search(std::next(where),ram->live.end(),known.begin(),known.end())==ram->live.end(),
      "source corruption fixture did not find unique stored payload");
  ram->corrupt_read_offset=static_cast<std::uint64_t>(std::distance(ram->live.begin(),where));
  ram->corrupt_read_number=1;ram->matching_reads=0;
  auto refused=std::make_shared<Ram>();refused->failure_unit=unit;
  rejects([&]{NativeFilesystemWriter::recompress(source,refused,next_identity());},"corrupt source copied");
  require(refused->extent()==0 && ram->live==original && ram->calls==original_calls,"corrupt-source refusal touched output/source");
  ram->corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)();
}
void directory_pages() {
  for(bool flat:{false,true}) {
    auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
    auto writer=flat?NativeFilesystemWriter::initialize(ObjectStoreWriter::create(
        ram,identity(),{StoreCompressionMode::raw,1,false})):fresh(ram);
    writer.apply(std::array{file_create("z"),file_create("AA"),file_create("a"),
        file_create("B"),file_create("empty",true),file_create("folder",true),file_create("folder/inside")});
    auto snapshot=writer.snapshot();const auto expected=snapshot.list();
    require(expected[0].path=="a" && expected[1].path=="AA" && expected[2].path=="B" && expected.back().path=="z",
        "directory listing lost canonical-name order");
    for(std::size_t maximum:{1U,3U,1024U}) {
      std::optional<NativeDirectoryCursor> cursor;std::size_t at=0,pages=0;
      do {
        const auto page=snapshot.list_inode_page(1,cursor.value_or(NativeDirectoryCursor{}),maximum);
        require(!page.entries.empty() && page.entries.size()<=maximum,"directory page cardinality");
        for(const auto& entry:page.entries) {
          require(at<expected.size() && entry.inode==expected[at].inode && entry.path==expected[at].path,
              "directory pagination repeated or skipped an entry");++at;
        }
        cursor=page.next;require(++pages<=expected.size(),"directory cursor failed to advance");
      }while(cursor);
      require(at==expected.size(),"directory page terminated early");
    }
    const auto first=snapshot.list_inode_page(1,{},1);require(first.next.has_value(),"missing continuation");
    const auto empty=snapshot.stat("empty").inode,folder=snapshot.stat("folder").inode;
    const auto empty_page=snapshot.list_inode_page(empty);
    require(empty_page.entries.empty() && !empty_page.next,"empty directory has a continuation");
    const auto reads=ram->read_calls;
    rejects([&]{snapshot.list_inode_page(folder,*first.next,1);},"cross-directory cursor accepted");
    rejects([&]{snapshot.list_inode_page(1,{},0);},"zero directory page accepted");
    rejects([&]{snapshot.list_inode_page(1,{},1025);},"excessive directory page accepted");
    rejects([&]{snapshot.list_inode_page(snapshot.stat_inode(expected.front().inode).inode);},"file enumerated as directory");
    require(ram->read_calls==reads,"invalid page/cursor reached backing I/O");
    {auto reopened=writer.snapshot();rejects([&]{reopened.list_inode_page(1,*first.next,1);},"reopened snapshot reused cursor");}
    writer.apply(std::array{file_rename("AA","renamed"),file_remove("B"),file_create("new")});
    const auto old_next=snapshot.list_inode_page(1,*first.next,1);
    require(old_next.entries.size()==1 && old_next.entries.front().path=="AA","retained cursor observed new namespace");
    auto moved=std::move(snapshot);
    require(moved.list_inode_page(1,*first.next,1).entries.front().path=="AA","snapshot move invalidated cursor");
    rejects([&]{snapshot.list_inode_page(1,*first.next,1);},"moved-from snapshot accepted cursor");
    NativeDirectoryCursor expired;
    {auto retired=writer.snapshot();expired=*retired.list_inode_page(1,{},1).next;}
    auto current=writer.snapshot();const auto before=ram->read_calls;
    rejects([&]{current.list_inode_page(1,expired,1);},"expired snapshot cursor accepted");
    auto other_ram=std::make_shared<Ram>();other_ram->failure_unit=4096;auto other=fresh(other_ram);
    rejects([&]{other.snapshot().list_inode_page(1,*first.next,1);},"different volume reused cursor");
    require(ram->read_calls==before,"expired cursor reached backing I/O");
  }
}
#ifdef FLITZFS_TEST_INODE_RECORDS
constexpr auto inode_bit=1ULL<<63;
#ifdef FLITZFS_TEST_DIRECTORY_INDEX
constexpr unsigned root_index_records=1;
#else
constexpr unsigned root_index_records=0;
#endif
void inode_records_and_incremental_writes() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
  for(unsigned start=0;start<128;start+=16) {
    std::vector<NativeFileMutation> batch;
    for(unsigned i=start;i<start+16;++i)batch.push_back(file_create("f"+std::to_string(i)));
    writer.apply(batch);
  }
  const auto prior=PagedObjectStoreSnapshot::open(ram,limits);
  const auto records=prior.list_page(0,512);const auto whole_metadata=prior.info().logical_bytes;
  require(prior.read(1).size()==64 && records.size()==130+root_index_records,"inode-record fixed-root/inventory framing");
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
void checked_live_reads() {
  constexpr unsigned unrelated_files=128;
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  auto writer=fresh(ram);
  const auto payload=data(23,701);
  const auto created=writer.apply(std::array{file_create("Data",true),file_create("Data/target"),
      file_write("Data/target",0,payload),file_create("Other",true)});
  const auto directory=created.created_inodes.at(0), target=created.created_inodes.at(1);
  for(unsigned first=0;first<unrelated_files;first+=16) {
    std::vector<NativeFileMutation> batch;
    for(unsigned i=first;i<first+16;++i)batch.push_back(file_create("Other/item-"+std::to_string(i)));
    writer.apply(batch);
  }
  file_apply(writer,file_rename("Data/target","Data/renamed"));
  const auto start=ram->read_bytes;
  require(writer.inspect_allocation_usage().live_extents==1,"complete namespace comparison fixture");
  const auto complete_bytes=ram->read_bytes-start;
  const auto image_bytes=ram->stable;
  const auto objects=PagedObjectStoreSnapshot::open(ram,limits);
  const auto offset_of=[&](std::uint64_t id) {
    const auto record=objects.read(id);
    const auto where=std::search(image_bytes.begin(),image_bytes.end(),record.begin(),record.end());
    require(!record.empty() && where!=image_bytes.end(),"owned raw record missing");
    require(std::search(std::next(where),image_bytes.end(),record.begin(),record.end())==image_bytes.end(),
        "owned raw record not unique");
    return static_cast<std::uint64_t>(std::distance(image_bytes.begin(),where));
  };
  const auto unrelated_id=writer.inspect_path("Other/item-99").info.inode;
  const auto unrelated_at=offset_of(inode_bit|unrelated_id);
  const auto selected_write_count=ram->writes;
  {
    auto session=NativeFileSession::create(std::move(writer));
    const auto handle=session.open("data/RENAMED",{true,true,false});
    const auto before_lookup=ram->read_bytes;
    const auto state=session.lookup("data/RENAMED");
    const auto lookup_bytes=ram->read_bytes-before_lookup;
    require(state.state.info.inode==target && state.state.info.logical_bytes==payload.size(),
        "live lookup changed inode or size");
    const auto before_data=ram->read_bytes;
    require(session.read(handle,0,128)==payload,"live scoped read changed bytes");
    const auto read_bytes=ram->read_bytes-before_data;
    require(lookup_bytes>0 && lookup_bytes<kNativeExtentBytes/2 &&
        read_bytes<kNativeExtentBytes+kNativeExtentBytes/2,
        "live query loaded duplicate or unrelated paged metadata");
    require(ram->writes==selected_write_count,"live query changed backing bytes");
    ram->corrupt_read_offset=unrelated_at;ram->corrupt_read_number=1;
    require(session.lookup("Data/renamed").state.info.inode==target &&
        session.read(handle,0,128)==payload && ram->matching_reads==0 && !session.poisoned(),
        "healthy scoped read touched another directory's inode");
    const auto flushes=ram->flushes;
    rejects([&]{(void)session.flush();},"explicit barrier stopped checking the full namespace");
    require(session.poisoned() && ram->matching_reads==1 && ram->flushes==flushes &&
        ram->stable==image_bytes,"full revalidation failure reached the barrier");
    std::cout<<"live_query_io inodes="<<1+created.created_inodes.size()+unrelated_files
        <<" namespace_read_bytes="<<complete_bytes
        <<" lookup_read_bytes="<<lookup_bytes<<" file_read_bytes="<<read_bytes<<'\n';
  }
  std::vector<std::uint64_t> checked{offset_of(1),offset_of(inode_bit|directory),offset_of(inode_bit|target)};
#ifdef FLITZFS_TEST_DIRECTORY_INDEX
  checked.push_back(offset_of((1ULL<<62)|directory));
#endif
  for(const auto offset:checked) {
    auto faulted=image(image_bytes,4096);
    auto trial=resume(faulted);
    auto session=NativeFileSession::create(std::move(trial));
    faulted->corrupt_read_offset=offset;faulted->corrupt_read_number=1;
    const auto writes=faulted->writes;
    rejects([&]{(void)session.lookup("Data/renamed");},"live lookup exposed corrupted metadata");
    require(session.poisoned() && faulted->matching_reads==1 && faulted->writes==writes,
        "selected metadata corruption did not fence before writes");
  }
  {
    auto faulted=image(image_bytes,4096);
    auto session=NativeFileSession::create(resume(faulted));
    require(session.lookup("Data/renamed").state.info.inode==target,"cached query fixture did not open");
    const auto root=static_cast<std::size_t>(objects.info().generation%2*kStoreCommitUnit);
    // Root reference primary and peer offsets, inside this owned fixture.
    for(auto field:{104U,112U}) {
      std::uint64_t page=0;
      for(unsigned i=0;i<8;++i)
        page|=std::uint64_t(std::to_integer<unsigned char>(image_bytes.at(root+field+i)))<<(8*i);
      require(page>=2*kStoreCommitUnit && page<image_bytes.size(),"fixture page reference outside image");
      faulted->live.at(static_cast<std::size_t>(page))^=std::byte{1};
    }
    const auto writes=faulted->writes;
    rejects([&]{(void)session.lookup("Data/renamed");},"query reused a metadata page from a previous operation");
    require(session.poisoned() && faulted->writes==writes && faulted->stable==image_bytes,
        "damaged metadata replicas were exposed or overwritten");
  }
  auto recovered=image(image_bytes,4096);
  const auto clean=view(recovered);
  require(clean.read("Data/renamed",0,128)==payload && clean.list("Other").size()==unrelated_files,
        "acknowledged checkpoint changed after failed reads");
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
    require(snapshot.list().size()==255 && snapshot.storage_info().objects==512+root_index_records,
        "inode metadata consumed the promised file/data inventory");
    rejects([&]{file_apply(writer,file_create("over-node-limit"));},"existing node bound silently changed");
    for(unsigned i=0;i<255;++i)require(snapshot.read("f"+std::to_string(i),0,2)==Bytes{std::byte{42}},"capacity file bytes");
  }
  {
    auto source=view(ram);auto target=std::make_shared<Ram>();target->failure_unit=4096;
    auto copy=NativeFilesystemWriter::compact(source,target,next_identity(),{StoreCompressionMode::raw,1,false});
    equivalent(source,copy.snapshot(),"",false);
    require(copy.snapshot().storage_info().objects==512+root_index_records,"maximum native inventory changed on copy");
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
  auto before=writer.snapshot();require(before.storage_info().objects==258+root_index_records,"large extent inventory");
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
    require((size==0 && recovered.storage_info().objects==3+root_index_records) ||
        (size==std::uint64_t(255)*kNativeExtentBytes && recovered.storage_info().objects==258+root_index_records),
        "large deletion published a partial metadata/data set");
    if(size)require(recovered.read("large",size-13,13)==Bytes(13,std::byte{37}),"large deletion recovery lost acknowledged tail");
    auto continued=resume(failed);file_apply(continued,file_write("large",0,Bytes{std::byte{43}}));
    require(view(failed).read("large",0,1)==Bytes{std::byte{43}},"large deletion recovery could not continue");
    ++faults;
  }
  std::cout<<"inode_records large_delete_operations=257 fault_cuts="<<cuts<<" modes=3\n";
  file_apply(writer,file_truncate("large",0));
  require(writer.snapshot().storage_info().objects==3+root_index_records && writer.snapshot().read("large",0,1).empty(),
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
      if(variant==5){key=1;record=snapshot.read(1);record[8]=std::byte{255};}
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
#ifdef FLITZFS_TEST_DIRECTORY_INDEX
constexpr auto directory_bit=1ULL<<62;
void directory_record_updates() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);
  writer.apply(std::array{file_create("A",true),file_create("B",true),file_create("A/sub",true),
      file_create("A/sub/p"),file_create("A/X.txt"),file_write("A/X.txt",0,data(17,5)),file_create("B/victim")});
  auto old=writer.snapshot();const auto a=old.stat("A").inode,b=old.stat("B").inode,
      sub=old.stat("A/sub").inode,p=old.stat("A/sub/p").inode;
  auto before=PagedObjectStoreSnapshot::open(ram,limits);
  file_apply(writer,file_rename("A/X.txt","A/x.TXT"));
  auto cased=PagedObjectStoreSnapshot::open(ram,limits);
  require(before.stat(directory_bit|a)->generation==cased.stat(directory_bit|a)->generation,
      "case-only rename rewrote unchanged canonical index");
  file_apply(writer,file_rename("A/sub","B/moved"));
  auto moved=PagedObjectStoreSnapshot::open(ram,limits);
  for(auto parent:{a,b})require(cased.stat(directory_bit|parent)->generation!=moved.stat(directory_bit|parent)->generation,
      "cross-parent move failed to update parent index");
  for(auto key:{directory_bit|1,directory_bit|sub,inode_bit|p})
    require(cased.stat(key)->generation==moved.stat(key)->generation,"folder move rewrote unaffected descendants/index");
  require(writer.snapshot().stat("B/moved/p").inode==p && old.stat("A/sub/p").inode==p,
      "folder move changed stable descendant identity or old snapshot");
  file_apply(writer,file_rename("A/x.TXT","B/victim",true));
  require(view(ram).read("B/victim",0,99)==data(17,5) && view(ram).list("A").empty(),
      "indexed replacement lost data or retained old name");
  auto current=writer.snapshot();const auto reads=ram->read_calls;
  require(current.stat("b/MOVED/P").inode==p && ram->read_calls>reads,"indexed path did not consult persisted names");
  current.verify_all();
}
void forged_directory_records() {
  auto original=std::make_shared<Ram>();original->failure_unit=4096;
  {auto writer=fresh(original);writer.apply(std::array{file_create("a"),file_create("b"),file_create("d",true)});}
  for(unsigned variant=0;variant<14;++variant) {
    auto ram=image(original->stable,4096);
    {
      auto snapshot=PagedObjectStoreSnapshot::open(ram,limits);
      auto objects=PagedObjectStoreWriter::resume(ram,identity(),snapshot.info().generation,limits,{StoreCompressionMode::raw,1,false});
      auto bytes=snapshot.read(directory_bit|1);auto key=directory_bit|1;bool erase=false;
      if(variant==0)bytes[0]^=std::byte{1};
      if(variant==1)bytes[8]=std::byte{2};
      if(variant==2)bytes[12]=std::byte{255};
      if(variant==3)bytes[16]=std::byte{2};
      if(variant==4)bytes[24]=std::byte{99};
      if(variant==5)bytes[32]=std::byte{255};
      if(variant==6)bytes[34]=std::byte{1};
      if(variant==7)bytes[36]=std::byte{'A'}; // Canonical keys are lowercase.
      if(variant==8)bytes[49]=std::byte{'a'}; // Duplicate key.
      if(variant==9)std::swap(bytes[36],bytes[49]); // Unsorted valid keys.
      if(variant==10)bytes.push_back(std::byte{0});
      if(variant==11)bytes.pop_back();
      if(variant==12)erase=true;
      if(variant==13)key=directory_bit|999;
      if(erase)bytes.clear();const StoreMutation mutation{key,erase,std::move(bytes)};objects.commit({&mutation,1});
    }
    const auto calls=ram->calls;const auto before=ram->live;
    rejects([&]{view(ram);},"intact malformed/foreign directory index exposed files");
    require(ram->calls==calls && ram->live==before,"directory-index rejection modified backing");
  }
}
void directory_maximum_recovery() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  {
    auto writer=fresh(ram);
    for(unsigned i=0;i<255;i+=8) {
      std::vector<NativeFileMutation> batch;
      for(unsigned n=i;n<(std::min)(i+8,255U);++n) {
        const auto name="f"+std::to_string(n);
        batch.push_back(file_create(name));batch.push_back(file_write(name,0,Bytes{std::byte{42},static_cast<std::byte>(n)}));
      }
      writer.apply(batch);
    }
  }
  // Serialize valid durable delete intents as a fixture, after all contexts
  // have retired. This avoids 255 unrelated transactions in this recovery test.
  {
    const auto snapshot=PagedObjectStoreSnapshot::open(ram,limits);
    auto objects=PagedObjectStoreWriter::resume(ram,identity(),snapshot.info().generation,limits,{StoreCompressionMode::raw,1,false});
    std::vector<StoreMutation> pending;
    for(const auto& record:snapshot.list_page(inode_bit,256))if(record.id!=(inode_bit|1)) {
      auto bytes=snapshot.read(record.id);bytes[20]=std::byte{1};pending.push_back({record.id,false,std::move(bytes)});
    }
    require(pending.size()==255,"maximum recovery fixture inode count");objects.commit(pending);
  }
  require(view(ram).needs_session_recovery(),"durable delete intents not detected");
  const auto baseline=ram->stable;auto probe=image(baseline,4096);std::size_t cuts=0;
  {const auto generation=PagedObjectStoreSnapshot::open(probe,limits).info().generation;
    auto objects=PagedObjectStoreWriter::resume(probe,identity(),generation,limits,{StoreCompressionMode::raw,1,false});
    const auto start=probe->calls;auto writer=NativeFilesystemWriter::resume_closed_session(std::move(objects));cuts=probe->calls-start;
    require(writer.snapshot().list().empty() && writer.snapshot().storage_info().objects==3,"maximum recovery partial index");}
  require(cuts>3 && cuts<128,"maximum recovery fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto failed=image(baseline,4096);
    const auto generation=PagedObjectStoreSnapshot::open(failed,limits).info().generation;
    auto objects=PagedObjectStoreWriter::resume(failed,identity(),generation,limits,{StoreCompressionMode::raw,1,false});
    failed->failure=mode;failed->fail_at=failed->calls+cut;
    rejects([&]{NativeFilesystemWriter::resume_closed_session(std::move(objects));},"faulted 513-operation recovery acknowledged");
    failed->live=failed->stable;failed->fail_at=(std::numeric_limits<std::size_t>::max)();
    auto recovered=view(failed);const auto count=recovered.list().size();
    require((count==255 && recovered.storage_info().objects==513) ||
        (!count && recovered.storage_info().objects==3),"recovery published partial inode/name/data state");
    if(count)for(unsigned i=0;i<255;++i)
      require(recovered.read("f"+std::to_string(i),0,2)==Bytes{std::byte{42},static_cast<std::byte>(i)},
          "failed recovery damaged acknowledged bytes");
    auto continued=resume(failed,true);file_apply(continued,file_create("continue"));
    require(view(failed).list().size()==1,"maximum recovery could not resume");++faults;
  }
  std::cout<<"directory_index recovery_operations=513 fault_cuts="<<cuts<<" modes=3\n";
}
#endif
#endif
}
int main() {try {
  shared_engine();sharing_and_sessions();closed_session_and_foreign_objects();
  failure_matrix(4096);failure_matrix(65536);flush_failures();paged_barrier_pressure();directory_pages();
  maintenance_success_and_admission();maintenance_faults(4096);maintenance_faults(65536);
#ifdef FLITZFS_TEST_INODE_RECORDS
  inode_records_and_incremental_writes();checked_live_reads();inode_record_capacity_and_large_delete();forged_inode_records();
#endif
#ifdef FLITZFS_TEST_DIRECTORY_INDEX
  directory_record_updates();forged_directory_records();directory_maximum_recovery();
#endif
  std::cout<<"PASS: "<<checks<<" native paged-backend checks; "<<faults
      <<" injected write/barrier cases; memory CPU only, namespace still bounded.\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
