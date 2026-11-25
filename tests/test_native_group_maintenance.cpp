#include "flitzfs/indexed_coherence.hpp"
#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/read_only_file.hpp"
#include "../src/native_group_anchor.hpp"
#include "../src/native_group_codec.hpp"
#include "../src/native_indexed_volume.hpp"
#include "../src/native_maintenance_source.hpp"
#include "../src/native_name_index.hpp"
#include "../src/record_digest.hpp"
#include "anchor_search_fixture.hpp"
#include "native_fs_fixture.hpp"
#include "native_corpus_fixture.hpp"
#include <cstring>
#include <iostream>
#include <map>
#include <set>
namespace {
using namespace flitzfs;using namespace native_fixture;
using Phase=NativeMaintenancePhase;using Defer=NativeMaintenanceDeferral;
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);}
StoreIdentity id(unsigned n){auto result=identity();result[0]=static_cast<std::byte>(n);return result;}
auto ram(std::uint32_t unit){auto result=std::make_shared<Ram>();result->failure_unit=unit;return result;}
Bytes family(unsigned n){auto bytes=data(kNativeExtentBytes,714);if(n){bytes[n*719%bytes.size()]^=std::byte{41};bytes[(n*1409+170)%bytes.size()]^=std::byte{77};}return bytes;}
void put(NativeFileSession& session,std::string path,const Bytes& bytes){session.create_entry(path);auto handle=session.open(path,{true,true,false});
  session.write(handle,0,bytes);session.cleanup(handle,false);session.close(handle);}
struct Clock {
  std::uint64_t sequence=0,now=1000;
  NativeMaintenanceObservation next(){NativeMaintenanceObservation o;o.sequence=++sequence;o.captured_ms=now+=1000;
    o.physical_total=16ULL<<30;o.physical_available=12ULL<<30;o.commit_limit=32ULL<<30;o.commit_available=24ULL<<30;
    o.cpu_total_ticks=1000;o.cpu_busy_ticks=0;o.cpu_window_ms=1000;o.valid=true;return o;}
};
struct Fixture {
  std::shared_ptr<Ram> head,image;std::optional<NativeFileSession> session;Clock clock;
  explicit Fixture(std::uint32_t unit,unsigned count=8,bool indexed=false,
      StoreCompressionOptions compression={StoreCompressionMode::raw,1,false},
      NativeIndexedDataLayout layout=NativeIndexedDataLayout::extents,
      NativeVolumeHeadLayout head_layout=NativeVolumeHeadLayout::legacy,
      PagedStoreBankLayout image_layout=PagedStoreBankLayout::shared):head(ram(unit)),image(ram(unit)) {
    if(indexed)session.emplace(NativeFileSession::create_indexed_volume(head,id(90),image,id(1),{kStoreMaxPhysicalBytes},
        compression,NativeDispositionPolicy::aggregate,{},NativeVolumePlacement::external,layout,head_layout,image_layout));
    else session.emplace(NativeFileSession::create_paged_volume(head,id(90),image,id(1),{kStoreMaxPhysicalBytes},compression));
    for(unsigned i=0;i<count;++i)put(*session,"f"+std::to_string(i),family(i));
  }
  Fixture(std::shared_ptr<Ram> selector,std::shared_ptr<Ram> backing,
      StoreCompressionOptions compression={StoreCompressionMode::raw,1,false})
      :head(std::move(selector)),image(std::move(backing)) {
    session.emplace(NativeFileSession::resume_closed_indexed_volume(head,id(90),1,
        [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?image:nullptr;},
        compression));
  }
  auto step(NativeGroupMaintenance& controller){const auto observed=clock.next();return controller.advance(observed,clock.now);}
  auto finish(NativeGroupMaintenance& controller){for(unsigned i=0;i<6000;++i){const auto result=step(controller);
    require(result.phase!=Phase::failed,"controller failed");if(result.phase==Phase::idle)return result;}throw std::runtime_error("controller did not quiesce");}
  void verify(unsigned count){const auto snapshot=session->snapshot();for(unsigned i=0;i<count;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"automatic grouping changed bytes");snapshot.verify_all();}
};

void indexed_cold_batch_copy() {
  if(!store_zstd_available()) {
    std::cout<<"indexed_cold_copy skipped=zstd_unavailable\n";
    return;
  }
  constexpr StoreCompressionOptions foreground{StoreCompressionMode::automatic,1,true};
  constexpr std::size_t file_count=192,file_bytes=3072;
  const PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  const auto corpus=cold_test_payload();
  Fixture f(4096,0,true,foreground);
  const auto path=[](std::size_t number){return "short-"+std::to_string(number);};
  const auto content=[&](std::size_t number){return std::span(corpus).subspan(number*file_bytes,file_bytes);};
  for(std::size_t i=0;i<file_count;++i) {
    const auto bytes=content(i);
    put(*f.session,path(i),Bytes(bytes.begin(),bytes.end()));
  }
  const auto retained=f.session->snapshot();
  const auto source_objects=PagedObjectStoreSnapshot::open(f.image,limits);
  const auto source=source_objects.info();
  const auto records=source_objects.list_page(0,static_cast<std::size_t>(source.objects));
  const auto cold_begin=std::find_if(records.begin(),records.end(),[](const auto& record) {
    return record.id>detail::kNativeIndexedDataObject;
  });
  require(cold_begin!=records.end(),"short-file cold copy has no data records");
  const auto first_cold=static_cast<std::uint64_t>(std::distance(records.begin(),cold_begin));
  const auto target=ram(4096);
  require(!f.session->idle_compaction_progress(),"no idle copy should be visible before staging");
  using Progress=NativeFileSession::RecompressionProgress;
  {
    Fixture cut(image(f.head->stable,4096),image(f.image->stable,4096),foreground);
    const auto interrupted=ram(4096);
    require(cut.session->begin_idle_compaction(interrupted,id(5),PagedRewriteMode::reconcile_foreground),
        "interrupted cold rewrite did not start");
    for(unsigned i=0;i<4000;++i) {
      const auto progress=cut.session->idle_compaction_progress();
      require(progress.has_value(),"interrupted cold rewrite lost its progress");
      if(progress->phase==PagedRewritePhase::copying && progress->completed_in_phase>=first_cold)break;
      require(cut.session->advance_recompression()==Progress::copying,"cold rewrite skipped its private copy");
    }
    const auto progress=cut.session->idle_compaction_progress();
    require(progress && progress->phase==PagedRewritePhase::copying && progress->completed_in_phase>=first_cold &&
        progress->completed_in_phase<records.size() &&
        records[static_cast<std::size_t>(progress->completed_in_phase)].id<2*detail::kNativeIndexedDataObject,
        "cold rewrite did not reach its private data copy");
    interrupted->fail_at=interrupted->calls;
    interrupted->failure=1;
    rejects([&]{(void)cut.session->advance_recompression();},"torn cold batch did not fail");
    ++faults;
    require(!cut.session->idle_compaction_progress(),"failed cold batch survived");
    const auto old_image=image(cut.image->stable,4096);
    const auto old=NativeVolumeHeadView::open(image(cut.head->stable,4096),id(90),
        [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?old_image:nullptr;});
    const auto first=content(0);
    require(old.info().selection.image_identity==id(1) &&
        old.filesystem().read(path(0),0,file_bytes)==Bytes(first.begin(),first.end()),
        "private cold batch failure changed acknowledged selection or data");
    old.filesystem().verify_all();
  }
  require(f.session->begin_idle_compaction(target,id(5),PagedRewriteMode::reconcile_foreground),
      "short-file cold rewrite did not start");
  std::uint64_t copy_steps=0,copy_reads=0,copy_writes=0,copy_write_bytes=0;
  auto result=Progress::copying;
  for(unsigned i=0;i<4000 && result==Progress::copying;++i) {
    const auto progress=f.session->idle_compaction_progress();
    require(progress.has_value(),"staged idle copy lost its progress");
    const auto reads=f.image->read_calls+target->read_calls;
    const auto writes=target->writes;
    const auto written=target->write_bytes;
    result=f.session->advance_recompression();
    if(progress->phase==PagedRewritePhase::copying) {
      ++copy_steps;
      copy_reads+=f.image->read_calls+target->read_calls-reads;
      copy_writes+=target->writes-writes;
      copy_write_bytes+=target->write_bytes-written;
    }
  }
  require(result==Progress::ready,"short-file cold rewrite never became ready");
  require(copy_steps<file_count/2,"eligible short files bypassed bounded batch copy");
  require(f.session->publish_idle_recompression().has_value(),"short-file cold rewrite did not publish");
  require(!f.session->idle_compaction_progress(),"published candidate was still reported as active");
  const auto target_objects=PagedObjectStoreSnapshot::open(target,limits).info();
  require(target_objects.objects==source.objects && target_objects.stored_payload_bytes<source.stored_payload_bytes,
      "cold copy did not reduce preserved object payloads");
  const auto reopened=NativeVolumeHeadView::open(f.head,id(90),
      [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(5)?target:key==id(1)?f.image:nullptr;});
  require(reopened.info().selection.image_identity==id(5),"CPU reopen did not select the cold copy");
  reopened.filesystem().verify_all();
  retained.verify_all();
  for(std::size_t i=0;i<file_count;++i) {
    const auto bytes=content(i);
    const Bytes expected(bytes.begin(),bytes.end());
    require(reopened.filesystem().read(path(i),0,file_bytes)==expected,"CPU reopen changed short-file data");
    require(retained.read(path(i),0,file_bytes)==expected,"cold copy changed pinned source data");
  }
  std::cout<<"indexed_cold_copy files="<<file_count<<" objects="<<target_objects.objects
      <<" copy_steps="<<copy_steps<<" copy_reads="<<copy_reads<<" copy_writes="<<copy_writes
      <<" copy_write_bytes="<<copy_write_bytes<<" source_payload="<<source.stored_payload_bytes
      <<" target_payload="<<target_objects.stored_payload_bytes<<" target_extent="<<target->extent()
      <<" selector_extent="<<f.head->extent()<<" exact_cpu=1 blake3=";
  constexpr std::string_view hex="0123456789abcdef";
  for(const auto value:detail::record_digest(target->live)) {
    const auto byte=std::to_integer<unsigned>(value);
    std::cout<<hex[byte>>4]<<hex[byte&15U];
  }
  std::cout<<'\n';
}

void indexed_candidate_yield_order() {
  if(!store_zstd_available())return;
  constexpr StoreCompressionOptions compression{StoreCompressionMode::automatic,3,true};
  Fixture f(4096,0,true,compression);
  std::array<Bytes,5> compact;
  std::array<Bytes,3> extensive;
  const auto tile=data(8192,28971);
  for(std::size_t i=0;i<compact.size();++i) {
    auto& bytes=compact[i];
    for(std::size_t offset=0;offset<kNativeExtentBytes;offset+=tile.size())
      bytes.insert(bytes.end(),tile.begin(),tile.end());
    bytes.back()^=std::byte(i);
    put(*f.session,"compact-"+std::to_string(i),bytes);
  }
  for(std::size_t i=0;i<extensive.size();++i) {
    extensive[i]=family(static_cast<unsigned>(i));
    put(*f.session,"extensive-"+std::to_string(i),extensive[i]);
  }
  const auto before=f.session->snapshot();
  const auto pages=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes});
  const detail::IndexedVolumeView source(pages,pages.read(1));
  const auto incumbent=[&](const auto& files,std::string_view prefix) {
    std::uint64_t stored=0;
    for(std::size_t i=0;i<files.size();++i) {
      const auto token=source.content_at({source.resolve(std::string(prefix)+std::to_string(i)).inode,0});
      require(token && token->stored_payload_bytes,"candidate pricing missed the checked record size");
      stored+=token->stored_payload_bytes;
    }
    return stored;
  };
  require(incumbent(extensive,"extensive-")>incumbent(compact,"compact-"),
      "fixture's three-member group needs greater recoverable bytes than the five-member group");
  NativeGroupMaintenance groups(*f.session);
  NativeMaintenanceProgress first;
  for(unsigned i=0;i<1200 && !first.published_groups;++i) {
    first=f.step(groups);
    require(first.phase!=Phase::failed,"candidate ordering failed");
  }
  require(first.published_groups==1 && first.candidate_yield_observations>0 &&
      f.session->snapshot().shared_base_info().members==extensive.size(),
      "candidate ordering did not prioritize the higher-byte group");
  const auto settled=f.finish(groups);
  require(settled.published_groups>=2 && settled.candidate_yield_observations>first.candidate_yield_observations,
      "candidate learning displaced the smaller group");
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto after=NativeFilesystemSnapshot::open_image(f.image,storage);
  for(const auto* snapshot:{&before,&after}) {
    snapshot->verify_all();
    for(std::size_t i=0;i<compact.size();++i)
      require(snapshot->read("compact-"+std::to_string(i),0,kNativeExtentBytes)==compact[i],"compact family changed");
    for(std::size_t i=0;i<extensive.size();++i)
      require(snapshot->read("extensive-"+std::to_string(i),0,kNativeExtentBytes)==extensive[i],"extensive family changed");
  }
  std::cout<<"candidate_yield_orders_verified incumbent_bytes="<<incumbent(extensive,"extensive-")
      <<" first_members="<<extensive.size()<<" observations="<<settled.candidate_yield_observations<<'\n';
}

void indexed_section_reference_reuse() {
  {
    const auto base=data(kNativeExtentBytes,10609);
    constexpr std::size_t displacement=3;
    constexpr auto frame_bytes=detail::kNativeGroupCopyMinimumBytes+4+displacement;
    auto member=base;
    std::rotate(member.begin(),member.begin()+displacement,member.end());
    const detail::NativeGroupAnchorIndex index(base);
    const auto frame=index.encode(member,base,detail::kNativeGroupBaseBit+1,1,frame_bytes+1);
    require(frame && frame->size()==frame_bytes &&
        detail::decode_native_group_member(*frame,base,detail::NativeGroupMethod::anchor_copies)==member,
        "Indexed anchor lost the matching prefix before its first seed");
    require(!index.encode(member,base,detail::kNativeGroupBaseBit+1,1,frame_bytes),
        "An extended anchor exceeded its exclusive competing frame size");
  }
  constexpr unsigned count=8;
  constexpr auto section=kNativeExtentBytes/count;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  Fixture f(4096,0,true,raw);
  const auto common=data(kNativeExtentBytes,10609);
  std::vector<Bytes> originals;
  originals.reserve(count);
  for(unsigned i=0;i<count;++i) {
    auto bytes=common;
    const auto replacement=data(section,1777+181*i);
    std::ranges::copy(replacement,bytes.begin()+i*section);
    put(*f.session,"f"+std::to_string(i),bytes);
    originals.push_back(std::move(bytes));
  }
  auto added=common;
  added[555]^=std::byte{0x25};
  {
    NativeGroupMaintenance controller(*f.session);
    const auto initial=f.finish(controller);
    const auto grouped=f.session->snapshot().shared_base_info();
    require(initial.section_reference_groups==1 && grouped.bases==1 && grouped.members==count,
        "expected a protected assembled base for the initial related files");
    put(*f.session,"first-extension",common);
    const auto extended=f.finish(controller);
    const auto live=f.session->snapshot().shared_base_info();
    require(extended.published_groups==initial.published_groups+1 && live.bases==1 && live.members==count+1,
        "running scan failed to extend its assembled base");
  }
  const auto removed=f.session->open("f0",{true,true,true});
  f.session->set_delete(removed,true);
  f.session->cleanup(removed,true);
  f.session->close(removed);
  for(unsigned tick=0;tick<32 && f.session->retirement_pending();++tick)
    (void)f.session->reclaim_retired_inode();
  require(!f.session->retirement_pending(),"retired witness remained in the test index");
  const auto resolver=[&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.reset();
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolver,raw));
  put(*f.session,"reopened-extension",added);
  const auto footprint=[&](const NativeFilesystemSnapshot& source,unsigned target) {
    auto copy=ram(4096);
    (void)NativeFilesystemWriter::compact(source,copy,id(target),raw);
    const auto reopened=NativeFilesystemSnapshot::open_image(copy,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    return copy->extent();
  };
  f.session.reset();
  const auto before=footprint(NativeVolumeHeadView::open(f.head,id(90),resolver).filesystem(),2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolver,raw));
  NativeMaintenanceProgress extended;
  {
    NativeGroupMaintenance resumed(*f.session);
    extended=f.finish(resumed);
  }
  f.session.reset();
  const auto verified=NativeVolumeHeadView::open(f.head,id(90),resolver);
  const auto& snapshot=verified.filesystem();
  snapshot.verify_all();
  const auto grouped=snapshot.shared_base_info();
  require(extended.published_groups==1 && !extended.section_reference_groups &&
      grouped.bases==1 && grouped.members==count+1,"restart did not reuse the protected base through a surviving file");
  for(unsigned i=1;i<count;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"related file changed after reuse");
  require(snapshot.read("first-extension",0,kNativeExtentBytes)==common &&
      snapshot.read("reopened-extension",0,kNativeExtentBytes)==added,"added file changed after reuse");
  const auto after=footprint(snapshot,3);
  require(after<before,"reusing an assembled base did not save occupied bytes");
  std::cout<<"assembled_base_reuse groups="<<grouped.bases<<" members="<<grouped.members
      <<" before_compacted_bytes="<<before<<" after_compacted_bytes="<<after
      <<" removed_witness=true reopened=true independent_cpu_readback=true\n";
}

constexpr unsigned prefix_family_count=6;
std::vector<Bytes> prefix_family(const Bytes& base=data(kNativeExtentBytes,3819),unsigned count=prefix_family_count) {
  std::vector<Bytes> originals{base};
  for(unsigned member=1;member<count;++member)
    anchor_search_fixture::apply_prefix_revision(originals.emplace_back(base),base,member);
  return originals;
}

void indexed_prefix_bank_boundary() {
  if(!store_zstd_available())return;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  constexpr std::uint32_t unit=65536;
  constexpr std::size_t start=kNativeExtentBytes/4,token=8,tokens=8192/token;
  const auto base=data(kNativeExtentBytes,3819);
  std::array<Bytes,3> files{base,base,base};
  for(std::size_t file=1;file<files.size();++file) {
    for(std::size_t at=0;at<tokens;++at) {
      const auto from=(at*(2*file+7)+19*file)%tokens;
      std::copy_n(base.begin()+start+from*token,token,files[file].begin()+start+at*token);
    }
  }
  const auto unrelated=data(49152,77);
  const auto name=[](std::size_t file){return "f"+std::to_string(file);};
  Fixture f(unit,0,true,policy);
  for(std::size_t file=0;file<files.size();++file)put(*f.session,name(file),files[file]);
  put(*f.session,"incumbent",unrelated);
  const auto prior=f.session->snapshot();
  NativeMaintenanceProgress progress;
  {
    NativeGroupMaintenance maintenance(*f.session);
    progress=f.finish(maintenance);
  }
  f.session.reset();
  auto target=ram(unit);
  const auto source=NativeFilesystemSnapshot::open_image(f.image,storage);
  (void)NativeFilesystemWriter::compact(source,target,id(3),policy);
  const auto settled=NativeFilesystemSnapshot::open_image(target,storage);
  // The ordinary grouped frames occupy a fifth pair of banks on this image.
  require(progress.published_groups==1 && progress.prefixed_zstd_members==2 && target->extent()==8ULL*unit,
      "New prefixes did not release a pair of 64 KiB allocation banks");
  for(const auto* snapshot:{&prior,&settled}) {
    snapshot->verify_all();
    for(std::size_t file=0;file<files.size();++file)
      require(snapshot->read(name(file),0,kNativeExtentBytes)==files[file],"Prefix bank comparison changed file bytes");
    require(snapshot->read("incumbent",0,unrelated.size())==unrelated,"Prefix comparison changed the unrelated file");
  }
  std::cout<<"prefix_bank settled="<<target->extent()<<" selector="<<f.head->extent()
      <<" prefixes="<<progress.prefixed_zstd_members<<" cpu_exact=1 retained_snapshot_exact=1\n";
}

void indexed_relocation_reuses_samples() {
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,false};
  constexpr unsigned count=12;
  const auto name=[](unsigned file){return "relocated-"+std::to_string(file);};
  Fixture f(4096,0,true,policy);
  for(unsigned file=0;file<count;++file)put(*f.session,name(file),data(kNativeExtentBytes,4117+file));
  NativeGroupMaintenance maintenance(*f.session);
  const auto initial=f.finish(maintenance);
  require(initial.sampled_extents==count && !initial.published_groups,"Independent relocation sample set");
  const auto retained=f.session->snapshot();

  auto target=ram(4096);
  require(f.session->begin_idle_compaction(target,id(22),PagedRewriteMode::reconcile_foreground),
      "Could not start indexed relocation");
  using Recompression=NativeFileSession::RecompressionProgress;
  auto phase=Recompression::copying;
  for(unsigned steps=0;steps<1024 && phase==Recompression::copying;++steps)phase=f.session->advance_recompression();
  require(phase==Recompression::ready && f.session->publish_idle_recompression().has_value(),
      "Verified relocation did not publish");

  const auto changed=data(kNativeExtentBytes,8293);
  auto handle=f.session->open(name(0),{true,true,false});
  f.session->write(handle,0,changed);
  f.session->cleanup(handle,false);f.session->close(handle);

  const auto reads=target->read_bytes;
  const auto settled=f.finish(maintenance);
  const auto sampled=settled.sampled_extents-initial.sampled_extents,reused=settled.reused_features-initial.reused_features;
  std::cout<<"relocated_samples new="<<sampled<<" reused="<<reused<<" backing_read_bytes="<<target->read_bytes-reads<<'\n';
  require(sampled==1 && reused==count-1 && !settled.published_groups,
      "Relocation did not reuse exactly the unchanged samples");
  const auto active=f.session->snapshot();
  retained.verify_all();active.verify_all();
  for(unsigned file=0;file<count;++file) {
    const auto original=data(kNativeExtentBytes,4117+file);
    require(retained.read(name(file),0,original.size())==original &&
        active.read(name(file),0,original.size())==(file?original:changed),"Relocation sample reuse changed file bytes");
  }
}

void indexed_prefixed_reference() {
  using Method=detail::NativeGroupMethod;
  constexpr auto count=prefix_family_count;
  const auto originals=prefix_family();
  const auto& base=originals.front();
  const auto frame=detail::encode_native_group_prefix(originals[1],base,detail::kNativeGroupBaseBit+1,1,kNativeExtentBytes);
  if(!store_zstd_available()) {
    require(!frame,"codec-free build encoded a Zstandard prefix");
    return;
  }
  require(frame && detail::decode_native_group_member(*frame,base,Method::prefixed_zstd)==originals[1],
      "prefixed member did not round-trip");
  rejects([&]{(void)detail::decode_native_group_member(*frame,base,Method::sparse_xor);},"old wire profile accepted a prefixed member");
  rejects([&]{(void)detail::decode_native_group_member(*frame,originals[2],Method::prefixed_zstd);},"incorrect prefix identity was accepted");
  auto damaged=*frame;
  damaged.back()^=std::byte{1};
  rejects([&]{(void)detail::decode_native_group_member(damaged,base,Method::prefixed_zstd);},"damaged prefixed member was returned");
  auto padded=*frame;
  padded.push_back(std::byte{0});
  const auto payload=padded.size()-detail::kNativeGroupMemberHeaderBytes;
  for(unsigned i=0;i<4;++i)padded[100+i]=static_cast<std::byte>(payload>>(8*i));
  rejects([&]{(void)detail::parse_native_group_member(padded,Method::prefixed_zstd);},"prefixed member accepted trailing bytes");

  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  Fixture f(4096,0,true,policy);
  for(unsigned i=0;i<count;++i)put(*f.session,"f"+std::to_string(i),originals[i]);
  const auto verify=[&](const NativeFilesystemSnapshot& snapshot) {
    snapshot.verify_all();
    for(unsigned i=0;i<count;++i)
      require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"prefixed group changed an acknowledged extent");
  };
  const auto compacted=[&](const auto& source,unsigned number) {
    auto image=ram(4096);
    (void)NativeFilesystemWriter::compact(source,image,id(number),policy);
    const auto verified=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verified.verify_all();
    return image->extent();
  };
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.reset();
  const auto before=compacted(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  const auto head_bytes=f.head->stable,image_bytes=f.image->stable;
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  NativeGroupMaintenance maintenance(*f.session);
  const auto calls=f.image->calls;
  const auto progress=f.finish(maintenance);
  const auto operations=f.image->calls-calls;
  maintenance.cancel();
  f.session.reset();
  const auto reopened=NativeVolumeHeadView::open(f.head,id(90),resolve);
  const auto& selected=reopened.filesystem();
  verify(selected);
  const auto after=compacted(selected,3);
  require(progress.prefixed_zstd_members>0 && progress.published_groups==1 &&
      selected.shared_base_info().members==count && after<before,
      "normal grouped preparation did not select the smaller prefixed candidate");

  unsigned previous=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<operations;++cut) {
    auto head=image(head_bytes,4096),backing=image(image_bytes,4096);
    Fixture attempt(head,backing,policy);
    NativeGroupMaintenance pending(*attempt.session);
    backing->failure=mode;backing->fail_at=backing->calls+cut;
    for(unsigned step=0;step<1000 && pending.pending();++step)(void)attempt.step(pending);
    require(pending.progress().phase==Phase::failed && attempt.session->poisoned(),
        "interrupted prefixed publication left a writable owner");
    const auto recovered=NativeFilesystemSnapshot::open_image(image(backing->stable,4096),
        {NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verify(recovered);
    const auto members=recovered.shared_base_info().members;
    if(members==0)++previous;
    else if(members==count)++complete;
    else throw std::runtime_error("interrupted prefix transaction exposed an incomplete group");
  }
  require(previous && complete,"prefixed publication did not exercise both atomic outcomes");
  faults+=previous+complete;

  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  const auto witness=f.session->open("f0",{true,true,true});
  f.session->set_delete(witness,true);f.session->cleanup(witness,true);f.session->close(witness);
  const auto surviving=f.session->snapshot();
  surviving.verify_all();
  require(surviving.shared_base_info().bases==1 && surviving.shared_base_info().members==count-1,
      "removing a file released a still-referenced Zstandard prefix");
  for(unsigned i=1;i<count;++i)
    require(surviving.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"prefixed survivor changed");
  std::cout<<"indexed_prefixed_members="<<progress.prefixed_zstd_members
      <<" before_compacted_bytes="<<before<<" after_compacted_bytes="<<after<<" cpu_readback=true"
      <<" cuts="<<previous+complete<<" previous="<<previous<<" complete="<<complete<<"\n";
}

void indexed_prefix_extension(bool verify_improvement,const char* corpus=nullptr) {
  if(!store_zstd_available())return;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  auto seed=data(kNativeExtentBytes,3819);
  if(corpus) {
    const auto input=ReadOnlyFile::open(corpus);
    require(input.size()>=seed.size(),"Public prefix input needs one full extent");
    input.read_exact_at(0,seed.data(),seed.size());
  }
  const auto originals=prefix_family(seed,prefix_family_count+1);
  Fixture f(4096,0,true,policy);
  const auto name=[](unsigned file){return "f"+std::to_string(file);};
  for(unsigned i=0;i<prefix_family_count;++i)put(*f.session,name(i),originals[i]);
  NativeMaintenanceProgress initial;
  {
    NativeGroupMaintenance seed_groups(*f.session);
    initial=f.finish(seed_groups);
    const auto initial_groups=f.session->snapshot().shared_base_info();
    require(initial_groups.bases==1 && initial_groups.members==prefix_family_count,
        "The initial prefix family did not use one protected base");
  }

  const auto first_base=[&] {
    const auto pages=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes});
    const detail::IndexedVolumeView source(pages,pages.read(1));
    const auto sample=source.sample_content({source.resolve(name(0)).inode,0},
        detail::IndexedContentLookup::published_reference,{});
    require(sample.has_value(),"The original group lost its witness");
    require(sample->payload==originals.front() && sample->token.grouped,
        "The original group lost its linked base");
    const auto record=pages.read(sample->token.object);
    const auto reference=detail::read_native_group_header(
        std::span(record).subspan(detail::kIndexedExtentHeaderBytes),detail::NativeGroupMethod::prefixed_zstd);
    return std::pair{reference.base_id,reference.base_generation};
  };
  const auto original_base=first_base();
  put(*f.session,name(prefix_family_count),originals.back());
  const auto compacted=[&](const auto& source,unsigned number) {
    auto target=ram(4096);
    (void)NativeFilesystemWriter::compact(source,target,id(number),policy);
    const auto checked=NativeFilesystemSnapshot::open_image(target,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    checked.verify_all();
    for(unsigned i=0;i<originals.size();++i)
      require(checked.read(name(i),0,kNativeExtentBytes)==originals[i],"Prefix extension changed acknowledged data");
    return target->extent();
  };
  f.session.reset();
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  const auto before=compacted(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  NativeMaintenanceProgress extended;
  {
    NativeGroupMaintenance maintenance(*f.session);
    extended=f.finish(maintenance);
  }
  f.session.reset();
  const auto selected=NativeVolumeHeadView::open(f.head,id(90),resolve);
  selected.filesystem().verify_all();
  const auto final_groups=selected.filesystem().shared_base_info();
  const auto after=compacted(selected.filesystem(),3);
  require(final_groups.bases==1 && final_groups.members==originals.size() && after<before && first_base()==original_base,
      "The new revision did not extend the same protected base");
  const auto reused_prefixes=extended.prefixed_zstd_members;
  if(verify_improvement)require(reused_prefixes>0,"The established base did not try the smaller prefix representation");
  std::cout<<"indexed_prefix_extension public_input="<<bool(corpus)<<" bases="<<final_groups.bases
      <<" files="<<originals.size()<<" seeded_prefixes="<<initial.prefixed_zstd_members
      <<" reused_prefixes="<<reused_prefixes
      <<" before_compacted_bytes="<<before<<" after_compacted_bytes="<<after
      <<" same_base_identity=true cpu_reopen_exact=true\n";
}

void indexed_batched_prefix() {
  if(!store_zstd_available())return;
  constexpr auto count=prefix_family_count;
  constexpr StoreCompressionOptions compression{StoreCompressionMode::automatic,3,true};
  constexpr NativeMaintenancePolicy admission{};
  const auto originals=prefix_family();
  const auto name=[](unsigned file){return "f"+std::to_string(file);};

  // Find enough memory for the ordinary group before taking away the
  // additional reference and prefix scratch.
  std::uint64_t lower=0,upper=kStoreMaxTransactionBytes*16ULL;
  while(lower<upper) {
    const auto middle=lower+(upper-lower)/2;
    if(detail::maintenance_group_member_limit({middle,false},true)>=count)upper=middle;
    else lower=middle+1;
  }
  require(detail::maintenance_group_member_limit({lower,false},true)>=count,"ordinary group allowance");

  const auto trial=[&](bool advice,bool pressure) {
    Fixture f(4096,0,true,compression);
    for(unsigned file=0;file<count;++file)put(*f.session,name(file),originals[file]);
    NativeGroupMaintenance maintenance(*f.session,admission);
    NativeMaintenanceProgress progress;
    for(unsigned tick=0;tick<800 && !progress.search_batch_ready;++tick) {
      const auto observed=f.clock.next();
      progress=maintenance.advance(observed,f.clock.now,{count-1,nullptr,true});
      require(progress.phase!=Phase::failed && !progress.published_groups,"prepared group published early");
    }
    const auto windows=maintenance.search_batch();
    require(windows.size()==1 && windows.front().members().size()==count-1,"missing complete advisory window");
    const std::array sizes{count-1};
    const auto plan=detail::plan_anchor_match_batch(sizes,anchor_match::max_members,
        anchor_match::device_bytes(anchor_match::max_members),count);
    detail::AnchorMatchBatchBuilder builder(42,plan);
    builder.append(1,windows.front());
    const auto batch=builder.finish();
    auto copies=detail::verify_anchor_match_records(batch.groups().front(),
        detail::cpu_anchor_match_records(batch.groups().front()));
    const NativeSharedBaseSearchReply reply{windows.front(),copies};

    unsigned preparing=0;
    bool pressure_observed=false;
    const auto barriers=f.image->calls;
    for(unsigned tick=0;tick<800 && progress.phase!=Phase::idle;++tick) {
      auto observed=f.clock.next();
      if(pressure && preparing>=count+2) {
        if(!pressure_observed)
          require(f.image->calls==barriers,"optional comparison wrote before it had a final proposal");
        pressure_observed=true;
        observed.physical_available=observed.physical_total/16+lower*admission.memory_share_divisor;
      }
      progress=maintenance.advance(observed,f.clock.now,{0,advice?&reply:nullptr,true});
      require(progress.phase!=Phase::failed && progress.deferral!=Defer::memory,"ordinary staged group lost its grant");
      if(progress.phase==Phase::preparing && progress.deferral==Defer::none)++preparing;
    }
    require(progress.phase==Phase::idle && progress.published_groups==1 &&
        (!pressure || (pressure_observed && !progress.prefixed_zstd_members)) &&
        (pressure || progress.prefixed_zstd_members>0),"staged reference selection or grant withdrawal failed");
    f.session.reset();
    const auto source=NativeFilesystemSnapshot::open_image(f.image,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    auto compact=ram(4096);
    const auto rebuilt=NativeFilesystemWriter::compact(source,compact,id(4),compression).snapshot();
    rebuilt.verify_all();
    for(unsigned file=0;file<count;++file)
      require(rebuilt.read(name(file),0,kNativeExtentBytes)==originals[file],"staged alternative changed file bytes");
    return compact->extent();
  };
  const auto advised=trial(true,false),fallback=trial(false,false),limited=trial(true,true);
  require(advised==fallback && advised<limited,"staged prefix did not save space over the ordinary admitted route");
  std::cout<<"staged_prefix_advised="<<advised<<" staged_prefix_cpu="<<fallback
      <<" ordinary_after_pressure="<<limited<<" independent_cpu_readback=true\n";
}

Bytes uneven_short_pack_file(unsigned i,unsigned pairs) {
  constexpr std::size_t phrases=256,phrase_bytes=72;
  const bool small=i>=pairs;
  const auto dictionary=data(phrases*phrase_bytes,20151+29*(i%pairs));
  auto bytes=data((small?24:40)*1024,185311+307*i);
  for(std::size_t slot=0;slot<phrases;++slot) {
    const auto phrase=small?((627*slot+154)&(phrases-1)):slot;
    const auto from=dictionary.begin()+static_cast<std::ptrdiff_t>(phrase*phrase_bytes);
    const auto into=bytes.begin()+static_cast<std::ptrdiff_t>(slot*(bytes.size()/phrases)+3);
    std::copy_n(from,phrase_bytes,into);
  }
  bytes.back()=std::byte{0x67};
  return bytes;
}

void indexed_uneven_short_packs(bool regression) {
  if(!store_zstd_available())return;
  constexpr unsigned pairs=6,count=2*pairs;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  std::array<Bytes,count> inputs;
  const auto path=[](unsigned i){return "uneven-"+std::to_string(i);};
  Fixture f(4096,0,true,policy);
  for(unsigned i=0;i<count;++i) {
    inputs[i]=uneven_short_pack_file(i,pairs);
    put(*f.session,path(i),inputs[i]);
  }
  const auto verify=[&](const NativeFilesystemSnapshot& snapshot) {
    snapshot.verify_all();
    for(unsigned i=0;i<count;++i)
      require(snapshot.read(path(i),0,kNativeExtentBytes)==inputs[i],"uneven short packing changed file contents");
  };
  const auto compact=[&](const NativeFilesystemSnapshot& snapshot,unsigned target) {
    auto backing=ram(4096);
    {
      const auto writer=NativeFilesystemWriter::compact(snapshot,backing,id(target),policy);
      require(writer.snapshot().identity()==id(target),"uneven short packing used an unexpected image");
    }
    verify(NativeFilesystemSnapshot::open_image(backing,storage));
    return backing->extent();
  };
  f.session.reset();
  const auto pinned=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto before=compact(pinned,2);
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  NativeMaintenanceProgress outcome;
  {
    NativeGroupMaintenance groups(*f.session);
    outcome=f.finish(groups);
    const auto idle_reads=f.image->read_calls,idle_writes=f.image->writes;
    const auto idle=f.step(groups);
    require(idle.phase==Phase::idle && !idle.owned_payload_bytes &&
        idle_reads==f.image->read_calls && idle_writes==f.image->writes,
        "a settled uneven packing window did more backing work");
    verify(pinned);
  }
  f.session.reset();
  const auto current=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto info=current.shared_base_info();
  const auto after=compact(current,3);
  std::cout<<"uneven_short_packing files="<<count<<" before_compacted="<<before
      <<" after_compacted="<<after<<" protected_bases="<<info.bases<<" protected_members="<<info.members
      <<" packed_groups="<<outcome.packed_reference_groups<<" attempts="<<outcome.attempted_groups
      <<" published="<<outcome.published_groups<<" independent_cpu_readback=1 pinned_readback=1 idle_io=0\n";
  if(regression)require(after<before && info.members==count && info.bases==pairs &&
      outcome.packed_reference_groups>=2,"packing did not match the later short files to the earlier long files");
}


void profile_protected_layout(const std::shared_ptr<Ram>& backing,
    std::uint64_t selector_bytes,std::string_view arm) {
  const auto snapshot=PagedObjectStoreSnapshot::open(backing,{kStoreMaxPhysicalBytes});
  const auto info=snapshot.info();
  require(info.mirrored_payloads && info.physical_end==backing->extent(),
      "layout comparison requires a fully packed mirrored image");
  std::vector<StoreObjectInfo> records;
  std::uint64_t after=0;
  for(;;) {
    const auto page=snapshot.list_page(after,64);
    if(page.empty())break;
    records.insert(records.end(),page.begin(),page.end());
    after=page.back().id;
  }
  require(records.size()==info.objects && snapshot.rebuilt_size(records)==info.physical_end,
      "packed accounting does not reconcile with the image");
  std::uint64_t data_bytes=0,metadata_bytes=0;
  constexpr auto domain_mask=~(detail::kNativeIndexedInodeLimit-1);
  for(auto& record:records) {
    const auto bytes=2ULL*record.stored_payload_bytes;
    if((record.id&domain_mask)==detail::kNativeIndexedDataObject) {
      data_bytes+=bytes;
      record.stored_payload_bytes=0;
    } else metadata_bytes+=bytes;
  }
  require(data_bytes+metadata_bytes==info.stored_payload_bytes &&
      info.stored_payload_bytes<=info.physical_end,"protected payload totals do not reconcile");
  const auto without_data=snapshot.rebuilt_size(records)+selector_bytes;
  for(auto& record:records)record.stored_payload_bytes=0;
  std::cout<<"protected_layout arm="<<arm<<" objects="<<info.objects
      <<" image_bytes="<<info.physical_end<<" selector_bytes="<<selector_bytes
      <<" mirrored_data_object_bytes="<<data_bytes<<" mirrored_metadata_object_bytes="<<metadata_bytes
      <<" record_index_and_padding_bytes="<<info.physical_end-info.stored_payload_bytes
      <<" zero_data_layout_bytes="<<without_data
      <<" zero_payload_layout_bytes="<<snapshot.rebuilt_size(records)+selector_bytes<<'\n';
}

void profile_similarity_packing(std::span<const Bytes> inputs,std::size_t expected_members=0,
    bool compare_reencoding=false,NativeIndexedDataLayout layout=NativeIndexedDataLayout::extents,
    NativeVolumeHeadLayout head_layout=NativeVolumeHeadLayout::legacy,
    PagedStoreBankLayout image_layout=PagedStoreBankLayout::shared) {
  if(!store_zstd_available())return;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto name=[](std::size_t i){return "pack-profile-"+std::to_string(i);};
  Fixture f(4096,0,true,policy,layout,head_layout,image_layout);
  std::uint64_t input_bytes=0;
  constexpr std::size_t write_bytes=1024*1024;
  for(std::size_t i=0;i<inputs.size();++i) {
    const auto path=name(i);
    const auto bytes=std::span(inputs[i]);
    f.session->create_entry(path);
    auto handle=f.session->open(path,{true,true,false});
    for(std::size_t offset=0;offset<bytes.size();offset+=write_bytes)
      f.session->write(handle,offset,bytes.subspan(offset,(std::min)(write_bytes,bytes.size()-offset)));
    f.session->cleanup(handle,false);
    f.session->close(handle);
    input_bytes+=bytes.size();
  }
  const auto ingested_bytes=f.image->extent();

  const auto verify=[&](const NativeFilesystemSnapshot& snapshot) {
    snapshot.verify_all();
    for(std::size_t i=0;i<inputs.size();++i) {
      const auto path=name(i);
      require(snapshot.stat(path).logical_bytes==inputs[i].size() &&
          snapshot.read(path,0,inputs[i].size())==inputs[i],"similarity packing changed file bytes");
    }
  };
  const auto check_image=[&](const std::shared_ptr<Ram>& backing) {
    const auto reader=NativeFilesystemSnapshot::open_image(backing,storage);
    const auto read_start=backing->read_bytes;
    for(std::size_t i=0;i<inputs.size();++i) {
      const auto length=(std::min)(std::size_t{128},inputs[i].size());
      const auto offset=(inputs[i].size()-length)/2;
      require(reader.read(name(i),offset,length)==Bytes(inputs[i].begin()+offset,inputs[i].begin()+offset+length),
          "short CPU range changed after packing");
    }
    const auto small_reads=backing->read_bytes-read_start;
    verify(reader);
    return std::pair{backing->extent(),small_reads};
  };
  const auto settled_image=[&](const NativeFilesystemSnapshot& source,unsigned target,
      std::string_view arm,int reencode_level=0) {
    auto backing=ram(4096);
    if(reencode_level)
      (void)NativeFilesystemWriter::recompress(source,backing,id(target),
          {StoreCompressionMode::automatic,reencode_level,true});
    else (void)NativeFilesystemWriter::compact(source,backing,id(target),policy);
    const auto result=check_image(backing);
    if(compare_reencoding)profile_protected_layout(backing,f.head->extent(),arm);
    return result;
  };
  const auto cold_copy=[&](Fixture& volume,std::string_view order) {
    using Recompression=NativeFileSession::RecompressionProgress;
    auto target=ram(4096);
    const auto retained=volume.session->snapshot();
    const auto reads=volume.image->read_bytes;
    require(volume.session->begin_idle_compaction(target,id(5),PagedRewriteMode::reconcile_foreground),
        "Could not start the ordinary cold copy");
    auto state=Recompression::copying;
    for(unsigned steps=0;steps<15000 && state==Recompression::copying;++steps)
      state=volume.session->advance_recompression();
    require(state==Recompression::ready,"Cold copy failed to settle in the public-file profile");
    const bool selected=volume.session->publish_idle_recompression().has_value();
    const auto source_reads=volume.image->read_bytes-reads;
    const auto [bytes,range_bytes]=check_image(target);
    verify(retained);
    profile_protected_layout(target,volume.head->extent(),order);
    std::cout<<"cold_order arm="<<order<<" settled="<<bytes<<" selector="<<volume.head->extent()
        <<" source_read_bytes="<<source_reads<<" target_write_bytes="<<target->write_bytes
        <<" range_read_bytes="<<range_bytes<<" selected="<<selected<<" cpu_exact=1\n";
    if(selected)volume.image=std::move(target);
  };

  f.session.reset();
  const auto prior=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto before=settled_image(prior,2,"independent_preserved");
  if(compare_reencoding) {
    for(const int level:{3,9}) {
      const auto [image_bytes,range_bytes]=settled_image(prior,4,level==3?"independent_level_3":"independent_level_9",level);
      std::cout<<"independent_reencode level="<<level<<" image_bytes="<<image_bytes
          <<" selector_bytes="<<f.head->extent()<<" range_read_bytes="<<range_bytes<<" cpu_exact=1\n";
    }
    Fixture cold_first(image(f.head->stable,4096),image(f.image->stable,4096),policy);
    cold_copy(cold_first,"cold_only");
    const auto read_start=cold_first.image->read_bytes,write_start=cold_first.image->write_bytes;
    NativeMaintenanceProgress progress;
    {
      NativeGroupMaintenance groups(*cold_first.session);
      progress=cold_first.finish(groups);
    }
    const auto read_bytes=cold_first.image->read_bytes-read_start,write_bytes=cold_first.image->write_bytes-write_start;
    cold_first.session.reset();
    const auto source=NativeFilesystemSnapshot::open_image(cold_first.image,storage);
    const auto [bytes,range_bytes]=settled_image(source,6,"cold_then_group");
    std::cout<<"cold_order arm=cold_then_group settled="<<bytes<<" selector="<<cold_first.head->extent()
        <<" source_read_bytes="<<read_bytes<<" target_write_bytes="<<write_bytes
        <<" range_read_bytes="<<range_bytes<<" groups="<<progress.published_groups<<" cpu_exact=1\n";
  }
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  NativeMaintenanceProgress outcome;
  const auto before_reads=f.image->read_bytes,before_writes=f.image->write_bytes;
  const auto before_read_calls=f.image->read_calls;
  {
    NativeGroupMaintenance groups(*f.session);
    outcome=f.finish(groups);
    const auto reads=f.image->read_calls,writes=f.image->writes;
    require(f.step(groups).phase==Phase::idle && f.image->read_calls==reads && f.image->writes==writes,
        "completed similarity packing did more backing I/O");
  }
  const auto maintenance_reads=f.image->read_bytes-before_reads,maintenance_writes=f.image->write_bytes-before_writes;
  const auto maintenance_read_calls=f.image->read_calls-before_read_calls;
  f.session.reset();
  verify(prior);
  const auto current=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto after=settled_image(current,3,"group_only");
  const auto members=current.shared_base_info().members;
  std::cout<<"similarity_packing files="<<inputs.size()<<" before_compacted="<<before.first
      <<" after_compacted="<<after.first<<" input_bytes="<<input_bytes
      <<" selector_bytes="<<f.head->extent()<<" ingested_image_bytes="<<ingested_bytes
      <<" maintained_image_bytes="<<f.image->extent()<<" before_range_read_bytes="<<before.second
      <<" after_range_read_bytes="<<after.second<<" maintenance_read_bytes="<<maintenance_reads
      <<" maintenance_read_calls="<<maintenance_read_calls
      <<" sampled_extents="<<outcome.sampled_extents<<" reused_features="<<outcome.reused_features
      <<" maintenance_write_bytes="<<maintenance_writes<<" work_ns="<<outcome.elapsed_work_ns
      <<" packed_groups="<<outcome.packed_reference_groups<<" attempts="<<outcome.attempted_groups
      <<" yield_observations="<<outcome.candidate_yield_observations
      <<" members="<<members<<" cpu_exact=1 retained_snapshot_exact=1 idle_io=0\n";
  if(compare_reencoding) {
    f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
    cold_copy(f,"group_then_cold");
  }
  if(expected_members) {
    require(after.first<before.first && members==expected_members && outcome.packed_reference_groups,
        "similarity ranking missed the fitting short-file families");
    require(outcome.packing_similar_partners==1 && outcome.packing_postings_examined &&
        outcome.packing_postings_examined<=2*std::tuple_size_v<detail::NativePackingSketch>,
        "singleton ranking did more than its sparse retained-family lookups");
  }
}

void profile_shuffled_short_packs() {
  constexpr unsigned pairs=6;
  std::array<Bytes,2*pairs> inputs;
  for(unsigned i=0;i<pairs;++i) {
    inputs[i]=uneven_short_pack_file(i,pairs);
    inputs[pairs+i]=uneven_short_pack_file(pairs+(i+2)%pairs,pairs);
  }
  profile_similarity_packing(inputs);
}


void profile_cross_window_short_families(bool regression=false) {
  constexpr auto files=kNativeMaxDirtyExtents+1;
  auto related=uneven_short_pack_file(1,1);
  related.resize(23*1024);
  related.back()=std::byte{0x67};
  auto query=uneven_short_pack_file(0,1);
  std::vector<Bytes> inputs;
  inputs.reserve(files);
  inputs.push_back(std::move(related));
  inputs.push_back(data(24*1024,33211)); // The size-only lookup prefers this unrelated extent.
  for(std::size_t i=2;i+1<files;++i) {
    auto& payload=inputs.emplace_back(data(kNativeExtentBytes,947320+17*i));
    payload.back()=std::byte{0x86};
  }
  inputs.push_back(std::move(query));
  profile_similarity_packing(inputs,regression?2:0);
}

void profile_retained_short_peer() {
  constexpr std::size_t first_window=kNativeMaxDirtyExtents;
  std::vector<Bytes> inputs;
  inputs.reserve(first_window+2);
  inputs.push_back(uneven_short_pack_file(1,1));
  for(std::size_t i=1;i<first_window;++i) {
    auto& bytes=inputs.emplace_back(data(kNativeExtentBytes,974113+53*i));
    bytes.back()=std::byte{0x81};
  }
  inputs.push_back(uneven_short_pack_file(0,1));
  inputs.push_back(data(24*1024,69931));
  inputs.back().back()=std::byte{0x67};
  profile_similarity_packing(inputs,2);
}

void profile_public_packs(const std::filesystem::path& filename,bool complete_files,bool compare_reencoding=false,
    NativeIndexedDataLayout layout=NativeIndexedDataLayout::extents,
    NativeVolumeHeadLayout head_layout=NativeVolumeHeadLayout::legacy,
    PagedStoreBankLayout image_layout=PagedStoreBankLayout::shared) {
  if(!store_zstd_available()) {
    require(!complete_files,"complete-file packing profile requires Zstandard");
    return;
  }
  const auto inputs=read_packed_corpus(filename,complete_files);
  profile_similarity_packing(inputs,0,compare_reencoding,layout,head_layout,image_layout);
}

void profile_indexed_pair_oracle(const std::filesystem::path& filename,bool tails=false) {
  require(store_zstd_available(),"pair oracle requires Zstandard");
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};

  // The first-extent profile isolates samples. Tail pricing keeps each
  // file's preceding extents and their sharing in the compacted image.
  auto files=read_packed_corpus(filename,true);
  constexpr std::size_t cohort_limit=32;
  struct SampledFile {Bytes bytes;std::size_t extent;};
  std::vector<SampledFile> cohort;
  for(auto& file:files) {
    if(tails ? file.size()<=kNativeExtentBytes || file.size()%kNativeExtentBytes==0 :
        file.size()<kNativeExtentBytes)continue;
    require(cohort.size()<cohort_limit,"pair oracle cohort exceeds 32 samples");
    if(tails) {
      const auto extent=file.size()/kNativeExtentBytes;
      cohort.push_back({std::move(file),extent});
    } else {
      file.resize(kNativeExtentBytes);
      cohort.push_back({std::move(file),0});
    }
  }
  require(cohort.size()>=2,"pair oracle cohort needs at least two samples");

  Fixture volume(4096,0,true,policy);
  const auto name=[](std::size_t i){return "oracle-"+std::to_string(i);};
  for(std::size_t i=0;i<cohort.size();++i) {
    const auto path=name(i);
    const auto bytes=std::span(cohort[i].bytes);
    volume.session->create_entry(path);
    auto handle=volume.session->open(path,{true,true,false});
    constexpr std::size_t write_bytes=1024*1024;
    for(std::size_t offset=0;offset<bytes.size();offset+=write_bytes)
      volume.session->write(handle,offset,bytes.subspan(offset,(std::min)(write_bytes,bytes.size()-offset)));
    volume.session->cleanup(handle,false);
    volume.session->close(handle);
  }
  const auto occupied=[&](const std::shared_ptr<Ram>& source) {
    auto compacted=ram(4096);
    (void)NativeFilesystemWriter::compact(NativeFilesystemSnapshot::open_image(source,storage),compacted,id(20),policy);
    const auto checked=NativeFilesystemSnapshot::open_image(compacted,storage);
    checked.verify_all();
    for(std::size_t i=0;i<cohort.size();++i) {
      const auto& expected=cohort[i].bytes;
      const auto path=name(i);
      require(checked.stat(path).logical_bytes==expected.size() &&
          checked.read(path,0,expected.size())==expected,"pair oracle CPU reopen differs");
      const auto end=std::span(expected).last(128);
      require(std::ranges::equal(checked.read(path,expected.size()-end.size(),end.size()),end),
          "pair oracle CPU range differs");
    }
    return compacted->extent()+volume.head->extent();
  };
  volume.session.reset();
  const auto independent=occupied(volume.image);
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{
    return key==id(1)?volume.image:nullptr;
  };
  volume.session.emplace(NativeFileSession::resume_closed_indexed_volume(volume.head,id(90),1,resolve,policy));
  NativeMaintenanceProgress progress;
  {
    NativeGroupMaintenance groups(*volume.session);
    progress=volume.finish(groups);
  }
  volume.session.reset();
  const auto automatic=occupied(volume.image);

  const auto frozen=PagedObjectStoreSnapshot::open(volume.image,limits);
  const detail::IndexedVolumeView view(frozen,frozen.read(1));
  std::vector<std::pair<std::size_t,NativeContentToken>> peers;
  for(std::size_t i=0;i<cohort.size();++i) {
    const auto& file=cohort[i];
    const auto sample=view.sample_content({view.resolve(name(i)).inode,file.extent},
        detail::IndexedContentLookup::published_reference,{});
    if(!sample)continue;
    const auto expected=std::span(file.bytes).subspan(file.extent*kNativeExtentBytes);
    require(sample->payload && sample->payload->size()==kNativeExtentBytes &&
        std::ranges::equal(std::span(*sample->payload).first(expected.size()),expected),
        "pair oracle bound to the wrong source extent");
    peers.emplace_back(i,sample->token);
  }

  const auto price=[&](const NativeContentToken& base,std::span<const NativeContentToken> members,bool pack=false)
      ->std::optional<std::uint64_t> {
    auto trial=image(volume.image->stable,4096);
    NativeSharedBaseCommit result;
    {
      auto writer=PagedObjectStoreWriter::resume(trial,id(1),frozen.info().generation,limits,policy);
      const auto source=writer.snapshot();
      auto root=detail::IndexedVolumeView(source,source.read(1)).root();
      std::vector<NativeExtentSelection> selected;
      std::vector<NativeContentToken> expected{base};
      for(const auto& member:members) {
        selected.push_back(member.extent);
        expected.push_back(member);
      }
      detail::IndexedSharedBasePreparation work(source,root,base.extent,selected,expected,false,pack);
      work.admit_variants(true,true,true);
      if(!pack) {
        require(!work.advance({},nullptr),"pair oracle did not enter encoding");
        std::weak_ptr<const void> owned_window;
        for(std::size_t i=0;i<members.size();++i)
          require(work.stage_search(members.size(),owned_window,NativeSharedBaseSearchPurpose::advice),
              "pair oracle did not stage its checked members");
      }
      bool done=false;
      for(std::size_t i=0;i<8*expected.size() && !done;++i)done=work.advance({},nullptr);
      require(done,"pair oracle did not finish a candidate");
      if(work.progress().phase==NativeSharedBasePhase::declined)return {};
      result=work.finish(writer,source,root);
    }
    if(!result.applied)return {};
    const auto bytes=occupied(trial);
    require(bytes<automatic,"pair oracle published a non-improving protected layout");
    return bytes;
  };

  // Try both orientations against the same settled checkpoint. Reuse a
  // checked base where one exists and leave already grouped peers in place.
  std::uint64_t best=automatic;
  std::size_t attempted=0,accepted=0;
  std::optional<std::pair<std::size_t,std::size_t>> best_pair;
  for(const auto& [base_index,base]:peers) {
    for(const auto& [member_index,member]:peers) {
      if(member.object==base.object || member.grouped)continue;
      ++attempted;
      if(const auto bytes=price(base,std::span(&member,1))) {
        ++accepted;
        if(*bytes<best) {
          best=*bytes;
          best_pair=std::pair{base_index,member_index};
        }
      }
    }
  }

  if(tails) {
    std::size_t packed=0,improved=0;
    std::uint64_t best_pack=automatic;
    for(const auto& [base_index,base]:peers) {
      for(const auto& [member_index,member]:peers) {
        if(member_index<=base_index || !base.standalone_bytes || !member.standalone_bytes ||
            base.standalone_bytes+member.standalone_bytes>kNativeExtentBytes)continue;
        ++packed;
        if(const auto bytes=price(base,std::span(&member,1),true)) {
          ++improved;
          best_pack=(std::min)(best_pack,*bytes);
        }
      }
    }
    std::cout<<"indexed_tail_oracle files="<<cohort.size()<<" stored_extents="<<peers.size()
        <<" automatic_attempts="<<progress.attempted_groups<<" automatic_groups="<<progress.published_groups
        <<" tested_pairs="<<attempted<<" improving_pairs="<<accepted
        <<" tested_packs="<<packed<<" improving_packs="<<improved
        <<" independent_bytes="<<independent<<" automatic_bytes="<<automatic
        <<" best_pair_bytes="<<best<<" best_pack_bytes="<<best_pack;
    if(best_pair)std::cout<<" best_base="<<best_pair->first<<" best_member="<<best_pair->second;
    std::cout<<" cpu_verified=1\n";
    return;
  }

  // A one-member move can leave its old base live. Move a whole existing
  // group in the same transaction to price retiring that dependency.
  std::map<std::uint64_t,std::vector<NativeContentToken>> groups;
  for(const auto& [file,token]:peers) {
    if(!token.grouped || token.packed_slice)continue;
    const auto frame=frozen.read(token.object);
    const auto ref=detail::read_native_group_header(
        std::span(frame).subspan(detail::kIndexedExtentHeaderBytes),detail::NativeGroupMethod::prefixed_zstd);
    groups[ref.base_id].push_back(token);
  }
  std::uint64_t best_merge=automatic;
  std::size_t tried_merges=0,saved_merges=0;
  for(const auto& [base,reference]:groups) {
    for(const auto& [other,members]:groups) {
      if(other==base)continue;
      ++tried_merges;
      if(const auto bytes=price(reference.front(),members)) {
        ++saved_merges;
        best_merge=(std::min)(best_merge,*bytes);
      }
    }
  }
  std::cout<<"indexed_pair_oracle files="<<cohort.size()<<" stored_extents="<<peers.size()
      <<" automatic_attempts="<<progress.attempted_groups<<" automatic_groups="<<progress.published_groups
      <<" tested_pairs="<<attempted<<" improving_pairs="<<accepted
      <<" independent_bytes="<<independent<<" automatic_bytes="<<automatic
      <<" best_pair_bytes="<<best<<" reusable_groups="<<groups.size()<<" tested_merges="<<tried_merges
      <<" improving_merges="<<saved_merges<<" best_merge_bytes="<<best_merge;
  if(best_pair)std::cout<<" best_base="<<best_pair->first<<" best_member="<<best_pair->second;
  std::cout<<" cpu_verified=1\n";
}

void indexed_cross_window_pack(bool replace_retained) {
  if(!store_zstd_available())return;
  constexpr auto count=kNativeMaxDirtyExtents+1;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto path=[](std::size_t i){return "cross-window-"+std::to_string(i);};
  Fixture f(4096,0,true,policy);
  std::vector<Bytes> originals;
  originals.reserve(count);
  for(std::size_t i=0;i<count;++i) {
    auto bytes=i==0?uneven_short_pack_file(0,1):
        i+1==count?uneven_short_pack_file(1,1):data(kNativeExtentBytes,33109+83*i);
    bytes.back()=std::byte{0x67};
    put(*f.session,path(i),bytes);
    originals.push_back(std::move(bytes));
  }
  f.session.reset();
  const auto pinned=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto replacement=data(kNativeExtentBytes,194117);
  const auto verify=[&](const NativeFilesystemSnapshot& snapshot,bool replaced) {
    snapshot.verify_all();
    for(std::size_t i=0;i<count;++i) {
      const auto& expected=replaced && !i?replacement:originals[i];
      require(snapshot.read(path(i),0,kNativeExtentBytes)==expected,"cross-window packing changed file bytes");
    }
  };
  const auto occupied=[&](const NativeFilesystemSnapshot& snapshot,unsigned target,bool replaced) {
    auto backing=ram(4096);
    {
      const auto writer=NativeFilesystemWriter::compact(snapshot,backing,id(target),policy);
    }
    verify(NativeFilesystemSnapshot::open_image(backing,storage),replaced);
    return backing->extent();
  };
  const auto before=occupied(pinned,2,false);
  const auto resolve=[&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  NativeMaintenanceProgress settled;
  {
    NativeGroupMaintenance groups(*f.session);
    for(unsigned tick=0;tick<600;++tick) {
      settled=f.step(groups);
      require(settled.phase!=Phase::failed && !settled.published_groups,
          "the first window published a group before the partner was discovered");
      if(settled.sampled_extents==kNativeMaxDirtyExtents && settled.phase==Phase::scanning)break;
    }
    require(settled.sampled_extents==kNativeMaxDirtyExtents && settled.phase==Phase::scanning,
        "the packing fixture did not cross a retained scan window");
    if(replace_retained) {
      const auto handle=f.session->open(path(0),{true,true,false});
      f.session->write(handle,0,replacement);
      f.session->cleanup(handle,false);
      f.session->close(handle);
    }
    settled=f.finish(groups);
    const auto reads=f.image->read_calls,writes=f.image->writes;
    require(f.step(groups).phase==Phase::idle && f.image->read_calls==reads && f.image->writes==writes,
        "the settled cross-window scan did more backing work");
  }
  f.session.reset();
  verify(pinned,false);
  const auto current=NativeFilesystemSnapshot::open_image(f.image,storage);
  const auto after=occupied(current,3,replace_retained);
  std::cout<<"cross_window_packing files="<<count<<" stale_partner="<<replace_retained
      <<" before_compacted="<<before<<" after_compacted="<<after
      <<" packed_groups="<<settled.packed_reference_groups<<" attempts="<<settled.attempted_groups
      <<" independent_cpu_readback=1 pinned_readback=1 idle_io=0\n";
  if(replace_retained)
    require(!settled.packed_reference_groups,"stale retained length was used to publish a pack");
  else
    require(settled.packed_reference_groups==1 && current.shared_base_info().members==2 && after<before,
        "short files in separate scan windows did not share a smaller protected base");
}

void indexed_pack_distinct_short_extents() {
  if(!store_zstd_available())return;
  constexpr unsigned count=40;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,3,true};
  std::vector<Bytes> pieces,inputs;
  for(unsigned i=0;i<127;++i)pieces.push_back(data(12,88531+17*i));
  for(unsigned i=0;i<count;++i) {
    auto& member=inputs.emplace_back();
    for(unsigned j=0;j<80;++j) {
      const auto& piece=pieces[(i*13+j*19)%pieces.size()];
      member.insert(member.end(),piece.begin(),piece.end());
      const auto separator=data(5,111770+i*761+j);
      member.insert(member.end(),separator.begin(),separator.end());
    }
    member.push_back(static_cast<std::byte>(i+1));
    member.resize(member.size()+i);
  }
  const auto path=[](unsigned i){return "small-"+std::to_string(i);};
  Fixture f(4096,0,true,policy);
  for(unsigned i=0;i<count;++i)put(*f.session,path(i),inputs[i]);
  const auto verify=[&](const NativeFilesystemSnapshot& snapshot) {
    snapshot.verify_all();
    for(unsigned i=0;i<count;++i)
      require(snapshot.read(path(i),0,kNativeExtentBytes)==inputs[i],"packing changed a short file or its zero tail");
  };
  const auto compare=[&](const NativeFilesystemSnapshot& view,unsigned target) {
    auto backing=ram(4096);
    const auto compact=NativeFilesystemWriter::compact(view,backing,id(target),policy);
    verify(compact.snapshot());
    return backing->extent();
  };
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.reset();
  const auto before=compare(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  const auto head_before=f.head->stable,image_before=f.image->stable;
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  std::size_t publication_steps=0;
  {
    NativeGroupMaintenance groups(*f.session);
    const auto calls=f.image->calls;
    const auto result=f.finish(groups);
    publication_steps=f.image->calls-calls;
    require(result.packed_reference_groups==1 && result.published_groups==1 &&
        f.session->snapshot().shared_base_info().members==count,
        "distinct short files did not share a protected packed base");
    require(!f.step(groups).owned_payload_bytes,"finished packing retained private payloads");
  }
  f.session.reset();
  const auto selected=NativeVolumeHeadView::open(f.head,id(90),resolve);
  const auto after=compare(selected.filesystem(),3);
  require(after<before,"packed short files did not reduce the complete image");

  {
    Fixture stale(image(head_before,4096),image(image_before,4096),policy);
    NativeGroupMaintenance groups(*stale.session);
    for(unsigned step=0;step<600 && !groups.progress().owned_payload_bytes;++step)(void)stale.step(groups);
    require(groups.active_group() && groups.progress().owned_payload_bytes>0,"missing private pack preparation");
    const auto handle=stale.session->open(path(0),{true,true,false});
    const auto replacement=inputs[0][0]^std::byte{0x01};
    stale.session->write(handle,0,{&replacement,1});
    stale.session->cleanup(handle,false);
    stale.session->close(handle);
    const auto calls=stale.image->calls;
    const auto rejected=stale.step(groups);
    const auto current=stale.session->snapshot();
    require(!groups.active_group() && !rejected.published_groups && stale.image->calls==calls &&
        current.read(path(0),0,1)==Bytes{replacement},"stale packing work touched acknowledged data");
    current.verify_all();
  }
  require(publication_steps>=2,"packed publication omitted modeled recovery boundaries");
  unsigned previous=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(const auto cut:{std::size_t{0},publication_steps-1}) {
    auto backing=image(image_before,4096);
    Fixture interrupted(image(head_before,4096),backing,policy);
    NativeGroupMaintenance groups(*interrupted.session);
    backing->failure=mode;
    backing->fail_at=backing->calls+cut;
    for(unsigned step=0;step<2000 && groups.pending();++step)(void)interrupted.step(groups);
    require(groups.progress().phase==Phase::failed && interrupted.session->poisoned(),
        "interrupted direct packing left a writable owner");
    const auto recovered=NativeFilesystemSnapshot::open_image(image(backing->stable,4096),
        {NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verify(recovered);
    const auto members=recovered.shared_base_info().members;
    if(!members)++previous;
    else if(members==count)++complete;
    else throw std::runtime_error("interrupted direct packing exposed a partial group");
  }
  require(previous && complete,"packing faults did not exercise both complete states");
  faults+=previous+complete;

  Fixture random(4096,0,true,policy);
  for(unsigned i=0;i<16;++i)put(*random.session,path(i),data(3107,907333+i*59));
  const auto size=random.image->extent();
  NativeGroupMaintenance groups(*random.session);
  const auto result=random.finish(groups);
  require(result.attempted_groups && !result.published_groups && random.image->extent()==size,
      "an unprofitable short-file pack wrote data");
  std::cout<<"distinct_short_files="<<count<<" before="<<before<<" after="<<after
      <<" previous="<<previous<<" complete="<<complete
      <<" stale_selection_rejected=true declined_random_without_writes=true independent_cpu_readback=true\n";
}

void indexed_packed_short_extents() {
  if(!store_zstd_available())return;
  constexpr auto names=16U;
  constexpr StoreCompressionOptions policy{StoreCompressionMode::automatic,6,true};
  std::vector<Bytes> pieces,inputs;
  for(unsigned i=0;i<29;++i)pieces.push_back(data(81+i*47%119,78011+i));
  const auto common=data(304,79429);
  for(unsigned i=0;i<names;++i) {
    auto& member=inputs.emplace_back(common);
    for(unsigned j=0;j<16;++j) {
      const auto& piece=pieces[(i*3+j*(2*i+1))%pieces.size()];
      member.insert(member.end(),piece.begin(),piece.end());
    }
    member.push_back(static_cast<std::byte>(i+1));
  }
  Fixture f(4096,0,true,policy);
  const auto path=[](unsigned id){return "short-"+std::to_string(id);};
  for(unsigned i=0;i<names;++i)put(*f.session,path(i),inputs[i]);
  const auto verify=[&](const NativeFilesystemSnapshot& snapshot,unsigned first=0) {
    snapshot.verify_all();
    for(auto i=first;i<names;++i)
      require(snapshot.read(path(i),0,kNativeExtentBytes)==inputs[i],"packed slice changed acknowledged bytes");
  };
  const auto compacted=[&](const NativeFilesystemSnapshot& source,unsigned role) {
    auto backing=ram(4096);
    const auto writer=NativeFilesystemWriter::compact(source,backing,id(role),policy);
    const auto checked=writer.snapshot();
    verify(checked);
    return backing->extent();
  };
  const auto resolve=[&](const StoreIdentity& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  f.session.reset();
  const auto before=compacted(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  const auto head_before=f.head->stable, image_before=f.image->stable;
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  std::size_t cuts=0;
  {
    NativeGroupMaintenance maintenance(*f.session);
    const auto calls=f.image->calls;
    const auto progress=f.finish(maintenance);
    cuts=f.image->calls-calls;
    require(progress.published_groups==1 && progress.packed_reference_groups==1,
        "normal maintenance did not choose the packed slice candidate");
  }
  f.session.reset();
  const auto reopened=NativeVolumeHeadView::open(f.head,id(90),resolve);
  const auto& selected=reopened.filesystem();
  const auto after=compacted(selected,3);
  require(after<before && selected.shared_base_info().bases==1 &&
      selected.shared_base_info().members==names,"packed short extents did not save complete image bytes");

  constexpr NativeImageStorage storage{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto objects=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes});
  const detail::NativeNameIndex index(objects);
  const auto object=index.find(detail::native_extent_key(selected.stat(path(0)).inode,0));
  require(object.has_value(),"missing packed extent binding");
  const auto frame=objects.read(*object);
  require(frame.size()==80 && std::memcmp(frame.data(),"FLTXPD02",8)==0,
      "packed member retained the legacy full-extent envelope");
  for(const auto offset:{std::size_t{24},std::size_t{32},std::size_t{40},std::size_t{44},std::size_t{48}}) {
    auto malformed=frame;
    malformed.at(offset)^=std::byte{0xff};
    auto backing=image(f.image->stable,4096);
    {
      auto writer=PagedObjectStoreWriter::resume(backing,id(1),objects.info().generation,{kStoreMaxPhysicalBytes},policy);
      const StoreMutation replacement{*object,false,std::move(malformed)};
      writer.commit({&replacement,1});
    }
    const auto bad=NativeFilesystemSnapshot::open_image(backing,storage);
    rejects([&]{(void)bad.read(path(0),0,kNativeExtentBytes);},"malformed packed reference returned data");
  }

  unsigned previous=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto head=image(head_before,4096),backing=image(image_before,4096);
    Fixture attempt(head,backing,policy);
    NativeGroupMaintenance maintenance(*attempt.session);
    backing->failure=mode;backing->fail_at=backing->calls+cut;
    for(unsigned step=0;step<1200 && maintenance.pending();++step)(void)attempt.step(maintenance);
    require(maintenance.progress().phase==Phase::failed && attempt.session->poisoned(),
        "interrupted packed publication left a writable owner");
    const auto recovered=NativeFilesystemSnapshot::open_image(image(backing->stable,4096),storage);
    verify(recovered);
    const auto members=recovered.shared_base_info().members;
    if(!members)++previous;
    else if(members==names)++complete;
    else throw std::runtime_error("interrupted packed publication exposed part of a group");
  }
  require(previous && complete,"packed interruptions did not exercise both valid checkpoints");
  faults+=previous+complete;

  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,policy));
  const auto removed=f.session->open(path(0),{true,true,true});
  f.session->set_delete(removed,true);f.session->cleanup(removed,true);f.session->close(removed);
  const auto surviving=f.session->snapshot();
  require(surviving.shared_base_info().bases==1 && surviving.shared_base_info().members==names-1,
      "removing a packed member retired a still-referenced base");
  verify(surviving,1);
  std::cout<<"indexed_packed_slices="<<names<<" before="<<before<<" after="<<after
      <<" cuts="<<previous+complete<<" previous="<<previous<<" complete="<<complete
      <<" malformed_rejected=5 surviving_bytes_verified=true\n";
}

void indexed_changed_extent_discovery() {
  constexpr unsigned unrelated=1024, originals=8, batch=32;
  const auto independent=[](unsigned i){return data(1024,21001+73*i);};
  const auto name=[](unsigned i){return "unrelated-"+std::to_string(i);};
  auto head=ram(4096),image=ram(4096);
  const auto populate=[&] {
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(image,id(1),{kStoreMaxPhysicalBytes},{StoreCompressionMode::raw,1,false}));
    std::vector<NativeFileMutation> seed;
    seed.reserve(batch*2);
    const auto add=[&](const std::string& path,Bytes contents) {
      seed.push_back(file_create(path));
      seed.push_back(file_write(path,0,std::move(contents)));
    };
    for(unsigned i=0;i<originals;++i)add("f"+std::to_string(i),family(i));
    seed.push_back(file_create("incoming"));
    writer.apply(seed);
    for(unsigned first=0;first<unrelated;first+=batch) {
      seed.clear();
      for(unsigned i=first;i<first+batch;++i)add(name(i),independent(i));
      writer.apply(seed);
    }
    return writer.snapshot();
  };
  (void)NativeVolumeHeadWriter::create(head,id(90),populate());
  Fixture f(head,image);

  NativeGroupMaintenance controller(*f.session);
  const auto visits=[](const auto& p){return p.sampled_extents+p.reused_features;};
  const auto initial=f.finish(controller);
  const auto unchanged=visits(initial);
  const auto group=f.session->snapshot().shared_base_info();
  require(unchanged>=unrelated && group.bases==1 && group.members==originals,
      "initial discovery did not learn the related group and its distant neighbors");
  const auto expected=family(originals);
  const auto incoming=f.session->open("incoming",{true,true,false});
  f.session->write(incoming,0,family(originals+1));
  f.session->write(incoming,0,expected);
  f.session->cleanup(incoming,false);
  f.session->close(incoming);
  const auto changed=f.finish(controller);
  const auto work=visits(changed)-unchanged;
  const auto attached=f.session->snapshot().shared_base_info();
  require(changed.published_groups==initial.published_groups+1 && attached.bases==1 &&
      attached.members==originals+1 && work>0 && work<=3,
      "a coalesced extent change replayed the learned volume or lost its match");

  const auto unmatched=data(kNativeExtentBytes,82091);
  put(*f.session,"unmatched",unmatched);
  const auto full=f.finish(controller);
  const auto unmatched_visits=visits(full)-visits(changed);
  require(unmatched_visits>=unrelated,"unmatched changed content did not get a wider scan");

  // Overflow of the advisory history requires the ordinary scan. All
  // overwritten coordinates still belong to the same authoritative volume.
  constexpr auto edits=kNativeMaxDirtyExtents+1;
  for(unsigned i=0;i<edits;++i) {
    const auto edit=f.session->open(name(i),{true,true,false});
    const std::array replacement{std::byte{37}};
    f.session->write(edit,0,replacement);
    f.session->cleanup(edit,false);
    f.session->close(edit);
  }
  const auto overflow=f.finish(controller);
  const auto overflow_visits=visits(overflow)-visits(full);
  require(overflow_visits>=unrelated,"lost history suppressed a full discovery sweep");
  f.session.reset();
  const auto snapshot=NativeFilesystemSnapshot::open_image(image,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
  snapshot.verify_all();
  for(unsigned i=0;i<unrelated;++i) {
    auto bytes=independent(i);
    if(i<edits)bytes.front()=std::byte{37};
    require(snapshot.read(name(i),0,bytes.size())==bytes,"discovery changed an unrelated file");
  }
  for(unsigned i=0;i<originals;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"extended group changed an original file");
  require(snapshot.read("incoming",0,expected.size())==expected &&
      snapshot.read("unmatched",0,unmatched.size())==unmatched,"discovery changed updated logical bytes");
  std::cout<<"changed_extent_discovery files="<<unrelated+originals+2
      <<" initial_visits="<<unchanged<<" matched_visits="<<work
      <<" unmatched_visits="<<unmatched_visits<<" overflow_visits="<<overflow_visits
      <<" coalesced=true independent_cpu_readback=true\n";
}

void advised_indexed_maintenance() {
  constexpr unsigned count=5,band_bytes=kNativeExtentBytes/8;
  Fixture f(4096,0,true);
  const auto base=family(0);
  std::vector<Bytes> original;
  for(unsigned i=0;i<count;++i) {
    auto bytes=i?anchor_search_fixture::fragmented(base,i):base;
    std::copy_n(base.begin(),band_bytes,bytes.begin());
    put(*f.session,"f"+std::to_string(i),bytes);
    original.push_back(std::move(bytes));
  }
  NativeGroupMaintenance controller(*f.session);
  detail::AnchorMatchAdvice hints;
  std::optional<NativeSharedBaseSearchReply> reply;
  for(unsigned tick=0;tick<1000;++tick) {
    const auto observed=f.clock.next();
    const auto progress=controller.advance(observed,f.clock.now,{
        count-1,reply?&*reply:nullptr});
    require(progress.phase!=Phase::failed,"advised maintenance failed");
    if(progress.published_groups)break;
    auto window=controller.search_window();
    if(!window || reply)continue;
    require(progress.search_window_ready && window.members().size()==count-1 && std::ranges::equal(window.base(),base),
        "automatic candidate was not staged for search");
    const std::array<std::uint32_t,1> counts{count-1};
    const auto quantum=anchor_match::max_members;
    const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),count+1);
    detail::AnchorMatchBatchBuilder builder(1,plan);
    builder.append(1,window);
    const auto request=builder.finish();
    const auto& group=request.groups().front();
    hints=detail::verify_anchor_match_records(group,detail::cpu_anchor_match_records(group));
    reply.emplace(NativeSharedBaseSearchReply{std::move(window),hints});

    auto busy=f.clock.next();
    busy.foreground_pending=true;
    const auto writes=f.image->calls,reads=f.image->read_calls;
    const auto deferred=controller.advance(busy,f.clock.now,{0,&*reply});
    require(deferred.deferral==Defer::foreground && f.image->calls==writes && f.image->read_calls==reads,
        "completed search bypassed foreground admission");
  }
  const auto result=f.finish(controller);
  require(reply && result.published_groups==1 && result.advised_members_published>0 &&
      result.owned_payload_bytes==0 && !controller.search_window(),"advised automatic group did not publish and retire");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(unsigned i=0;i<count;++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==original[i],
        "automatic search advice changed committed bytes");
}
void collective_dense_advice() {
  constexpr auto members=4U;
  Fixture f(4096,0,true);
  const auto base=family(0);
  std::array<Bytes,members+1> originals;
  for(unsigned index=0;index<originals.size();++index) {
    auto& bytes=originals[index];
    bytes=base;
    if(index) {
      const auto changes=data(kNativeExtentBytes,8121+index);
      for(std::size_t at=4096;at<bytes.size();++at)bytes[at]^=changes[at]&std::byte{1};
    }
    put(*f.session,"f"+std::to_string(index),bytes);
  }
  NativeGroupMaintenance controller(*f.session);
  std::vector<Bytes> bodies;
  std::vector<NativePackedGroupCheck> verdicts;
  std::optional<NativeSharedBaseSearchReply> reply;
  for(unsigned tick=0;tick<1000;++tick) {
    const auto observation=f.clock.next();
    const auto progress=controller.advance(observation,f.clock.now,{members,reply?&*reply:nullptr});
    require(progress.phase!=Phase::failed,"Collective packed advice failed");
    if(progress.published_groups)break;
    const auto window=controller.search_window();
    if(!window || reply)continue;
    require(progress.search_window_ready && window.members().size()==members,
        "Collective candidate did not stage a complete group");
    for(const auto& bytes:window.members()) {
      const auto packed=detail::encode_native_group_xor(bytes,window.base(),detail::kNativeGroupBaseBit+1,1,kNativeExtentBytes);
      require(packed && packed->size()>detail::kNativeGroupMemberHeaderBytes,"Missing dense XOR fixture body");
      bodies.push_back(anchor_search_fixture::collective_advice_payload(anchor_match::CollectiveBody::packed_xor,
          std::span(*packed).subspan(detail::kNativeGroupMemberHeaderBytes)));
    }
    // Keep valid framing and make one body reconstruct the wrong plaintext.
    bodies.front().back()^=std::byte{1};
    verdicts.resize(members,NativePackedGroupCheck::pending);
    reply.emplace(NativeSharedBaseSearchReply{window,{},bodies,verdicts,NativePackedGroupMethod::collective});
  }
  const auto result=f.finish(controller);
  require(reply && result.published_groups==1 && result.advised_members_published==members-1 &&
      verdicts.front()==NativePackedGroupCheck::invalid &&
      std::ranges::count(verdicts,NativePackedGroupCheck::verified)==members-1 &&
      result.minimum_compacted_bytes_saved && !result.owned_payload_bytes && !controller.has_retained_search(),
      "Collective dense bodies did not publish with independent CPU fallback");

  f.session.reset();
  const auto original=NativeFilesystemSnapshot::open_image(f.image,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
  auto packed=ram(f.image->failure_unit);
  (void)NativeFilesystemWriter::compact(original,packed,id(2),{StoreCompressionMode::raw,1,false});
  const auto reopened=NativeFilesystemSnapshot::open_image(packed,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
  reopened.verify_all();
  for(unsigned index=0;index<originals.size();++index)
    require(reopened.read("f"+std::to_string(index),0,kNativeExtentBytes)==originals[index],
        "Collective dense advice changed recovered bytes");
  std::cout<<"collective_dense_advice cpu_fixture=true accepted="<<result.advised_members_published
      <<" rejected=1 files="<<originals.size()<<" compacted_bytes="<<packed->extent()
      <<" independent_cpu_readback=true\n";
}
enum class BatchOutcome {accepted,cpu_fallback,changed_source,memory_pressure,
  overlap_success,overlap_fallback,full_overlap_success,full_overlap_fallback};
void batched_indexed_maintenance(BatchOutcome outcome) {
  constexpr unsigned members=4,files=members+1;
  const bool full_overlap=outcome==BatchOutcome::full_overlap_success ||
      outcome==BatchOutcome::full_overlap_fallback;
  const unsigned cohorts=full_overlap?3:2,staged_count=full_overlap?1:cohorts;
  Fixture f(4096,0,true);
  std::vector<Bytes> originals;
  for(unsigned cohort=0;cohort<cohorts;++cohort) {
    const auto base=data(kNativeExtentBytes,714+cohort*5731);
    for(unsigned i=0;i<files;++i) {
      auto bytes=i?anchor_search_fixture::fragmented(base,i):base;
      std::copy_n(base.begin(),kNativeExtentBytes/8,bytes.begin());
      put(*f.session,"f"+std::to_string(originals.size()),bytes);
      originals.push_back(std::move(bytes));
    }
  }
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned tick=0;tick<800;++tick) {
    const auto observed=f.clock.next();
    progress=controller.advance(observed,f.clock.now,{staged_count*members,nullptr,true});
    require(progress.phase!=Phase::failed && !progress.published_groups,"batch published before receiving advice");
    if(progress.search_batch_ready)break;
  }
  const auto windows=controller.search_batch();
  require(windows.size()==staged_count && controller.active_group_members()==staged_count*files,
      "independent indexed candidates did not fill the admitted batch");
  const auto selection=f.session->snapshot().stat("f0").inode;
  rejects([&]{(void)f.session->prepare_shared_base({selection,0},{});},
      "exclusive writer work entered an active maintenance batch");

  const std::vector<std::uint32_t> counts(staged_count,members);
  const auto quantum=anchor_match::max_members;
  const auto plan=detail::plan_anchor_match_batch(counts,quantum,anchor_match::device_bytes(quantum),staged_count*files);
  detail::AnchorMatchBatchBuilder builder(1,plan);
  for(std::size_t i=0;i<windows.size();++i)builder.append(i+1,windows[i]);
  const auto request=builder.finish();
  std::vector<detail::AnchorMatchAdvice> hints(staged_count);
  std::vector<NativeSharedBaseSearchReply> replies(staged_count);
  for(std::size_t i=0;i<windows.size();++i) {
    const auto& group=request.groups()[i];
    hints[i]=detail::verify_anchor_match_records(group,detail::cpu_anchor_match_records(group));
    replies[staged_count-1-i]={windows[i],hints[i]};
  }
  const bool changed=outcome==BatchOutcome::changed_source;
  if(changed) {
    originals.front()=data(kNativeExtentBytes,32771);
    const auto h=f.session->open("f0",{true,true,false});
    f.session->write(h,0,originals.front());f.session->cleanup(h,false);f.session->close(h);
    require(controller.search_batch().size()==1,"foreground edit did not invalidate exactly one staged group");
  }
  auto foreground=f.clock.next();
  foreground.foreground_pending=true;
  const auto io=f.image->calls,reads=f.image->read_calls;
  require(controller.advance(foreground,f.clock.now,{0,nullptr,true,replies}).deferral==Defer::foreground &&
      f.image->calls==io && f.image->read_calls==reads,"batch advice bypassed foreground admission");
  if(outcome==BatchOutcome::memory_pressure) {
    auto pressure=f.clock.next();
    pressure.physical_available=pressure.physical_total/16;
    const auto deferred=controller.advance(pressure,f.clock.now,{0,nullptr,true,replies});
    require(deferred.deferral==Defer::memory && !deferred.owned_payload_bytes &&
        !controller.active_group() && !controller.has_retained_search(),
        "batch kept preparations after losing its memory grant");
  }
  const bool overlaps=full_overlap || outcome==BatchOutcome::overlap_success ||
      outcome==BatchOutcome::overlap_fallback;
  if(overlaps) {
    const auto staged=std::span<const NativeSharedBaseSearchWindow>{windows};
    const auto prefix=staged.first(1);
    const auto unchanged=[&](std::span<const NativeSharedBaseSearchWindow> submitted,bool foreground_busy,Defer expected) {
      auto observed=f.clock.next();
      observed.foreground_pending=foreground_busy;
      const auto writes=f.image->calls, reads=f.image->read_calls;
      require(controller.advance(observed,f.clock.now,{0,nullptr,true,{},submitted}).deferral==expected &&
          f.image->calls==writes && f.image->read_calls==reads,"pending adviser changed a protected group");
    };
    if(!full_overlap)unchanged(staged.last(1),false,Defer::other_maintenance);
    unchanged(prefix,true,Defer::foreground);
    const auto expected_cpu=cohorts-1;
    for(unsigned tick=0;tick<800 && progress.published_groups<expected_cpu;++tick) {
      const auto observed=f.clock.next();
      progress=controller.advance(observed,f.clock.now,{0,nullptr,true,{},prefix});
      require(progress.phase!=Phase::failed && !progress.search_batch_ready &&
          !progress.checkpoints_published,"Independent CPU batching overlapped the pending adviser");
    }
    require(progress.published_groups==expected_cpu && !progress.advised_members_published &&
        controller.active_group_members()==files && controller.has_retained_search(),
        full_overlap?"a fully reserved batch blocked disjoint CPU groups":
            "unsubmitted work did not finish independently of the adviser");
    for(unsigned tick=0;tick<300;++tick) {
      const auto observed=f.clock.next();
      progress=controller.advance(observed,f.clock.now,{0,nullptr,true,{},prefix});
      require(progress.published_groups==expected_cpu && !progress.search_batch_ready &&
          controller.active_group_members()==files && controller.has_retained_search(),
          "the pending adviser lost its reserved group");
      if(progress.deferral==Defer::other_maintenance)break;
    }
    require(progress.deferral==Defer::other_maintenance,"disjoint CPU work did not park at the reservation");
  }
  const bool accepts=outcome==BatchOutcome::accepted || changed;
  const auto all_replies=std::span<const NativeSharedBaseSearchReply>{replies};
  const auto supplied=accepts?all_replies:
      (outcome==BatchOutcome::overlap_success || outcome==BatchOutcome::full_overlap_success)?all_replies.last(1):
      std::span<const NativeSharedBaseSearchReply>{};
  const auto expected=changed?1U:cohorts;
  for(unsigned tick=0;tick<800;++tick) {
    const auto observed=f.clock.next();
    progress=controller.advance(observed,f.clock.now,{0,nullptr,true,supplied});
    require(progress.phase!=Phase::failed,"staged batch failed after optional adviser completion");
    if(!controller.active_group() && progress.published_groups==expected)break;
  }
  require(progress.published_groups==expected && !controller.active_group() && !controller.has_retained_search() &&
      progress.owned_payload_bytes==0,"staged batch did not publish and retire its independent groups");
  const auto eligible_advice=members*(changed?1:supplied.size());
  const auto replaced_by_cpu=members*progress.section_reference_groups+progress.prefixed_zstd_members;
  require(progress.advised_members_published<=eligible_advice &&
      eligible_advice-progress.advised_members_published<=replaced_by_cpu,
      "batch advice fell outside its live windows or lacked a competing CPU representation");
  const auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().bases==expected,"staged groups lost independent bases");
  snapshot.verify_all();
  for(std::size_t i=0;i<originals.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==originals[i],"batch readback differs");
}

void indexed_batch_across_scan_windows() {
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  constexpr auto expected_members=anchor_search_fixture::indexed_batch_members;
  Fixture f(4096,0,true);
  const auto originals=anchor_search_fixture::indexed_batch_files();
  for(std::size_t i=0;i<originals.size();++i)put(*f.session,"f"+std::to_string(i),originals[i]);
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned tick=0;tick<2000;++tick) {
    const auto observed=f.clock.next();
    // Leave some work unfilled. Reaching the end of the scan must send the
    // partial batch without waiting for more files to appear.
    progress=controller.advance(observed,f.clock.now,{expected_members+families,nullptr,true});
    require(progress.phase!=Phase::failed && !progress.published_groups,
        "cross-window batch published before receiving advice");
    if(progress.search_batch_ready)break;
  }
  auto windows=controller.search_batch();
  require(progress.search_batch_ready && windows.size()==families,
      "batch stopped at the boundary of a scan window");
  std::size_t members=0;
  for(const auto& window:windows) {
    require(window.members().size()==kNativeMaxDirtyExtents-1,"batch changed the transaction work bound");
    members+=window.members().size();
  }
  require(members==expected_members && progress.sampled_extents==families*kNativeMaxDirtyExtents,
      "batch omitted members or rescanned source extents");

  auto pressure=f.clock.next();
  pressure.physical_available=pressure.physical_total/16;
  // The test owns independent references just like the adviser. Retire them
  // before confirming the maintenance owner's memory has been released.
  windows.clear();
  const auto deferred=controller.advance(pressure,f.clock.now,{expected_members+families,nullptr,true});
  require(deferred.deferral==Defer::memory && !deferred.owned_payload_bytes &&
      !controller.active_group() && !controller.has_retained_search(),
      "cross-window batch retained work after its resource grant contracted");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(std::size_t i=0;i<originals.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,originals[i].size())==originals[i],
        "cross-window retirement changed acknowledged data");
  std::cout<<"indexed batch scan windows="<<families<<" admitted members="<<members
      <<" source extents="<<progress.sampled_extents<<" retired under memory pressure\n";
}

void indexed_batch_reclaims_append_space() {
  require(!NativeVolumeSpace{100,25,16,0}.reclamation_worthwhile() &&
      !NativeVolumeSpace{100,25,16,17}.reclamation_worthwhile() &&
      NativeVolumeSpace{100,25,16,16}.reclamation_worthwhile(),
      "Reclamation did not price the proved append-space shortfall");
  constexpr auto members=anchor_search_fixture::indexed_batch_members;
  constexpr auto families=anchor_search_fixture::indexed_batch_families;
  Fixture f(kStoreCommitUnit,0,true);
  const auto original=anchor_search_fixture::indexed_batch_files();
  for(std::size_t i=0;i<original.size();++i)put(*f.session,"f"+std::to_string(i),original[i]);
  const auto extent=f.image->extent();
  const auto committed=f.image->calls;
  NativeGroupMaintenance controller(*f.session);
  NativeMaintenanceProgress progress;
  for(unsigned i=0;i<2000 && !progress.search_batch_ready;++i) {
    const auto o=f.clock.next();
    progress=controller.advance(o,f.clock.now,{members+families,nullptr,true});
    require(progress.phase!=Phase::failed,"Capacity fixture did not prepare");
  }
  require(controller.search_batch().size()==families,"Capacity fixture did not retain a complete batch");
  for(unsigned i=0;i<2000 && progress.deferral!=Defer::capacity;++i) {
    const auto o=f.clock.next();
    progress=controller.advance(o,f.clock.now,{0,nullptr,true});
    require(progress.phase!=Phase::failed,"Append refusal fenced the indexed volume");
  }
  require(progress.deferral==Defer::capacity && progress.append_shortfall_bytes &&
      progress.published_groups==0 && progress.declined_groups==1 &&
      !controller.pending() && !controller.has_retained_search() && !progress.owned_payload_bytes &&
      f.image->extent()==extent && f.image->calls==committed,
      "Append refusal did not release the batch before publication");
  const auto space=f.session->indexed_volume_space();
  require(space.compression_shortfall_bytes==progress.append_shortfall_bytes && space.reclamation_worthwhile(),
      "The selected image did not offer bounded capacity reclamation");
  const auto reads=f.image->read_calls;
  for(unsigned i=0;i<4;++i) {
    const auto o=f.clock.next();
    require(controller.advance(o,f.clock.now,{members+families,nullptr,true}).deferral==Defer::capacity &&
        !controller.pending(),"Unchanged capacity refusal restarted expensive preparation");
  }
  require(f.image->calls==committed && f.image->read_calls==reads,
      "Paused capacity refusal read or modified the image");

  auto replacement=ram(kStoreCommitUnit);
  require(f.session->begin_idle_compaction(replacement,id(9)),"Capacity fixture could not start a protected rebuild");
  bool ready=false;
  for(unsigned i=0;i<4000 && !ready;++i)
    ready=f.session->advance_recompression()==NativeFileSession::RecompressionProgress::ready;
  require(ready && f.session->publish_idle_recompression().has_value() && f.image->calls==committed &&
      f.session->indexed_volume_space().free_bytes>space.free_bytes &&
      !f.session->indexed_volume_space().compression_shortfall_bytes && controller.pending(),
      "Verified reclamation did not reopen compression on the selected copy");
  // Sanitized operations can require more idle credit. Count admitted work
  // toward the progress bound, and require each idle deferral to pay down debt.
  for(unsigned admitted=0;admitted<1000 && !progress.published_groups;) {
    const auto previous_delay=progress.deferral==Defer::duty?progress.next_opportunity_delay_ms:0;
    progress=f.step(controller);
    require(progress.phase!=Phase::failed,"Compression failed after reclaiming append space");
    if(progress.deferral==Defer::duty) {
      require(progress.next_opportunity_delay_ms &&
          (!previous_delay || progress.next_opportunity_delay_ms<previous_delay),
          "Idle observations did not advance the capacity resume gate");
    } else {
      ++admitted;
    }
  }
  require(progress.published_groups!=0,"Compression did not resume after verified compaction");
  const auto snapshot=f.session->snapshot();
  snapshot.verify_all();
  for(std::size_t i=0;i<original.size();++i)
    require(snapshot.read("f"+std::to_string(i),0,original[i].size())==original[i],
        "Reclamation or resumed compression changed acknowledged bytes");
  std::cout<<"indexed capacity shortfall="<<space.compression_shortfall_bytes
      <<" free_before="<<space.free_bytes<<" source_unchanged=true resumed=true\n";
}

void indexed_partial_overlap_preserves_candidates() {
  const auto bank=data(anchor_search_fixture::indexed_sample_bank_bytes,4839);
  auto original=anchor_search_fixture::indexed_batch_files(bank);
  const auto independent=data(kNativeExtentBytes,15293);
  // Put a small independent cohort in the window containing the larger,
  // partially overlapping cohorts. Its candidate ranks below their conflict.
  for(std::size_t i=0;i<3;++i) {
    auto extent=std::span(original[i]).last(kNativeExtentBytes);
    std::ranges::copy(independent,extent.begin());
    extent[23]^=static_cast<std::byte>(i);
  }
  const auto compare=[&](bool offer_batch) {
    Fixture f(4096,0,true);
    for(std::size_t i=0;i<original.size();++i)put(*f.session,"f"+std::to_string(i),original[i]);
    NativeGroupMaintenance controller(*f.session);
    bool emitted=false;
    for(unsigned i=0;i<5200 && controller.pending();++i) {
      const auto o=f.clock.next();
      const auto grant=offer_batch && !emitted?anchor_search_fixture::indexed_batch_members:0;
      const auto progress=controller.advance(o,f.clock.now,{grant,nullptr,offer_batch});
      require(progress.phase!=Phase::failed && progress.deferral!=Defer::capacity,"Overlap fixture did not settle");
      if(progress.search_batch_ready) {
        const auto windows=controller.search_batch();
        std::cout<<"indexed overlap first_batch_groups="<<windows.size()<<'\n';
        require(std::ranges::any_of(windows,[&](const auto& window) {
          return std::ranges::equal(window.base(),independent) && window.members().size()==2;
        }),"A partial overlap prevented the independent candidate from joining the batch");
        emitted=true;
      }
    }
    require(!controller.pending() && emitted==offer_batch && controller.progress().published_groups,
        "Overlap fixture did not exercise its requested grouping path");
    f.session.reset();
    const auto closed=NativeVolumeHeadView::open(f.head,id(90),[&](const auto& image)->std::shared_ptr<StoreByteIo> {
      return image==id(1)?f.image:nullptr;
    });
    auto compacted=ram(4096);
    (void)NativeFilesystemWriter::compact(closed.filesystem(),compacted,id(2),{StoreCompressionMode::raw,1,false});
    auto verified=NativeFilesystemSnapshot::open_image(compacted,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verified.verify_all();
    for(std::size_t i=0;i<original.size();++i)
      require(verified.read("f"+std::to_string(i),0,original[i].size())==original[i],"Overlap fixture changed source bytes");
    return compacted->extent();
  };
  const auto cpu=compare(false),batched=compare(true);
  require(batched<=cpu,"Staging discarded partly overlapping candidates");
  std::cout<<"indexed overlap cpu_bytes="<<cpu<<" batched_bytes="<<batched<<'\n';
}

void modular_delta_indexed_family(const Bytes& base,StoreCompressionOptions compression,bool require_delta) {
  constexpr unsigned count=9;
  Fixture f(4096,0,true,compression);
  std::vector<Bytes> original;
  for(unsigned i=0;i<count;++i) {
    original.push_back(calibrated_extent(base,i));
    put(*f.session,"f"+std::to_string(i),original.back());
  }
  const auto compact_size=[&](const auto& snapshot,unsigned identity) {
    auto output=ram(4096);
    (void)NativeFilesystemWriter::compact(snapshot,output,id(identity),compression);
    auto reopened=NativeFilesystemSnapshot::open_image(output,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    for(unsigned i=0;i<count;++i)
      require(reopened.read("f"+std::to_string(i),0,kNativeExtentBytes)==original[i],
          "Modular delta compaction changed source bytes");
    return output->extent();
  };
  const auto resolve=[&](const auto& image)->std::shared_ptr<StoreByteIo> {return image==id(1)?f.image:nullptr;};
  f.session.reset();
  const auto baseline=compact_size(NativeVolumeHeadView::open(f.head,id(90),resolve).filesystem(),2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,compression));
  NativeGroupMaintenance controller(*f.session);
  const auto start=std::chrono::steady_clock::now();
  const auto progress=f.finish(controller);
  const auto maintenance_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-start).count();
  controller.cancel();
  f.session.reset();
  const auto closed=NativeVolumeHeadView::open(f.head,id(90),resolve);
  const auto grouped=compact_size(closed.filesystem(),3);
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  std::cout<<"indexed_modular_delta files="<<count<<" groups="<<progress.published_groups
      <<" capability="<<capability<<" input_bytes="<<baseline<<" output_bytes="<<grouped
      <<" maintenance_ns="<<maintenance_ns<<" cpu_readback=true\n";
  if(require_delta)require(progress.published_groups && capability==6 && grouped<baseline,
      "Automatic maintenance missed fully shifted numeric data");
}

void shifted_indexed_family(const Bytes& base,StoreCompressionOptions compression,bool corpus,bool section_shifts=false) {
  constexpr unsigned count=12;
  constexpr std::uint32_t unit=4096;
  Fixture f(unit,0,true,compression);
  std::vector<Bytes> originals;
  for(unsigned i=0;i<count;++i) {
    auto input=base;
    if(i && section_shifts) {
      constexpr std::size_t section_bytes=256;
      for(std::size_t offset=0;offset<input.size();offset+=section_bytes) {
        const auto from=input.begin()+offset;
        std::rotate(from,from+i,from+section_bytes);
      }
    } else if(i) {
      const auto prefix=7*i+1;
      std::copy_n(base.begin(),base.size()-prefix,input.begin()+prefix);
      std::fill_n(input.begin(),prefix,static_cast<std::byte>(i));
    }
    put(*f.session,"shifted"+std::to_string(i),input);
    originals.push_back(std::move(input));
  }
  std::size_t anchor_frame_bytes=0;
  if(section_shifts) {
    const detail::NativeGroupAnchorIndex anchor(base);
    for(const auto& member:std::span(originals).subspan(1)) {
      const auto frame=anchor.encode(member,base,detail::kNativeGroupBaseBit+1,1,kNativeExtentBytes);
      require(frame && detail::decode_native_group_member(*frame,base,detail::NativeGroupMethod::anchor_copies)==member,
          "Indexed copy comparison changed the member bytes");
      anchor_frame_bytes+=frame->size();
    }
  }
  const auto before=f.session->snapshot();
  const auto workload=f.session->workload_info();
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const auto baseline_source=image(f.image->stable,unit);
  auto baseline=ram(unit);
  auto copied=NativeFilesystemWriter::compact(NativeFilesystemSnapshot::open_image(baseline_source,indexed),
      baseline,id(2),compression);
  copied.snapshot().verify_all();
  NativeGroupMaintenance controller(*f.session);
  const auto result=f.finish(controller);
  const auto after=f.session->snapshot();
  require(result.published_groups==1 && after.shared_base_info().members==count &&
      after.shared_base_info().bases==1 && f.session->workload_info()==workload,"shifted extents were not grouped");
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  if(!section_shifts)require(root[8]==std::byte{4},"shifted group did not promote the indexed capability");
  controller.cancel();
  f.session.reset();
  auto compacted=ram(unit);
  (void)NativeFilesystemWriter::compact(after,compacted,id(3),compression);
  const auto reopened=NativeFilesystemSnapshot::open_image(compacted,indexed);
  reopened.verify_all();
  for(unsigned i=0;i<count;++i) {
    const auto path="shifted"+std::to_string(i);
    require(reopened.read(path,0,kNativeExtentBytes)==originals[i] &&
        before.read(path,0,kNativeExtentBytes)==originals[i],"shifted group changed current or prior bytes");
  }
  require(compacted->extent()<baseline->extent(),"complete shifted image did not shrink");
  std::cout<<"shifted_indexed corpus="<<corpus<<" section_shifts="<<section_shifts
      <<" members="<<count<<" capability="<<std::to_integer<unsigned>(root[8])
      <<" assembled_groups="<<result.section_reference_groups<<" before_bytes="<<baseline->extent()
      <<" after_bytes="<<compacted->extent()<<" minimum_compacted_saved="<<result.minimum_compacted_bytes_saved
      <<" anchor_frame_bytes="<<anchor_frame_bytes<<'\n';
}
void xor_indexed_family(bool require_xor,bool throughout=false) {
  constexpr unsigned count=9,unit=4096;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  Fixture f(unit,0,true,raw);
  const auto base=data(kNativeExtentBytes,714);
  std::array<Bytes,count> originals;
  for(unsigned member=0;member<count;++member) {
    originals[member]=base;
    if(member) {
      const auto variation=data(kNativeExtentBytes,861+member);
      // The projected case varies both middle bit planes in every band.
      const auto start=throughout?0U:kNativeExtentBytes/8;
      const auto mask=throughout?std::byte{0x30}:std::byte{1};
      for(std::size_t i=start;i<base.size();++i)
        originals[member][i]^=variation[i]&mask;
    }
    put(*f.session,"xor"+std::to_string(member),originals[member]);
  }
  const auto prior=f.session->snapshot();
  const auto baseline_source=image(f.image->stable,unit);
  auto baseline=ram(unit);
  (void)NativeFilesystemWriter::compact(NativeFilesystemSnapshot::open_image(baseline_source,indexed),
      baseline,id(2),raw);

  NativeGroupMaintenance maintenance(*f.session);
  const auto workload=f.session->workload_info();
  const auto result=f.finish(maintenance);
  const auto after=f.session->snapshot();
  const auto root=PagedObjectStoreSnapshot::open(f.image,{kStoreMaxPhysicalBytes}).read(1);
  const auto capability=std::to_integer<unsigned>(root[8]);
  require(f.session->workload_info()==workload,"XOR grouping changed user input provenance");
  maintenance.cancel();
  f.session.reset();
  auto compacted=ram(unit);
  (void)NativeFilesystemWriter::compact(after,compacted,id(3),raw);
  const auto reopened=NativeFilesystemSnapshot::open_image(compacted,indexed);
  reopened.verify_all();
  for(unsigned member=0;member<count;++member) {
    const auto path="xor"+std::to_string(member);
    require(reopened.read(path,0,kNativeExtentBytes)==originals[member] &&
        prior.read(path,0,kNativeExtentBytes)==originals[member],"XOR group changed current or prior bytes");
  }
  if(require_xor)
    require(capability==5 && result.published_groups==1 && reopened.shared_base_info().members==count &&
        reopened.shared_base_info().bases==1 && compacted->extent()<baseline->extent()/2,
        "automatic indexed XOR group was absent or missed its expected size");
  std::cout<<"indexed_xor throughout="<<throughout<<" capability="<<capability
      <<" members="<<reopened.shared_base_info().members
      <<" logical_bytes="<<count*kNativeExtentBytes<<" before_bytes="<<baseline->extent()
      <<" after_bytes="<<compacted->extent()<<" groups="<<result.published_groups
      <<" accounted_savings="<<result.minimum_compacted_bytes_saved<<'\n';
}
void xor_reference_selection(anchor_search_fixture::IndexedBatchPattern pattern) {
  using Pattern=anchor_search_fixture::IndexedBatchPattern;
  constexpr unsigned count=9,unit=4096;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const NativeImageStorage indexed{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes};
  const bool full=pattern==Pattern::xor_full_reference;
  std::array<Bytes,count> originals;
  const auto base=data(kNativeExtentBytes,74181);
  for(unsigned i=0;i<count;++i) {
    originals[i]=base;
    if(full)anchor_search_fixture::apply_full_reference_variation(originals[i],base,i,count);
    else if(pattern==Pattern::xor_sparse_reference)anchor_search_fixture::apply_sparse_reference_variation(originals[i],i,count);
    else if(pattern==Pattern::xor_tile_reference)anchor_search_fixture::apply_tile_reference_variation(originals[i],i,count);
    else anchor_search_fixture::apply_reference_variation(originals[i],i);
  }
  const auto run=[&](bool automatic) {
    Fixture f(unit,0,true,raw);
    for(unsigned i=0;i<count;++i)put(*f.session,"r"+std::to_string(i),originals[i]);
    const auto before=f.session->snapshot();
    const auto workload=f.session->workload_info();
    if(automatic) {
      NativeGroupMaintenance controller(*f.session);
      if(full) {
        for(unsigned opportunity=0;opportunity<4*count && !controller.progress().cpu_reference_pairs;++opportunity)
          f.step(controller);
        require(controller.progress().cpu_reference_pairs==1,"CPU pricing did not start one pair at a time");
        auto foreground=f.clock.next();
        foreground.foreground_pending=true;
        const auto reads=f.image->read_calls,writes=f.image->calls;
        const auto deferred=controller.advance(foreground,f.clock.now);
        require(deferred.deferral==Defer::foreground && deferred.cpu_reference_pairs==1 &&
            f.image->read_calls==reads && f.image->calls==writes,"CPU pricing did not yield to foreground work");
      }
      const auto result=f.finish(controller);
      require(result.published_groups==1,"Automatic reference did not publish one group");
      if(full)require(result.cpu_reference_pairs==32 && result.reselected_reference_groups==1,
          "The complete search did not improve the sampled reference");
    }else {
      // Apply the same encoder and publication checks using the first extent.
      std::array<NativeExtentSelection,count> group;
      for(unsigned i=0;i<count;++i)group[i]={before.stat("r"+std::to_string(i)).inode,0};
      auto work=f.session->prepare_shared_base(group.front(),group);
      bool ready=false;
      for(unsigned step=0;step<4*count && !ready;++step)ready=f.session->advance_shared_base(work);
      require(ready && f.session->finish_shared_base(work).applied,"First-reference control failed to publish");
    }
    const auto after=f.session->snapshot();
    require(after.shared_base_info().members==count && after.shared_base_info().bases==1 &&
        f.session->workload_info()==workload,"Reference choice changed the group or its input accounting");
    f.session.reset();
    auto packed=ram(unit);
    (void)NativeFilesystemWriter::compact(after,packed,id(2),raw);
    const auto reopened=NativeFilesystemSnapshot::open_image(packed,indexed);
    reopened.verify_all();
    for(unsigned i=0;i<count;++i) {
      const auto path="r"+std::to_string(i);
      require(reopened.read(path,0,kNativeExtentBytes)==originals[i] &&
          before.read(path,0,kNativeExtentBytes)==originals[i],"Reference choice altered current or prior bytes");
    }
    return packed->extent();
  };
  const auto first=run(false),selected=run(true);
  require(selected<first,"Automatic reference did not improve the complete image");
  const auto label=full?"xor_full_reference":pattern==Pattern::xor_sparse_reference?"xor_sparse_reference":
      pattern==Pattern::xor_tile_reference?"xor_tile_reference":"xor_reference";
  std::cout<<label<<" members="<<count
      <<" first_bytes="<<first<<" selected_bytes="<<selected
      <<" byte_exact=true\n";
}
void xor_reference_decline_fallback() {
  constexpr std::size_t count=3,sites=8,tile_bytes=256,spacing=kNativeExtentBytes/(sites*tile_bytes);
  constexpr std::size_t bands=16,band_bytes=64,band_spacing=kNativeExtentBytes/bands;
  Fixture f(4096,0,true);
  const auto base=data(kNativeExtentBytes,73217);
  std::array<Bytes,count> originals{base,base,base};
  const auto variation=data(kNativeExtentBytes,31804);
  for(std::size_t i=0;i<base.size();++i)
    originals[i<base.size()/2?1:2][i]^=variation[i];
  for(std::size_t band=0;band<bands;++band) {
    // Preserve the shared masked signature without introducing long copies
    // between members whose nonmatching halves contain independent bytes.
    for(std::size_t i=0;i<band_bytes;++i) {
      const auto offset=band*band_spacing+i;
      originals[1][offset]=base[offset]^(i%2?std::byte{0x80}:std::byte{});
      originals[2][offset]=originals[1][offset]^std::byte{0x80};
    }
  }
  const auto second=data(kNativeExtentBytes,89325),third=data(kNativeExtentBytes,61773);
  for(std::size_t site=0;site<sites;++site) {
    // The second member shares one plane with each of the other members in
    // the sampled tiles. Those few matches cannot pay for an encoded frame
    // between the second and third members.
    const auto offset=(site*spacing+2*((site*7)%(spacing/2))+1)*tile_bytes;
    for(std::size_t i=offset;i<offset+tile_bytes;++i) {
      originals[1][i]=(second[i]&~std::byte{2})|(base[i]&std::byte{2});
      originals[2][i]=(third[i]&~std::byte{1})|(originals[1][i]&std::byte{1});
    }
  }
  for(std::size_t i=0;i<count;++i)put(*f.session,"r"+std::to_string(i),originals[i]);
  NativeGroupMaintenance controller(*f.session);
  const auto result=f.finish(controller);
  const auto snapshot=f.session->snapshot();
  require(result.published_groups==1 && result.declined_groups>=1 &&
      snapshot.shared_base_info().members==count,"Declined estimate suppressed the first reference");
  snapshot.verify_all();
  for(std::size_t i=0;i<count;++i)
    require(snapshot.read("r"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
        "Reference fallback changed file contents");
  std::cout<<"xor_reference_fallback declined="<<result.declined_groups<<" groups="<<result.published_groups
      <<" byte_exact=true\n";
}
void indexed_group_with_outlier() {
  constexpr unsigned related=4;
  Fixture f(4096,0,true);
  std::array<Bytes,related+1> originals;
  const auto base=family(0);
  for(unsigned i=0;i<related;++i) {
    originals[i]=base;
    const auto variation=data(base.size(),917+4627*i);
    for(std::size_t at=0;at<base.size();++at)
      originals[i][at]^=static_cast<std::byte>(std::to_integer<unsigned>(variation[at])%3);
  }
  auto& outlier=originals.back();
  outlier=data(kNativeExtentBytes,435187);
  // Match one sampled masked-XOR signature. Outside those 16 short ranges
  // the unrelated extents have no common byte runs or complete bit planes.
  for(std::size_t sample=0;sample<16;++sample) {
    const auto start=sample*kNativeExtentBytes/16;
    for(std::size_t at=start;at<start+64;++at)outlier[at]=base[at]^std::byte{3};
  }
  for(unsigned i=0;i<originals.size();++i)put(*f.session,"outlier-"+std::to_string(i),originals[i]);

  NativeGroupMaintenance controller(*f.session);
  bool saw_full_window=false;
  for(unsigned tick=0;tick<1200 && !controller.progress().published_groups;++tick) {
    const auto observation=f.clock.next();
    const auto result=controller.advance(observation,f.clock.now,{related,nullptr});
    require(result.phase!=Phase::failed,"outlier proposal failed");
    if(const auto window=controller.search_window()) {
      if(window.members().size()==related && std::ranges::any_of(window.members(),[&](const auto& member) {
        return member==outlier;
      }))saw_full_window=true;
    }
  }
  const auto result=f.finish(controller);
  const auto snapshot=f.session->snapshot();
  require(saw_full_window && result.published_groups==1 && result.declined_groups>0 &&
      result.minimum_compacted_bytes_saved>0 && snapshot.shared_base_info().members==related,
      "strong members were lost or the outlier joined the published group");
  snapshot.verify_all();
  for(unsigned i=0;i<originals.size();++i)
    require(snapshot.read("outlier-"+std::to_string(i),0,kNativeExtentBytes)==originals[i],
        "subset publication changed file bytes");
  std::cout<<"indexed_group_outlier members="<<snapshot.shared_base_info().members
      <<" declined="<<result.declined_groups<<" byte_exact=true\n";
}
void automatic_and_extension(std::uint32_t unit,bool indexed=false) {
  Fixture f(unit,8,indexed);const auto workload=f.session->workload_info();NativeGroupMaintenance original(*f.session);
  rejects([&]{NativeGroupMaintenance duplicate(*f.session);},"duplicate controller admitted");
  auto controller=std::move(original);require(original.progress().phase==Phase::failed,"moved controller stayed live");
  auto result=f.finish(controller);f.verify(8);
  require(result.published_groups==1 && result.minimum_compacted_bytes_saved>0 && result.retained_history_bytes_added>0,
      "related files not grouped as one beneficial group");
  require(f.session->snapshot().shared_base_info().members==8 && f.session->snapshot().shared_base_info().bases==1 &&
      f.session->workload_info()==workload,"grouping changed provenance or fragmented family");
  const auto reads=f.image->read_calls,calls=f.image->calls,attempts=result.attempted_groups;
  for(unsigned i=0;i<100;++i)require(f.step(controller).phase==Phase::idle,"unchanged source did not stay idle");
  require(f.image->read_calls==reads && f.image->calls==calls,"unchanged opportunities performed backing I/O");
  put(*f.session,"f8",family(8));const auto input=f.session->workload_info();result=f.finish(controller);f.verify(9);
  require(result.published_groups==2 && f.session->snapshot().shared_base_info().members==9 && f.session->snapshot().shared_base_info().bases==1,
      "new related file did not merge into protected existing group");
  require(f.session->workload_info()==input && result.attempted_groups>attempts,"regrouping minted input");
  std::cout<<"automatic_groups indexed="<<indexed<<" unit="<<unit<<" published="<<result.published_groups<<" sampled="<<result.sampled_extents
      <<" reused="<<result.reused_features<<" minimum_compacted_saved="<<result.minimum_compacted_bytes_saved<<'\n';
}
void admission() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  const auto reads=f.image->read_calls,calls=f.image->calls;
  for(unsigned variant=0;variant<12;++variant) {
    auto o=f.clock.next();const auto now=f.clock.now;
    if(variant==0)o.valid=false;if(variant==1)o.captured_ms=now+1;if(variant==2)o.captured_ms=now-251;
    if(variant==3)o.physical_total=0;if(variant==4)o.physical_available=o.physical_total+1;
    if(variant==5)o.commit_limit=0;if(variant==6)o.commit_available=o.commit_limit+1;
    if(variant==7)o.cpu_total_ticks=0;if(variant==8)o.cpu_busy_ticks=1001;
    if(variant==9)o.cpu_window_ms=0;if(variant==10)o.cpu_window_ms=2001;if(variant==11)o.captured_ms=0;
    require(controller.advance(o,now).deferral==Defer::observation,"bad observation admitted");
  }
  auto o=f.clock.next();o.foreground_pending=true;require(controller.advance(o,f.clock.now).deferral==Defer::foreground,"foreground queue ignored");
  o=f.clock.next();o.cpu_busy_ticks=126;require(controller.advance(o,f.clock.now).deferral==Defer::cpu,"busy CPU admitted");
  o=f.clock.next();o.physical_available=0;require(controller.advance(o,f.clock.now).deferral==Defer::memory,"pressure admitted");
  require(f.image->read_calls==reads && f.image->calls==calls,"refused observations performed I/O");
  o=f.clock.next();controller.advance(o,f.clock.now);const auto sampled=controller.progress().sampled_extents;
  require(controller.advance(o,f.clock.now).deferral==Defer::observation && controller.progress().sampled_extents==sampled,"replayed observation admitted");
  const auto snapshot=f.session->snapshot();std::vector<NativeExtentSelection> keys;
  for(unsigned i=0;i<4;++i)keys.push_back({snapshot.stat("f"+std::to_string(i)).inode,0});
  auto explicit_work=f.session->prepare_shared_base(keys.front(),keys);const auto before=f.image->read_calls;
  require(f.step(controller).deferral==Defer::other_maintenance && f.image->read_calls==before,"competing work was not deferred");
  explicit_work.cancel();f.finish(controller);f.verify(4);
}
void continued_preparation_admission() {
  Fixture f(4096,8,true);
  NativeMaintenancePolicy policy;policy.work_share_ppm=50000;
  NativeGroupMaintenance controller(*f.session,policy);
  auto seen=f.clock.next();
  controller.advance(seen,f.clock.now);
  const auto held_reads=f.image->read_calls,held_writes=f.image->calls;
  auto invalid=f.clock.next();invalid.continued=true;
  require(controller.advance(invalid,f.clock.now).deferral==Defer::observation &&
      f.image->read_calls==held_reads && f.image->calls==held_writes,"Continuation advanced its CPU capture");

  unsigned continued=0;
  for(unsigned sample=0;sample<128 && controller.progress().phase!=Phase::publishing;++sample) {
    seen=f.clock.next();
    auto result=controller.advance(seen,f.clock.now);
    require(result.phase!=Phase::failed,"Live group preparation failed");
    for(unsigned step=0;step<16 && result.deferral==Defer::none &&
        result.phase!=Phase::publishing && result.phase!=Phase::idle;++step) {
      auto reuse=seen;reuse.sequence=++f.clock.sequence;reuse.continued=true;
      const auto image_calls=f.image->calls,head_calls=f.head->calls;
      result=controller.advance(reuse,f.clock.now);
      require(result.phase!=Phase::failed && f.image->calls==image_calls && f.head->calls==head_calls,
          "Continued preparation changed backing storage");
      if(result.deferral==Defer::none)++continued;
    }
  }
  require(continued && controller.progress().phase==Phase::publishing,"Continuation did not reach prepared group");
  auto reuse=seen;reuse.sequence=++f.clock.sequence;reuse.continued=true;
  const auto image_calls=f.image->calls,head_calls=f.head->calls;
  require(controller.advance(reuse,f.clock.now).deferral==Defer::observation &&
      f.image->calls==image_calls && f.head->calls==head_calls,"Reused CPU capture published a group");
  reuse.sequence=++f.clock.sequence;reuse.foreground_pending=true;
  require(controller.advance(reuse,f.clock.now).deferral==Defer::foreground &&
      f.image->calls==image_calls && f.head->calls==head_calls,"Continued work ignored foreground activity");
  const auto complete=f.finish(controller);
  require(complete.published_groups==1,"Fresh observation did not publish prepared group");
  f.verify(8);
}
void rejected_observation_drops_duty_delay() {
  Fixture f(4096,32,true);
  NativeMaintenancePolicy policy;
  policy.work_share_ppm=1;
  NativeGroupMaintenance controller(*f.session,policy);
  auto observed=f.clock.next();
  auto progress=controller.advance(observed,observed.captured_ms);
  // Each new sample earns one nanosecond of credit. Reach a real duty
  // deferral without relying on host scheduling or a long wall-clock wait.
  for(unsigned i=0;i<64 && progress.deferral!=Defer::duty && controller.pending();++i) {
    ++observed.sequence;
    ++observed.captured_ms;
    progress=controller.advance(observed,observed.captured_ms);
  }
  require(progress.deferral==Defer::duty && progress.next_opportunity_delay_ms,
      "The fixture did not establish an outstanding duty estimate");
  const auto reads=f.image->read_calls,writes=f.image->calls;
  const auto stale=controller.advance(observed,observed.captured_ms);
  require(stale.deferral==Defer::observation && !stale.next_opportunity_delay_ms,
      "Rejected observation retained a previous duty delay");
  ++observed.sequence;
  ++observed.captured_ms;
  const auto fresh=controller.advance(observed,observed.captured_ms);
  require(fresh.deferral==Defer::duty && fresh.next_opportunity_delay_ms &&
      f.image->read_calls==reads && f.image->calls==writes,
      "Reporting a refused observation erased outstanding duty credit");
  std::cout<<"observation_retry stale_delay_cleared=1 remaining_duty_preserved=1 backing_io=0\n";
}
void negative_cache_and_incompressible() {
  Fixture f(4096,2);const auto snapshot=f.session->snapshot();
  const std::array<NativeExtentSelection,2> keys{{{snapshot.stat("f0").inode,0},{snapshot.stat("f1").inode,0}}};
  auto optimized=f.session->prepare_shared_base(keys.front(),keys);
  while(!f.session->advance_shared_base(optimized)){}
  require(f.session->finish_shared_base(optimized).applied,"negative fixture not already optimized");
  NativeGroupMaintenance controller(*f.session);const auto state=f.finish(controller);
  require(state.published_groups==0 && state.declined_groups>0,"unprofitable pair published");
  const auto reads=f.image->read_calls,calls=f.image->calls;
  for(unsigned i=0;i<50;++i)f.step(controller);
  require(f.image->read_calls==reads && f.image->calls==calls,"negative source repeated I/O");
  f.session->create_entry("empty");const auto before=controller.progress();const auto after=f.finish(controller);
  require(after.sampled_extents==before.sampled_extents && after.attempted_groups==before.attempted_groups,
      "metadata-only change repeated content sampling/failed group");
  Fixture other(4096,0);put(*other.session,"a",data(kNativeExtentBytes,1));put(*other.session,"b",data(kNativeExtentBytes,2));
  NativeGroupMaintenance no_match(*other.session);const auto result=other.finish(no_match);
  require(result.published_groups==0 && result.attempted_groups==0 && result.sampled_extents==2,"unrelated data incorrectly nominated");
}
void ownership_and_pressure() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().owned_payload_bytes==0;++i)f.step(controller);
  require(controller.progress().owned_payload_bytes>0,"no private work to pressure");
  auto observation=f.clock.next();observation.commit_available=0;const auto writes=f.image->writes;
  const auto refused=controller.advance(observation,f.clock.now);
  require(refused.deferral==Defer::memory && refused.owned_payload_bytes==0 && f.image->writes==writes,"pressure retained buffers or wrote data");
  NativeFileSession moved=std::move(*f.session);f.session.reset();f.session.emplace(std::move(moved));f.finish(controller);f.verify(4);
  const auto reads=f.image->read_calls;f.session.reset();require(f.step(controller).phase==Phase::failed && f.image->read_calls==reads,"expired session accessed data");
  Fixture next(4096,2);NativeGroupMaintenance cancelled(*next.session);cancelled.cancel();
  require(next.step(cancelled).phase==Phase::failed,"cancelled controller restarted");NativeGroupMaintenance replacement(*next.session);next.finish(replacement);
}
void foreground_change_during_preparation() {
  Fixture f(4096,4);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().owned_payload_bytes==0;++i)f.step(controller);
  require(controller.progress().owned_payload_bytes>0,"no prepared content to invalidate");
  const auto replacement=data(kNativeExtentBytes,919);
  auto handle=f.session->open("f0",{true,true,false});f.session->write(handle,0,replacement);
  f.session->cleanup(handle,false);f.session->close(handle);
  const auto sampled=controller.progress().sampled_extents;
  const auto result=f.finish(controller);const auto snapshot=f.session->snapshot();
  require(result.sampled_extents>sampled && result.published_groups==1,"changed content was not resampled and reconciled");
  require(snapshot.read("f0",0,kNativeExtentBytes)==replacement,"stale prepared group overwrote foreground content");
  for(unsigned i=1;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"reconciled group changed other data");
  require(snapshot.shared_base_info().members==3,"unrelated replacement entered stale group");snapshot.verify_all();
}
void indexed_scan_windows() {
  Fixture f(4096,0,true);NativeGroupMaintenance controller(*f.session);
  require(!controller.pending(),"empty indexed volume requested discovery");
  constexpr unsigned count=2*kNativeMaxDirtyExtents+3;
  constexpr std::uint64_t sparse_offset=1ULL<<38;
  f.session->create_entry("churn");
  auto churn=f.session->open("churn",{true,true,true});
  Bytes expected{std::byte{0x40}};
  f.session->write(churn,sparse_offset,expected);
  require(!controller.pending(),"one sparse extent requested a group");
  for(unsigned i=0;i<count;++i)put(*f.session,"f"+std::to_string(i),family(i));
  require(controller.pending(),"indexed volume did not request discovery");
  std::uint64_t max_scan_bytes=0;unsigned rewrites=0;
  for(unsigned i=0;i<1000 && controller.progress().published_groups==0;++i) {
    if(i%11==0) {
      expected[0]^=std::byte{1};f.session->write(churn,sparse_offset,expected);++rewrites;
    }
    const auto reads=f.image->read_bytes;const auto prior=controller.progress();
    const auto state=f.step(controller);
    require(state.phase!=Phase::failed,"indexed discovery failed while advancing a live cursor");
    if(prior.phase==Phase::scanning || prior.phase==Phase::candidates)
      max_scan_bytes=(std::max)(max_scan_bytes,f.image->read_bytes-reads);
  }
  require(controller.progress().published_groups!=0 && rewrites>1,"indexed cursor did not reach a family under foreground writes");
  const auto final=f.finish(controller);
  require(final.published_groups>=3 && final.minimum_compacted_bytes_saved>0 && max_scan_bytes<1024*1024,
      "indexed batching, sizing or one-extent scan bound failed");
  auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().members==count && snapshot.shared_base_info().bases==1,
      "indexed windows failed to extend one immutable base");
  require(snapshot.read("churn",sparse_offset,1)==expected,"background scan changed sparse foreground bytes");
  f.verify(count);
  f.session->rename(churn,"renamed");
  f.session->cleanup(churn,false);f.session->close(churn);
  f.session->create_entry("empty");
  const auto reads=f.image->read_calls,calls=f.image->calls;
  require(!controller.pending() && f.step(controller).phase==Phase::idle &&
      f.image->read_calls==reads && f.image->calls==calls,"indexed metadata changes restarted payload discovery");
  std::cout<<"indexed_scan members="<<count<<" published_groups="<<final.published_groups<<" foreground_rewrites="<<rewrites
      <<" max_scan_read_bytes="<<max_scan_bytes<<" lower_bound_savings="<<final.minimum_compacted_bytes_saved<<'\n';
}
void indexed_stale_binding_and_padding() {
  Fixture f(4096,4,true);NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.progress().phase!=Phase::candidates;++i)f.step(controller);
  require(controller.progress().phase==Phase::candidates,"indexed sample window was not ready");
  auto handle=f.session->open("f0",{true,true,false});
  const auto changed=data(kNativeExtentBytes,449);
  f.session->write(handle,0,changed);f.session->cleanup(handle,false);f.session->close(handle);
  for(unsigned i=0;i<1000 && !controller.active_group();++i)f.step(controller);
  require(controller.active_group(),"saved candidate did not reach its generation check");
  const auto reads=f.image->read_bytes,calls=f.image->calls;
  for(unsigned i=0;i<1000 && controller.active_group();++i)f.step(controller);
  require(!controller.active_group() && f.image->read_bytes-reads<kNativeExtentBytes && f.image->calls==calls &&
      !f.session->poisoned(),"a changed sampled base was decoded or published");
  (void)f.finish(controller);
  auto view=f.session->snapshot();
  require(view.shared_base_info().members==3 && view.read("f0",0,kNativeExtentBytes)==changed,
      "stale sample caused incomplete recovery or changed the acknowledged bytes");
  view.verify_all();
  Fixture small(4096,0,true);
  put(*small.session,"first",data(128,1));put(*small.session,"second",data(192,2));
  NativeGroupMaintenance padding(*small.session);const auto result=small.finish(padding);
  require(result.sampled_extents==2 && result.attempted_groups==0,"zero-padded bands nominated unrelated small files");
}
void publication_faults(std::uint32_t unit) {
  Fixture original(unit,4);const auto bytes=original.image->stable,head=original.head->stable;original.session.reset();
  auto setup=[&](const auto& data_io,const auto& control){return NativeFileSession::resume_volume(control,id(90),1,
      [&](const auto&)->std::shared_ptr<StoreByteIo>{return data_io;},{StoreCompressionMode::raw,1,false});};
  auto advance_ready=[&](auto& controller,Clock& clock){for(unsigned i=0;i<1000;++i){const auto o=clock.next();const auto result=controller.advance(o,clock.now);
    if(result.phase==Phase::publishing)return;}throw std::runtime_error("publication phase not reached");};
  auto probe=image(bytes,unit),probe_head=image(head,unit);std::size_t cuts=0;
  {auto session=setup(probe,probe_head);NativeGroupMaintenance controller(session);Clock clock;advance_ready(controller,clock);const auto start=probe->calls;
    for(unsigned i=0;i<1000 && controller.progress().published_groups==0;++i){const auto o=clock.next();controller.advance(o,clock.now);}
    require(controller.progress().published_groups==1,"probe did not publish");cuts=probe->calls-start;}
  require(cuts>3 && cuts<256,"automatic publication fault envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto data_io=image(bytes,unit),control=image(head,unit);
    {auto session=setup(data_io,control);NativeGroupMaintenance controller(session);Clock clock;advance_ready(controller,clock);
      data_io->failure=mode;data_io->fail_at=data_io->calls+cut;
      for(unsigned i=0;i<1000 && controller.progress().phase!=Phase::failed;++i){const auto o=clock.next();controller.advance(o,clock.now);}
      const auto result=controller.progress();
      require(result.phase==Phase::failed && result.owned_payload_bytes==0 && session.poisoned(),"failed automatic publication stayed active");
      const auto calls=data_io->calls,reads=data_io->read_calls;
      for(unsigned i=0;i<5;++i){const auto next=clock.next();controller.advance(next,clock.now);}
      require(data_io->calls==calls && data_io->read_calls==reads,"failed source retried I/O");}
    data_io->live=data_io->stable;
    auto snapshot=NativeFilesystemSnapshot::open_image(data_io,{NativeImageKind::paged,kStoreMaxPhysicalBytes});
    for(unsigned i=0;i<4;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"automatic fault lost file data");
    snapshot.verify_all();++faults;
  }
  std::cout<<"automatic_group_faults unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
void reopen_indexed(Fixture& fixture,const std::shared_ptr<Ram>& advice,std::uint64_t sequence) {
  fixture.session.reset();
  fixture.session.emplace(NativeFileSession::resume_closed_indexed_volume(fixture.head,id(90),1,
      [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?fixture.image:nullptr;},
      {StoreCompressionMode::raw,1,false}));
  require(fixture.session->resume_budgeted_optimization_area(advice,sequence).phase==NativeOptimizationPhase::ready,
      "indexed scan advice could not be rebound after closing");
}
void finish_pending(Fixture& fixture,NativeGroupMaintenance& controller) {
  for(unsigned i=0;i<6000&&controller.pending();++i)
    require(fixture.step(controller).phase!=Phase::failed,"indexed scan stopped");
  require(!controller.pending(),"indexed scan did not finish");
}
void persistent_indexed_scan() {
  constexpr unsigned count=static_cast<unsigned>(kNativeMaxDirtyExtents)+9;
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "reserved indexed scan advice unavailable");
  for(unsigned i=0;i<count;++i)put(*f.session,"f"+std::to_string(i),family(i));
  // Sparse all-zero data supplies genuine accepted input without changing the
  // similarity window. Each operation is below the native transaction limit.
  put(*f.session,"sparse-a",Bytes(kOptimizationGroupScanInputBytes/2));
  put(*f.session,"sparse-b",Bytes(kOptimizationGroupScanInputBytes/2));
  {
    NativeGroupMaintenance controller(*f.session);
    for(unsigned i=0;i<3500&&controller.progress().checkpoints_published==0;++i)
      require(f.step(controller).phase!=Phase::failed,"initial indexed scan failed");
    require(controller.progress().checkpoints_published==1 && controller.pending(),
        "indexed scan did not durably checkpoint its first window");
  }
  const auto partial=read_optimization_state(*advice,id(90));
  require(partial.code==OptimizationReadCode::available && !partial.degraded && partial.state.group_scan &&
      !partial.state.group_scan->complete && !partial.state.group_scan->revisit &&
      partial.state.settled_input_bytes==kOptimizationGroupScanInputBytes &&
      std::ranges::any_of(partial.state.group_scan->representatives,[](const auto& item){return item.exact_base;}),
      "first scan checkpoint lacks a checked reusable representation");
  auto old_checkpoint=image(advice->stable,4096);
  reopen_indexed(f,advice,partial.sequence);
  std::uint64_t resumed_samples=0;
  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    const auto state=controller.progress();resumed_samples=state.sampled_extents;
    require(state.checkpoints_restored==1 && state.checkpoints_published==1 &&
        state.published_groups==1 && resumed_samples<count-kNativeMaxDirtyExtents+3,
        "reopened discovery repeated the old window or lost its shared base");
    const auto actual=f.session->snapshot().shared_base_info();
    require(actual.bases==1&&actual.members==count,"reopened discovery did not extend the existing group");
  }
  f.verify(count);
  const auto completed=read_optimization_state(*advice,id(90));
  require(completed.state.group_scan && completed.state.group_scan->complete &&
      completed.state.settled_input_bytes==2*kOptimizationGroupScanInputBytes &&
      completed.sequence==partial.sequence+1,"completion or durable write charge is missing");
  reopen_indexed(f,advice,completed.sequence);
  {
    NativeGroupMaintenance controller(*f.session);
    const auto writes=advice->writes;finish_pending(f,controller);
    require(controller.progress().checkpoints_restored==1 && controller.progress().sampled_extents==0 &&
        advice->writes==writes,"completed unchanged scan repeated sampling or wrote state");
  }
  const auto replacement=data(kNativeExtentBytes,809);
  auto handle=f.session->open("f0",{true,true,false});f.session->write(handle,0,replacement);
  f.session->cleanup(handle,false);f.session->close(handle);
  reopen_indexed(f,old_checkpoint,partial.sequence);
  {
    NativeGroupMaintenance controller(*f.session);finish_pending(f,controller);
    require(controller.progress().checkpoints_restored==1 && controller.progress().sampled_extents>=count,
        "changed prefix was skipped after resuming an older cursor");
    const auto snapshot=f.session->snapshot();
    require(snapshot.read("f0",0,kNativeExtentBytes)==replacement,"older checkpoint changed replacement bytes");
    for(unsigned i=1;i<count;++i)
      require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==family(i),"older checkpoint changed a peer");
    snapshot.verify_all();
  }
  std::cout<<"persistent_indexed_scan total="<<count<<" resumed_samples="<<resumed_samples
      <<" checkpoint_store_bytes="<<advice->extent()<<" write_cost="<<kOptimizationAreaBytes<<'\n';
}
void retain_distant_indexed_candidate(bool projected=false) {
  constexpr auto prefix=2*kNativeMaxDirtyExtents;
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "long-range discovery advice unavailable");
  const auto initial_advice_bytes=advice->write_bytes;
  put(*f.session,"original",family(0));
  for(std::size_t i=1;i<prefix;++i)
    put(*f.session,"unrelated-"+std::to_string(i),data(kNativeExtentBytes,static_cast<unsigned>(3000+i)));
  auto distant=projected?family(0):family(1);
  if(projected) {
    const auto variation=data(kNativeExtentBytes,9121);
    for(std::size_t i=0;i<distant.size();++i)distant[i]^=variation[i]&std::byte{0x30};
  }
  put(*f.session,"distant",distant);
  put(*f.session,"sparse-funding",Bytes(kOptimizationGroupScanInputBytes/2));
  const auto original_inode=f.session->snapshot().stat("original").inode;
  {
    NativeGroupMaintenance controller(*f.session);
    for(unsigned expected=1;expected<=2;++expected) {
      for(unsigned tick=0;tick<3500&&controller.progress().checkpoints_published<expected;++tick)
        require(f.step(controller).phase!=Phase::failed,"long-range discovery failed");
      require(controller.progress().checkpoints_published==expected &&
          controller.progress().attempted_groups==0 && controller.pending(),
          "unrelated windows did not checkpoint before reaching the distant member");
      require(advice->stable.at(7)==std::byte{'5'},"checkpoint did not retain projected samples");
    }
  }
  const auto saved=read_optimization_state(*advice,id(90));
  require(saved.code==OptimizationReadCode::available && !saved.degraded && saved.state.group_scan &&
      !saved.state.group_scan->complete && saved.state.group_scan->representatives.size()==prefix &&
      saved.state.group_scan->representatives.front().inode==original_inode &&
      saved.state.group_scan->xor_sampling && saved.state.group_scan->projections.size()==prefix &&
      saved.state.group_scan->projections.front().representative==0,
      "extended checkpoint lost the early candidate");
  require(advice->extent()==kOptimizationAreaBytes &&
      advice->write_bytes-initial_advice_bytes==2*kOptimizationAreaBytes,
      "retaining more representatives increased the checkpoint footprint or write traffic");
  // A foreground edit after the checkpoint changes one earlier extent.
  const auto replacement=data(kNativeExtentBytes,9001);
  const auto handle=f.session->open("unrelated-1",{true,true,false});
  f.session->write(handle,0,replacement);f.session->cleanup(handle,false);f.session->close(handle);
  reopen_indexed(f,advice,saved.sequence);
  NativeGroupMaintenance controller(*f.session);
  for(unsigned tick=0;tick<2500 && controller.progress().published_groups==0;++tick)
    require(f.step(controller).phase!=Phase::failed,"reopened distant selection failed");
  const auto selected=controller.progress();
  require(selected.checkpoints_restored==1 && selected.published_groups==1 &&
      selected.minimum_compacted_bytes_saved>0 && selected.sampled_extents==1,
      "reopened scanner missed the distant match before revisiting the edited prefix");
  finish_pending(f,controller);
  const auto progress=controller.progress();
  require(progress.reused_features>=prefix-1 && progress.sampled_extents<prefix,
      "unrelated edit discarded the saved scan work");
  const auto snapshot=f.session->snapshot();
  require(snapshot.shared_base_info().members==2 && snapshot.shared_base_info().bases==1 &&
      snapshot.read("original",0,kNativeExtentBytes)==family(0) &&
      snapshot.read("distant",0,kNativeExtentBytes)==distant &&
      snapshot.read("unrelated-1",0,kNativeExtentBytes)==replacement,
      "distant group reopened with different contents");
  snapshot.verify_all();
  std::cout<<"indexed_distant_match projected="<<projected<<" prefix_representatives="<<prefix
      <<" samples_before_match="<<selected.sampled_extents
      <<" total_resumed_samples="<<progress.sampled_extents<<" reused_features="<<progress.reused_features
      <<" minimum_compacted_bytes_saved="<<progress.minimum_compacted_bytes_saved
      <<" checkpoint_bytes="<<advice->extent()<<'\n';
}
class CatalogReadGate final:public StoreByteIo {
 public:
  explicit CatalogReadGate(std::shared_ptr<Ram> backing):backing_(std::move(backing)) {}
  bool refuse=false;
  std::uint64_t extent() const override{return backing_->extent();}
  StoreIoContract contract() const noexcept override{return backing_->contract();}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if(refuse)throw std::runtime_error("injected coherence catalog read failure");
    backing_->read_exact(at,out);
  }
  void admit_write(std::uint64_t) override{throw std::runtime_error("read-only catalog fixture");}
  void write_exact(std::uint64_t,std::span<const std::byte>) override{throw std::runtime_error("read-only catalog fixture");}
  void flush() override{throw std::runtime_error("read-only catalog fixture");}
 private:
  std::shared_ptr<Ram> backing_;
};
enum class CatalogCase {available,unavailable,stale,aligned_only};
void indexed_catalog_discovery(CatalogCase scenario,bool projected=false) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "coherence discovery advice binding");
  auto backing=ram(4096);
  auto catalog=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  f.session->bind_indexed_coherence_writer(std::move(catalog));
  const auto original=family(0);
  put(*f.session,"original",original);
  const bool long_range=projected && (scenario==CatalogCase::available || scenario==CatalogCase::aligned_only);
  const auto noise_count=long_range?kOptimizationGroupRepresentatives+1:4;
  if(long_range) {
    f.session->create_entry("noise");
    const auto noise=f.session->open("noise",{true,true,false});
    for(std::size_t first=0;first<noise_count;first+=kNativeMaxDirtyExtents) {
      const auto count=(std::min)(noise_count-first,kNativeMaxDirtyExtents);
      Bytes batch(count*kNativeExtentBytes);
      for(std::size_t i=0;i<count;++i) {
        const auto bytes=data(kNativeExtentBytes,4000+static_cast<unsigned>(first+i));
        std::ranges::copy(bytes,batch.begin()+i*kNativeExtentBytes);
      }
      f.session->write(noise,first*kNativeExtentBytes,batch);
    }
    f.session->cleanup(noise,false);
    f.session->close(noise);
    // Fund the optional postings and retain a settled checkpoint after the
    // last window, even when the catalog also carries numeric projections.
    for(unsigned i=0;i<6;++i)
      put(*f.session,"funding-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  }else {
    for(unsigned i=0;i<noise_count;++i)
      put(*f.session,"noise-"+std::to_string(i),data(kNativeExtentBytes,590+i));
    put(*f.session,"funding",Bytes(kOptimizationGroupScanInputBytes-5*kNativeExtentBytes));
  }
  put(*f.session,"catalog-input",Bytes(kOptimizationGroupScanInputBytes/2));
  OptimizationGroupFeature original_feature;
  OptimizationXorSignatures original_signatures{};
  {
    NativeGroupMaintenance initial(*f.session);
    for(unsigned tick=0;tick<200 && !initial.progress().checkpoints_published;++tick)
      require(f.step(initial).phase!=Phase::failed,"catalog scanning failed");
    require(!initial.progress().coherence_lookups,"empty catalog consumed a lookup opportunity");
    const auto saved=read_optimization_state(*advice,id(90));
    const auto before_generations=f.session->snapshot();
    require(saved.state.group_scan && !saved.state.group_scan->representatives.empty() &&
        saved.state.group_scan->representatives.front().inode==before_generations.stat("original").inode &&
        !saved.state.group_scan->projections.empty() && saved.state.group_scan->projections.front().representative==0,
        "archive lacked an actual native extent binding");
    original_feature=saved.state.group_scan->representatives.front();
    original_signatures=saved.state.group_scan->projections.front().signatures;
    finish_pending(f,initial);
    require(initial.progress().checkpoints_published && initial.progress().published_groups==0,
        "catalog feature source lacked a funded sample");
    if(long_range) {
      const auto latest=f.session->optimization_snapshot().state.group_scan;
      require(latest && std::ranges::find(latest->representatives,original_feature.object,
          &OptimizationGroupFeature::object)==latest->representatives.end(),
          "long-range test still retained the original in checkpoint history");
    }
  }
  constexpr auto masked_band=static_cast<std::uint32_t>(kOptimizationGroupBands)+2;
  const auto archive=IndexedCoherenceSnapshot::open(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  auto expected=original_feature;expected.bands={};
  require(archive.has_projected_keys() && archive.find(0,original_feature.bands[0])==original_feature &&
      archive.find(masked_band,original_signatures[2])==expected,
      "ordinary funded sampling did not populate both paged key families");
  if(scenario==CatalogCase::aligned_only) {
    backing=ram(4096);
    auto control=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
    const IndexedCoherencePosting legacy{0,original_feature};
    control.publish(std::span(&legacy,1),UINT64_MAX);
    require(!control.snapshot().has_projected_keys(),"aligned-only control acquired projections");
  }
  auto distant=original;
  if(projected) {
    const auto variation=data(distant.size(),9121);
    for(std::size_t i=0;i<distant.size();++i)distant[i]^=variation[i]&std::byte{0x30};
  }else distant[distant.size()-91]^=std::byte{0x31};
  put(*f.session,"distant",distant);
  const auto edited=scenario==CatalogCase::stale;
  const auto current_original=edited?data(kNativeExtentBytes,811):original;
  if(edited) {
    const auto file=f.session->open("original",{true,true,false});
    f.session->write(file,0,current_original);f.session->cleanup(file,false);f.session->close(file);
  }
  f.session.reset();
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,
      [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;},
      {StoreCompressionMode::raw,1,false}));
  auto gate=std::make_shared<CatalogReadGate>(backing);
  const auto catalog_reads_before=backing->read_calls;
  rejects([&]{f.session->bind_indexed_coherence(f.image,{kStoreMaxPhysicalBytes});},
      "native image accepted as optional catalog backing");
  NativeGroupMaintenance controller(*f.session);
  if(edited)finish_pending(f,controller);
  f.session->bind_indexed_coherence(gate,{kStoreMaxPhysicalBytes});
  gate->refuse=scenario==CatalogCase::unavailable;
  require(controller.pending(),"new catalog did not wake settled maintenance");
  const auto lookup_quanta=(noise_count+2)*(2+kOptimizationXorProjections+kCoherenceModularWidths);
  for(std::size_t tick=0;tick<3000+lookup_quanta && controller.pending();++tick) {
    auto observation=f.clock.next();
    // Reserve just enough modeled headroom for two ordinary members and two
    // cached representatives, forcing the original out of the live window.
    observation.physical_available=observation.physical_total/16+64*(33ULL*kNativeExtentBytes);
    const auto lookups=controller.progress().coherence_lookups;
    const auto progress=controller.advance(observation,f.clock.now);
    require(progress.phase!=Phase::failed && progress.coherence_lookups<=lookups+1,
        "optional catalog exceeded one lookup or affected data maintenance health");
  }
  const auto progress=controller.progress();
  require(!controller.pending() && !f.session->poisoned(),"indexed catalog maintenance did not settle");
  if(scenario==CatalogCase::unavailable)
    require(progress.coherence_lookups==1 && progress.coherence_failures==1 && progress.published_groups==0,
        "failed catalog lookup retried or manufactured a group");
  else if(scenario==CatalogCase::aligned_only)
    require(!progress.coherence_matches && !progress.coherence_failures && !progress.attempted_groups &&
        !progress.published_groups,"aligned-only catalog found an extent with no matching aligned key");
  else
    require(progress.coherence_matches>=1 && !progress.coherence_failures &&
        progress.published_groups==(edited?0U:1U) && progress.attempted_groups==(edited?0U:1U),
        "paged candidate was missed or an edited binding entered preparation");
  const auto view=f.session->snapshot();
  require(view.read("original",0,kNativeExtentBytes)==current_original &&
      view.read("distant",0,kNativeExtentBytes)==distant &&
      view.shared_base_info().members==(scenario==CatalogCase::available?2:0),"catalog integration changed file data");
  view.verify_all();
  if(long_range)std::cout<<"projected_catalog_discovery aligned_only="<<(scenario==CatalogCase::aligned_only)
      <<" intervening_extents="<<noise_count<<" lookups="<<progress.coherence_lookups
      <<" groups="<<progress.published_groups<<" minimum_compacted_bytes_saved="<<progress.minimum_compacted_bytes_saved
      <<" catalog_bytes="<<progress.coherence_catalog_bytes
      <<" catalog_read_calls="<<backing->read_calls-catalog_reads_before<<'\n';
}
void indexed_catalog_displaced_candidate() {
  constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  Fixture seed(4096,0,true);
  std::vector<std::pair<std::string,Bytes>> originals;
  originals.emplace_back("original",family(0));
  for(unsigned i=0;i<10;++i)
    originals.emplace_back("noise-"+std::to_string(i),data(kNativeExtentBytes,44000+i));
  auto distant=originals.front().second;
  distant[kNativeExtentBytes/2+17]^=std::byte{0x5c};
  originals.emplace_back("distant",std::move(distant));
  auto decoy=originals.front().second;
  decoy[kNativeExtentBytes/3]^=std::byte{0x71};
  originals.emplace_back("edited",std::move(decoy));
  for(const auto& [name,bytes]:originals)put(*seed.session,name,bytes);
  const auto image_pages=PagedObjectStoreSnapshot::open(seed.image,limits);
  const detail::IndexedVolumeView source(image_pages,image_pages.read(1));
  const auto posting=[&](const auto& file) {
    const auto inode=source.resolve(file.first).inode;
    const auto sample=source.sample_content({inode,0},detail::IndexedContentLookup::exact,{});
    require(sample && sample->payload==file.second,"displaced candidate fixture binding");
    const auto digest=detail::record_digest(std::span(file.second).first(kNativeExtentBytes/8));
    OptimizationGroupFeature feature{inode,0,sample->token.object,sample->token.revision,detail::record_digest(file.second)};
    for(unsigned i=0;i<8;++i)feature.bands[0]|=std::uint64_t(std::to_integer<unsigned>(digest[i]))<<(8*i);
    return IndexedCoherencePosting{0,feature};
  };
  const auto old=posting(originals.front()),latest=posting(originals.back());
  require(old.representative.bands[0]==latest.representative.bands[0] &&
      old.representative.revision<latest.representative.revision,"expected a newer nomination for the same key");
  auto& edited=originals.back();
  edited.second=data(kNativeExtentBytes,72727);
  const auto handle=seed.session->open(edited.first,{true,true,false});
  seed.session->write(handle,0,edited.second);seed.session->cleanup(handle,false);seed.session->close(handle);
  struct Outcome {std::uint64_t groups,image_bytes,catalog_bytes,selector_bytes,lookups,writes;};
  const auto run=[&](bool seeded_history) {
    auto catalog=ram(4096);
    std::uint64_t writes=0;
    {
      auto writer=IndexedCoherenceWriter::create(catalog,id(90),id(1),limits);
      if(seeded_history)writes+=writer.publish(std::span(&old,1),UINT64_MAX).write_bytes_upper_bound;
      writes+=writer.publish(std::span(&latest,1),UINT64_MAX).write_bytes_upper_bound;
      require(writer.snapshot().find(0,latest.representative.bands[0])==latest.representative,
          "newest representative did not survive catalog publication");
    }
    Fixture f(image(seed.head->stable,4096),image(seed.image->stable,4096),raw);
    f.session->bind_indexed_coherence(catalog,limits);
    NativeGroupMaintenance controller(*f.session);
    for(unsigned tick=0;tick<2000 && controller.pending();++tick) {
      auto observed=f.clock.next();
      observed.physical_available=observed.physical_total/16+64*(33ULL*kNativeExtentBytes);
      const auto prior=controller.progress().coherence_lookups;
      const auto progress=controller.advance(observed,f.clock.now);
      require(progress.phase!=Phase::failed && progress.coherence_lookups<=prior+1,
          "displaced candidate exceeded one lookup per admitted step");
    }
    const auto progress=controller.progress();
    require(!controller.pending() && !progress.coherence_failures,"displaced candidate scan did not settle");
    controller.cancel();f.session.reset();
    const auto closed=NativeVolumeHeadView::open(f.head,id(90),
        [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;});
    const auto compacted=ram(4096);
    (void)NativeFilesystemWriter::compact(closed.filesystem(),compacted,id(2),raw);
    const auto reopened=NativeFilesystemSnapshot::open_image(compacted,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    reopened.verify_all();
    for(const auto& [name,bytes]:originals)
      require(reopened.read(name,0,kNativeExtentBytes)==bytes,"displaced candidate changed acknowledged bytes");
    const Outcome outcome{progress.published_groups,compacted->extent(),catalog->extent(),f.head->extent(),
        progress.coherence_lookups,writes};
    std::cout<<"coherence_displacement seeded_history="<<seeded_history<<" groups="<<outcome.groups
        <<" image_bytes="<<outcome.image_bytes<<" catalog_bytes="<<outcome.catalog_bytes
        <<" selector_bytes="<<outcome.selector_bytes<<" lookups="<<outcome.lookups
        <<" catalog_write_bound="<<outcome.writes<<" files="<<originals.size()<<" exact_cpu_readback=1\n";
    return outcome;
  };
  const auto latest_only=run(false),with_previous=run(true);
  require(!latest_only.groups && with_previous.groups==1 &&
      with_previous.image_bytes+with_previous.catalog_bytes+with_previous.selector_bytes<
          latest_only.image_bytes+latest_only.catalog_bytes+latest_only.selector_bytes,
      "the displaced live match did not reduce the occupied volume and catalog");
}
void indexed_catalog_shift_candidate(bool secondary=false,Bytes public_sample={}) {
  constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  Fixture seed(4096,0,true);
  auto advice=ram(4096),catalog=ram(4096);
  require(seed.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "shifted discovery requires funded advisory storage");
  seed.session->bind_indexed_coherence_writer(IndexedCoherenceWriter::create(catalog,id(90),id(1),limits));
  std::vector<std::pair<std::string,Bytes>> files;
  const bool public_input=!public_sample.empty();
  if(!public_input)public_sample=data(kNativeExtentBytes,secondary?715:714);
  require(public_sample.size()==kNativeExtentBytes,"shift fixture requires one complete extent");
  files.emplace_back("earlier",std::move(public_sample));
  files.emplace_back("other",data(kNativeExtentBytes,9137));
  for(unsigned i=0;i<2;++i)files.emplace_back("input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  for(const auto& [name,bytes]:files)put(*seed.session,name,bytes);
  {
    NativeGroupMaintenance scanner(*seed.session);
    finish_pending(seed,scanner);
    require(scanner.progress().checkpoints_published==1 && !scanner.progress().published_groups,
        "initial shifted scan did not publish its funded samples");
  }
  const auto retained_advice=seed.session->optimization_snapshot().state.group_scan;
  require(retained_advice && retained_advice->representatives.size()==2 && retained_advice->projections.size()==2,
      "shifted scan did not retain its earlier sample");
  const auto& earlier=retained_advice->representatives.front();
  const auto shifted_band=detail::coherence_shift_band(earlier.bands);
  require(shifted_band && shifted_band!=detail::coherence_primary_band(earlier.bands),
      "shifted fixture needs a separate rolling key");
  const auto persisted=IndexedCoherenceSnapshot::open(catalog,id(90),id(1),limits);
  require(persisted.find(*shifted_band,earlier.bands[*shifted_band])==earlier,
      "funded scan omitted its shifted posting");

  // Build the prior key families from the same exact checkpoint samples.
  auto prior_catalog=ram(4096);
  {
    std::vector<IndexedCoherencePosting> previous;
    for(std::uint32_t row=0;row<retained_advice->representatives.size();++row) {
      const auto& feature=retained_advice->representatives[row];
      const auto band=detail::coherence_primary_band(feature.bands);
      require(band.has_value() && retained_advice->projections[row].representative==row &&
          feature.content==detail::record_digest(files[row].second),
          "prior catalog control is not bound to the same sample");
      previous.push_back({*band,feature});
      const auto& masked=retained_advice->projections[row].signatures;
      const auto numeric=detail::native_group_delta_signatures(files[row].second);
      for(std::uint32_t i=0;i<masked.size();++i)if(masked[i])previous.push_back({kCoherenceMaskedBand+i,feature,masked[i]});
      for(std::uint32_t i=0;i<numeric.size();++i)if(numeric[i])previous.push_back({kCoherenceModularBand+i,feature,numeric[i]});
    }
    auto writer=IndexedCoherenceWriter::create(prior_catalog,id(90),id(1),limits);
    writer.publish(previous,UINT64_MAX);
    require(!writer.snapshot().find(*shifted_band,earlier.bands[*shifted_band]),"prior catalog gained a rolling key");
  }
  for(unsigned i=0;i<12;++i) {
    files.emplace_back("intervening-"+std::to_string(i),data(kNativeExtentBytes,44000+i));
    put(*seed.session,files.back().first,files.back().second);
  }
  auto displaced=files.front().second;
  constexpr std::size_t shift=67;
  std::copy_n(files.front().second.begin(),displaced.size()-shift,displaced.begin()+shift);
  std::fill_n(displaced.begin(),shift,std::byte{0xf2});
  if(secondary) {
    const auto slot=*shifted_band-(kOptimizationGroupBands-detail::kNativeGroupShiftFeatures);
    const auto stored_key=earlier.bands[*shifted_band];
    constexpr auto edit_bytes=2048U;
    const auto matches_secondary=[&] {
      const auto keys=detail::native_group_shift_features(displaced);
      return keys[slot]==stored_key && std::ranges::any_of(keys,[&](auto key){return key && key<stored_key;});
    };
    if(public_input) {
      const auto shifted_bytes=displaced;
      for(unsigned salt=0;salt<256;++salt) {
        const auto offset=(salt%(kNativeExtentBytes/edit_bytes))*edit_bytes;
        displaced=shifted_bytes;
        std::ranges::copy(data(edit_bytes,32150+salt),displaced.begin()+offset);
        if(matches_secondary())break;
      }
    }else {
      std::ranges::copy(data(edit_bytes,0x85ebca6bU),displaced.begin());
    }
    require(matches_secondary(),"controlled edit did not keep the older key in a secondary band");
  }
  files.emplace_back("later",std::move(displaced));
  put(*seed.session,files.back().first,files.back().second);
  const auto run=[&](const std::shared_ptr<Ram>& backing,bool shifted,bool stale) {
    Fixture trial(image(seed.head->stable,4096),image(seed.image->stable,4096),raw);
    const auto before=trial.session->snapshot();
    const auto replacement=data(kNativeExtentBytes,72831);
    if(stale) {
      const auto handle=trial.session->open("earlier",{true,true,false});
      trial.session->write(handle,0,replacement);trial.session->cleanup(handle,false);trial.session->close(handle);
    }
    trial.session->bind_indexed_coherence(backing,limits);
    NativeGroupMaintenance scanner(*trial.session);
    for(unsigned tick=0;tick<2000 && scanner.pending();++tick) {
      auto observed=trial.clock.next();
      observed.physical_available=observed.physical_total/16+64*(33ULL*kNativeExtentBytes);
      const auto lookups=scanner.progress().coherence_lookups;
      const auto progress=scanner.advance(observed,trial.clock.now);
      require(progress.phase!=Phase::failed && progress.coherence_lookups<=lookups+1,
          "shifted discovery exceeded its admitted lookup quantum");
    }
    const auto progress=scanner.progress();
    const auto expected_groups=shifted && !stale?1U:0U;
    require(!scanner.pending() && !progress.coherence_failures &&
        progress.published_groups==expected_groups && progress.attempted_groups==expected_groups,
        "shifted discovery missed a live match or opened a stale one");
    scanner.cancel();trial.session.reset();
    const auto current=NativeVolumeHeadView::open(trial.head,id(90),[&](const auto& key)->std::shared_ptr<StoreByteIo> {
      return key==id(1)?trial.image:nullptr;
    });
    const auto compacted=ram(4096);
    (void)NativeFilesystemWriter::compact(current.filesystem(),compacted,id(2),raw);
    const auto reopened=NativeFilesystemSnapshot::open_image(compacted,{NativeImageKind::indexed_paged,limits.maximum_physical_bytes});
    reopened.verify_all();before.verify_all();
    for(const auto& [name,bytes]:files) {
      const auto& expected=stale && name=="earlier"?replacement:bytes;
      for(std::size_t at=0;at<bytes.size();at+=kNativeExtentBytes) {
        const auto length=(std::min)(std::size_t{kNativeExtentBytes},bytes.size()-at);
        const auto prior=before.read(name,at,length);
        const auto recovered=reopened.read(name,at,length);
        require(std::ranges::equal(prior,std::span(bytes).subspan(at,length)) &&
            std::ranges::equal(recovered,std::span(expected).subspan(at,length)),"shifted discovery changed file bytes");
      }
    }
    const auto total=compacted->extent()+trial.head->extent()+advice->extent()+backing->extent();
    std::cout<<"coherence_shift persisted="<<shifted<<" stale="<<stale<<" secondary="<<secondary
        <<" public_input="<<public_input<<" groups="<<progress.published_groups
        <<" lookups="<<progress.coherence_lookups<<" complete_bytes="<<total
        <<" image_bytes="<<compacted->extent()<<" catalog_bytes="<<backing->extent()
        <<" intervening_extents=12 exact_cpu_readback=1\n";
    return total;
  };
  const auto unshifted=run(prior_catalog,false,false);
  const auto settled=run(catalog,true,false);
  require(settled<unshifted,"a live rolling match did not reduce the protected footprint");
  (void)run(catalog,true,true);
}
void indexed_catalog_metadata_space_refusal() {
  OptimizationGroupFeature feature{2,0,detail::kNativeIndexedDataObject+1,1,
      detail::record_digest(data(32,48317))};
  feature.bands[0]=0xf29b267913c004eaULL;
  const IndexedCoherencePosting posting{0,feature};
  const auto control=ram(4096);
  auto original=IndexedCoherenceWriter::create(control,id(90),id(1),{kStoreMaxPhysicalBytes});
  const auto before=control->extent();
  const auto required=original.publish(std::span(&posting,1),UINT64_MAX).storage.physical_end;
  const PagedStoreLimits limits{before+4096}; // Space for the payload before allocating its paired pages.
  const auto target=ram(4096);
  auto constrained=IndexedCoherenceWriter::create(target,id(90),id(1),limits);
  const auto writes=target->calls;
  const auto state=target->stable;
  bool refused=false;
  try {(void)constrained.publish(std::span(&posting,1),UINT64_MAX);}
  catch(const PagedStoreCapacityError& error) {
    refused=true;
    std::cout<<"coherence_metadata_capacity required="<<required<<" limit="<<limits.maximum_physical_bytes
        <<" physical="<<error.physical_space_exhausted()<<" shortfall="<<error.append_shortfall_bytes()<<'\n';
    require(error.physical_space_exhausted() && error.append_shortfall_bytes()==required-limits.maximum_physical_bytes,
        "paged metadata capacity did not report the full append shortfall");
  }
  require(refused && target->calls==writes && target->stable==state && !constrained.poisoned(),
      "metadata space refusal modified or fenced the old catalog");
}
void indexed_catalog_alternate_publication() {
  constexpr PagedStoreLimits limits{1024*1024};
  constexpr std::uint64_t fingerprint=0x910fc628acc971a1;
  std::vector<IndexedCoherencePosting> postings;
  for(unsigned i=1;i<=96;++i) {
    OptimizationGroupFeature feature{i+1,0,detail::kNativeIndexedDataObject+i,i,
        detail::record_digest(data(64,34564+i))};
    feature.bands[0]=fingerprint;
    postings.push_back({0,feature});
  }
  const auto make_writer=[&](const auto& backing) {
    return IndexedCoherenceWriter::create(backing,id(90),id(1),limits);
  };
  auto control_backing=ram(4096);
  auto control=make_writer(control_backing);
  const auto latest=std::span(postings).last(1);
  const auto primary_budget=control.publish(latest,UINT64_MAX).write_bytes_upper_bound;
  const auto primary=control.snapshot();
  require(!primary.has_alternates() && primary.find(0,fingerprint)==latest.front().representative &&
      !primary.next_alternate(0,fingerprint),"legacy single-row catalog did not reopen");

  auto bounded=ram(4096);
  auto limited=make_writer(bounded);
  const auto stable=bounded->stable;
  const auto before=bounded->calls;
  rejects([&]{limited.publish(postings,0);},"unfunded alternatives or primary rows published");
  require(!limited.poisoned() && bounded->calls==before && bounded->stable==stable,
      "unfunded catalog plan wrote or poisoned the owner");
  const auto restricted=limited.publish(postings,primary_budget);
  const auto limited_view=limited.snapshot();
  require(restricted.write_bytes_upper_bound==primary_budget && !limited_view.has_alternates() &&
      limited_view.find(0,fingerprint)==latest.front().representative,
      "primary-only credit did not preserve the newest row");

  auto expanded=ram(4096);
  auto writer=make_writer(expanded);
  const auto full=writer.publish(postings,UINT64_MAX);
  const auto written=expanded->calls;
  require(full.write_bytes_upper_bound>primary_budget &&
      !writer.publish(postings,0).write_bytes_upper_bound && expanded->calls==written,
      "multi-row replay spent catalog credit");
  const auto view=IndexedCoherenceSnapshot::open(image(expanded->stable,4096),id(90),id(1),limits);
  require(view.has_alternates() && !view.has_projected_keys() && !view.has_modular_keys() &&
      view.find(0,fingerprint)==latest.front().representative && view.info().height>0,
      "paged alternate family changed the primary band profile");
  std::uint64_t cursor=0;
  std::set<std::uint64_t> locations;
  while(const auto next=view.next_alternate(0,fingerprint,cursor)) {
    require(next->cursor>cursor && next->representative,"paged alternate did not advance to a bound row");
    const auto& feature=*next->representative;
    const auto match=std::ranges::find(postings,feature,&IndexedCoherencePosting::representative);
    require(match!=postings.end() && match!=postings.end()-1 && locations.insert(feature.inode).second,
        "catalog returned a foreign, duplicate or primary location");
    cursor=next->cursor;
  }
  require(locations.size()>64 && locations.size()+2==view.info().objects &&
      !view.next_alternate(0,fingerprint+1),"sparse alternate lookup missed its paged range");
  rejects([&]{(void)view.next_alternate(0,fingerprint+1,cursor);},"foreign alternate cursor accepted");

  const auto first=std::span(postings).first(1);
  std::vector<IndexedCoherencePosting> initial{first.front()},changed{latest.front()},
      prior_bindings{first.front()};
  for(unsigned i=1;i<=144;++i) {
    auto old=postings[i%postings.size()];
    old.representative.inode=1024+i;
    old.representative.object=detail::kNativeIndexedDataObject+1024+i;
    old.representative.revision=1024+i;
    old.representative.bands[0]=fingerprint+2*i;
    initial.push_back(old);
    if(i%4==0) {
      prior_bindings.push_back(old);
      old.representative.inode+=1024;
      old.representative.object+=1024;
      old.representative.revision+=1024;
      changed.push_back(old);
    }
  }
  for(const auto unit:{4096U,65536U}) {
    auto previous_backing=ram(unit);
    auto previous=make_writer(previous_backing);
    const auto source=previous.publish(initial,UINT64_MAX).storage;
    require(source.height>=2,"catalog interruption fixture did not span multiple branch levels");
    const auto durable=previous_backing->stable;
    const auto publisher=[&](const auto& target) {
      return IndexedCoherenceWriter::resume(target,id(90),id(1),source.generation,limits);
    };
    auto exact=image(durable,unit);
    auto promotion=publisher(exact);
    const auto promotion_start=exact->calls;
    promotion.publish(changed,UINT64_MAX);
    const auto promotion_calls=exact->calls-promotion_start;
    require(promotion_calls>=3,"catalog promotion fixture missed publication barriers");
    std::size_t old_survives=0,new_survives=0;
    for(const unsigned failure:{0U,1U,2U})for(std::size_t cut=0;cut<promotion_calls;++cut) {
      auto crash=image(durable,unit);
      auto trial=publisher(crash);
      crash->fail_at=crash->calls+cut;crash->failure=failure;
      rejects([&]{trial.publish(changed,UINT64_MAX);},"catalog promotion ignored the publication cut");
      const auto recovered=IndexedCoherenceSnapshot::open(image(crash->stable,unit),id(90),id(1),limits);
      const bool updated=recovered.has_alternates();
      const auto& expected=updated?changed:prior_bindings;
      require(recovered.info().metadata_page_bytes==1024 && recovered.info().height>=2 &&
          recovered.find(0,initial[1].representative.bands[0])==initial[1].representative,
          "recovered catalog lost unchanged geometry or bindings");
      for(std::size_t i=0;i<expected.size();++i) {
        const auto key=expected[i].representative.bands[0];
        const auto alternate=recovered.next_alternate(0,key);
        require(recovered.find(0,key)==expected[i].representative &&
            (updated?(alternate && alternate->representative==prior_bindings[i].representative):!alternate),
            "catalog root exposed a partial batch across changed pages");
      }
      if(updated)++new_survives;
      else ++old_survives;
    }
    require(old_survives && new_survives,"catalog fault matrix missed either complete checkpoint");
    std::cout<<"coherence_metadata_cuts failure_unit_bytes="<<unit<<" old_checkpoints="<<old_survives
        <<" new_checkpoints="<<new_survives<<'\n';
  }
  std::cout<<"coherence_alternate_publication offered="<<postings.size()<<" retained_alternates="<<locations.size()
      <<" primary_write_bound="<<primary_budget<<" full_write_bound="<<full.write_bytes_upper_bound<<'\n';
}
IndexedCoherenceWriter legacy_coherence_catalog(const std::shared_ptr<Ram>& backing,PagedStoreLimits limits) {
  Bytes typed_root(32);
  const std::string_view signature="FLTZCOR1";
  const auto marker=std::as_bytes(std::span(signature));
  std::ranges::copy(marker,typed_root.begin());
  const auto image_id=id(1);
  std::ranges::copy(image_id,typed_root.begin()+8);
  typed_root[24]=std::byte{1};
  const auto generation=[&] {
    auto writer=PagedObjectStoreWriter::create(backing,id(90),limits,{StoreCompressionMode::raw,1,false});
    const StoreMutation root{1,false,std::move(typed_root)};
    return writer.commit(std::span(&root,1)).generation;
  }();
  return IndexedCoherenceWriter::resume(backing,id(90),image_id,generation,limits);
}
void indexed_catalog_sparse_range_reads(unsigned fingerprint_count=4096,
    PagedStoreLimits limits={64*1024*1024},bool legacy=false) {
  constexpr unsigned groups_per_batch=128;
  constexpr std::uint64_t fingerprint_base=0xd49bd1a5de10aceeULL;
  const auto backing=ram(4096);
  backing->capacity=limits.maximum_physical_bytes;
  const auto feature=[](unsigned group,bool latest) {
    const auto index=group*2+static_cast<unsigned>(latest);
    OptimizationGroupFeature member{2+index,0,detail::kNativeIndexedDataObject+1+index,1+index,
        detail::record_digest(data(32,98347+index))};
    member.bands[0]=fingerprint_base+2*group;
    return member;
  };
  {
    auto writer=legacy?legacy_coherence_catalog(backing,limits):
        IndexedCoherenceWriter::create(backing,id(90),id(1),limits);
    std::optional<IndexedCoherenceSnapshot> earlier;
    std::vector<IndexedCoherencePosting> postings;
    postings.reserve(2*groups_per_batch);
    for(unsigned start=0;start<fingerprint_count;start+=groups_per_batch) {
      postings.clear();
      for(unsigned group=start;group<start+groups_per_batch;++group) {
        if(group%4==0)postings.push_back({0,feature(group,false)});
        postings.push_back({0,feature(group,true)});
      }
      const auto before=backing->write_bytes;
      const auto update=writer.publish(postings,UINT64_MAX);
      require(update.write_bytes_upper_bound==backing->write_bytes-before,
          "catalog charge did not account for every byte written");
      if(!start)earlier.emplace(writer.snapshot());
    }
    const auto last_group=fingerprint_count-4;
    const auto later=writer.snapshot();
    const auto oldest=earlier->next_alternate(0,fingerprint_base);
    const auto key=fingerprint_base+2*last_group;
    const auto added=later.next_alternate(0,key);
    require(earlier->info().generation<later.info().generation && oldest &&
        oldest->representative==feature(0,false) && !earlier->next_alternate(0,key) &&
        added && added->representative==feature(last_group,false),
        "catalog snapshots reused a tree root across checkpoints");
  }
  auto persisted=image(std::move(backing->stable),4096);
  auto gate=std::make_shared<CatalogReadGate>(persisted);
  const auto view=IndexedCoherenceSnapshot::open(gate,id(90),id(1),limits);
  const auto info=view.info();
  require(view.has_alternates() && info.height>=2 &&
      info.objects==1+fingerprint_count+fingerprint_count/4 && info.metadata_page_bytes==(legacy?4096U:1024U),
      "catalog fixture did not reopen the expected geometry");

  const auto initial_reads=persisted->read_calls;
  const auto initial_bytes=persisted->read_bytes;
  const auto missing_queries=fingerprint_count+3*fingerprint_count/4;
  for(unsigned group=0;group<fingerprint_count;++group) {
    const auto key=fingerprint_base+2*group;
    require(!view.next_alternate(0,key+1),"unpopulated prefix resolved to a neighboring posting");
    if(group%4)require(!view.next_alternate(0,key),"primary-only prefix resolved to an alternate");
  }
  const auto miss_reads=persisted->read_calls-initial_reads;
  const auto miss_bytes=persisted->read_bytes-initial_bytes;
  const auto positive_start=persisted->read_bytes;
  for(unsigned group=0;group<fingerprint_count;group+=4) {
    const auto key=fingerprint_base+2*group;
    const auto first=view.next_alternate(0,key);
    require(first && first->representative==feature(group,false) &&
        view.find(0,key)==feature(group,true) && !view.next_alternate(0,key,first->cursor),
        "range lookup lost a positive binding or escaped its prefix");
  }
  const auto positive_bytes=persisted->read_bytes-positive_start;
  std::cout<<"coherence_sparse_ranges objects="<<info.objects<<" height="<<info.height
      <<" metadata_page_bytes="<<info.metadata_page_bytes
      <<" catalog_bytes="<<info.physical_end<<" catalog_write_bytes="<<backing->write_bytes
      <<" fingerprints="<<fingerprint_count<<" missing_queries="<<missing_queries
      <<" miss_page_reads="<<miss_reads<<" miss_read_bytes="<<miss_bytes
      <<" checked_positive_ranges="<<fingerprint_count/4<<" positive_read_bytes="<<positive_bytes<<'\n';
  const auto descendant_reads=2*static_cast<std::uint64_t>(missing_queries)*info.height;
  require(!persisted->writes && miss_bytes==miss_reads*info.metadata_page_bytes && miss_reads<descendant_reads,
      "empty alternate ranges reread the verified root or followed unrelated leaves");

  require(view.find(0,fingerprint_base)==feature(0,true),"primary warm lookup lost its representative");
  const auto primary_start=persisted->read_bytes;
  require(view.find(0,fingerprint_base)==feature(0,true),"primary repeated lookup changed its result");
  const auto primary_bytes=persisted->read_bytes-primary_start;
  const auto alternate=view.next_alternate(0,fingerprint_base);
  require(alternate && alternate->representative==feature(0,false),"alternate warm lookup lost its representative");
  const auto alternate_start=persisted->read_bytes;
  const auto repeated=view.next_alternate(0,fingerprint_base);
  require(repeated && repeated->representative==alternate->representative && repeated->cursor==alternate->cursor,
      "alternate repeated lookup changed its result");
  const auto alternate_bytes=persisted->read_bytes-alternate_start;
  std::cout<<"coherence_hot_lookup primary_bytes="<<primary_bytes<<" alternate_bytes="<<alternate_bytes
      <<" metadata_page_bytes="<<info.metadata_page_bytes<<'\n';
  require(primary_bytes && primary_bytes<info.metadata_page_bytes &&
      alternate_bytes && alternate_bytes<info.metadata_page_bytes,
      "repeated catalog probes reread checked lookup pages");
  gate->refuse=true;
  rejects([&]{(void)view.next_alternate(0,fingerprint_base);},"hot catalog accepted a posting without checking its payload");
  gate->refuse=false;
  require(view.find(0,fingerprint_base)==feature(0,true),"failed optional read changed authority");

  if(fingerprint_count>4096) {
    auto target=ram(4096);
    const auto source=PagedObjectStoreSnapshot::open(persisted,limits);
    const auto packed=PagedObjectStoreWriter::rebuild(source,target,id(91),limits,{StoreCompressionMode::raw,1,false});
    const auto checked=IndexedCoherenceSnapshot::open(target,id(91),id(1),limits);
    require(checked.info().objects==info.objects && checked.info().metadata_page_bytes==info.metadata_page_bytes &&
        checked.info().physical_end==target->extent() && !packed.poisoned(),
        "catalog packing changed the index inventory or geometry");
    for(unsigned group=0;group<fingerprint_count;++group) {
      const auto key=fingerprint_base+2*group;
      require(checked.find(0,key)==feature(group,true),"packed catalog lost its latest binding");
      if(group%4==0) {
        const auto alternate=checked.next_alternate(0,key);
        require(alternate && alternate->representative==feature(group,false),"packed catalog lost its older binding");
      }
    }
    std::cout<<"coherence_packed_catalog objects="<<checked.info().objects
        <<" height="<<checked.info().height<<" metadata_page_bytes="<<checked.info().metadata_page_bytes
        <<" catalog_bytes="<<target->extent()<<" write_bytes="<<target->write_bytes<<'\n';
  }
}
enum class ModularCatalogCase {available,masked_only,stale};
void indexed_modular_catalog_discovery(ModularCatalogCase scenario) {
  Fixture f(4096,0,true);
  const PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  auto advice=ram(4096),backing=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "modular catalog needs a funded advisory journal");
  f.session->bind_indexed_coherence_writer(IndexedCoherenceWriter::create(backing,id(90),id(1),limits));
  const auto original=data(kNativeExtentBytes,19463);
  const auto distant=calibrated_extent(original,1);
  put(*f.session,"original",original);
  put(*f.session,"other",data(kNativeExtentBytes,9137));
  for(unsigned i=0;i<2;++i)put(*f.session,"input-"+std::to_string(i),Bytes(3*kOptimizationGroupScanInputBytes/4));
  {
    NativeGroupMaintenance initial(*f.session);
    finish_pending(f,initial);
    require(initial.progress().checkpoints_published==1 && !initial.progress().published_groups,
        "initial numeric sample did not settle");
  }
  const auto checkpoint=f.session->optimization_snapshot().state.group_scan;
  require(checkpoint && checkpoint->representatives.size()==2 && checkpoint->projections.size()==2,
      "numeric source was absent from the settled scan");
  const auto& measured=checkpoint->representatives.front();
  const auto& masked=checkpoint->projections.front().signatures;
  constexpr auto modular_band=kCoherenceModularBand+2;
  const auto key=detail::native_group_delta_signatures(original)[2];
  require(key && key==detail::native_group_delta_signatures(distant)[2],"fixture lacks a shared numeric signature");
  {
    const auto archived=IndexedCoherenceSnapshot::open(backing,id(90),id(1),limits);
    auto expected=measured;
    expected.bands={};
    require(archived.has_modular_keys() && archived.find(modular_band,key)==expected,
        "automatic funded sampling did not persist the numeric signature");
  }
  if(scenario==ModularCatalogCase::masked_only) {
    backing=ram(4096);
    auto control=IndexedCoherenceWriter::create(backing,id(90),id(1),limits);
    std::vector<IndexedCoherencePosting> postings{{0,measured}};
    for(std::uint32_t i=0;i<masked.size();++i)postings.push_back({kCoherenceMaskedBand+i,measured,masked[i]});
    control.publish(postings,UINT64_MAX);
    require(control.snapshot().has_projected_keys() && !control.snapshot().has_modular_keys(),
        "control did not preserve the preceding catalog schema");
  }
  constexpr unsigned intervening=8;
  for(unsigned i=0;i<intervening;++i)
    put(*f.session,"noise-"+std::to_string(i),data(kNativeExtentBytes,4201+i));
  put(*f.session,"distant",distant);
  const auto current=scenario==ModularCatalogCase::stale?data(kNativeExtentBytes,52943):original;
  if(scenario==ModularCatalogCase::stale) {
    const auto file=f.session->open("original",{true,true,false});
    f.session->write(file,0,current);
    f.session->cleanup(file,false);
    f.session->close(file);
  }
  const auto resolve=[&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;};
  const auto compacted_bytes=[&](unsigned identity) {
    const auto closed=NativeVolumeHeadView::open(f.head,id(90),resolve);
    auto output=ram(4096);
    const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
    (void)NativeFilesystemWriter::compact(closed.filesystem(),output,id(identity),raw);
    auto verified=NativeFilesystemSnapshot::open_image(output,{NativeImageKind::indexed_paged,kStoreMaxPhysicalBytes});
    verified.verify_all();
    require(verified.read("original",0,kNativeExtentBytes)==current &&
        verified.read("distant",0,kNativeExtentBytes)==distant,"numeric catalog trial changed a file");
    return output->extent();
  };
  f.session.reset();
  const auto before=compacted_bytes(2);
  f.session.emplace(NativeFileSession::resume_closed_indexed_volume(f.head,id(90),1,resolve,
      {StoreCompressionMode::raw,1,false}));
  f.session->bind_indexed_coherence(backing,limits);
  NativeGroupMaintenance controller(*f.session);
  for(unsigned i=0;i<1000 && controller.pending();++i) {
    auto observed=f.clock.next();
    observed.physical_available=observed.physical_total/16+64*(33ULL*kNativeExtentBytes);
    const auto prior=controller.progress().coherence_lookups;
    const auto next=controller.advance(observed,f.clock.now);
    require(next.phase!=Phase::failed && next.coherence_lookups<=prior+1,
        "numeric catalog exceeded the admitted lookup quantum");
  }
  const auto progress=controller.progress();
  const bool grouped=scenario==ModularCatalogCase::available;
  require(!controller.pending() && !f.session->poisoned() && !progress.coherence_failures &&
      progress.published_groups==(grouped?1U:0U) && progress.attempted_groups==(grouped?1U:0U) &&
      (scenario==ModularCatalogCase::masked_only?!progress.coherence_matches:progress.coherence_matches>0),
      "numeric catalog missed a live nomination or used a stale binding");
  controller.cancel();
  f.session.reset();
  const auto after=compacted_bytes(3);
  const auto overhead=f.head->extent()+advice->extent()+backing->extent();
  require(grouped?after<before:after==before,"numeric catalog compacted size changed unexpectedly");
  std::cout<<"modular_catalog_discovery scenario="<<static_cast<unsigned>(scenario)
      <<" intervening_extents="<<intervening<<" lookups="<<progress.coherence_lookups
      <<" groups="<<progress.published_groups<<" catalog_bytes="<<backing->extent()
      <<" complete_before="<<before+overhead<<" complete_after="<<after+overhead<<" byte_exact=true\n";
}
enum class CatalogFunding {available,checkpoint_only,journal_failure,catalog_failure,earlier_keys_only};
void funded_indexed_catalog(CatalogFunding outcome) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "funded catalog needs a work journal");
  auto backing=ram(4096);
  auto catalog=IndexedCoherenceWriter::create(backing,id(90),id(1),{kStoreMaxPhysicalBytes});
  const auto initial_end=catalog.snapshot().info().physical_end;
  f.session->bind_indexed_coherence_writer(std::move(catalog));

  const auto first=data(kNativeExtentBytes,117),second=data(kNativeExtentBytes,846);
  put(*f.session,"first",first);put(*f.session,"second",second);
  put(*f.session,"input-a",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
  if(outcome!=CatalogFunding::checkpoint_only)
    put(*f.session,"input-b",Bytes(kOptimizationGroupScanInputBytes/2));
  const auto before=read_optimization_state(*advice,id(90));
  const auto catalog_writes=backing->write_bytes;
  const auto catalog_calls=backing->calls;
  if(outcome==CatalogFunding::journal_failure)
    advice->fail_at=advice->calls+2; // The first paid slot survives, then the peer fails.
  if(outcome==CatalogFunding::catalog_failure)backing->fail_at=backing->calls;

  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    require(controller.progress().checkpoints_published==(outcome==CatalogFunding::journal_failure?0U:1U),
        "catalog reservation confused an acknowledged checkpoint with a failed one");
    const auto reads=backing->read_calls,calls=backing->calls;
    for(unsigned tick=0;tick<4;++tick)f.step(controller);
    require(backing->calls==calls && backing->read_calls==reads,
        "unchanged workload retried catalog maintenance");
  }
  const auto settled=read_optimization_state(*image(advice->stable,4096),id(90));
  const auto view=IndexedCoherenceSnapshot::open(image(backing->stable,4096),id(90),id(1),{kStoreMaxPhysicalBytes});
  require(settled.code==OptimizationReadCode::available && settled.state.group_scan &&
      settled.sequence==before.sequence+1,"funded catalog lost the checked settlement");
  const auto recorded=*settled.state.settled_input_bytes;
  if(outcome==CatalogFunding::available) {
    const auto spent=backing->write_bytes-catalog_writes;
    require(view.info().objects==1+2*(2+kOptimizationXorProjections+kCoherenceModularWidths) &&
        view.has_modular_keys() && view.info().physical_end>initial_end && spent>0 &&
        recorded>=kOptimizationGroupScanInputBytes+spent*kOptimizationGroupScanInputPerWriteByte,
        "automatic feature publication escaped its durable write charge");
    for(const auto& feature:settled.state.group_scan->representatives) {
      const auto shifted=detail::coherence_shift_band(feature.bands);
      require(view.find(0,feature.bands[0])==feature && shifted &&
          view.find(*shifted,feature.bands[*shifted])==feature,"catalog lost its aligned or rolling nomination");
    }
  } else if(outcome==CatalogFunding::checkpoint_only) {
    require(recorded==kOptimizationGroupScanInputBytes && backing->calls==catalog_calls &&
        view.info().objects==1,"checkpoint-only credit permitted catalog writes");
  } else if(outcome==CatalogFunding::journal_failure) {
    require(recorded>kOptimizationGroupScanInputBytes && settled.degraded &&
        backing->calls==catalog_calls && view.info().objects==1 &&
        f.session->optimization_info().phase==NativeOptimizationPhase::terminal,
        "interrupted settlement released catalog writes");
  } else {
    require(recorded>kOptimizationGroupScanInputBytes && backing->calls==catalog_calls+1 &&
        view.info().objects==1 && f.session->optimization_info().phase==NativeOptimizationPhase::ready,
        "a catalog failure discarded the paid checkpoint or retried");
    // Reopening the last valid catalog and charged journal cannot spend the
    // acknowledged input again, even though the optional output was lost.
    reopen_indexed(f,advice,settled.sequence);
    auto recovered=IndexedCoherenceWriter::resume(backing,id(90),id(1),view.info().generation,{kStoreMaxPhysicalBytes});
    f.session->bind_indexed_coherence_writer(std::move(recovered));
    const auto calls=backing->calls;
    NativeGroupMaintenance resumed(*f.session);
    finish_pending(f,resumed);
    require(backing->calls==calls &&
        f.session->optimization_snapshot().state.settled_input_bytes==recorded,
        "catalog restart minted previously charged input");
  }
  const auto user=f.session->snapshot();
  require(!f.session->poisoned() && user.read("first",0,kNativeExtentBytes)==first &&
      user.read("second",0,kNativeExtentBytes)==second,"optional catalog altered acknowledged files");
  user.verify_all();
}
void automatic_indexed_catalog(CatalogFunding outcome) {
  Fixture f(4096,0,true);
  auto advice=ram(4096),backing=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "automatic catalog requires the funded advisory journal");
  unsigned reservations=0;
  const PagedStoreLimits limits{kStoreMaxPhysicalBytes};
  auto storage=std::make_shared<IndexedCoherenceStorage>(limits,[&] {
    ++reservations;
    return backing;
  });
  f.session->configure_indexed_coherence(storage);
  require(!reservations && !backing->extent(),"session attachment allocated an empty catalog");

  const auto first=data(kNativeExtentBytes,214),second=data(kNativeExtentBytes,624);
  put(*f.session,"first",first);put(*f.session,"second",second);
  put(*f.session,"base-input",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
  const bool earlier_only=outcome==CatalogFunding::earlier_keys_only;
  if(earlier_only) {
    // The grant covers the primary and masked rows plus the root writes.
    // The numeric rows require one further 4 KiB page in this model.
    constexpr auto input=37*4096*kOptimizationGroupScanInputPerWriteByte;
    put(*f.session,"more-input-a",Bytes(input/2));
    put(*f.session,"more-input-b",Bytes(input/2));
  }else if(outcome!=CatalogFunding::checkpoint_only) {
    put(*f.session,"more-input-a",Bytes(3*kOptimizationGroupScanInputBytes/4));
    put(*f.session,"more-input-b",Bytes(3*kOptimizationGroupScanInputBytes/4));
  }
  if(outcome==CatalogFunding::journal_failure)advice->fail_at=advice->calls+2;
  if(outcome==CatalogFunding::catalog_failure)backing->fail_at=0;
  {
    NativeGroupMaintenance controller(*f.session);
    finish_pending(f,controller);
    require(controller.progress().checkpoints_published==(outcome==CatalogFunding::journal_failure?0U:1U),
        "catalog creation changed checkpoint acknowledgment");
  }

  const auto journal=read_optimization_state(*image(advice->stable,4096),id(90));
  require(journal.code==OptimizationReadCode::available && journal.state.group_scan,
      "catalog creation left no recoverable scan checkpoint");
  const auto settled=*journal.state.settled_input_bytes;
  if(outcome==CatalogFunding::checkpoint_only) {
    require(!reservations && !backing->calls && settled==kOptimizationGroupScanInputBytes,
        "checkpoint-only credit allocated or wrote a catalog");
  }else {
    require(reservations==1 && settled>kOptimizationGroupScanInputBytes,
        "a funded catalog creation failed to reserve exactly once");
    if(outcome==CatalogFunding::available || earlier_only) {
      const auto snapshot=IndexedCoherenceSnapshot::open(backing,id(90),id(1),limits);
      const auto projected=kOptimizationXorProjections+(earlier_only?0:kCoherenceModularWidths);
      const auto keys_per_feature=1+projected+static_cast<unsigned>(!earlier_only);
      require(snapshot.info().objects==1+2*keys_per_feature &&
          snapshot.has_projected_keys() && snapshot.has_modular_keys()!=earlier_only && snapshot.info().generation==2 &&
          settled==kOptimizationGroupScanInputBytes+backing->write_bytes*kOptimizationGroupScanInputPerWriteByte,
          "initial catalog and postings escaped one complete charged checkpoint");
      if(earlier_only)std::cout<<"modular_catalog_tight_grant catalog_bytes="<<snapshot.info().physical_end
          <<" charged_catalog_writes="<<backing->write_bytes<<" reservations="<<reservations<<'\n';
      for(const auto& feature:journal.state.group_scan->representatives) {
        const auto shifted=detail::coherence_shift_band(feature.bands);
        require(snapshot.find(0,feature.bands[0])==feature && shifted,
            "initial catalog lost its aligned sample or its expected rolling key");
        const auto rolling=snapshot.find(*shifted,feature.bands[*shifted]);
        require(earlier_only?!rolling:rolling==feature,
            "initial catalog retained a rolling row outside its write grant");
      }
      reopen_indexed(f,advice,journal.sequence);
      const auto mutations=backing->calls;
      f.session->configure_indexed_coherence(storage);
      NativeGroupMaintenance resumed(*f.session);
      finish_pending(f,resumed);
      require(reservations==1 && backing->calls==mutations &&
          f.session->optimization_snapshot().state.settled_input_bytes==settled,
          "retained catalog was reprovisioned or spent credit after reopen");
    } else if(outcome==CatalogFunding::journal_failure) {
      require(!backing->calls && !backing->extent() && journal.degraded,
          "unverified initial settlement exposed catalog writes");
    } else {
      require(backing->calls==1 && !backing->extent() && !journal.degraded,
          "an interrupted initial catalog write invalidated or replayed its paid journal");
      reopen_indexed(f,advice,journal.sequence);
      f.session->configure_indexed_coherence(storage);
      NativeGroupMaintenance resumed(*f.session);
      finish_pending(f,resumed);
      require(reservations==1 && backing->calls==1 &&
          f.session->optimization_snapshot().state.settled_input_bytes==settled,
          "an interrupted initial catalog retried on unchanged evidence");
    }
  }
  const auto user=f.session->snapshot();
  require(!f.session->poisoned() && user.read("first",0,kNativeExtentBytes)==first &&
      user.read("second",0,kNativeExtentBytes)==second,"automatic catalog creation lost acknowledged data");
  user.verify_all();
}
enum class CoherenceCopyOutcome {completed,charge_cut,target_cut};
enum class CatalogSelectionCase {lease_only,selected,before_selection,after_first_barrier,after_selection};
std::size_t compact_full_indexed_catalog(CoherenceCopyOutcome outcome,std::size_t target_cut=0,
    bool alternate_fixture=false,CatalogSelectionCase selection_case=CatalogSelectionCase::lease_only) {
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "catalog copy needs funded advice");
  // The backing holds exactly one incremental page rewrite after creation.
  // This is fixture pressure; Windows uses a headroom-derived layout.
  const PagedStoreLimits limits{39*4096};
  const bool identified=selection_case!=CatalogSelectionCase::lease_only;
  const bool selection_failed=identified && selection_case!=CatalogSelectionCase::selected;
  const auto first_identity=identified?id(40):StoreIdentity{};
  const auto second_identity=identified?id(41):StoreIdentity{};
  std::vector<std::shared_ptr<Ram>> backings;
  const auto reserve=[&]() -> std::shared_ptr<StoreByteIo> {
    if(backings.size()==2)throw std::runtime_error("unexpected catalog reservation");
    return backings.emplace_back(ram(4096));
  };
  auto owner=identified?std::make_shared<IndexedCoherenceStorage>(limits,[&] {
    const auto identity=backings.empty()?first_identity:second_identity;
    return IndexedCoherenceStorage::Reservation{identity,reserve()};
  },[](StoreIdentity)->std::shared_ptr<StoreByteIo> {return {};})
      :std::make_shared<IndexedCoherenceStorage>(limits,reserve);
  f.session->configure_indexed_coherence(owner);
  const auto stable=data(kNativeExtentBytes,1207);
  auto old=data(kNativeExtentBytes,373);
  if(alternate_fixture) {
    // Share one measured masked signature while leaving the rest of the
    // data unrelated. The later live extent becomes the primary row.
    constexpr auto differing=std::byte{0x30};
    for(std::size_t sample=0;sample<16;++sample)for(std::size_t i=0;i<64;++i) {
      const auto at=sample*(kNativeExtentBytes/16)+i;
      old[at]=(old[at]&differing)|(stable[at]&~differing);
    }
    put(*f.session,"edited",old);
    put(*f.session,"keep",stable);
  }else {
    put(*f.session,"keep",stable);
    put(*f.session,"edited",old);
  }
  unsigned funding=0;
  const auto earn=[&](unsigned count) {
    for(unsigned i=0;i<count;++i)
      put(*f.session,"credit-"+std::to_string(funding++),Bytes(3*kOptimizationGroupScanInputBytes/4));
  };
  const auto edit=[&](const Bytes& bytes) {
    const auto opened=f.session->open("edited",{true,true,false});
    f.session->write(opened,0,bytes);f.session->cleanup(opened,false);f.session->close(opened);
  };
  const auto earn_selection=[&] {
    if(identified)for(unsigned i=0;i<2;++i)
      put(*f.session,"credit-"+std::to_string(funding++),Bytes(kOptimizationGroupScanInputBytes/2));
  };
  earn(3);
  earn_selection();
  NativeGroupMaintenance controller(*f.session);
  finish_pending(f,controller);
  require(backings.size()==1 && controller.progress().coherence_catalog_bytes>0,"fixture failed to create initial catalog");
  const auto data_view=f.session->snapshot();
  if(identified) {
    const auto initial=IndexedCoherenceSnapshot::open(backings.front(),id(90),id(1),limits,first_identity);
    require(f.session->optimization_snapshot().state.catalog_selection==
        OptimizationCatalogSelection{id(1),first_identity,initial.info().generation},
        "first catalog lacks a durable backing identity");
  }
  const auto first_scan=*f.session->optimization_snapshot().state.group_scan;
  const auto find_feature=[&](std::string_view name) {
    const auto inode=data_view.stat(name).inode;
    const auto found=std::ranges::find(first_scan.representatives,inode,&OptimizationGroupFeature::inode);
    if(found==first_scan.representatives.end())throw std::runtime_error("fixture lost a sampled file");
    return *found;
  };
  const auto keep=find_feature("keep"),stale=find_feature("edited");
  const auto projected_key=[&](const OptimizationGroupFeature& feature) {
    const auto row=std::ranges::find(first_scan.representatives,feature.object,&OptimizationGroupFeature::object)-
        first_scan.representatives.begin();
    const auto projection=std::ranges::find(first_scan.projections,row,&OptimizationGroupProjection::representative);
    if(projection==first_scan.projections.end())throw std::runtime_error("fixture lost its masked signature");
    return projection->signatures[2];
  };
  constexpr auto masked_band=static_cast<std::uint32_t>(kOptimizationGroupBands)+2;
  const auto keep_key=projected_key(keep),stale_key=projected_key(stale);
  auto projected_keep=keep;projected_keep.bands={};
  const auto active=backings.front();
  edit(data(kNativeExtentBytes,57012));
  earn(2);
  finish_pending(f,controller);
  const auto full=active->extent();
  const auto before=f.session->optimization_snapshot();
  require(backings.size()==1 && full==limits.maximum_physical_bytes,"fixture did not fill the selected catalog");
  if(identified)require(before.state.catalog_selection && before.state.catalog_selection->backing==first_identity,
      "append changed the selected backing");

  const auto latest=data(kNativeExtentBytes,27992);
  edit(latest);
  earn(1);
  const auto old_writes=active->calls;
  finish_pending(f,controller);
  require(backings.size()==2 && !backings.back()->calls && !backings.back()->extent() &&
      f.session->optimization_snapshot().sequence==before.sequence,
      "blocked copy spent its charge or wrote before sufficient credit");
  const auto candidate=backings.back();
  const auto probe_reads=active->read_calls+candidate->read_calls;
  for(unsigned i=0;i<10;++i)f.step(controller);
  require(!controller.pending() && active->read_calls+candidate->read_calls==probe_reads &&
      active->calls==old_writes && !candidate->calls,"credit wait polled the old catalog or replacement");

  if(outcome==CoherenceCopyOutcome::charge_cut)advice->fail_at=advice->calls+2;
  if(outcome==CoherenceCopyOutcome::target_cut)candidate->fail_at=target_cut;
  if(selection_failed) {
    // The copy charge writes a full two-slot pair first. Cut the separate
    // selection pair before its first write, after its first barrier or
    // after its final barrier reported completion to the backing.
    const auto selection_start=advice->calls+4;
    advice->fail_at=selection_start+(selection_case==CatalogSelectionCase::before_selection?0:
        (selection_case==CatalogSelectionCase::after_first_barrier?1:3));
    if(selection_case!=CatalogSelectionCase::before_selection)advice->failure=1;
  }
  const auto unrelated=data(kNativeExtentBytes,69923);
  put(*f.session,"other",unrelated);
  earn(2); // While the planner pins the previous, immutable file checkpoint.
  earn_selection();
  finish_pending(f,controller);
  const auto progress=controller.progress();
  const auto copy_calls=candidate->calls;
  require(backings.size()==2 && active->calls==old_writes,"compaction modified the old catalog or restarted on unrelated input");
  const auto original=IndexedCoherenceSnapshot::open(active,id(90),id(1),limits,first_identity);
  constexpr auto numeric_band=kCoherenceModularBand+2;
  const auto keep_numeric=detail::native_group_delta_signatures(stable)[2];
  const auto stale_numeric=detail::native_group_delta_signatures(old)[2];
  const auto keep_shift=detail::coherence_shift_band(keep.bands),stale_shift=detail::coherence_shift_band(stale.bands);
  require(keep_shift && stale_shift && original.find(*keep_shift,keep.bands[*keep_shift])==keep &&
      original.find(*stale_shift,stale.bands[*stale_shift])==stale,
      "compaction changed the original rolling bindings");
  require(original.find(0,keep.bands[0])==keep && original.find(0,stale.bands[0])==stale,
      "compaction altered the existing catalog's live or old bindings");
  require(original.find(numeric_band,keep_numeric)==projected_keep && original.find(numeric_band,stale_numeric),
      "compaction changed the original numeric catalog");
  if(alternate_fixture) {
    auto projected_stale=stale;projected_stale.bands={};
    const auto retired=original.next_alternate(masked_band,keep_key);
    require(keep_key==stale_key && original.has_alternates() && retired &&
        retired->representative==projected_stale && original.find(masked_band,keep_key)==projected_keep,
        "rotation fixture did not preserve the stale alternate beside its live primary");
  }
  const auto settled=read_optimization_state(*image(advice->stable,4096),id(90));
  require(settled.code==OptimizationReadCode::available &&
      *settled.state.settled_input_bytes>*before.state.settled_input_bytes,"copy did not settle its write budget");
  if(outcome==CoherenceCopyOutcome::completed) {
    const auto replacement=IndexedCoherenceSnapshot::open(candidate,id(90),id(1),limits,second_identity);
    const bool stale_masked_removed=alternate_fixture?
        replacement.has_alternates() && !replacement.next_alternate(masked_band,keep_key):
        !replacement.find(masked_band,stale_key);
    require(replacement.info().objects==3+kOptimizationXorProjections+kCoherenceModularWidths &&
        replacement.find(0,keep.bands[0])==keep && replacement.find(numeric_band,keep_numeric)==projected_keep &&
        !replacement.find(numeric_band,stale_numeric) &&
        replacement.find(*keep_shift,keep.bands[*keep_shift])==keep &&
        !replacement.find(*stale_shift,stale.bands[*stale_shift]) &&
        replacement.find(masked_band,keep_key)==projected_keep && !replacement.find(0,stale.bands[0]) &&
        stale_masked_removed,"compaction kept stale postings or lost a current representative");
    if(selection_failed) {
      require(!progress.coherence_compactions && progress.coherence_discards==1 &&
          progress.coherence_catalog_bytes==full && f.session->optimization_info().phase==NativeOptimizationPhase::terminal,
          "an uncertain selector publication displaced the live catalog");
    }else {
      require(progress.coherence_compactions==1 && !progress.coherence_discards &&
          progress.coherence_compacted_bytes_saved==full-replacement.info().physical_end,
          "verified replacement did not switch the live catalog");
    }
    if(alternate_fixture)std::cout<<"coherence_alternate_rotation old_bytes="<<full
        <<" new_bytes="<<replacement.info().physical_end<<" stale_alternate_removed=1 live_primary_preserved=1\n";
    const bool selected_replacement=identified && selection_case!=CatalogSelectionCase::before_selection;
    const auto selection_cost=selected_replacement?kOptimizationGroupScanInputBytes:0;
    require(*settled.state.settled_input_bytes-*before.state.settled_input_bytes==
        kOptimizationGroupScanInputBytes+selection_cost+candidate->write_bytes*kOptimizationGroupScanInputPerWriteByte,
        "candidate grew beyond the durable write charge");
    if(identified) {
      require(settled.state.catalog_selection==(selected_replacement?
          std::optional(OptimizationCatalogSelection{id(1),second_identity,replacement.info().generation}):
          before.state.catalog_selection),"power-cut journal chose the wrong catalog");
      require(settled.degraded==(selection_case==CatalogSelectionCase::after_first_barrier),
          "selector did not preserve the expected paired-slot state");
      const auto restart=[&](bool misdirect) {
        Fixture restarted(image(f.head->stable,4096),image(f.image->stable,4096));
        auto recovered_advice=image(advice->stable,4096);
        const auto rebound=restarted.session->resume_budgeted_optimization_area(recovered_advice,settled.sequence);
        require(rebound.repair_pending==settled.degraded &&
            rebound.phase==(settled.degraded?NativeOptimizationPhase::unavailable:NativeOptimizationPhase::ready),
            "restart could not reopen the selected advice");
        std::vector<StoreIdentity> resolved;
        const auto prior=image(active->stable,4096),next=image(candidate->stable,4096);
        std::size_t new_reservations=0;
        auto storage=std::make_shared<IndexedCoherenceStorage>(limits,[&] {
          ++new_reservations;
          return IndexedCoherenceStorage::Reservation{};
        },[&](StoreIdentity identity)->std::shared_ptr<StoreByteIo> {
          resolved.push_back(identity);
          return (identity==second_identity)!=misdirect?next:prior;
        });
        restarted.session->configure_indexed_coherence(storage);
        require(resolved.empty(),"configuring the catalog performed synchronous recovery I/O");
        NativeGroupMaintenance reopened(*restarted.session);
        if(rebound.repair_pending) {
          for(unsigned i=0;i<4;++i)restarted.step(reopened);
          require(resolved.empty(),"catalog opened from advice awaiting funded repair");
          put(*restarted.session,"repair-credit",Bytes(kOptimizationAreaBytes));
          require(restarted.session->repair_budgeted_optimization_area().state.phase==NativeOptimizationPhase::ready,
              "fresh input did not repair a split selector pair");
        }
        for(unsigned i=0;i<50 && resolved.empty();++i)restarted.step(reopened);
        const auto catalog_bytes=(selected_replacement?next:prior)->extent();
        require(resolved==std::vector<StoreIdentity>{settled.state.catalog_selection->backing} &&
            reopened.progress().coherence_catalog_bytes==(misdirect?0:catalog_bytes) && !new_reservations,
            "fresh session did not resolve only the durably selected backing");
        require(!prior->writes && !next->writes,"restart rewrote a catalog before fresh work");
        const auto reopened_data=restarted.session->snapshot();
        require(reopened_data.read("keep",0,kNativeExtentBytes)==stable &&
            reopened_data.read("edited",0,kNativeExtentBytes)==latest &&
            reopened_data.read("other",0,kNativeExtentBytes)==unrelated,"restart changed acknowledged files");
        reopened_data.verify_all();
        if(selection_case==CatalogSelectionCase::after_selection) {
          const auto after=data(kNativeExtentBytes,40552);
          put(*restarted.session,"after-restart",after);
          put(*restarted.session,"fresh-credit-a",Bytes(3*kOptimizationGroupScanInputBytes/4));
          put(*restarted.session,"fresh-credit-b",Bytes(3*kOptimizationGroupScanInputBytes/4));
          finish_pending(restarted,reopened);
          const auto continued=IndexedCoherenceSnapshot::open(next,id(90),id(1),limits,second_identity);
          require(continued.info().generation>replacement.info().generation && !prior->writes &&
              restarted.session->optimization_snapshot().state.catalog_selection==settled.state.catalog_selection &&
              continued.find(0,keep.bands[0])==keep &&
              restarted.session->snapshot().read("after-restart",0,kNativeExtentBytes)==after,
              "reopened catalog could not publish fresh advice without changing its selection");
        }
      };
      restart(false);
      if(selection_case==CatalogSelectionCase::before_selection)restart(true);
    }
    if(!selection_failed) {
      put(*f.session,"after-copy",data(kNativeExtentBytes,40552));
      earn(2);
      finish_pending(f,controller);
      const auto newer=IndexedCoherenceSnapshot::open(candidate,id(90),id(1),limits,second_identity);
      require(newer.info().generation>replacement.info().generation &&
          newer.find(0,keep.bands[0])==keep,"the compacted catalog did not admit a new write");
    }
  }else {
    require(!progress.coherence_compactions && progress.coherence_discards==1 &&
        progress.coherence_catalog_bytes==full,"interrupted copy displaced the checked old catalog");
    require(outcome==CoherenceCopyOutcome::charge_cut?(!candidate->calls && settled.degraded):
        (candidate->calls==target_cut+1 && (candidate->extent()!=0)==(target_cut!=0) && !settled.degraded),
        "catalog started before the verified charge or retried a failed write");
  }
  const auto view=f.session->snapshot();
  require(!f.session->poisoned() && view.read("keep",0,kNativeExtentBytes)==stable &&
      view.read("edited",0,kNativeExtentBytes)==latest && view.read("other",0,kNativeExtentBytes)==unrelated,
      "catalog copy or recovery affected acknowledged file bytes");
  view.verify_all();
  return copy_calls;
}
void indexed_catalog_reclamation_at_scale(std::optional<std::size_t> final_flush_cut={}) {
  constexpr PagedStoreLimits limits{64*1024*1024};
  constexpr unsigned offered_keys=16384;
  constexpr std::uint64_t fingerprint_base=0xd49bd1a5de10aceeULL;
  Fixture f(4096,0,true);
  auto advice=ram(4096);
  require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
      "large catalog requires an admitted budget area");
  const auto left=data(kNativeExtentBytes,12107),right=data(kNativeExtentBytes,97001);
  put(*f.session,"left",left);
  put(*f.session,"right",right);
  const auto feature=[&](std::string_view path,const Bytes& expected) {
    const auto pages=PagedObjectStoreSnapshot::open(f.image,limits);
    const detail::IndexedVolumeView view(pages,pages.read(1));
    const auto inode=view.resolve(path).inode;
    const auto sample=view.sample_content({inode,0},detail::IndexedContentLookup::exact,{});
    require(sample && sample->payload==expected,"seeded extent is absent or different");
    return OptimizationGroupFeature{inode,0,sample->token.object,sample->token.revision,detail::record_digest(expected)};
  };
  const auto older=feature("left",left),newer=feature("right",right);
  require(older.revision<newer.revision,"fixture needs displaced live nominations");
  auto active=ram(4096);
  const std::weak_ptr<Ram> retired=active;
  auto row=[&](unsigned index,bool newest) {
    auto sample=newest?newer:older;
    sample.bands[0]=fingerprint_base+2*index;
    return IndexedCoherencePosting{0,sample};
  };
  unsigned seeded_keys=0;
  {
    auto seed=IndexedCoherenceWriter::create(active,id(90),id(1),limits);
    std::vector<IndexedCoherencePosting> postings;
    postings.reserve(160);
    for(unsigned width=128;seeded_keys<offered_keys;) {
      const auto count=(std::min)(width,offered_keys-seeded_keys);
      postings.clear();
      for(unsigned i=seeded_keys;i<seeded_keys+count;++i) {
        if(i%4==0)postings.push_back(row(i,false));
        postings.push_back(row(i,true));
      }
      try {
        (void)seed.publish(postings,UINT64_MAX);
        seeded_keys+=count;
      }catch(const PagedStoreCapacityError& error) {
        require(error.physical_space_exhausted() && !seed.poisoned(),"catalog seeding did not reach a clean space refusal");
        if(width==1)break;
        width/=2;
      }
    }
  }
  auto pinned=std::optional(IndexedCoherenceSnapshot::open(active,id(90),id(1),limits));
  const auto old=pinned->info();
  require(seeded_keys>=offered_keys/2 && seeded_keys<offered_keys &&
      old.objects>seeded_keys && old.physical_end<=limits.maximum_physical_bytes,
      "large catalog did not exercise append pressure with live alternates");
  const auto compact_bound=[&] {
    const auto pages=PagedObjectStoreSnapshot::open(active,limits);
    std::vector<StoreObjectInfo> records;
    for(std::uint64_t after=0;;) {
      const auto page=pages.list_page(after,4096);
      if(page.empty())break;
      records.insert(records.end(),page.begin(),page.end());
      after=page.back().id;
    }
    return pages.rebuilt_size(records)+8192; // Both commit records are written again.
  }();
  std::shared_ptr<Ram> replacement;
  auto storage=std::make_shared<IndexedCoherenceStorage>(limits,[&]() -> std::shared_ptr<StoreByteIo> {
    require(!replacement,"large catalog repeatedly reserved a replacement");
    return replacement=ram(4096);
  },IndexedCoherenceWriter::resume(active,id(90),id(1),old.generation,limits));
  f.session->configure_indexed_coherence(storage);
  const auto writes_before=active->write_bytes;

  const Bytes zeros(1024*1024);
  f.session->create_entry("credit");
  const auto credit_handle=f.session->open("credit",{true,true,false});
  std::uint64_t credited=0;
  const auto fund=[&](std::uint64_t bytes) {
    while(bytes) {
      const auto count=static_cast<std::size_t>((std::min)(bytes,std::uint64_t{zeros.size()}));
      f.session->write(credit_handle,credited,std::span(zeros).first(count));
      credited+=count;
      bytes-=count;
    }
  };
  fund(kOptimizationGroupScanInputBytes+zeros.size());
  NativeGroupMaintenance controller(*f.session);
  std::uint64_t opportunities=0,maximum_step_read=0,maximum_step_write=0;
  bool foreground_checked=false;
  const auto advance=[&] {
    require(++opportunities<120000,"large catalog did not settle within the modeled opportunities");
    const auto read=active->read_bytes+(replacement?replacement->read_bytes:0);
    const auto written=active->write_bytes+(replacement?replacement->write_bytes:0);
    const auto progress=f.step(controller);
    require(progress.phase!=Phase::failed,"large catalog maintenance failed");
    maximum_step_read=(std::max)(maximum_step_read,active->read_bytes+(replacement?replacement->read_bytes:0)-read);
    maximum_step_write=(std::max)(maximum_step_write,active->write_bytes+(replacement?replacement->write_bytes:0)-written);
    if(replacement && !foreground_checked) {
      const auto traffic=active->read_bytes+active->write_bytes+replacement->read_bytes+replacement->write_bytes;
      auto foreground=f.clock.next();
      foreground.foreground_pending=true;
      const auto deferred=controller.advance(foreground,f.clock.now);
      require(deferred.deferral==Defer::foreground &&
          traffic==active->read_bytes+active->write_bytes+replacement->read_bytes+replacement->write_bytes,
          "catalog copy performed I/O during foreground admission");
      foreground_checked=true;
    }
  };
  while(controller.pending())advance();
  const auto unfunded=f.session->optimization_snapshot();
  require(replacement && !replacement->extent() && !replacement->write_bytes && foreground_checked &&
      !unfunded.state.group_scan && active->write_bytes==writes_before,
      "space refusal failed to wait for credit on the untouched old catalog");
  const auto waiting_reads=active->read_bytes;
  for(unsigned i=0;i<8;++i)f.step(controller);
  require(!controller.pending() && active->read_bytes==waiting_reads && !replacement->calls,
      "unfunded catalog copy continued polling its backings");
  fund((compact_bound+2*1024*1024)*kOptimizationGroupScanInputPerWriteByte);
  f.session->cleanup(credit_handle,false);
  f.session->close(credit_handle);
  const auto verify_files=[&] {
    const auto source=NativeVolumeHeadView::open(f.head,id(90),
        [&](const auto& key)->std::shared_ptr<StoreByteIo>{return key==id(1)?f.image:nullptr;});
    const auto& independent=source.filesystem();
    independent.verify_all();
    require(independent.read("left",0,kNativeExtentBytes)==left &&
        independent.read("right",0,kNativeExtentBytes)==right && independent.stat("credit").logical_bytes==credited,
        "catalog reclamation affected acknowledged file bytes");
    for(std::uint64_t at=0;at<credited;at+=zeros.size()) {
      const auto count=static_cast<std::uint32_t>((std::min)(credited-at,std::uint64_t{zeros.size()}));
      require(std::ranges::equal(independent.read("credit",at,count),std::span(zeros).first(count)),
          "catalog reclamation changed sparse file contents");
    }
  };
  if(final_flush_cut) {
    replacement->fail_at=*final_flush_cut;
    replacement->failure=1;
  }
  while(controller.pending() && !controller.progress().coherence_compactions &&
      !controller.progress().coherence_discards)advance();
  const auto rotation=controller.progress();
  const auto packed=IndexedCoherenceSnapshot::open(replacement,id(90),id(1),limits);
  const auto packed_info=packed.info();
  const auto charged=f.session->optimization_snapshot();
  const auto copy_writes=replacement->write_bytes;
  require(packed_info.objects==old.objects && packed_info.metadata_page_bytes==4096 &&
      packed_info.physical_end<compact_bound-8192 && active->write_bytes==writes_before &&
      !charged.state.group_scan &&
      *charged.state.settled_input_bytes-*unfunded.state.settled_input_bytes==
          kOptimizationGroupScanInputBytes+copy_writes*kOptimizationGroupScanInputPerWriteByte,
      "large copy failed to retain every row, choose the smaller layout or settle its exact charge");
  if(final_flush_cut) {
    require(!rotation.coherence_compactions && rotation.coherence_discards==1 &&
        rotation.coherence_catalog_bytes==old.physical_end && replacement->calls==*final_flush_cut+1 &&
        pinned->find(0,fingerprint_base)==row(0,true).representative,
        "interrupted replacement displaced the selected catalog");
    verify_files();
    std::cout<<"coherence_scale_cut cut="<<*final_flush_cut<<" source_bytes="<<old.physical_end
        <<" unpublished_bytes="<<packed_info.physical_end<<" copy_writes="<<copy_writes
        <<" old_catalog_selected=1 exact_cpu_readback=1\n";
    return;
  }
  require(rotation.coherence_compactions==1 && !rotation.coherence_discards,
      "verified catalog copy was not selected");
  const auto copy_calls=replacement->calls;
  while(controller.pending())advance();
  const auto resumed=IndexedCoherenceSnapshot::open(replacement,id(90),id(1),limits);
  const auto scan=f.session->optimization_snapshot().state.group_scan;
  require(scan && resumed.info().generation>packed_info.generation &&
      resumed.info().objects>old.objects,"the ordinary scan did not append after reclamation");
  std::uint64_t checked_alternates=0;
  for(unsigned i=0;i<seeded_keys;++i) {
    const auto key=fingerprint_base+2*i;
    const auto expected=row(i,true).representative;
    require(pinned->find(0,key)==expected && packed.find(0,key)==expected && resumed.find(0,key)==expected,
        "rotation or resumed append lost a live primary nomination");
    const auto alt=pinned->next_alternate(0,key);
    const auto next=resumed.next_alternate(0,key);
    require(bool(next)==bool(alt) && (!alt || next->representative==alt->representative),
        "rotation lost a live alternate nomination");
    checked_alternates+=bool(alt);
  }
  require(old.objects==1+seeded_keys+checked_alternates &&
      active->write_bytes==writes_before && active->extent()==old.physical_end,
      "the pinned source changed or its inventory was incompletely checked");
  const auto combined=active->extent()+replacement->extent();
  active.reset();
  require(!retired.expired(),"old catalog did not remain owned by its reader");
  pinned.reset();
  require(retired.expired(),"retired catalog backing outlived its final reader");
  verify_files();
  std::cout<<"coherence_scale_reclaimed primary="<<seeded_keys<<" alternates="<<checked_alternates
      <<" source_bytes="<<old.physical_end<<" packed_bytes="<<packed_info.physical_end
      <<" resumed_bytes="<<resumed.info().physical_end<<" retained_reader_combined_bytes="<<combined
      <<" compact_page_projection_bytes="<<compact_bound-8192
      <<" copy_writes="<<copy_writes<<" copy_calls="<<copy_calls<<" opportunities="<<opportunities
      <<" maximum_step_read_bytes="<<maximum_step_read<<" maximum_step_write_bytes="<<maximum_step_write
      <<" input_credit_bytes="<<credited<<" source_released=1 foreground_yield=1 exact_cpu_readback=1\n";
}

void indexed_scan_checkpoint_faults() {
  for(const auto unit:{4096U,65536U})for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<4;++cut) {
    Fixture f(4096,0,true);auto advice=ram(unit);
    require(f.session->create_budgeted_optimization_area(advice).phase==NativeOptimizationPhase::ready,
        "checkpoint fault fixture advice ownership");
    put(*f.session,"f0",family(0));put(*f.session,"f1",family(1));
    put(*f.session,"funding",Bytes(kOptimizationGroupScanInputBytes-2*kNativeExtentBytes));
    advice->fail_at=advice->calls+cut;advice->failure=mode;
    NativeGroupMaintenance controller(*f.session);finish_pending(f,controller);
    require(!f.session->poisoned() && controller.progress().published_groups==1 &&
        controller.progress().checkpoints_published==0 &&
        f.session->optimization_info().phase==NativeOptimizationPhase::terminal,
        "optional checkpoint failure fenced data or acknowledged an incomplete write");
    const auto writes=advice->writes;
    f.step(controller);require(advice->writes==writes,"failed checkpoint retried");
    const auto recovered=read_optimization_state(*image(advice->stable,unit),id(90));
    require(recovered.code==OptimizationReadCode::available && (recovered.sequence==1 || recovered.sequence==2) &&
        (recovered.sequence==1?!recovered.state.group_scan:
         recovered.state.group_scan && recovered.state.group_scan->complete),
        "power cut exposed a partial scan checkpoint");
    f.verify(2);++faults;
  }
}
}
int main(int argc,char** argv){try{
  if(argc==2 && std::string_view(argv[1])=="--indexed-cold-copy") {
    indexed_cold_batch_copy();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-resume") {
    persistent_indexed_scan();
    retain_distant_indexed_candidate();
    retain_distant_indexed_candidate(true);
    indexed_scan_checkpoint_faults();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-displaced-coherence") {
    indexed_catalog_metadata_space_refusal();
    indexed_catalog_displaced_candidate();
    indexed_catalog_alternate_publication();
    compact_full_indexed_catalog(CoherenceCopyOutcome::completed,0,true);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-coherence-capacity") {
    indexed_catalog_metadata_space_refusal();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-coherence-ranges") {
    indexed_catalog_sparse_range_reads();
  } else if(argc==2 && std::string_view(argv[1])=="--profile-coherence-ingestion") {
    indexed_catalog_sparse_range_reads(16384,{256*1024*1024});
  } else if(argc==2 && std::string_view(argv[1])=="--profile-coherence-ingestion-legacy") {
    indexed_catalog_sparse_range_reads(16384,{256*1024*1024},true);
  } else if(argc==2 && std::string_view(argv[1])=="--profile-coherence-reclamation") {
    indexed_catalog_reclamation_at_scale();
  } else if(argc==3 && std::string_view(argv[1])=="--profile-coherence-reclamation-cut") {
    indexed_catalog_reclamation_at_scale(std::stoul(argv[2]));
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-secondary-coherence") {
    const auto file=ReadOnlyFile::open(argv[2]);
    require(file.size()==kNativeExtentBytes,"public secondary-coherence fixture must be one extent");
    Bytes sample(kNativeExtentBytes);
    file.read_exact_at(0,sample.data(),sample.size());
    indexed_catalog_shift_candidate(true,std::move(sample));
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-projected-coherence") {
    indexed_catalog_shift_candidate();
    indexed_catalog_shift_candidate(true);
    for(const auto scenario:{ModularCatalogCase::available,ModularCatalogCase::masked_only,ModularCatalogCase::stale})
      indexed_modular_catalog_discovery(scenario);
    for(const auto scenario:{CatalogCase::available,CatalogCase::aligned_only,CatalogCase::unavailable,CatalogCase::stale})
      indexed_catalog_discovery(scenario,true);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-coherence") {
    for(const auto scenario:{CatalogCase::available,CatalogCase::unavailable,CatalogCase::stale})
      indexed_catalog_discovery(scenario);
    for(const auto outcome:{CatalogFunding::available,CatalogFunding::checkpoint_only,
        CatalogFunding::journal_failure,CatalogFunding::catalog_failure}) {
      funded_indexed_catalog(outcome);
      automatic_indexed_catalog(outcome);
    }
    automatic_indexed_catalog(CatalogFunding::earlier_keys_only);
    const auto writes_and_barriers=compact_full_indexed_catalog(CoherenceCopyOutcome::completed);
    require(writes_and_barriers>1,"completed catalog copy never reached the final barrier");
    compact_full_indexed_catalog(CoherenceCopyOutcome::charge_cut);
    compact_full_indexed_catalog(CoherenceCopyOutcome::target_cut);
    compact_full_indexed_catalog(CoherenceCopyOutcome::target_cut,writes_and_barriers-1);
    for(const auto selection:{CatalogSelectionCase::selected,CatalogSelectionCase::before_selection,
        CatalogSelectionCase::after_first_barrier,CatalogSelectionCase::after_selection})
      compact_full_indexed_catalog(CoherenceCopyOutcome::completed,0,false,selection);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-xor") {
    xor_indexed_family(true);
    xor_indexed_family(true,true);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_tile_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_sparse_reference);
    xor_reference_selection(anchor_search_fixture::IndexedBatchPattern::xor_full_reference);
    xor_reference_decline_fallback();
    indexed_group_with_outlier();
  } else if(argc==2 && std::string_view(argv[1])=="--profile-projected-xor") {
    xor_indexed_family(false,true);
  } else if(argc==2 && std::string_view(argv[1])=="--profile-group-xor") {
    xor_indexed_family(false);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-modular-delta") {
    modular_delta_indexed_family(data(kNativeExtentBytes,20561),{StoreCompressionMode::raw,1,false},true);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-modular-delta") {
    auto input=ReadOnlyFile::open(argv[2]);
    require(input.size()==kNativeExtentBytes,"expected one extent of public source data");
    Bytes bytes(kNativeExtentBytes);
    input.read_exact_at(0,bytes.data(),bytes.size());
    modular_delta_indexed_family(bytes,{StoreCompressionMode::automatic,6,true},false);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-advice") {
    advised_indexed_maintenance();
    collective_dense_advice();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-yield-order") {
    indexed_candidate_yield_order();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-section-reuse") {
    indexed_section_reference_reuse();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-prefix") {
    indexed_prefixed_reference();
    indexed_prefix_extension(true);
    indexed_prefix_bank_boundary();
  } else if((argc==2 || argc==3) && std::string_view(argv[1])=="--profile-indexed-prefix-extension") {
    indexed_prefix_extension(false,argc==3?argv[2]:nullptr);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batched-prefix") {
    indexed_batched_prefix();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-packed-short-extents") {
    indexed_packed_short_extents();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-pack-distinct-short-extents") {
    indexed_pack_distinct_short_extents();
    indexed_uneven_short_packs(true);
    if(store_zstd_available()) {
      profile_cross_window_short_families(true);
      profile_retained_short_peer();
    }
  } else if(argc==2 && std::string_view(argv[1])=="--profile-retained-short-peer") {
    profile_retained_short_peer();
  } else if(argc==2 && std::string_view(argv[1])=="--profile-shuffled-short-packs") {
    profile_shuffled_short_packs();
  } else if(argc==2 && std::string_view(argv[1])=="--profile-cross-window-short-families") {
    profile_cross_window_short_families();
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-short-packs") {
    profile_public_packs(argv[2],false);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-files") {
    profile_public_packs(argv[2],true);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-reencode") {
    profile_public_packs(argv[2],true,true);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-indexed-tail-oracle") {
    profile_indexed_pair_oracle(argv[2],true);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-indexed-pair-oracle") {
    profile_indexed_pair_oracle(argv[2]);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-compact-checkpoints") {
    profile_public_packs(argv[2],true,true,NativeIndexedDataLayout::extents,
        NativeVolumeHeadLayout::compact,PagedStoreBankLayout::compact);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-compact-selector") {
    profile_public_packs(argv[2],true,true,NativeIndexedDataLayout::extents,NativeVolumeHeadLayout::compact);
  } else if(argc==3 && std::string_view(argv[1])=="--profile-public-inline") {
    profile_public_packs(argv[2],true,true,NativeIndexedDataLayout::inline_small_files);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-cross-window-pack") {
    indexed_cross_window_pack(false);
    indexed_cross_window_pack(true);
  } else if(argc==2 && std::string_view(argv[1])=="--profile-uneven-short-packs") {
    indexed_uneven_short_packs(false);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-changes") {
    indexed_changed_extent_discovery();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batch") {
    batched_indexed_maintenance(BatchOutcome::accepted);
    batched_indexed_maintenance(BatchOutcome::cpu_fallback);
    batched_indexed_maintenance(BatchOutcome::changed_source);
    batched_indexed_maintenance(BatchOutcome::memory_pressure);
    batched_indexed_maintenance(BatchOutcome::overlap_success);
    batched_indexed_maintenance(BatchOutcome::overlap_fallback);
    batched_indexed_maintenance(BatchOutcome::full_overlap_success);
    batched_indexed_maintenance(BatchOutcome::full_overlap_fallback);
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-multiwindow-batch") {
    indexed_batch_across_scan_windows();
    indexed_partial_overlap_preserves_candidates();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed-batch-capacity") {
    indexed_batch_reclaims_append_space();
  } else if(argc==2 && std::string_view(argv[1])=="--indexed") {
    for(auto unit:{4096U,65536U})automatic_and_extension(unit,true);
    indexed_scan_windows();indexed_stale_binding_and_padding();
    indexed_relocation_reuses_samples();
    shifted_indexed_family(data(kNativeExtentBytes,714),{StoreCompressionMode::raw,1,false},false);
  } else if(argc==3 && (std::string_view(argv[1])=="--shifted-corpus" ||
      std::string_view(argv[1])=="--shifted-sections-corpus")) {
    auto input=ReadOnlyFile::open(argv[2]);
    require(input.size()==kNativeExtentBytes,"expected one extent of public source data");
    Bytes bytes(kNativeExtentBytes);
    input.read_exact_at(0,bytes.data(),bytes.size());
    const bool sections=std::string_view(argv[1])=="--shifted-sections-corpus";
    shifted_indexed_family(bytes,sections?StoreCompressionOptions{StoreCompressionMode::raw,1,false}:
        StoreCompressionOptions{StoreCompressionMode::automatic,6,true},true,sections);
  } else if(argc==1) {
    for(auto unit:{4096U,65536U}){automatic_and_extension(unit);publication_faults(unit);}
    rejected_observation_drops_duty_delay();
    admission();continued_preparation_admission();negative_cache_and_incompressible();ownership_and_pressure();
    foreground_change_during_preparation();
  } else throw std::invalid_argument("unknown group maintenance test mode");
  std::cout<<"PASS: "<<checks<<" automatic group maintenance checks; "<<faults<<" injected publication cases; model resource observations; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
