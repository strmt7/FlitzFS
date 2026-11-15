#include "native_fs_fixture.hpp"
#include <iostream>
#include <set>
namespace {
using namespace flitzfs;
using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F> void rejects(F&& action,const char* why) {
  ++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
StoreIdentity next_id(){auto id=identity();id[7]=std::byte{54};return id;}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,unsigned layout) {
  auto objects=PagedObjectStoreWriter::create(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  if(layout==1)return NativeFilesystemWriter::initialize_inode_records(std::move(objects));
  if(layout==2)return NativeFilesystemWriter::initialize_indexed_names(std::move(objects));
  return NativeFilesystemWriter::initialize(std::move(objects));
}
std::uint64_t io(const Ram& a,const Ram& b){return a.read_calls+b.read_calls+a.calls+b.calls;}
void staged_native(unsigned layout,std::uint32_t unit,bool reencode) {
  auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=unit;
  auto writer=fresh(source,layout);writer.apply(baseline());
  const auto original=source->live;const auto calls=source->calls;
  auto first=writer.prepare_paged_recompression(target,next_id(),{StoreCompressionMode::automatic,1,false},reencode);
  require(target->extent()==0 && target->calls==0,"preparation wrote candidate");
  auto candidate=std::move(first);require(first.progress().phase==PagedRewritePhase::failed,"moved candidate stayed active");
  auto extra=std::make_shared<Ram>();extra->failure_unit=unit;const auto before_extra=io(*source,*extra);
  rejects([&]{writer.prepare_paged_recompression(extra,next_id());},"multiple candidates admitted for one source");
  require(io(*source,*extra)==before_extra,"duplicate candidate performed I/O");
  rejects([&]{writer.finish_paged_recompression(candidate);},"premature finish accepted");
  rejects([&]{PagedObjectStoreWriter::create(target,next_id(),limits);},"candidate did not lease target");
  std::set<PagedRewritePhase> phases;unsigned steps=0;std::uint64_t maximum_bytes=0;
  while(candidate.progress().phase!=PagedRewritePhase::ready) {
    const auto before=candidate.progress();phases.insert(before.phase);
    const auto read_bytes=source->read_bytes+target->read_bytes,write_bytes=target->write_bytes;
    writer.advance_paged_recompression(candidate);
    const auto after=candidate.progress();
    if(before.phase==after.phase)require(after.completed_in_phase-before.completed_in_phase<=1,"step processed more than one object");
    const auto bytes=source->read_bytes+target->read_bytes-read_bytes+target->write_bytes-write_bytes;
    maximum_bytes=(std::max)(maximum_bytes,bytes);
    require(bytes<=8ULL*1024*1024,"bounded step exceeded fixture I/O envelope");
    if(after.phase==PagedRewritePhase::verifying_source)require(target->extent()==0,"source verification wrote target");
    if(after.phase==PagedRewritePhase::copying || after.phase==PagedRewritePhase::verifying_target)
      require(std::all_of(target->live.begin(),target->live.begin()+2*kStoreCommitUnit,
          [](auto byte){return byte==std::byte{0};}),"roots published before candidate verification");
    require(writer.read_inode(writer.inspect_path("stable.bin").info.inode,0,100)==data(31,18),"foreground read failed between steps");
    require(++steps<256,"staged copy made no forward progress");
  }
  require(phases.size()==5,"candidate skipped a verification/publication phase");
  const auto before_finish=io(*source,*target);auto copied=writer.finish_paged_recompression(candidate);
  require(io(*source,*target)==before_finish,"finish hid a full backing scan or barrier");
  require(candidate.progress().phase==PagedRewritePhase::failed,"finished candidate remained usable");
  expected(copied.snapshot(),false);require(copied.snapshot().identity()==next_id(),"native root identity not rebound");
  auto independent=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(target,limits));expected(independent,false);
  file_apply(copied,file_write("stable.bin",0,Bytes{std::byte{77}}));
  require(writer.snapshot().read("stable.bin",0,100)==data(31,18),"candidate changed source");
  require(source->live==original && source->calls==calls,"staged operation wrote or flushed source");
  std::cout<<"staged_native layout="<<layout<<" unit="<<unit<<" reencode="<<reencode<<" steps="<<steps
      <<" maximum_step_io_bytes="<<maximum_bytes<<'\n';
}
void stale_phases(unsigned layout) {
  for(const auto phase:{PagedRewritePhase::verifying_source,PagedRewritePhase::copying,
      PagedRewritePhase::verifying_target,PagedRewritePhase::publishing,PagedRewritePhase::verifying_publication,PagedRewritePhase::ready}) {
    auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=4096;
    auto writer=fresh(source,layout);writer.apply(baseline());auto candidate=writer.prepare_paged_recompression(target,next_id());
    unsigned steps=0;while(candidate.progress().phase!=phase) {
      writer.advance_paged_recompression(candidate);require(++steps<256,"stale phase unreachable");
    }
    file_apply(writer,file_create("foreground"));const auto before=io(*source,*target),extent=target->extent();
    rejects([&]{writer.advance_paged_recompression(candidate);},"stale candidate continued");
    require(candidate.progress().phase==PagedRewritePhase::failed && io(*source,*target)==before && target->extent()==extent,
        "stale path performed I/O or stayed active");
    require(writer.snapshot().stat("foreground").inode!=0,"stale copy lost foreground mutation");
  }
}
void cancellation_and_owner() {
  auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=4096;
  {
    auto writer=fresh(source,2);writer.apply(baseline());
    {auto canceled=writer.prepare_paged_recompression(target,next_id());}
    auto reusable=PagedObjectStoreWriter::create(target,next_id(),limits);
  }
  target=std::make_shared<Ram>();target->failure_unit=4096;
  std::optional<PagedObjectStoreWriter::Rewrite> candidate;
  {
    auto objects=PagedObjectStoreSnapshot::open(source,limits);
    auto writer=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(source,identity(),objects.info().generation,limits));
    candidate.emplace(writer.prepare_paged_recompression(target,next_id()));
  }
  auto objects=PagedObjectStoreSnapshot::open(source,limits);
  auto reopened=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(source,identity(),objects.info().generation,limits));
  const auto before=io(*source,*target);
  rejects([&]{reopened.advance_paged_recompression(*candidate);},"new owner reused old candidate");
  require(io(*source,*target)==before,"expired owner reached backing");
  auto reusable=PagedObjectStoreWriter::create(target,next_id(),limits);
}
void generic_maximum_and_empty() {
  for(bool populated:{false,true}) {
    auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=4096;
    auto writer=PagedObjectStoreWriter::create(source,identity(),limits,{StoreCompressionMode::raw,1,false});
    const auto bytes=data(kStoreMaxObjectBytes,29);
    if(populated){const StoreMutation item{700,false,bytes};writer.commit({&item,1});}
    auto original=writer.snapshot();const auto reads=source->read_bytes;
    auto candidate=writer.prepare_rebuild(target,next_id(),{StoreCompressionMode::raw,1,false});
    require(source->read_bytes-reads<=16384,"prepare hid a source inventory scrub");
    require(!candidate.replaces_native_root(),"generic rebuild claims native rebinding");
    auto moved=std::move(writer);unsigned steps=0;
    while(!candidate.advance(moved))require(++steps<32,"generic rebuild stalled");
    const auto before=io(*source,*target);auto copy=candidate.finish(moved);
    require(io(*source,*target)==before,"generic finish performed backing I/O");
    require(copy.snapshot().info().objects==(populated?1U:0U),"generic inventory differs");
    if(populated)require(copy.snapshot().read(700)==bytes && original.read(700)==bytes,"maximum object bytes differ");
  }
}
void admission_and_read_failures() {
  auto source=std::make_shared<Ram>();source->failure_unit=4096;auto writer=fresh(source,2);writer.apply(baseline());
  const auto original=source->live;const auto calls=source->calls;
  for(bool contract:{false,true}) {
    auto target=std::make_shared<Ram>();target->failure_unit=4096;
    auto candidate=writer.prepare_paged_recompression(target,next_id());
    if(contract)target->failure_unit=65536;else target->live.push_back(std::byte{0});
    const auto before=io(*source,*target);
    rejects([&]{writer.advance_paged_recompression(candidate);},"changed target extent/contract admitted");
    require(io(*source,*target)==before && candidate.progress().phase==PagedRewritePhase::failed,
        "bad target transition performed data I/O or retained resources");
  }
  auto target=std::make_shared<Ram>();target->failure_unit=4096;
  Bytes known(kNativeExtentBytes);const auto bytes=data(31,18);std::copy(bytes.begin(),bytes.end(),known.begin());
  const auto at=std::search(source->live.begin(),source->live.end(),known.begin(),known.end());
  require(at!=source->live.end(),"source integrity fixture not found");
  auto candidate=writer.prepare_paged_recompression(target,next_id());
  source->corrupt_read_offset=std::distance(source->live.begin(),at);source->corrupt_read_number=1;
  rejects([&]{while(!writer.advance_paged_recompression(candidate)){};},"source integrity failure accepted");
  require(target->extent()==0 && candidate.progress().phase==PagedRewritePhase::failed,"source refusal wrote target");
  source->corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)();
  {
    auto canceled=writer.prepare_paged_recompression(target,next_id());
    while(!writer.advance_paged_recompression(canceled)){}
  }
  const auto objects=PagedObjectStoreSnapshot::open(target,limits);
  auto reused=PagedObjectStoreWriter::resume(target,next_id(),objects.info().generation,limits);
  require(source->live==original && source->calls==calls,"admission/read failure altered source");
}
void failures(unsigned layout,std::uint32_t unit) {
  auto source=std::make_shared<Ram>();source->failure_unit=unit;auto writer=fresh(source,layout);writer.apply(baseline());
  const auto original=source->live;const auto source_calls=source->calls;
  auto run=[&](const std::shared_ptr<Ram>& target) {
    auto candidate=writer.prepare_paged_recompression(target,next_id(),{StoreCompressionMode::automatic,1,false});
    while(!writer.advance_paged_recompression(candidate)){}
    return writer.finish_paged_recompression(candidate);
  };
  auto probe=std::make_shared<Ram>();probe->failure_unit=unit;{auto result=run(probe);expected(result.snapshot(),false);}
  const auto cuts=probe->calls;require(cuts>3 && cuts<160,"staged failure envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto target=std::make_shared<Ram>();target->failure_unit=unit;target->failure=mode;target->fail_at=cut;
    rejects([&]{run(target);},"failed staged target acknowledged");
    require(source->live==original && source->calls==source_calls && !writer.poisoned(),"target failure changed/poisoned source");
    target->live=target->stable;target->fail_at=(std::numeric_limits<std::size_t>::max)();
    std::optional<PagedObjectStoreSnapshot> exposed;
    try{exposed=PagedObjectStoreSnapshot::open(target,limits);}catch(const std::exception&){}
    if(exposed){auto independent=NativeFilesystemSnapshot::open(std::move(*exposed));expected(independent,false);}
    ++faults;
  }
  for(bool root_slot:{false,true}) {
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto candidate=writer.prepare_paged_recompression(target,next_id());unsigned steps=0;
    const auto desired=root_slot?PagedRewritePhase::publishing:PagedRewritePhase::verifying_target;
    while(candidate.progress().phase!=desired){writer.advance_paged_recompression(candidate);require(++steps<256,"corruption phase unreachable");}
    if(root_slot)target->corrupt_read_offset=0;
    else {
      const std::array magic{std::byte{'F'},std::byte{'L'},std::byte{'T'},std::byte{'X'},std::byte{'P'},std::byte{'G'},std::byte{'N'},std::byte{'1'}};
      const auto at=std::search(target->live.begin(),target->live.end(),magic.begin(),magic.end());
      require(at!=target->live.end(),"missing staged page fixture");target->corrupt_read_offset=std::distance(target->live.begin(),at);
    }
    target->corrupt_read_number=1;
    rejects([&]{writer.advance_paged_recompression(candidate);},"bad root/page verification acknowledged");
    require(candidate.progress().phase==PagedRewritePhase::failed,"failed verification remained active");
  }
  require(source->live==original && source->calls==source_calls,"staged failures changed source");
  file_apply(writer,file_create("continued"));require(writer.snapshot().stat("continued").inode!=0,"foreground could not continue");
  std::cout<<"staged_faults layout="<<layout<<" unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
}
int main(){try {
  generic_maximum_and_empty();cancellation_and_owner();admission_and_read_failures();
  for(unsigned layout=0;layout<3;++layout) {
    stale_phases(layout);
    for(auto unit:{4096U,65536U}) {
      for(bool reencode:{false,true})staged_native(layout,unit,reencode);
      failures(layout,unit);
    }
  }
  std::cout<<"PASS: "<<checks<<" staged paged checks; "<<faults<<" injected write/barrier cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
