#include "flitzfs/volume_head.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "record_digest.hpp"
#include <iostream>
#include <map>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;using namespace native_fixture;
unsigned checks=0,cases=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn){++checks;try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected head rejection");}
template<class F>void rejects_code(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected exact volume head error");
}
StoreIdentity id(unsigned n){auto v=identity();v[0]=static_cast<std::byte>(n);return v;}
NativeFilesystemSnapshot view(std::shared_ptr<Ram> r){return NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(std::move(r)));}
std::shared_ptr<Ram> seed(StoreCompressionMode mode,bool compact) {
  auto r=std::make_shared<Ram>();r->failure_unit=compact?4096:65536;
  auto w=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(r,id(1),{mode,6,false},{compact}));
  const std::array<NativeFileMutation,8> ops{{file_create("d",true),file_create("d/f"),
      file_write("d/f",0,data(73,1)),file_create("sparse"),
      file_write("sparse",kNativeMaxFileBytes-80,data(17,2)),file_truncate("sparse",kNativeMaxFileBytes-1),
      file_create("keep"),file_write("keep",0,data(37,3))}};
  const std::array<NativeBasicMutation,2> basic{{{1,0,{1,2,3,4,0x10}},{0,2,{11,12,13,14,0x20}}}};
  const std::array<NativeSecurityMutation,2> security{{{1,0,native_security_fixture::descriptor()},
      {0,2,native_security_fixture::descriptor(2)}}};
  w.apply_with_metadata(ops,basic,security);
  file_apply(w,file_write("d/f",0,data(73,9)));file_apply(w,file_write("d/f",0,data(73,1)));
  return r;
}
std::shared_ptr<Ram> copied(std::shared_ptr<Ram> source,StoreIdentity next,StoreCompressionMode mode,bool compact) {
  auto out=std::make_shared<Ram>();out->failure_unit=compact?4096:65536;
  {auto w=NativeFilesystemWriter::compact(view(source),out,next,{mode,6,false},{compact});}
  return out;
}
struct Fixture {
  std::shared_ptr<Ram> source,target,head;StoreCompressionMode mode;bool compact;
  Fixture(StoreCompressionMode m,bool c):source(seed(m,c)),target(copied(source,id(2),m,c)),
      head(std::make_shared<Ram>()),mode(m),compact(c) {
    head->failure_unit=c?4096:65536;
    auto w=NativeVolumeHeadWriter::create(head,id(90),view(source));
    require(source->live.size()+target->live.size()+head->live.size()<=3U*1024*1024,"combined handoff fixture cap");
    require(target->live.size()<source->live.size(),"compaction did not remove source history");
  }
  NativeVolumeHeadView open() {
    return NativeVolumeHeadView::open(head,id(90),[&](const auto& selected)->std::shared_ptr<StoreByteIo>{
      if(selected==id(1))return source;if(selected==id(2))return target;return {};
    });
  }
};
void expected(const NativeFilesystemSnapshot& v,bool continued=false) {
  v.verify_all();
  require(v.stat("d/f").inode==3&&v.read("d/f",0,100)==data(73,1)&&v.read("keep",0,100)==data(37,3),
      "handoff lost stable IDs or acknowledged bytes");
  require(v.stat("sparse").logical_bytes==kNativeMaxFileBytes-1&&
      v.read("sparse",kNativeMaxFileBytes-80,17)==data(17,2)&&v.read("sparse",1000,19)==Bytes(19),
      "sparse handoff contents changed");
  require(v.basic_metadata(3)==NativeBasicMetadata{11,12,13,14,0x20}&&
      v.security_descriptor(3)==native_security_fixture::descriptor(2),"handoff lost basic/security metadata");
  if(continued)require(v.read("continued",0,100)==data(71,7),"selected image continuation lost");
}
void reset(std::shared_ptr<Ram> r){r->calls=0;r->writes=0;r->flushes=0;r->write_bytes=0;r->fail_at=(std::numeric_limits<std::size_t>::max)();}
void redigest(Bytes& b,unsigned slot) {
  const auto begin=slot*kStoreCommitUnit;
  const auto d=detail::record_digest(std::span<const std::byte>(b).subspan(begin,kStoreCommitUnit-32));
  std::copy(d.begin(),d.end(),b.begin()+begin+kStoreCommitUnit-32);
}
void invariants(StoreCompressionMode mode,bool compact) {
  Fixture f(mode,compact);auto source=view(f.source),target=view(f.target);
  auto w=NativeVolumeHeadWriter::resume(f.head,id(90),1,source);
  const auto old=f.source->live;const auto writes=f.source->writes,flushes=f.source->flushes;
  auto old_view=f.open();auto info=w.publish_compacted(source,target);
  require(info.selection.epoch==2&&info.selection.volume_identity==id(90)&&
      info.selection.image_identity==id(2)&&info.selection.parent_identity==id(1)&&!info.degraded,"wrong published selection");
  require(f.head->extent()==kVolumeHeadBytes&&f.source->live==old&&
      f.source->writes==writes&&f.source->flushes==flushes,"publication changed source or head footprint");
  expected(old_view.filesystem());auto current=f.open();expected(current.filesystem());
  require(current.filesystem().identity()==id(2)&&old_view.filesystem().identity()==id(1),"head used wrong image");
  rejects([&]{auto second=NativeVolumeHeadWriter::resume(f.head,id(90),2,target);});
  rejects([&]{w.publish_compacted(source,target);});require(!w.poisoned(),"wrong source poisoned healthy owner");
  unsigned resolved=0;
  rejects([&]{NativeVolumeHeadView::open(f.head,id(90),[&](const auto&)->std::shared_ptr<StoreByteIo>{++resolved;return f.source;});});
  require(resolved==1,"reader silently tried another image");
  // Valid digest with unsupported framing must NOT fall back to intact peer.
  auto malformed=image(f.head->stable);malformed->live[8]=std::byte{9};redigest(malformed->live,0);
  rejects([&]{NativeVolumeHeadView::open(malformed,id(90),[&](const auto&){return f.target;});});
  // Two intact selectors of the same epoch must agree.
  malformed=image(f.head->stable);malformed->live[40]=std::byte{77};redigest(malformed->live,0);
  rejects([&]{NativeVolumeHeadView::open(malformed,id(90),[&](const auto&){return f.target;});});
  // Either native metadata root may be lost without violating the minimum
  // FIRST checkpoint bound. A healthy head must still select that SAME image.
  for(unsigned lost=0;lost<2;++lost) {
    auto damaged=image(f.target->stable,compact?4096:65536);
    std::fill_n(damaged->live.begin()+lost*kStoreCommitUnit,4096,std::byte{});
    auto read=NativeVolumeHeadView::open(f.head,id(90),[&](const auto&){return damaged;});
    expected(read.filesystem());
    require(read.filesystem().storage_info().degraded_checkpoint,"image metadata loss hidden");
    auto control=image(f.head->stable);
    rejects([&]{auto unsafe=NativeVolumeHeadWriter::resume(control,id(90),2,view(damaged));});
    require(control->calls==0,"head repair used unrepaired image metadata");
  }
  auto unknown=std::make_shared<Ram>();unknown->failure_unit=0;
  rejects([&]{auto denied=NativeVolumeHeadWriter::create(unknown,id(90),source);});
  require(!unknown->calls,"unknown head contract wrote bytes");
  auto stale=std::make_shared<Ram>();
  {auto native=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(stale,id(2),{mode,6,false}));}
  rejects([&]{NativeVolumeHeadView::open(f.head,id(90),[&](const auto&){return stale;});});
  auto stale_control=image(f.head->stable);const auto fences=f.target->flushes;
  rejects([&]{auto wrong=NativeVolumeHeadWriter::resume(stale_control,id(90),1,target);});
  require(!stale_control->calls&&f.target->flushes==fences,"stale epoch caused a write or image fence");
  auto exhausted=image(f.head->stable);
  for(unsigned slot=0;slot<2;++slot) {
    for(unsigned i=0;i<8;++i)exhausted->live[slot*kStoreCommitUnit+16+i]=std::byte{0xff};
    redigest(exhausted->live,slot);
  }
  exhausted->stable=exhausted->live;
  auto full=NativeVolumeHeadWriter::resume(exhausted,id(90),(std::numeric_limits<std::uint64_t>::max)(),target);
  const auto calls=exhausted->calls;
  auto third=copied(f.target,id(3),mode,compact);
  rejects_code([&]{full.publish_compacted(target,view(third));},NativeFsErrorCode::limit);
  require(exhausted->calls==calls&&!full.poisoned(),"epoch exhaustion touched storage or fenced healthy head");
  // A changed target is not an equivalent compaction candidate.
  Fixture changed(mode,compact);
  {auto latest=view(changed.target);auto edit=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(
      changed.target,id(2),latest.storage_info().generation,{mode,6,false}));
    file_apply(edit,file_write("d/f",0,data(73,8)));}
  auto h=NativeVolumeHeadWriter::resume(changed.head,id(90),1,view(changed.source));
  const auto before=changed.head->stable;const auto target_flushes=changed.target->flushes;
  rejects([&]{h.publish_compacted(view(changed.source),view(changed.target));});
  require(!h.poisoned()&&changed.head->stable==before&&changed.target->flushes==target_flushes,
      "non-equivalent candidate changed durable head or target");
  {
    Fixture advanced(mode,compact);
    {auto latest=view(advanced.source);
      auto active=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(advanced.source,id(1),
          latest.storage_info().generation,{mode,6,false}));
      const std::array<NativeFileMutation,2> ops{{file_create("acknowledged"),file_write("acknowledged",0,data(81,5))}};
      active.apply(ops);
    }
    auto owner=NativeVolumeHeadWriter::resume(advanced.head,id(90),1,view(advanced.source));
    rejects([&]{owner.publish_compacted(view(advanced.source),view(advanced.target));});
    require(!owner.poisoned()&&advanced.open().filesystem().read("acknowledged",0,100)==data(81,5),
        "stale candidate lost a later acknowledged transaction");
  }
  {
    // Handoff is not a one-shot copy: later acknowledged data must survive
    // either head loss, either image-root loss, and another compacted image.
    Fixture later(mode,compact);
    auto owner=NativeVolumeHeadWriter::resume(later.head,id(90),1,view(later.source));
    owner.publish_compacted(view(later.source),view(later.target));
    {
      auto latest=view(later.target);
      auto active=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(later.target,id(2),
          latest.storage_info().generation,{mode,6,false}));
      const std::array<NativeFileMutation,2> ops{{file_create("continued"),file_write("continued",0,data(71,7))}};
      active.apply(ops);
    }
    auto retained=view(later.target);expected(retained,true);
    for(unsigned head_peer=0;head_peer<2;++head_peer)for(unsigned image_peer=0;image_peer<2;++image_peer) {
      auto control=image(later.head->stable),data_image=image(later.target->stable,compact?4096:65536);
      std::fill_n(control->live.begin()+head_peer*kStoreCommitUnit,kStoreCommitUnit,std::byte{});
      std::fill_n(data_image->live.begin()+image_peer*kStoreCommitUnit,4096,std::byte{});
      auto recovered=NativeVolumeHeadView::open(control,id(90),[&](const auto&){return data_image;});
      require(recovered.info().degraded&&recovered.info().selection.epoch==2&&
          recovered.info().selection.image_identity==id(2),"head loss selected historical image");
      expected(recovered.filesystem(),true);
    }
    auto next=copied(later.target,id(3),mode,compact);
    owner.publish_compacted(view(later.target),view(next));
    require(owner.info().selection.epoch==3&&later.head->extent()==kVolumeHeadBytes,
        "repeated handoff did not keep fixed authority footprint");
    auto latest=NativeVolumeHeadView::open(later.head,id(90),[&](const auto&){return next;});
    expected(latest.filesystem(),true);expected(retained,true);
    require(latest.info().selection.image_identity==id(3),"third image not selected");
  }
  // A live native writer is not accepted as quiescent source.
  Fixture busy(mode,compact);auto v=view(busy.source);
  auto image_owner=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(
      busy.source,id(1),v.storage_info().generation,{mode,6,false}));
  rejects([&]{auto denied=NativeVolumeHeadWriter::resume(busy.head,id(90),1,v);});
  expected(busy.open().filesystem());
}
void continue_selected(Fixture& f) {
  auto read=f.open();expected(read.filesystem());const auto selected=read.info().selection;
  auto active=selected.image_identity==id(1)?f.source:f.target;
  auto active_view=view(active);
  auto owner=NativeVolumeHeadWriter::resume(f.head,id(90),selected.epoch,active_view);
  require(!owner.info().degraded,"head repair did not restore both selectors");
  {auto native=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(active,selected.image_identity,
      active_view.storage_info().generation,{f.mode,6,false}));
    const std::array<NativeFileMutation,2> ops{{file_create("continued"),file_write("continued",0,data(71,7))}};
    native.apply(ops);}
  expected(f.open().filesystem(),true);require(f.head->extent()==kVolumeHeadBytes,"head grew during continuation");
}
void trace(std::string_view phase,StoreCompressionMode mode,bool compact,bool head,std::size_t steps,unsigned peer=0) {
  std::cout<<"Native volume-head evidence: {\"schema\":1,\"trace_only\":true,\"phase\":\""<<phase
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"domain\":\""<<(head?"head":"image")
      <<"\",\"lost_peer\":"<<peer<<",\"io_steps\":"<<steps<<"}\n";
}
void publish_faults(StoreCompressionMode mode,bool compact) {
  for(bool on_head:{true,false}) {
    Fixture probe(mode,compact);auto w=NativeVolumeHeadWriter::resume(probe.head,id(90),1,view(probe.source));
    reset(probe.head);reset(probe.target);w.publish_compacted(view(probe.source),view(probe.target));
    const auto steps=(on_head?probe.head:probe.target)->calls;require(steps>0,"missing handoff I/O trace");
    trace("publish",mode,compact,on_head,steps);
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
      Fixture f(mode,compact);const auto original=f.source->stable;
      {
        auto h=NativeVolumeHeadWriter::resume(f.head,id(90),1,view(f.source));
        reset(f.head);reset(f.target);auto fault=on_head?f.head:f.target;
        fault->failure=failure;fault->fail_at=step;
        auto baseline=view(f.source),candidate=view(f.target);
        rejects([&]{h.publish_compacted(baseline,candidate);});
        require(h.poisoned()&&h.info().selection.epoch==0,"failed head publication returned usable authority");
        const auto count=f.head->calls,reads=f.head->read_calls+f.source->read_calls+f.target->read_calls;
        rejects([&]{h.publish_compacted(baseline,candidate);});
        require(f.head->calls==count&&reads==f.head->read_calls+f.source->read_calls+f.target->read_calls,
            "terminal head writer retried backing I/O");
        rejects([&]{auto competing=NativeVolumeHeadWriter::resume(f.head,id(90),1,baseline);});
        require(f.head->calls==count&&reads==f.head->read_calls+f.source->read_calls+f.target->read_calls,
            "poisoned owner released its exclusive head lease");
      }
      require(f.source->stable==original,"handoff failure changed source");
      f.head=image(f.head->stable);f.target=image(f.target->stable,compact?4096:65536);
      auto read=f.open();require(read.info().selection.epoch==1||read.info().selection.epoch==2,"invalid crash selection");
      expected(read.filesystem());continue_selected(f);++cases;
    }
  }
}
void create_faults(StoreCompressionMode mode,bool compact) {
  for(bool on_head:{true,false}) {
    auto source=seed(mode,compact),head=std::make_shared<Ram>();head->failure_unit=compact?4096:65536;
    reset(source);{auto h=NativeVolumeHeadWriter::create(head,id(90),view(source));}
    const auto steps=(on_head?head:source)->calls;require(steps>0,"missing initial I/O trace");
    trace("create",mode,compact,on_head,steps);
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
      auto data_image=seed(mode,compact),control=std::make_shared<Ram>();control->failure_unit=compact?4096:65536;
      const auto original=data_image->stable;reset(data_image);auto fault=on_head?control:data_image;
      fault->failure=failure;fault->fail_at=step;
      rejects([&]{auto h=NativeVolumeHeadWriter::create(control,id(90),view(data_image));});
      require(data_image->stable==original,"failed initial head changed source bytes");
      control=image(control->stable);
      bool complete=false;
      try{auto v=NativeVolumeHeadView::open(control,id(90),[&](const auto&){return data_image;});expected(v.filesystem());complete=true;}
      catch(const NativeFsError& e){require(e.code()==NativeFsErrorCode::corrupt,"unexpected initial recovery failure");}
      if(complete)require(control->extent()==kVolumeHeadBytes,"partial initial head exposed");++cases;
    }
  }
}
void repair_faults(StoreCompressionMode mode,bool compact) {
  for(unsigned lost=0;lost<2;++lost)for(bool on_head:{true,false}) {
    Fixture probe(mode,compact);{auto w=NativeVolumeHeadWriter::resume(probe.head,id(90),1,view(probe.source));w.publish_compacted(view(probe.source),view(probe.target));}
    std::fill_n(probe.head->live.begin()+lost*kStoreCommitUnit,kStoreCommitUnit,std::byte{});probe.head->stable=probe.head->live;
    reset(probe.head);reset(probe.target);
    {auto h=NativeVolumeHeadWriter::resume(probe.head,id(90),2,view(probe.target));}
    const auto steps=(on_head?probe.head:probe.target)->calls;require(steps>0,"missing head repair trace");
    trace("repair",mode,compact,on_head,steps,lost);
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
      Fixture f(mode,compact);{auto h=NativeVolumeHeadWriter::resume(f.head,id(90),1,view(f.source));h.publish_compacted(view(f.source),view(f.target));}
      std::fill_n(f.head->live.begin()+lost*kStoreCommitUnit,kStoreCommitUnit,std::byte{});f.head->stable=f.head->live;
      reset(f.head);reset(f.target);auto fault=on_head?f.head:f.target;fault->failure=failure;fault->fail_at=step;
      rejects([&]{auto h=NativeVolumeHeadWriter::resume(f.head,id(90),2,view(f.target));});
      f.head=image(f.head->stable);f.target=image(f.target->stable,compact?4096:65536);
      require(f.open().info().selection.epoch==2&&f.open().info().selection.image_identity==id(2),
          "repair fell back to previous image");continue_selected(f);++cases;
    }
  }
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("head test phase required");const std::string_view phase=argv[1];
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
    if(phase=="--invariants")invariants(mode,compact);
    else if(phase=="--create")create_faults(mode,compact);
    else if(phase=="--publish")publish_faults(mode,compact);
    else if(phase=="--repair")repair_faults(mode,compact);
    else throw std::runtime_error("unknown head test phase");
  }
  std::cout<<"Native volume-head evidence: {\"schema\":1,\"phase\":\""<<phase<<"\",\"checks\":"<<checks
      <<",\"fault_cases\":"<<cases<<",\"head_bytes\":"<<kVolumeHeadBytes
      <<",\"files_created\":false,\"mounted\":false,\"gpu\":false,\"complete\":true}\n";return 0;
}catch(const std::exception& e){std::cerr<<"Volume head failure: "<<e.what()<<"\n";return 1;}}
