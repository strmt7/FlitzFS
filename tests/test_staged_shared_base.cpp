#include "../src/native_group_anchor.hpp"
#include "../src/native_group_codec.hpp"
#include "anchor_search_fixture.hpp"
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
void staged_search_fallback() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  auto writer=fresh(ram,true);populate(writer,4);
  const auto keys=selections(writer.snapshot(),4);
  auto work=writer.prepare_shared_base(keys.back(),keys);
  require(!writer.stage_shared_base_search(work,2),"staging skipped private base verification");
  writer.advance_shared_base(work);
  require(writer.stage_shared_base_search(work,2) && !work.search_window(),"partial window became visible");
  const auto after_stage=io(*ram);
  require(!writer.stage_shared_base_search(work,1) && io(*ram)==after_stage,"reduced grant staged another member");
  writer.advance_shared_base(work);
  require(io(*ram)==after_stage && work.progress().completed_members==1 && !work.search_window(),
      "CPU fallback reread a staged member or exposed a partial window");
  require(!writer.stage_shared_base_search(work,0),"zero grant allocated search input");
  ready(writer,work);
  require(writer.finish_shared_base(work).applied,"abandoned advice blocked CPU publication");
  verify(writer.snapshot(),4);
}
void staged_packed_xor_advice() {
  constexpr unsigned count=4;
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  auto writer=fresh(ram,true);
  const auto base=family(0);
  std::array<Bytes,count> originals;
  for(unsigned i=0;i<count;++i) {
    originals[i]=base;
    if(i) {
      const auto variation=data(kNativeExtentBytes,1061+i);
      for(std::size_t j=0;j<base.size();++j)originals[i][j]^=variation[j]&std::byte{1};
    }
    const auto path="f"+std::to_string(i);
    writer.apply(std::array{file_create(path),file_write(path,0,originals[i])});
  }
  const auto before=writer.snapshot();
  const auto keys=selections(before,count);
  auto work=writer.prepare_shared_base(keys.front(),keys);
  writer.advance_shared_base(work);
  for(unsigned i=1;i<count;++i)require(writer.stage_shared_base_search(work,count-1),"XOR advice staging");
  const auto window=work.search_window();
  require(window.members().size()==count-1,"XOR reply window shape");
  std::vector<Bytes> bodies(count-1);
  for(unsigned i=0;i<2;++i) {
    const auto frame=detail::encode_native_group_xor(window.members()[i],base,detail::kNativeGroupBaseBit+1,1,
        kNativeExtentBytes);
    require(frame && frame->size()>128,"XOR advice fixture body");
    bodies[i].assign(frame->begin()+128,frame->end());
  }
  // A valid body for the wrong member must select the independent CPU encoder.
  bodies[1]=bodies[0];
  std::array<NativePackedXorCheck,count-1> outcomes{};
  const NativeSharedBaseSearchReply reply{window,{},bodies,outcomes};
  const auto inspect=[&]{return detail::inspect_anchor_xor_checks(std::span(&reply,1));};
  using Status=detail::AnchorXorBatchStatus;
  require(inspect().status==Status::pending,"unconsumed XOR advice was counted as verified");
  require(!writer.advance_shared_base(work,{},&reply) && !writer.advance_shared_base(work,{},&reply) &&
      outcomes[0]==NativePackedXorCheck::verified && inspect().status==Status::pending,
      "one successful comparison verified the entire batch");
  while(!writer.advance_shared_base(work,{},&reply)) {}
  require(outcomes[1]==NativePackedXorCheck::invalid && outcomes[2]==NativePackedXorCheck::pending &&
      inspect().status==Status::invalid && work.progress().selected_advice_members==1,
      "wrong XOR advice did not report its integrity fault and select CPU encoding");
  bodies[1].clear();
  const auto confirmed=inspect();
  require(confirmed.status==Status::verified && confirmed.verified_groups==1,
      "empty slots prevented completion after genuine byte verification");
  bodies[0].clear();
  require(inspect().status==Status::empty,"an empty result was counted as a verified batch");
  require(writer.finish_shared_base(work).applied,"verified XOR advice did not reach publication");
  const auto after=writer.snapshot();
  after.verify_all();
  for(unsigned i=0;i<count;++i) {
    const auto path="f"+std::to_string(i);
    require(before.read(path,0,kNativeExtentBytes)==originals[i] &&
        after.read(path,0,kNativeExtentBytes)==originals[i],"XOR advice changed acknowledged bytes");
  }
}
void staged_search_binding() {
  constexpr unsigned count=5,base_index=2;
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  auto writer=fresh(ram,true);
  const auto base=family(0);
  std::vector<Bytes> original;
  for(unsigned i=0;i<count;++i) {
    original.push_back(i==base_index?base:anchor_search_fixture::fragmented(base,i+1));
    const auto path="f"+std::to_string(i);
    writer.apply(std::array{file_create(path),file_write(path,0,original.back())});
  }
  const auto before=writer.snapshot();
  const auto keys=selections(before,count);
  const auto prepare=[&] {
    auto work=writer.prepare_shared_base(keys[base_index],keys);
    writer.advance_shared_base(work);
    const auto calls=ram->calls;
    for(unsigned i=1;i<count;++i) {
      require(writer.stage_shared_base_search(work,count-1),"admitted search failed to stage");
      require(work.progress().completed_members==0 && ram->calls==calls &&
          bool(work.search_window())==(i==count-1),"search staged outside a private member step");
    }
    return work;
  };
  std::vector<std::vector<NativeSharedBaseCopyRange>> copies;
  Bytes contiguous;
  for(unsigned i=0;i<count;++i)if(i!=base_index) {
    copies.push_back(anchor_search_fixture::fragment_ranges(i+1));
    contiguous.insert(contiguous.end(),original[i].begin(),original[i].end());
  }
  auto old=prepare();
  const std::vector<Bytes> old_xor(count-1,Bytes{std::byte{1}});
  std::array<NativePackedXorCheck,count-1> old_checks{};
  NativeSharedBaseSearchReply prior{old.search_window(),copies,old_xor,old_checks};
  old.cancel();
  require(!old.search_window() && std::ranges::equal(prior.window.base(),base),"cancel changed leased source bytes");
  auto wrong=writer.prepare_shared_base(keys[base_index],keys);
  writer.advance_shared_base(wrong);
  const auto retained=io(*ram);
  require(!writer.stage_shared_base_search(wrong,count-1) && io(*ram)==retained,
      "the writer staged a second window while the consumer retained the first");
  require(writer.advance_shared_base(wrong,{},&prior) && wrong.progress().phase==Phase::declined &&
      wrong.progress().selected_advice_members==0 &&
      std::ranges::all_of(old_checks,[](auto check){return check==NativePackedXorCheck::pending;}),
      "reply from retired work influenced a new group or received a verdict");
  prior.window={};
  auto work=prepare();
  NativeSharedBaseSearchReply reply{work.search_window(),copies};
  const std::array<std::uint32_t,1> counts{count-1};
  const auto quantum=anchor_match::max_members;
  const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),count+1);
  detail::AnchorMatchBatchBuilder staged(1,plan),flat(1,plan);
  staged.append(1,reply.window);flat.append(1,base,contiguous);
  const auto staged_request=staged.finish(),flat_request=flat.finish();
  require(std::ranges::equal(staged_request.snapshot()->bytes(),flat_request.snapshot()->bytes()),
      "staged member ordering changed worker bytes");
  const auto no_io=io(*ram);
  while(!writer.advance_shared_base(work,{},&reply)) {}
  require(io(*ram)==no_io && work.progress().selected_advice_members==count-1 && !work.search_window(),
      "consumption reread staged data or used advice at the wrong position");
  require(writer.finish_shared_base(work).applied,"verified staged advice did not publish");
  const auto after=writer.snapshot();
  after.verify_all();
  for(unsigned i=0;i<count;++i) {
    const auto path="f"+std::to_string(i);
    require(before.read(path,0,kNativeExtentBytes)==original[i] &&
        after.read(path,0,kNativeExtentBytes)==original[i],"staged advice changed acknowledged bytes");
  }
  reply.window={};
  auto invalid=prepare();
  const NativeSharedBaseSearchReply obsolete{invalid.search_window(),copies};
  file_apply(writer,file_write("f1",0,Bytes{original[1][0]^std::byte{1}}));
  const auto edited=io(*ram);
  require(!invalid.search_window(),"selected write left a searchable window");
  rejects([&]{writer.advance_shared_base(invalid,{},&obsolete);},"selected write accepted an obsolete window");
  require(io(*ram)==edited && !writer.poisoned() && invalid.progress().owned_payload_bytes==0,
      "rejection touched the image or retained its prepared payload");
}
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
int main(int argc,char** argv){try {
  if(argc==2 && std::string_view(argv[1])=="--search-window") {
    staged_search_fallback();
    staged_search_binding();
    staged_packed_xor_advice();
    std::cout<<"PASS: "<<checks<<" staged search window checks\n";
    return 0;
  }
  if(argc!=1)throw std::invalid_argument("unknown staged shared-base test mode");
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
