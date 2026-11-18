#include "native_fs_fixture.hpp"
#include <iostream>
namespace {
using namespace flitzfs;using namespace native_fixture;
using Phase=NativeSharedBasePhase;using Work=NativeFilesystemWriter::SharedBaseWork;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F&& action,const char* reason){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(reason);}
Bytes family(unsigned i){auto bytes=data(kNativeExtentBytes,714);if(i){bytes[i*719%bytes.size()]^=std::byte{41};bytes[(i*1409+170)%bytes.size()]^=std::byte{77};}return bytes;}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,bool indexed=false) {
  auto store=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  return indexed?NativeFilesystemWriter::initialize_indexed(std::move(store)):
      NativeFilesystemWriter::initialize_shared_bases(std::move(store));
}
void populate(NativeFilesystemWriter& writer,unsigned count){for(unsigned i=0;i<count;++i)
  writer.apply(std::array{file_create("f"+std::to_string(i)),file_write("f"+std::to_string(i),0,family(i))});}
std::vector<NativeExtentSelection> selections(const NativeFilesystemSnapshot& snapshot,unsigned count){
  std::vector<NativeExtentSelection> result;for(unsigned i=0;i<count;++i)result.push_back({snapshot.stat("f"+std::to_string(i)).inode,0});return result;}
std::uint64_t io(const Ram& ram){return ram.read_calls+ram.calls;}
void ready(NativeFilesystemWriter& writer,Work& work){unsigned steps=0;while(!writer.advance_shared_base(work))require(++steps<512,"group preparation stalled");}
void verify(const NativeFilesystemSnapshot& snapshot,unsigned count){for(unsigned i=0;i<count;++i)
  require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"family readback differs");snapshot.verify_all();}
void foreground(std::uint32_t unit,unsigned count,bool indexed) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=unit;auto writer=fresh(ram,indexed);populate(writer,count);
  // The maximum-node legacy fixture has no space for an unrelated extra inode.
  const bool extra=indexed || count<kNativeMaxExtents;
  if(extra)writer.apply(std::array{file_create("noise"),file_write("noise",0,Bytes{std::byte{0}})});
  const auto keys=selections(writer.snapshot(),count);auto before=writer.snapshot();const auto initial_io=io(*ram);
  auto work=writer.prepare_shared_base(keys.front(),keys);
  require(io(*ram)==initial_io && work.progress().total_members==count,"prepare performed I/O or lost members");
  rejects([&]{writer.prepare_shared_base(keys.front(),keys);},"duplicate group preparation admitted");
  require(io(*ram)==initial_io,"duplicate work performed I/O");
  auto moved=std::move(work);require(work.progress().phase==Phase::failed,"moved work remained active");
  auto current=std::move(writer);unsigned steps=0;std::uint64_t maximum_read_bytes=0;
  while(moved.progress().phase!=Phase::ready) {
    const auto prior=moved.progress();const auto reads=ram->read_bytes,calls=ram->calls;
    current.advance_shared_base(moved);const auto after=moved.progress();
    require(after.completed_members-prior.completed_members<=1 && ram->calls==calls,"step decoded multiple members or wrote backing");
    maximum_read_bytes=(std::max)(maximum_read_bytes,ram->read_bytes-reads);
    require(ram->read_bytes-reads<=1024*1024 && after.owned_payload_bytes<=kStoreMaxTransactionBytes+131072,"step exceeded bounded fixture envelope");
    if(extra)file_apply(current,file_write("noise",0,Bytes{static_cast<std::byte>(steps+1)}));
    if(steps==0) {
      file_apply(current,file_rename("f0","renamed"));
      if(indexed) {
        const Bytes outside{std::byte{81}};
        current.write_inode(keys.front().inode,2*kNativeExtentBytes,outside);
      }
    }
    require(current.read_inode(keys.back().inode,0,kNativeExtentBytes)==family(count-1),"foreground read failed between steps");
    require(++steps<=count+1,"excess preparation steps");
  }
  require(steps==count+1,"unexpected one-base/one-member step count");
  const auto latest=current.snapshot();const auto workload=latest.workload_info();const auto file=latest.stat("renamed");
  const auto result=current.finish_shared_base(moved);
  require(result.applied && result.members==count && moved.progress().phase==Phase::completed && moved.progress().owned_payload_bytes==0,
      "prepared family did not publish/retire");
  const auto after=current.snapshot();require(after.workload_info()==workload && after.stat("renamed").modified_sequence==file.modified_sequence,
      "publication overwrote current workload/metadata");
  for(unsigned i=0;i<count;++i)require(after.read_inode(keys[i].inode,0,kNativeExtentBytes)==family(i),"prepared member bytes differ");
  if(extra)require(after.read("noise",0,1)==Bytes{static_cast<std::byte>(steps)},"unrelated writes were lost");
  if(indexed)require(after.read("renamed",2*kNativeExtentBytes,1)==Bytes{std::byte{81}},
      "a write to an unselected extent of a selected inode was lost");
  verify(before,count);after.verify_all();
  const auto consumed=io(*ram);rejects([&]{current.finish_shared_base(moved);},"group published twice");
  moved.cancel();
  require(io(*ram)==consumed && !current.poisoned() && moved.progress().phase==Phase::completed,
      "repeat completion changed terminal state or touched backing");
  std::cout<<"staged_group indexed="<<indexed<<" unit="<<unit<<" members="<<count<<" steps="<<steps
      <<" maximum_step_read_bytes="<<maximum_read_bytes<<'\n';
}
void staleness(unsigned action,unsigned phase,bool indexed) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram,indexed);populate(writer,4);
  const auto keys=selections(writer.snapshot(),4);auto work=writer.prepare_shared_base(keys.front(),keys);
  if(phase==1){writer.advance_shared_base(work);writer.advance_shared_base(work);}
  if(phase==2)ready(writer,work);
  const auto changed=family(1).front()^std::byte{1};
  if(action==0)file_apply(writer,file_write("f1",0,Bytes{changed}));
  if(action==1)file_apply(writer,file_truncate("f1",0));
  if(action==2)file_apply(writer,file_remove("f1"));
  if(action==3){file_apply(writer,file_remove("f1"));writer.apply(std::array{file_create("replacement"),file_write("replacement",0,family(1))});}
  if(action==4){file_apply(writer,file_write("f1",0,Bytes{changed}));file_apply(writer,file_write("f1",0,Bytes{family(1).front()}));}
  if(action==5)writer.write_inode_storage_result(keys[1].inode,0,Bytes{changed});
  if(action==6)writer.truncate_inode_storage_result(keys[1].inode,0);
  if(action==7)writer.set_delete_pending(keys[1].inode,true);
  if(action==8) {
    writer.set_delete_pending(keys[1].inode,true);
    writer.unlink_inode(keys[1].inode);
    writer.retire_inode(keys[1].inode);
    while(writer.retirement_pending())writer.reclaim_retired_inode();
  }
  if(indexed)require(work.progress().phase==Phase::failed,"indexed invalidation was not visible before I/O");
  const auto before=io(*ram),writes=ram->writes;
  if(phase==2)rejects([&]{writer.finish_shared_base(work);},"changed selection published");
  else rejects([&]{writer.advance_shared_base(work);},"changed selection prepared");
  require(io(*ram)==before && ram->writes==writes && work.progress().phase==Phase::failed && !writer.poisoned(),
      "stale rejection used I/O or poisoned writer");
  work.cancel();
  require(work.progress().phase==Phase::failed && work.progress().owned_payload_bytes==0,
      "cancellation erased a terminal failure");
  auto next=writer.prepare_shared_base(keys.front(),std::span<const NativeExtentSelection>(&keys[2],1));next.cancel();
}
void ownership(bool indexed) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram,indexed);populate(writer,4);
  const auto keys=selections(writer.snapshot(),4);
  for(unsigned phase=0;phase<3;++phase) {
    const auto refs=ram.use_count();auto work=writer.prepare_shared_base(keys.front(),keys);
    if(phase==1)writer.advance_shared_base(work);if(phase==2)ready(writer,work);
    const auto before=io(*ram);work.cancel();
    require(work.progress().phase==Phase::cancelled && work.progress().owned_payload_bytes==0 && io(*ram)==before && ram.use_count()==refs,
        "cancel retained payload/source ownership or used I/O");
    rejects([&]{writer.advance_shared_base(work);},"cancelled work advanced");
    require(work.progress().phase==Phase::cancelled && io(*ram)==before,"repeat operation changed cancellation or touched backing");
  }
  {auto abandoned=writer.prepare_shared_base(keys.front(),keys);writer.advance_shared_base(abandoned);}
  {auto work=writer.prepare_shared_base(keys.front(),keys);const auto before=io(*ram);
    rejects([&]{writer.finish_shared_base(work);},"incomplete work published");require(io(*ram)==before,"incomplete finish used I/O");}
  auto other_ram=std::make_shared<Ram>();other_ram->failure_unit=4096;auto other=fresh(other_ram,indexed);
  auto foreign=writer.prepare_shared_base(keys.front(),keys);const auto before=io(*ram)+io(*other_ram);
  rejects([&]{other.advance_shared_base(foreign);},"foreign owner advanced work");
  require(io(*ram)+io(*other_ram)==before && !writer.poisoned() && !other.poisoned(),"foreign work touched backing/owners");
  std::optional<Work> expired;
  auto retired=std::make_shared<Ram>();retired->failure_unit=4096;
  {auto temporary=fresh(retired,indexed);populate(temporary,4);const auto selected=selections(temporary.snapshot(),4);
    expired.emplace(temporary.prepare_shared_base(selected.front(),selected));temporary.advance_shared_base(*expired);}
  require(expired->progress().phase==Phase::failed,"retired owner reports live work");
  expired->cancel();
  require(expired->progress().phase==Phase::failed && expired->progress().owned_payload_bytes==0,
      "cancellation erased owner-expiry failure or kept its payload");
  auto info=PagedObjectStoreSnapshot::open(retired,limits).info();auto reopened=NativeFilesystemWriter::resume(
      PagedObjectStoreWriter::resume(retired,identity(),info.generation,limits,{StoreCompressionMode::raw,1,false}));
  const auto calls=io(*retired);rejects([&]{reopened.advance_shared_base(*expired);},"reopened owner revived work");
  require(io(*retired)==calls && !reopened.poisoned(),"expired work used I/O or poisoned reopened owner");
}
void unchanged_and_budget() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);populate(writer,4);
  const auto keys=selections(writer.snapshot(),4);auto work=writer.prepare_shared_base(keys.front(),keys);writer.advance_shared_base(work);
  const auto old=family(1).front();writer.apply(std::array{file_write("f1",0,Bytes{old^std::byte{1}}),file_write("f1",0,Bytes{old})});
  ready(writer,work);require(writer.finish_shared_base(work).applied,"same-transaction content reversion invalidated work");
  auto dense_ram=std::make_shared<Ram>();dense_ram->failure_unit=4096;auto dense=fresh(dense_ram);
  for(unsigned i=0;i<90;++i){auto bytes=family(0);if(i)for(std::size_t j=0;j<56000;++j)bytes[j]^=static_cast<std::byte>(i);
    dense.apply(std::array{file_create("f"+std::to_string(i)),file_write("f"+std::to_string(i),0,std::move(bytes))});}
  const auto selected=selections(dense.snapshot(),90);auto bounded=dense.prepare_shared_base(selected.front(),selected);const auto writes=dense_ram->writes;
  ready(dense,bounded);require(bounded.progress().phase==Phase::declined && bounded.progress().owned_payload_bytes==0,"oversize private candidate retained/admitted");
  require(!dense.finish_shared_base(bounded).applied && dense_ram->writes==writes,"oversize candidate wrote backing");
}
void read_failure(bool indexed) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram,indexed);populate(writer,4);
  const auto keys=selections(writer.snapshot(),4);auto work=writer.prepare_shared_base(keys.front(),keys);const auto base=family(0);
  unsigned matches=0;for(auto at=ram->live.begin();;) {
    at=std::search(at,ram->live.end(),base.begin(),base.begin()+32);if(at==ram->live.end())break;at[17]^=std::byte{1};++matches;++at;
  }
  require(matches>=2,"missing synthetic payload copies");const auto writes=ram->writes;
  rejects([&]{writer.advance_shared_base(work);},"bad source bytes prepared");
  require(writer.poisoned() && work.progress().phase==Phase::failed && work.progress().owned_payload_bytes==0 && ram->writes==writes,
      "failed read kept work/writer active or wrote backing");
}
void publication_faults(std::uint32_t unit,bool indexed) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  {auto writer=fresh(original,indexed);populate(writer,4);file_apply(writer,file_create("noise"));}
  const auto keys=selections(NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(original,limits)),4);
  auto resume=[&](const auto& ram){const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
    return NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),generation,limits,{StoreCompressionMode::raw,1,false}));};
  auto prepare=[&](auto& writer){auto work=writer.prepare_shared_base(keys.front(),keys);writer.advance_shared_base(work);
    file_apply(writer,file_write("noise",0,Bytes{std::byte{91}}));ready(writer,work);return work;};
  auto probe=image(original->stable,unit);std::size_t cuts=0;
  {auto writer=resume(probe);auto work=prepare(writer);const auto start=probe->calls;require(writer.finish_shared_base(work).applied,"fault probe declined");cuts=probe->calls-start;}
  require(cuts>3 && cuts<256,"publication fault budget");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto ram=image(original->stable,unit);
    {auto writer=resume(ram);auto work=prepare(writer);ram->failure=mode;ram->fail_at=ram->calls+cut;
      rejects([&]{writer.finish_shared_base(work);},"faulted group publication acknowledged");
      require(writer.poisoned() && work.progress().phase==Phase::failed && work.progress().owned_payload_bytes==0,"failed publication retained active state");}
    ram->live=ram->stable;ram->fail_at=(std::numeric_limits<std::size_t>::max)();
    auto snapshot=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,limits));verify(snapshot,4);
    require(snapshot.read("noise",0,1)==Bytes{std::byte{91}},"group recovery lost unrelated acknowledged write");
    const auto info=snapshot.shared_base_info();require((info.members==0 && info.bases==0)||(info.members==4 && info.bases==1),"partial prepared group recovered");
    auto continued=resume(ram);file_apply(continued,file_create("continued"));++faults;
  }
  std::cout<<"staged_group_faults indexed="<<indexed<<" unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
}
int main(){try {
  for(const bool indexed:{false,true}) {
    for(auto unit:{4096U,65536U}) {
      foreground(unit,8,indexed);
      publication_faults(unit,indexed);
    }
    ownership(indexed);
    read_failure(indexed);
    for(unsigned phase=0;phase<3;++phase)
      for(unsigned action=0;action<(indexed?9U:5U);++action)staleness(action,phase,indexed);
  }
  foreground(4096,255,false);
  foreground(4096,64,true);
  unchanged_and_budget();
  std::cout<<"PASS: "<<checks<<" staged shared-base checks; "<<faults<<" injected publication cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
