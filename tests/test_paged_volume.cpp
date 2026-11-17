#include "flitzfs/native_session.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "record_digest.hpp"
#include <iostream>
namespace {
using namespace flitzfs;using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
constexpr NativeImageStorage storage{NativeImageKind::paged,kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F&& action,const char* reason){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(reason);}
StoreIdentity id(unsigned n){auto value=identity();value[0]=static_cast<std::byte>(n);return value;}
auto ram(std::uint32_t unit){auto result=std::make_shared<Ram>();result->failure_unit=unit;return result;}
std::string image_digest(std::span<const std::byte> bytes) {
  constexpr char hex[]="0123456789abcdef";
  std::string result;
  for (const auto byte : detail::record_digest(bytes)) {
    const auto value=std::to_integer<unsigned>(byte);
    result.push_back(hex[value>>4]);
    result.push_back(hex[value&15]);
  }
  return result;
}
Bytes family(unsigned n){auto bytes=data(kNativeExtentBytes,714);if(n){bytes[n*719%bytes.size()]^=std::byte{41};bytes[(n*1409+170)%bytes.size()]^=std::byte{77};}return bytes;}
NativeFilesystemSnapshot view(const std::shared_ptr<Ram>& image){return NativeFilesystemSnapshot::open_image(image,storage);}
void verify(const NativeFilesystemSnapshot& snapshot,unsigned count){for(unsigned i=0;i<count;++i)
  require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"paged volume bytes differ");snapshot.verify_all();}
void populate(NativeFileSession& session,unsigned count){for(unsigned i=0;i<count;++i){const auto path="f"+std::to_string(i);
  session.create_entry(path);auto handle=session.open(path,{true,true,false});session.write(handle,0,family(i));session.cleanup(handle,false);session.close(handle);}}
auto selections(const NativeFilesystemSnapshot& snapshot,unsigned count){std::vector<NativeExtentSelection> result;
  for(unsigned i=0;i<count;++i)result.push_back({snapshot.stat("f"+std::to_string(i)).inode,0});return result;}
void group(NativeFileSession& session,unsigned count){const auto keys=selections(session.snapshot(),count);
  auto work=session.prepare_shared_base(keys.front(),keys);unsigned steps=0;
  while(!session.advance_shared_base(work))require(++steps<512,"session group stalled");
  require(session.finish_shared_base(work).applied,"session group did not publish");}
struct Fixture {
  std::shared_ptr<Ram> head,source,target,area;std::optional<NativeFileSession> session;
  NativeFileHandle handle;
  explicit Fixture(std::uint32_t unit,unsigned count=4):head(ram(unit)),source(ram(unit)),target(ram(unit)),area(ram(unit)) {
    session.emplace(NativeFileSession::create_paged_volume(head,id(90),source,id(1),limits,{StoreCompressionMode::raw,1,false}));
    populate(*session,count);handle=session->open("f0",{true,true,false});
  }
  NativeVolumeImageResolver resolver(){return [this](const auto& which)->std::shared_ptr<StoreByteIo>{if(which==id(1))return source;if(which==id(2))return target;return {};};}
  NativeVolumeHeadView selected(){return NativeVolumeHeadView::open(head,id(90),resolver());}
  void retire(){session->cleanup(handle,false);session->close(handle);session.reset();}
};
void lifecycle(std::uint32_t unit) {
  Fixture f(unit);const auto format=f.session->volume_info().selection;
  require(format.storage==storage && f.head->live[8]==std::byte{2} && f.head->live[96]==std::byte{1},"paged head descriptor missing");
  require(f.session->snapshot().shared_base_info().enabled && f.session->workload_info().enabled,"paged features not active");
  const auto before_info=f.source->read_bytes;
  const auto inode=f.session->info(f.handle).info.inode;
  const auto inode_bytes=f.source->read_bytes-before_info;
  const auto before_lookup=f.source->read_bytes;
  const auto found=f.session->lookup("f0");
  const auto lookup_bytes=f.source->read_bytes-before_lookup;
  require(found.state.info.inode==inode && found.state.info.logical_bytes==kNativeExtentBytes &&
      inode_bytes>0 && lookup_bytes==inode_bytes,"paged lookup repeated namespace reads or lost inode metadata");
  std::cout<<"paged_volume_lookup unit="<<unit<<" inode_query_bytes="<<inode_bytes
      <<" path_query_bytes="<<lookup_bytes<<'\n';
  f.session->create_budgeted_optimization_area(f.area,4);const auto before=f.session->workload_info();
  const auto settled=f.session->optimization_snapshot().state.settled_input_bytes;
  group(*f.session,4);require(f.session->read(f.handle,0,kNativeExtentBytes)==family(0),"open context lost after grouping");
  require(f.session->workload_info()==before && f.session->optimization_snapshot().state.settled_input_bytes==settled,"group minted input/credit");
  auto proposal=f.session->optimization_snapshot().state;proposal.profiles.push_back({7,100,50,1,0,1,1,1000000});
  require(f.session->publish_budgeted_optimization_state(proposal).progress==NativeOptimizationBudgetProgress::deferred,"group minted advisory entitlement");
  f.session->create_entry("noise");auto noise=f.session->open("noise",{true,true,false});const Bytes extra(131072,std::byte{29});
  f.session->write(noise,0,extra);f.session->cleanup(noise,false);f.session->close(noise);
  require(f.session->publish_budgeted_optimization_state(proposal).progress==NativeOptimizationBudgetProgress::published,"real paged input did not fund advice");
  const auto workload=f.session->workload_info();const auto advisory=f.session->optimization_snapshot();const auto old_source=f.source->live;
  const auto keys=selections(f.session->snapshot(),4);auto pending=f.session->prepare_shared_base(keys.front(),keys);
  auto unused=ram(unit);const auto unused_calls=unused->calls;const auto unused_reads=unused->read_calls;
  require(!f.session->begin_idle_recompression(unused,id(5)) && unused->calls==unused_calls && unused->read_calls==unused_reads,
      "legacy idle route touched paged target");
  rejects([&]{f.session->begin_recompression(unused,id(5));},"legacy staged writer accepted paged source");
  rejects([&]{f.session->compact_volume(unused,id(5),{StoreCompressionMode::raw,1,false},{true});},"flat compact option accepted for paged volume");
  require(!f.session->poisoned() && unused->extent()==0,"invalid option retired source/touched target");
  const auto selected=f.session->compact_volume(f.target,id(2),{StoreCompressionMode::raw,1,false});
  require(selected.selection.epoch==2 && selected.selection.storage==storage && f.source->live==old_source,"paged handoff changed source/descriptor");
  require(f.session->read(f.handle,0,kNativeExtentBytes)==family(0) && f.session->workload_info()==workload,"handoff lost context/workload");
  require(f.session->optimization_snapshot().sequence==advisory.sequence && f.session->optimization_snapshot().state==advisory.state,"handoff reset advice/credit");
  require(f.selected().filesystem().read("noise",0,extra.size())==extra,"selected image lost unrelated input");verify(f.selected().filesystem(),4);
  const auto calls=f.target->calls,reads=f.target->read_calls;
  rejects([&]{f.session->advance_shared_base(pending);},"handoff revived prior group owner");
  require(f.target->calls==calls && f.target->read_calls==reads && !f.session->poisoned(),"expired group performed I/O/fenced new writer");
  f.retire();f.session.emplace(NativeFileSession::resume_volume(f.head,id(90),2,f.resolver(),{StoreCompressionMode::raw,1,false}));
  f.handle=f.session->open("f0",{true,true,false});
  f.session->resume_budgeted_optimization_area(f.area,advisory.sequence,4);
  require(f.session->workload_info()==workload && f.session->optimization_snapshot().state==advisory.state,"resume reset provenance/advice");
  proposal=f.session->optimization_snapshot().state;++proposal.profiles[0].cpu_ns;
  require(f.session->publish_budgeted_optimization_state(proposal).progress==NativeOptimizationBudgetProgress::deferred,"resume reissued spent credit");
  f.retire();std::cout<<"paged_volume_lifecycle unit="<<unit<<" selected_epoch=2\n";
  std::cout<<"paged_volume_wire unit="<<unit<<" head="<<image_digest(f.head->live)
      <<" source="<<image_digest(f.source->live)<<" target="<<image_digest(f.target->live)<<'\n';
}
void namespace_inventory() {
  Fixture f(4096,130);group(*f.session,130);require(f.session->snapshot().storage_info().objects>256,"large inventory did not cross old bound");
  f.session->recompress_volume(f.target,id(2),{StoreCompressionMode::raw,1,false});verify(f.selected().filesystem(),130);f.retire();
}
void redigest(Bytes& bytes,std::size_t offset){const auto hash=detail::record_digest(std::span<const std::byte>(bytes).subspan(offset,kStoreCommitUnit-32));
  std::copy(hash.begin(),hash.end(),bytes.begin()+offset+kStoreCommitUnit-32);}
void malformed_heads() {
  Fixture f(4096);f.retire();
  for(unsigned variant=0;variant<8;++variant) {
    auto bytes=f.head->stable;
    if(variant==0)bytes[8]=std::byte{3};if(variant==1)bytes[96]=std::byte{0};if(variant==2)bytes[100]=std::byte{1};
    if(variant==3)std::fill_n(bytes.begin()+104,8,std::byte{0});if(variant==4)bytes[112]=std::byte{1};
    if(variant==5){std::fill_n(bytes.begin()+104,8,std::byte{0});bytes[104]=std::byte{1};}
    if(variant==6)bytes[105]^=std::byte{1};if(variant==7)bytes[8]=std::byte{1};
    redigest(bytes,0);auto head=image(bytes,4096);
    rejects([&]{NativeVolumeHeadView::open(head,id(90),f.resolver());},"intact bad descriptor rolled back/selected");
  }
  rejects([&]{NativeFilesystemSnapshot::open_image(f.source,{});},"paged image silently retried as flat");
  auto head=ram(4096),source=ram(4096);const auto before=head->calls+source->calls;
  rejects([&]{NativeFileSession::create_paged_volume(head,id(90),source,id(1),{});},"zero capacity admitted");
  require(head->calls+source->calls==before,"invalid capacity wrote backing");
}
std::vector<std::size_t> base_offsets(const Bytes& bytes) {
  const auto base=family(0);std::vector<std::size_t> result;
  for(auto at=bytes.begin();;){at=std::search(at,bytes.end(),base.begin(),base.begin()+32);if(at==bytes.end())break;result.push_back(at-bytes.begin());++at;}
  require(result.size()==2,"unique mirrored base fixture missing");return result;
}
void surviving_repair(std::uint32_t unit) {
  auto original=ram(unit),source=ram(unit),head=ram(unit);
  {auto writer=NativeFilesystemWriter::initialize_shared_bases(PagedObjectStoreWriter::create_mirrored(original,id(1),limits,{StoreCompressionMode::raw,1,false}));
    for(unsigned i=0;i<4;++i)writer.apply(std::array{file_create("f"+std::to_string(i)),file_write("f"+std::to_string(i),0,family(i))});
    const auto keys=selections(writer.snapshot(),4);writer.compress_shared_base(keys.front(),keys);}
  {auto compact=NativeFilesystemWriter::compact(view(original),source,id(2),{StoreCompressionMode::raw,1,false});}
  {auto owner=NativeVolumeHeadWriter::create(head,id(90),view(source));}
  const auto offsets=base_offsets(source->stable);
  for(const auto offset:offsets) {
    auto bad=source->stable;bad[offset+17]^=std::byte{1};auto damaged=image(bad,unit),control=image(head->stable,unit),target=ram(unit);
    auto readable=view(damaged);verify(readable,4);
    require(readable.storage_info().degraded_checkpoint,"lost copy did not report degradation");
    rejects([&]{PagedObjectStoreWriter::resume(damaged,id(2),readable.storage_info().generation,limits);},"degraded paged data resumed writable");
    auto owner=NativeVolumeHeadWriter::resume(control,id(90),1,readable);
    {auto repaired=NativeFilesystemWriter::compact(readable,target,id(3),{StoreCompressionMode::raw,1,false});}
    const auto next=owner.publish_compacted(readable,view(target));require(next.selection.epoch==2 && damaged->live==bad,"repair selection damaged source");
    auto selected=NativeVolumeHeadView::open(control,id(90),[&](const auto& selected_id)->std::shared_ptr<StoreByteIo>{return selected_id==id(3)?target:damaged;});
    verify(selected.filesystem(),4);require(!selected.filesystem().storage_info().degraded_checkpoint,"repaired selection remains degraded");
  }
  for(const auto offset:offsets) {
    auto target=image(source->stable,unit); // A separate same-content image needs a new identity.
    auto rebuilt=ram(unit);{auto copied=NativeFilesystemWriter::compact(view(target),rebuilt,id(4),{StoreCompressionMode::raw,1,false});}
    const auto target_offsets=base_offsets(rebuilt->stable);const auto chosen=offset==offsets.front()?target_offsets.front():target_offsets.back();
    rebuilt->live[chosen+17]^=std::byte{1};rebuilt->stable=rebuilt->live;
    auto new_head=ram(unit);
    rejects([&]{NativeVolumeHeadWriter::create(new_head,id(91),view(rebuilt));},"new head admitted degraded initial image");
    require(new_head->calls==0,"degraded initial image wrote a new head");
    auto control=image(head->stable,unit);auto owner=NativeVolumeHeadWriter::resume(control,id(90),1,view(source));
    const auto bytes=control->live;const auto calls=control->calls;
    rejects([&]{owner.publish_compacted(view(source),view(rebuilt));},"degraded target selected");
    require(control->live==bytes && control->calls==calls && !owner.poisoned(),"bad target changed/fenced healthy head");
  }
}
void handoff_faults(std::uint32_t unit) {
  Fixture probe(unit);const auto source=probe.source->stable,head=probe.head->stable;
  const auto h=probe.head->calls,s=probe.source->calls,t=probe.target->calls;
  probe.session->compact_volume(probe.target,id(2),{StoreCompressionMode::raw,1,false});
  const std::array<std::size_t,3> cuts{probe.head->calls-h,probe.source->calls-s,probe.target->calls-t};probe.retire();
  for(unsigned backend=0;backend<3;++backend) {
    require(cuts[backend]<512,"handoff fault envelope");
    for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts[backend];++cut) {
      auto control=image(head,unit),data_io=image(source,unit),target=ram(unit);
      auto resolver=[&](const auto& which)->std::shared_ptr<StoreByteIo>{if(which==id(1))return data_io;if(which==id(2))return target;return {};};
      auto session=NativeFileSession::resume_volume(control,id(90),1,resolver,{StoreCompressionMode::raw,1,false});
      auto handle=session.open("f0",{true,true,false});const std::array<std::shared_ptr<Ram>,3> stores{control,data_io,target};
      auto selected=stores[backend];selected->failure=mode;selected->fail_at=selected->calls+cut;
      rejects([&]{session.compact_volume(target,id(2),{StoreCompressionMode::raw,1,false});},"faulted handoff acknowledged");
      require(session.poisoned(),"failed handoff stayed active");
      for(const auto& io:stores){io->live=io->stable;io->fail_at=(std::numeric_limits<std::size_t>::max)();}
      auto view=NativeVolumeHeadView::open(control,id(90),resolver);verify(view.filesystem(),4);verify(::view(data_io),4);
      const auto recovered=session.recover_volume();require((recovered.selection.epoch==1 || recovered.selection.epoch==2) && recovered.selection.storage==storage,
          "recovery selected foreign transition/format");
      require(session.read(handle,0,kNativeExtentBytes)==family(0),"recovery lost open context");
      session.cleanup(handle,false);session.close(handle);++faults;
    }
  }
  std::cout<<"paged_volume_faults unit="<<unit<<" head_cuts="<<cuts[0]<<" source_cuts="<<cuts[1]<<" target_cuts="<<cuts[2]<<" modes=3\n";
}
}
int main(){try{for(auto unit:{4096U,65536U}){lifecycle(unit);surviving_repair(unit);handoff_faults(unit);}namespace_inventory();malformed_heads();
  std::cout<<"PASS: "<<checks<<" managed paged-volume checks; "<<faults<<" injected handoff cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
