#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "record_digest.hpp"
#include <iostream>
#include <set>
namespace {
using namespace flitzfs;using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
constexpr std::size_t indexed_extent_header_bytes=24;
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);}
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,unsigned count=8) {
  if(at>bytes.size() || count>bytes.size()-at)throw std::runtime_error("fixture framing");
  std::uint64_t value=0;for(unsigned i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(i*8);return value;
}
void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned count=8){for(unsigned i=0;i<count;++i)bytes[at+i]=static_cast<std::byte>(value>>(i*8));}
StoreIdentity next_id(){auto id=identity();id[7]=std::byte{98};return id;}
struct Location{std::uint64_t id=0,primary=0,peer=0,stored=0;};
std::vector<Location> locations(const Ram& ram) {
  require(get(ram.live,8,4)==2,"missing mirrored root capability");std::vector<Location> result;
  auto visit=[&](auto&& self,std::uint64_t page,unsigned depth)->void {
    require(depth<=16,"fixture tree depth");const auto count=get(ram.live,page+40,4),height=get(ram.live,page+12,4);
    for(std::uint64_t i=0;i<count;++i) {
      if(height)self(self,get(ram.live,page+64+i*112+40),depth+1);
      else {const auto at=page+64+i*72;result.push_back({get(ram.live,at),get(ram.live,at+16),get(ram.live,at+64),get(ram.live,at+28,4)});}
    }
  };
  visit(visit,get(ram.live,104),0);return result;
}
void lose_unit(Ram& ram,std::uint64_t unit) {
  const auto at=unit*ram.failure_unit;require(at<=ram.live.size() && ram.failure_unit<=ram.live.size()-at,"fixture unit bounds");
  std::fill_n(ram.live.begin()+static_cast<std::ptrdiff_t>(at),ram.failure_unit,std::byte{0});ram.stable=ram.live;
}
void damage_record(Ram& ram,std::uint64_t offset) {
  require(offset<ram.live.size() && offset<ram.stable.size(),"fixture record bounds");
  ram.live[offset]^=std::byte{1};
  ram.stable[offset]=ram.live[offset];
}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,unsigned layout) {
  auto objects=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  if(layout==1)return NativeFilesystemWriter::initialize_inode_records(std::move(objects));
  if(layout==2)return NativeFilesystemWriter::initialize_indexed_names(std::move(objects));
  return NativeFilesystemWriter::initialize(std::move(objects));
}
NativeFilesystemSnapshot view(const std::shared_ptr<Ram>& ram){return NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,limits));}
void packing_and_boundaries(std::uint32_t unit) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  std::vector<StoreMutation> changes;
  for(unsigned i=1;i<=96;++i)changes.push_back({i,false,data(i==1?kStoreMaxObjectBytes:(i==2?unit-257:i),i)});
  writer.commit(changes);auto snapshot=writer.snapshot();snapshot.verify_all();
  std::uint64_t stored=0;for(const auto& change:changes){require(snapshot.read(change.id)==change.bytes,"striped record boundary mismatch");stored+=change.bytes.size();}
  require(snapshot.info().mirrored_payloads && !snapshot.info().degraded_payloads && snapshot.info().stored_payload_bytes==2*stored,
      "mirror policy/accounting differs");
  const auto entries=locations(*ram);require(entries.size()==96,"mirrored leaf split inventory");
  require(entries[2].id==3 && entries[2].primary%unit==unit-1,"header-straddling boundary fixture missing");
  for(const auto& entry:entries)require(entry.peer==entry.primary+unit && (entry.primary/unit)%2==0,"payload units not independently paired");
  require(ram->extent()<2*stored+2*kStoreCommitUnit+16ULL*unit,"record packing retained per-object padding");
  std::cout<<"mirror_packing unit="<<unit<<" objects=96 encoded_bytes_one_copy="<<stored<<" image_bytes="<<ram->extent()<<'\n';
}
void survivor_and_rebuild(std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;const auto bytes=data(kNativeExtentBytes,42);
  {auto writer=PagedObjectStoreWriter::create_mirrored(original,identity(),limits,{StoreCompressionMode::raw,1,false});
    const StoreMutation change{7,false,bytes};writer.commit({&change,1});}
  const auto entry=locations(*original).front();
  const auto chunks=(entry.primary%unit+128+entry.stored+unit-1)/unit;
  for(bool peer:{false,true})for(std::uint64_t chunk=0;chunk<chunks;++chunk) {
    auto ram=image(original->stable,unit);lose_unit(*ram,(peer?entry.peer:entry.primary)/unit+2*chunk);
    const auto before=ram->live;auto source=PagedObjectStoreSnapshot::open(ram,limits);
    require(source.read(7)==bytes,"one payload unit loss lost bytes");source.verify_all();
    require(source.info().degraded_payloads,"payload loss not reported by scrub");
    rejects([&]{PagedObjectStoreWriter::resume(ram,identity(),source.info().generation,limits);},"degraded payload writable resume accepted");
    require(ram->live==before && ram->calls==0,"read/scrub/refused resume wrote source");
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto repaired=PagedObjectStoreWriter::rebuild(source,target,next_id(),limits,{StoreCompressionMode::raw,1,false});
    auto healed=repaired.snapshot();healed.verify_all();
    require(healed.read(7)==bytes && healed.info().mirrored_payloads && !healed.info().degraded_payloads,"new-image repair failed");
    require(ram->live==before && ram->calls==0,"repair modified source");
  }
  auto both=image(original->stable,unit);lose_unit(*both,entry.primary/unit);lose_unit(*both,entry.peer/unit);
  auto failed=PagedObjectStoreSnapshot::open(both,limits);
  rejects([&]{failed.read(7);},"both lost copies read as good data");rejects([&]{failed.verify_all();},"both lost copies scrubbed as healthy");
  auto empty=std::make_shared<Ram>();empty->failure_unit=unit;
  rejects([&]{PagedObjectStoreWriter::rebuild(failed,empty,next_id(),limits);},"unrecoverable source repaired with fabricated data");
  std::cout<<"mirror_survivor unit="<<unit<<" single_unit_losses="<<2*chunks<<'\n';
}
void append_only_payload_repair(std::uint32_t unit) {
  using Progress=PagedPayloadRepairProgress;
  auto bytes=data(kNativeExtentBytes,309);
  bytes.back()=std::byte{1};
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  std::uint64_t current_extent=0;
  const auto run=[&](std::optional<std::size_t> cut,unsigned mode,bool tight=false) {
    auto head=std::make_shared<Ram>(),ram=std::make_shared<Ram>();
    head->failure_unit=ram->failure_unit=unit;
    const auto capacity=tight?PagedStoreLimits{current_extent}:limits;
    auto session=NativeFileSession::create_indexed_volume(head,next_id(),ram,identity(),capacity,raw);
    session.create_entry("payload");
    const auto handle=session.open("payload",{true,true,true});
    session.write(handle,0,bytes);
    current_extent=ram->extent();
    const auto sequence=session.snapshot().sequence();
    const auto records=locations(*ram);
    const auto found=std::ranges::find(records,bytes.size()+indexed_extent_header_bytes,&Location::stored);
    require(found!=records.end(),"payload repair fixture lost its raw record");
    const auto version=PagedObjectStoreSnapshot::open(ram,limits).stat(found->id);
    const auto retained=session.snapshot();
    damage_record(*ram,found->primary);
    const auto damaged=ram->live;
    const auto calls=ram->calls,head_calls=head->calls;
    require(retained.read("payload",0,kNativeExtentBytes)==bytes && !session.indexed_payload_repair_pending(),
        "independently pinned reader scheduled a repair");
    require(session.read(handle,0,kNativeExtentBytes)==bytes && session.indexed_payload_repair_pending() &&
        ram->calls==calls,"verified indexed read did not queue repair without I/O mutation");
    if(tight) {
      require(session.repair_indexed_payload()==Progress::capacity &&
          !session.indexed_payload_repair_pending() && ram->calls==calls &&
          session.read(handle,0,kNativeExtentBytes)==bytes &&
          session.repair_indexed_payload()==Progress::idle && ram->calls==calls,
          "capacity refusal did not retain the readable copy or suppress a repeated attempt");
      return std::size_t{0};
    }
    if(cut){ram->failure=mode;ram->fail_at=calls+*cut;}
    if(cut) {
      rejects([&]{(void)session.repair_indexed_payload();},"interrupted repair reported success");
      require(session.poisoned(),"uncertain repair did not fence the writer");
      ++faults;
    }else {
      require(session.repair_indexed_payload()==Progress::repaired,"localized mirror repair did not publish");
      require(!session.indexed_payload_repair_pending(),"isolated repair left pending records");
      require(session.snapshot().sequence()==sequence && session.volume_info().selection.epoch==1 &&
          session.read(handle,0,kNativeExtentBytes)==bytes &&
          retained.read("payload",0,kNativeExtentBytes)==bytes,"repair changed namespace state or pinned bytes");
      const auto now=PagedObjectStoreSnapshot::open(ram,limits).stat(found->id);
      require(now && version && now->generation==version->generation && now->logical_bytes==version->logical_bytes,
          "repair changed the logical object version");
      const auto healthy=PagedObjectStoreSnapshot::open(ram,limits);
      healthy.verify_all();
      require(!healthy.info().degraded_payloads,"repair did not restore both copies");
    }
    require(head->calls==head_calls &&
        std::equal(damaged.begin()+2*kStoreCommitUnit,damaged.end(),ram->live.begin()+2*kStoreCommitUnit),
        "repair wrote a pre-existing payload or metadata bank");
    auto durable=image(ram->stable,unit);
    const auto recovered=NativeVolumeHeadView::open(head,next_id(),[&](const auto&){return durable;});
    require(recovered.filesystem().read("payload",0,kNativeExtentBytes)==bytes &&
        recovered.filesystem().sequence()==sequence,"repair interruption lost acknowledged bytes");
    return ram->calls-calls;
  };
  const auto cuts=run({},0);
  require(cuts>3 && cuts<80,"payload repair exceeded an isolated transaction");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut)(void)run(cut,mode);
  (void)run({},0,true);
  std::cout<<"append_only_payload_repair unit="<<unit<<" operation_boundaries="<<cuts<<" modes=3\n";
}

void payload_repair_rechecks() {
  using Progress=PagedPayloadRepairProgress;
  auto head=std::make_shared<Ram>(),ram=std::make_shared<Ram>();
  head->failure_unit=ram->failure_unit=4096;
  auto session=NativeFileSession::create_indexed_volume(head,next_id(),ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  session.create_entry("payload");
  const auto handle=session.open("payload",{true,true,true});
  auto bytes=data(kNativeExtentBytes,528);
  bytes.back()=std::byte{1};
  session.write(handle,0,bytes);
  const auto record=[&] {
    const auto records=locations(*ram);
    const auto found=std::ranges::find(records,bytes.size()+indexed_extent_header_bytes,&Location::stored);
    require(found!=records.end(),"missing current payload in repair fixture");
    return *found;
  };
  const auto observe=[&](const Location& entry) {
    damage_record(*ram,entry.primary);
    require(session.read(handle,0,kNativeExtentBytes)==bytes && session.indexed_payload_repair_pending(),
        "repair observation lost verified data");
  };

  const auto stable=ram->stable;
  observe(record());
  ram->live=ram->stable=stable;
  auto calls=ram->calls;
  require(session.repair_indexed_payload()==Progress::checked && ram->calls==calls,
      "healthy recheck caused unnecessary writes");

  observe(record());
  auto updated=bytes;updated[1]^=std::byte{1};
  bool retry=false;
  try{session.write(handle,0,updated);}
  catch(const NativeFsError& error){retry=error.code()==NativeFsErrorCode::replica_repair_pending;}
  require(retry && !session.poisoned() && session.indexed_payload_repair_pending() && ram->calls==calls,
      "recoverable single-copy failure fenced a valid writer or mutated storage");
  ram->live=ram->stable=stable;
  bytes=std::move(updated);
  session.write(handle,0,bytes);
  calls=ram->calls;
  require(session.repair_indexed_payload()==Progress::checked && ram->calls==calls &&
      session.read(handle,0,kNativeExtentBytes)==bytes,"stale repair displaced a foreground write");

  const auto last=record();
  observe(last);
  damage_record(*ram,last.peer);
  calls=ram->calls;
  rejects([&]{(void)session.repair_indexed_payload();},"repair fabricated bytes without a verified copy");
  require(session.poisoned() && ram->calls==calls,"unrecoverable record caused extra writes");
}

void proactive_payload_scrub(std::uint32_t unit) {
  using Scan=PagedPayloadScrubState;
  auto head=std::make_shared<Ram>(),ram=std::make_shared<Ram>();
  head->failure_unit=ram->failure_unit=unit;
  auto session=NativeFileSession::create_indexed_volume(head,next_id(),ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  require(!session.indexed_payload_scrub_eligible(),"empty volume queued a scrub");
  const auto add_file=[&](const char* path,const Bytes& bytes) {
    session.create_entry(path);
    const auto handle=session.open(path,{true,true,true});
    session.write(handle,0,bytes);
    session.cleanup(handle,false);session.close(handle);
  };
  const auto surviving=data(3073,832),lost=data(4097,833),late=data(5191,834);
  add_file("surviving",surviving);add_file("lost",lost);
  const auto high_water=locations(*ram).back().id;
  const auto first=session.scrub_indexed_payload();
  require(first.state==Scan::checked && first.object_id,"scrub could not start its current key range");
  add_file("late",late);
  const auto entries=locations(*ram);
  const auto record=[&](std::size_t bytes) {
    const auto found=std::ranges::find(entries,bytes+indexed_extent_header_bytes,&Location::stored);
    require(found!=entries.end(),"scrub fixture lost its payload descriptor");
    return *found;
  };
  const auto degraded=record(surviving.size()),unverified=record(lost.size());
  const auto later_inode=entries.back().id;
  require(later_inode>high_water,"foreground append did not extend the key range");
  damage_record(*ram,degraded.peer);
  damage_record(*ram,unverified.primary);damage_record(*ram,unverified.peer);
  const auto head_calls=head->calls;
  auto last=first.object_id;
  bool repaired=false,reported=false,finished=false;
  for(unsigned i=0;i<64 && !finished;++i) {
    const auto calls=ram->calls;
    const auto step=session.scrub_indexed_payload();
    require(ram->calls==calls && head->calls==head_calls && !session.poisoned(),"scrub mutated or fenced live storage");
    if(step.state==Scan::complete){finished=true;continue;}
    require(step.object_id>last && step.object_id<=high_water,"scrub lost bounded keyset progress");
    last=step.object_id;
    if(step.object_id==degraded.id) {
      require(step.state==Scan::repair_queued && session.indexed_payload_repair_pending(),
          "scrub missed bad peer behind a healthy primary");
      require(session.repair_indexed_payload()==PagedPayloadRepairProgress::repaired,
          "verified scrub observation did not use the existing repair transaction");
      repaired=true;
    } else if(step.object_id==unverified.id) {
      require(step.state==Scan::unverified_payload && !session.indexed_payload_repair_pending(),
          "scrub offered to repair a record with no verified source");
      reported=true;
    } else require(step.state==Scan::checked,"scrub misclassified an intact record");
  }
  require(finished && repaired && reported && session.snapshot().read("surviving",0,8192)==surviving &&
      session.snapshot().read("late",0,8192)==late,"scrub skipped a fault or lost unrelated bytes");

  damage_record(*ram,unverified.primary);damage_record(*ram,unverified.peer);
  const auto index=get(ram->live,104);
  damage_record(*ram,index);
  const auto calls=ram->calls;
  const auto metadata=session.scrub_indexed_payload();
  require(metadata.state==Scan::unverified_index && !metadata.object_id && ram->calls==calls &&
      !session.indexed_payload_repair_pending(),"scrub trusted a degraded index or wrote through it");
  damage_record(*ram,index);
  bool checked_later=false;
  for(unsigned i=0;i<64;++i) {
    const auto step=session.scrub_indexed_payload();
    if(step.state==Scan::complete)break;
    require(step.state==Scan::checked,"restored fixture failed a fresh bounded pass");
    checked_later|=step.object_id==later_inode;
  }
  const auto cpu=view(ram);cpu.verify_all();
  require(checked_later && !cpu.storage_info().degraded_checkpoint && cpu.read("surviving",0,8192)==surviving &&
      cpu.read("lost",0,8192)==lost && cpu.read("late",0,8192)==late,"independent reader did not verify the repaired checkpoint");
  std::cout<<"proactive_payload_scrub unit="<<unit
           <<" latent_peer=1 double_loss_reported=1 bounded_high_water=1 degraded_index_refused=1\n";
}

void startup_payload_repair(std::uint32_t unit) {
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  const auto first=data(3073,737),second=data(6131,738);
  const auto make=[&](PagedStoreLimits capacity) {
    auto head=std::make_shared<Ram>(),store=std::make_shared<Ram>();
    head->failure_unit=store->failure_unit=unit;
    auto session=NativeFileSession::create_indexed_volume(head,next_id(),store,identity(),capacity,raw);
    const auto write=[&](const char* path,const Bytes& bytes) {
      session.create_entry(path);
      const auto handle=session.open(path,{true,true,true});
      session.write(handle,0,bytes);
      session.cleanup(handle,false);session.close(handle);
    };
    write("first",first);write("second",second);
    return std::pair{head,store};
  };
  const auto [base_head,base_store]=make(limits);
  const auto records=locations(*base_store);
  const auto record=[&](std::size_t bytes) {
    const auto found=std::ranges::find(records,bytes+indexed_extent_header_bytes,&Location::stored);
    require(found!=records.end(),"startup fixture lost a raw payload descriptor");
    return *found;
  };
  const auto a=record(first.size()),b=record(second.size());
  const auto prior=PagedObjectStoreSnapshot::open(base_store,limits).stat(a.id);
  const auto sequence=view(base_store).sequence();
  const auto verify_bytes=[&](const NativeFilesystemSnapshot& snapshot,bool healthy) {
    require(snapshot.sequence()==sequence && snapshot.read("first",0,8192)==first &&
        snapshot.read("second",0,8192)==second,"startup repair changed acknowledged bytes or namespace");
    if(healthy)require(!snapshot.storage_info().degraded_checkpoint && !snapshot.storage_info().degraded_root,
        "startup repair admitted degraded protection");
  };
  const auto inspect=[&](const auto& head,const auto& store) {
    return NativeVolumeHeadView::open_indexed_recovery(head,next_id(),[&](const auto&){return store;});
  };
  const auto resume=[&](const auto& head,const auto& store,std::uint64_t epoch=1) {
    return NativeFileSession::resume_closed_indexed_volume(head,next_id(),epoch,[&](const auto&){return store;},raw);
  };

  const auto trial=[&](std::optional<std::size_t> cut,unsigned mode,bool root_damage=false) {
    auto head=image(base_head->stable,unit),store=image(base_store->stable,unit);
    damage_record(*store,a.primary);damage_record(*store,b.peer);
    if(root_damage)damage_record(*store,0);
    const auto old=store->live;
    const auto pinned=inspect(head,store);
    verify_bytes(pinned.filesystem(),false);
    if(cut){store->fail_at=*cut;store->failure=mode;}
    bool opened=false;
    try {
      auto session=resume(head,store);
      opened=true;
      verify_bytes(session.snapshot(),true);
      require(session.volume_info().selection.epoch==1 && session.volume_info().selection.image_identity==identity(),
          "startup append repair changed the volume selector");
    }catch(const std::exception&){if(!cut || opened || store->calls<=*cut)throw;}
    require(opened==!cut,"startup repair reported success after a failed write or barrier");
    require(!head->writes && std::equal(old.begin()+2*kStoreCommitUnit,old.end(),store->live.begin()+2*kStoreCommitUnit),
        "startup repair overwrote a pre-existing body bank");
    verify_bytes(pinned.filesystem(),false);
    const auto durable=image(store->stable,unit);
    auto replayed=image(head->stable,unit);
    verify_bytes(inspect(replayed,durable).filesystem(),!cut);
    if(cut) {
      auto retried=resume(replayed,durable);
      verify_bytes(retried.snapshot(),true);
    }
    const auto now=PagedObjectStoreSnapshot::open(durable,limits).stat(a.id);
    require(now && prior && now->generation==prior->generation,"startup repair changed an object generation");
    return store->calls;
  };
  const auto operations=trial({},0);
  require(operations>4 && operations<180,"startup repair operation count escaped the fixture");
  (void)trial({},0,true);
  for(unsigned mode=0;mode<3;++mode)
    for(std::size_t cut=0;cut<operations;++cut)(void)trial(cut,mode);

  for(unsigned fault=0;fault<3;++fault) {
    auto head=image(base_head->stable,unit),store=image(base_store->stable,unit);
    damage_record(*store,a.primary);
    if(fault==1){damage_record(*store,b.primary);damage_record(*store,b.peer);}
    if(fault==2)damage_record(*store,get(store->live,104));
    const auto before=store->live;
    rejects([&]{(void)resume(head,store,fault==0?2:1);},"unsafe startup entered writable admission");
    require(store->live==before && !store->calls && !head->calls,"refused startup reached the write path");
  }
  auto [tight_head,tight_store]=make({base_store->extent()});
  require(tight_store->extent()==base_store->extent(),"startup capacity fixture was not full");
  damage_record(*tight_store,a.primary);
  const auto prior_writes=tight_store->writes;
  const auto full=tight_store->live;
  bool refused=false;
  try{(void)resume(tight_head,tight_store);}
  catch(const PagedStoreCapacityError&){refused=true;}
  require(refused && tight_store->writes==prior_writes && tight_store->live==full,
      "startup repair bypassed full-volume append admission");
  verify_bytes(inspect(tight_head,tight_store).filesystem(),false);
  std::cout<<"startup_payload_repair unit="<<unit<<" restored_copies=2 cuts="<<operations*3
      <<" mixed_root=1 preflight_rejections=3 full_refused=1\n";
}

void native_recovery(unsigned layout,std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  {auto writer=fresh(original,layout);writer.apply(baseline());}
  const auto records=locations(*original);std::set<std::uint64_t> units;
  for(const auto& entry:records){units.insert(entry.primary/unit);units.insert(entry.peer/unit);}
  for(const auto lost:units) {
    auto ram=image(original->stable,unit);lose_unit(*ram,lost);const auto before=ram->live;
    auto source=view(ram);expected(source,false);source.verify_all();require(source.storage_info().degraded_checkpoint,"native payload degradation hidden");
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto repaired=NativeFilesystemWriter::compact(source,target,next_id(),{StoreCompressionMode::raw,1,false});
    expected(repaired.snapshot(),false);auto independent=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(target,limits));
    independent.verify_all();require(!independent.storage_info().degraded_checkpoint,"native repair stayed degraded");
    require(ram->calls==0 && ram->live==before,"native repair wrote source");
  }
  auto ram=image(original->stable,unit);auto source=PagedObjectStoreSnapshot::open(ram,limits);
  auto writer=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),source.info().generation,limits));
  auto target=std::make_shared<Ram>();target->failure_unit=unit;
  auto candidate=writer.prepare_paged_recompression(target,next_id());
  while(!writer.advance_paged_recompression(candidate)){}
  auto rebuilt=writer.finish_paged_recompression(candidate);expected(rebuilt.snapshot(),false);
  const auto pg=PagedObjectStoreSnapshot::open(target,limits);require(pg.info().mirrored_payloads,"staged native copy dropped protection");
  // A live writer can retain CPU-readable source views while a separately
  // owned staged candidate restores a subsequently lost payload bank.
  const auto lost=locations(*ram).front().primary/unit;lose_unit(*ram,lost);
  const auto damaged=ram->live;const auto calls=ram->calls;
  auto healing=std::make_shared<Ram>();healing->failure_unit=unit;auto recovery_id=next_id();recovery_id[6]=std::byte{44};
  auto repair=writer.prepare_paged_recompression(healing,recovery_id);
  while(!writer.advance_paged_recompression(repair)){}
  auto recovered=writer.finish_paged_recompression(repair);expected(recovered.snapshot(),false);
  auto checked=PagedObjectStoreSnapshot::open(healing,limits);checked.verify_all();
  require(!checked.info().degraded_payloads && checked.info().mirrored_payloads,"live staged repair retained bad copies");
  require(ram->live==damaged && ram->calls==calls,"live staged repair modified damaged source");
  expected(writer.snapshot(),false);
  std::cout<<"mirror_native layout="<<layout<<" unit="<<unit<<" damaged_start_units="<<units.size()<<'\n';
}
void fault_matrix(unsigned layout,std::uint32_t unit) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  {auto writer=fresh(original,layout);writer.apply(baseline());}
  auto resume_native=[&](const std::shared_ptr<Ram>& ram) {
    const auto generation=PagedObjectStoreSnapshot::open(ram,limits).info().generation;
    return NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),generation,limits,{StoreCompressionMode::raw,1,false}));
  };
  auto probe=image(original->stable,unit);std::size_t cuts=0;
  {auto writer=resume_native(probe);const auto start=probe->calls;writer.apply(update());cuts=probe->calls-start;}
  require(cuts>3 && cuts<512,"mirrored transaction failure envelope");
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto ram=image(original->stable,unit);
    {auto writer=resume_native(ram);ram->failure=mode;ram->fail_at=ram->calls+cut;
      rejects([&]{writer.apply(update());},"faulted mirror commit acknowledged");require(writer.poisoned(),"indeterminate mirror writer remained active");}
    ram->live=ram->stable;ram->fail_at=(std::numeric_limits<std::size_t>::max)();
    auto recovered=view(ram);expected(recovered,recovered.sequence()==3);recovered.verify_all();
    auto continued=resume_native(ram);file_apply(continued,file_create("continue"));
    require(continued.snapshot().stat("continue").inode!=0,"mirrored recovery could not continue");++faults;
  }
  std::cout<<"mirror_faults layout="<<layout<<" unit="<<unit<<" cuts="<<cuts<<" modes=3\n";
}
void forged_descriptors() {
  auto original=std::make_shared<Ram>();original->failure_unit=4096;
  {auto writer=PagedObjectStoreWriter::create_mirrored(original,identity(),limits,{StoreCompressionMode::raw,1,false});
    const StoreMutation object{7,false,data(65536,19)};writer.commit({&object,1});}
  for(unsigned variant=0;variant<5;++variant) {
    auto ram=image(original->stable,4096);const auto page=get(ram->live,104),peer=get(ram->live,112);
    auto pointer=get(ram->live,page+64+16);const auto at=page+64+64;
    if(variant==0)put(ram->live,at,0);
    if(variant==1)put(ram->live,at,pointer);
    if(variant==2)put(ram->live,at,pointer+8192);
    if(variant==3)put(ram->live,at,ram->extent()+4096);
    if(variant==4)put(ram->live,page+64+16,pointer+4096);
    const auto hash=detail::record_digest(std::span<const std::byte>(ram->live).subspan(page,4064));
    std::copy(hash.begin(),hash.end(),ram->live.begin()+page+4064);
    std::copy_n(ram->live.begin()+page,4096,ram->live.begin()+peer);
    for(const auto root:{std::uint64_t{0},kStoreCommitUnit}) {
      std::copy(hash.begin(),hash.end(),ram->live.begin()+root+144);
      const auto digest=detail::record_digest(std::span<const std::byte>(ram->live).subspan(root,4064));
      std::copy(digest.begin(),digest.end(),ram->live.begin()+root+4064);
    }
    ram->stable=ram->live;rejects([&]{PagedObjectStoreSnapshot::open(ram,limits);},"intact invalid mirror descriptor accepted");
    require(ram->calls==0,"malformed mirror reader wrote bytes");
  }
}
void fresh_replica_readback(std::uint32_t unit) {
  for(bool header:{false,true}) {
    auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
    auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
    const auto before=writer.snapshot().info();
    ram->corrupt_read_offset=before.physical_end+(header?unit:128);ram->corrupt_read_number=1;
    const StoreMutation object{7,false,data(65536,9)};
    rejects([&]{writer.commit({&object,1});},"fresh bad replica acknowledged");
    require(writer.poisoned(),"fresh replica failure did not fence writer");
    ram->corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)();
    auto selected=PagedObjectStoreSnapshot::open(ram,limits);
    require(selected.info().generation==before.generation && selected.info().objects==0,"bad replica published a root");
  }
}
void native_shared_lifetime() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram,2);const auto bytes=data(65536,54);
  writer.apply(std::array{file_create("a"),file_write("a",0,bytes),file_create("b"),file_write("b",0,bytes)});
  writer.coalesce_equal_extents();auto retained=writer.snapshot();
  file_apply(writer,file_remove("a"));file_apply(writer,file_write("b",19,Bytes{std::byte{11}}));
  require(retained.read("a",0,65536)==bytes && retained.read("b",0,65536)==bytes,"mirrored shared snapshot damaged");
  auto expected_bytes=bytes;expected_bytes[19]=std::byte{11};
  require(writer.snapshot().read("b",0,65536)==expected_bytes,"mirrored private update differs");
  PagedObjectStoreSnapshot::open(ram,limits).verify_all();
}
}
int main(){try {
  forged_descriptors();native_shared_lifetime();payload_repair_rechecks();
  for(auto unit:{4096U,65536U}) {
    proactive_payload_scrub(unit);
    startup_payload_repair(unit);
    packing_and_boundaries(unit);survivor_and_rebuild(unit);fresh_replica_readback(unit);
    append_only_payload_repair(unit);
    for(unsigned layout=0;layout<3;++layout){native_recovery(layout,unit);fault_matrix(layout,unit);}
  }
  std::cout<<"PASS: "<<checks<<" mirrored payload checks; "<<faults<<" injected write/barrier cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
