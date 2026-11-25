#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "native_name_index.hpp"
#include "native_object_storage.hpp"
#include "record_digest.hpp"
#include "store_codec.hpp"
#include <chrono>
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
  const auto capability=get(ram.live,8,4);
  require((capability==2 || capability==3) && get(ram.live,60,4)==4096,
      "fixture requires a mirrored root with full-size index pages");
  std::vector<Location> result;
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
    if(repaired || step.object_id==degraded.id)
      require(!step.resumable,"scrub checkpoint skipped a previously unverified replica");
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

void retained_indexed_reopen_fences(bool compressed) {
  class FenceBacking final:public StoreByteIo {
   public:
    explicit FenceBacking(std::shared_ptr<Ram> backing):ram(std::move(backing)){}
    std::shared_ptr<Ram> ram;
    std::function<void()> on_fence;
    bool leased() const noexcept{return has_writer();}
    std::uint64_t extent() const override{return ram->extent();}
    StoreIoContract contract() const noexcept override{return ram->contract();}
    void admit_write(std::uint64_t length) override{ram->admit_write(length);}
    void read_exact(std::uint64_t offset,std::span<std::byte> output) const override{ram->read_exact(offset,output);}
    void write_exact(std::uint64_t offset,std::span<const std::byte> input) override{ram->write_exact(offset,input);}
    void flush() override{ram->flush();if(on_fence)on_fence();}
  };

  const StoreCompressionOptions encoding{compressed?StoreCompressionMode::automatic:StoreCompressionMode::raw,1,compressed};
  const auto head=std::make_shared<FenceBacking>(std::make_shared<Ram>());
  auto empty=std::make_shared<Ram>();
  empty->failure_unit=4096;
  const auto volume=next_id();
  {
    auto session=NativeFileSession::create_indexed_volume(head,volume,std::make_shared<FenceBacking>(empty),identity(),limits,encoding);
    session.create_entry("data");
  }
  const auto selected=NativeVolumeHeadView::inspect_selector(*head,volume);
  const auto reopen=[&](const std::shared_ptr<FenceBacking>& body) {
    return NativeFileSession::resume_closed_indexed_volume(head,volume,selected.selection.epoch,
        [&](const auto& which)->std::shared_ptr<StoreByteIo>{return which==identity()?body:nullptr;},encoding);
  };
  const auto original=compressed?Bytes(kNativeExtentBytes,std::byte{0x36}):data(kNativeExtentBytes,10211);
  const auto different=compressed?Bytes(kNativeExtentBytes,std::byte{0x71}):data(kNativeExtentBytes,10212);
  const auto produce=[&](const Bytes& content) {
    auto body=image(empty->stable,4096);
    auto session=reopen(std::make_shared<FenceBacking>(body));
    const auto file=session.open("data",{true,true,true});
    session.write(file,0,content);
    session.cleanup(file,false);session.close(file);
    return body;
  };
  const auto initial=produce(original),fork=produce(different);
  const auto snapshot=view(initial),fork_snapshot=view(fork);
  const auto before=snapshot.storage_info(),other=fork_snapshot.storage_info();
  require(snapshot.identity()==fork_snapshot.identity() && snapshot.sequence()==fork_snapshot.sequence() &&
      before.generation==other.generation && before.checkpoint_generation==other.checkpoint_generation &&
      before.physical_end==other.physical_end && initial->extent()==fork->extent() &&
      fork_snapshot.read("data",0,kNativeExtentBytes)==different,
      "post-fence fork fixture must share identity, counters and extent");
  const auto inventory=locations(*initial);
  const auto payload=std::ranges::find(inventory,detail::kNativeIndexedDataObject+1,&Location::id);
  const auto name_root=PagedObjectStoreSnapshot::open(initial,limits).read(detail::kNativeNameIndexObject,40);
  const auto name_page=std::ranges::find(inventory,get(name_root,16),&Location::id);
  require(payload!=inventory.end() && name_page!=inventory.end(),"fence test payload or name record missing");
  const auto codec=get(initial->live,payload->primary+12,4);
  require(codec==(compressed?detail::kStoreZstd:detail::kStoreRaw),"reopen fence fixture used the wrong codec");
  const unsigned damage_offset=compressed?128:0;
  require(payload->primary%initial->failure_unit+damage_offset<initial->failure_unit,
      "fence fixture damage does not address the record stripe");
  enum class FenceFault {peer, both, name_peer, forked_root, appended_tail};
  for(bool image_fence:{true,false})for(const auto fault:{FenceFault::peer,FenceFault::both,FenceFault::name_peer,
      FenceFault::forked_root,FenceFault::appended_tail}) {
    auto body=image(initial->stable,4096);
    auto observed=std::make_shared<FenceBacking>(body);
    const auto head_writes=head->ram->writes;
    auto& damaged_fence=image_fence?*observed:*head;
    unsigned barriers=0;
    damaged_fence.on_fence=[&] {
      require(head->leased() && observed->leased(),"checked reopen lost an exclusive lease during a fence");
      ++barriers;
      if(fault==FenceFault::forked_root) {
        body->live=body->stable=fork->stable;
      }else if(fault==FenceFault::appended_tail) {
        body->stable.resize(body->stable.size()+body->failure_unit);
        body->live=body->stable;
      }else if(fault==FenceFault::name_peer) {
        damage_record(*body,name_page->peer);
      }else {
        damage_record(*body,payload->peer+damage_offset);
        if(fault==FenceFault::both)damage_record(*body,payload->primary+damage_offset);
      }
    };
    rejects([&]{(void)reopen(observed);},"writable reopen accepted damage or a changed image at a fence");
    damaged_fence.on_fence={};
    require(barriers==1 && body->flushes==1 && !observed->leased() && !head->leased() && !body->writes &&
        head->ram->writes==head_writes && NativeVolumeHeadView::inspect_selector(*head,volume).selection==selected.selection,
        "failed fenced reopen changed authoritative storage or retained a lease");
    if(fault==FenceFault::peer || fault==FenceFault::name_peer) {
      {
        auto session=reopen(observed);
        require(session.snapshot().read("data",0,kNativeExtentBytes)==original,"qualified retry lost the surviving payload");
      }
      auto healed=view(body);healed.verify_all();
      require(!healed.storage_info().degraded_checkpoint,"qualified retry did not restore both copies");
    }else if(fault==FenceFault::appended_tail) {
      require(view(body).read("data",0,kNativeExtentBytes)==original,
          "an unexpected extent change lost the selected CPU-readable data");
    }
  }
  {
    auto body=image(initial->stable,4096);
    auto observed=std::make_shared<FenceBacking>(body);
    const auto head_before=head->ram->flushes;
    observed->on_fence=[&] {
      require(head->leased() && observed->leased(),"clean image fence did not hold both owners");
    };
    head->on_fence=[&] {
      require(body->flushes==1 && head->leased() && observed->leased(),"selector fenced before the single image barrier");
    };
    auto session=reopen(observed);
    observed->on_fence={};head->on_fence={};
    require(body->flushes==1 && head->ram->flushes==head_before+1,
        "healthy reopen repeated a storage barrier");
    require(head->leased() && observed->leased(),"healthy reopen did not retain its checked owners");
    const auto file=session.open("data",{true,true,true});
    session.write(file,0,different);
    session.cleanup(file,false);session.close(file);
    auto checked=view(body);checked.verify_all();
    require(checked.read("data",0,kNativeExtentBytes)==different,"retained owner could not commit through reopened storage");
  }
  std::cout<<"retained_indexed_fences image_and_selector=1 peer_loss=1 name_index_peer_loss=1 double_loss=1 same_generation_fork=1"
      " changed_extent=1 independent_repair_and_continued_write=1 stored_codec="<<codec<<'\n';
}

void durable_payload_scrub() {
  using Scan=PagedPayloadScrubState;
  const StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  constexpr std::uint64_t start=133'000'000'000'000'000ULL;
  constexpr std::uint64_t required_read=64*kOptimizationAreaBytes;
  constexpr auto volume_capacity=64ULL*1024*1024;
  constexpr std::size_t chunk=1024*1024;
  auto head=std::make_shared<Ram>(),ram=std::make_shared<Ram>(),advice=std::make_shared<Ram>();
  head->failure_unit=ram->failure_unit=4096;
  auto expected=data(5*chunk,891);
  OptimizationSnapshot old;
  {
    auto session=NativeFileSession::create_indexed_volume(head,next_id(),ram,identity(),{volume_capacity},raw);
    session.create_budgeted_optimization_area(advice,2);
    session.create_entry("payload");
    const auto file=session.open("payload",{true,true,true});
    for(std::size_t at=0;at<expected.size();at+=chunk)
      session.write(file,at,std::span<const std::byte>(expected).subspan(at,chunk));
    session.cleanup(file,false);session.close(file);
    auto state=session.optimization_snapshot().state;
    for(unsigned i=1;i<=kOptimizationMaxProfiles;++i)state.profiles.push_back({i,64,32,i,0,1,1,1000000});
    for(unsigned i=1;i<=kOptimizationMaxBenchmarks;++i)state.benchmarks.push_back({i,i,1,64,32,1});
    auto& scan=state.group_scan.emplace();
    scan.image=identity();scan.namespace_sequence=1;scan.inode=2;scan.xor_sampling=true;
    for(std::size_t i=0;i<kOptimizationGroupRepresentatives;++i) {
      OptimizationGroupFeature row;
      row.inode=2;row.extent_index=i;row.object=(1ULL<<61)+i+1;row.revision=1;
      scan.representatives.push_back(row);
    }
    scan.projections={{0,{1,2,3,4}},{1,{5,6,7,8}},{2,{9,10,11,12}}};
    require(session.publish_budgeted_optimization_state(state).progress==NativeOptimizationBudgetProgress::published,
        "legacy optimization fixture with full group and profile tables could not be published");
    old=session.optimization_snapshot();
    require(get(advice->stable,8,4)==5 && !old.state.payload_scrub,"legacy advice fixture was not capability 5");
  }
  const auto legacy=advice->stable;
  const auto open=[&](const std::shared_ptr<Ram>& area) {
    auto session=NativeFileSession::resume_closed_indexed_volume(head,next_id(),1,
        [&](const auto& which)->std::shared_ptr<StoreByteIo>{return which==identity()?ram:nullptr;},raw);
    const auto loaded=read_optimization_state(*area,next_id());
    if(loaded.code==OptimizationReadCode::available)session.resume_budgeted_optimization_area(area,loaded.sequence);
    return session;
  };
  const auto verify_data=[&] {
    auto verified=NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,{volume_capacity}));
    verified.verify_all();
    for(std::size_t at=0;at<expected.size();at+=chunk) {
      const auto actual=verified.read("payload",at,chunk);
      require(actual.size()==chunk &&
          std::equal(actual.begin(),actual.end(),expected.begin()+static_cast<std::ptrdiff_t>(at)),
          "advisory checkpoint changed independently decoded data");
    }
  };
  const auto run_until_checkpoint=[&](NativeFileSession& session,Ram& area,bool permit) {
    const auto before=area.calls;
    const auto writes=ram->writes;
    std::uint64_t scanned=0;
    for(unsigned step=0;step<300;++step) {
      const auto result=session.scrub_indexed_payload(start);
      require(result.state==Scan::checked && result.resumable,"clean regression scrub ended before its funded checkpoint");
      scanned+=result.verified_record_bytes;
      const auto published=session.checkpoint_indexed_payload_scrub();
      if(published || area.calls!=before) {
        require(scanned>=required_read && published==permit && ram->writes==writes,
            "advisory scrub used premature credit, changed authority or acknowledged a failed write");
        return *result.resumable;
      }
    }
    throw std::runtime_error("scrub checkpoint never performed its admitted publication");
  };

  OptimizationSnapshot saved;
  {
    auto session=open(advice);
    const auto before=advice->write_bytes;
    const auto position=run_until_checkpoint(session,*advice,true);
    require(!session.checkpoint_indexed_payload_scrub(),"unchanged checkpoint repeated a write");
    saved=session.optimization_snapshot();
    auto combined=old.state;
    combined.payload_scrub=OptimizationPayloadScrub{position,start};
    require(advice->write_bytes-before==kOptimizationAreaBytes && get(advice->stable,8,4)==6 &&
        saved.sequence==old.sequence+1 && saved.state==combined,
        "checkpoint changed funded credit, profile history or maximal collective scan tables");
  }
  const auto after=advice->stable;
  const auto records=locations(*ram);
  const auto first=records.front().id;
  const auto position=saved.state.payload_scrub->position;
  const auto next=[&](std::uint64_t id) {
    const auto current=locations(*ram);
    const auto entry=std::ranges::find_if(current,[&](const auto& record){return record.id>id;});
    require(entry!=current.end(),"fixture requires another record past the checkpoint");
    return entry->id;
  };
  const auto count_pass=[&](const std::shared_ptr<Ram>& area) {
    auto session=open(area);
    std::uint64_t objects=0,bytes=0;
    for(unsigned step=0;step<300;++step) {
      const auto result=session.scrub_indexed_payload(start+1);
      if(result.state==Scan::complete)return std::pair{objects,bytes};
      require(result.state==Scan::checked && result.object_id,"comparison scrub did not check a complete record");
      ++objects;bytes+=result.verified_record_bytes;
    }
    throw std::runtime_error("comparison scrub failed to terminate");
  };
  const auto cold=count_pass(image(legacy));
  const auto resumed=count_pass(advice);
  require(cold.first==records.size() && resumed.first<cold.first &&
      resumed.second<cold.second && cold.second-resumed.second>=required_read,
      "durable resume did not avoid the previously verified scan work");

  for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<4;++cut) {
    auto interrupted=image(legacy);
    {
      auto session=open(interrupted);
      interrupted->failure=mode;interrupted->fail_at=interrupted->calls+cut;
      require(run_until_checkpoint(session,*interrupted,false)==position &&
          session.optimization_info().phase==NativeOptimizationPhase::terminal && !session.poisoned(),
          "failed advisory owner did not remain isolated from data ownership");
    }
    auto durable=image(interrupted->stable);
    const auto loaded=read_optimization_state(*durable,next_id());
    require(loaded.code==OptimizationReadCode::available &&
        ((loaded.sequence==old.sequence && loaded.state==old.state) ||
         (loaded.sequence==saved.sequence && loaded.state==saved.state)),
        "interruption selected an invalid partial checkpoint or changed the settled input");
    auto session=open(durable);
    const auto check=session.scrub_indexed_payload(start+1);
    require(check.state==Scan::checked && check.object_id==(loaded.state.payload_scrub?next(position.after):first),
        "restart did not resume the exact selected verified progress");
    require(session.snapshot().read("payload",expected.size()-4096,4096)==
        Bytes(expected.end()-4096,expected.end()),"advisory interruption changed data");
  }
  verify_data();

  auto degraded=image(legacy);
  damage_record(*degraded,0);
  {
    auto session=open(degraded);
    require(session.optimization_info().phase==NativeOptimizationPhase::unavailable,
        "damaged funded state exposed policy before writes were earned");
    run_until_checkpoint(session,*degraded,true);
    const auto repaired=session.optimization_snapshot();
    require(!repaired.degraded && repaired.state.settled_input_bytes==session.workload_info().committed_input_bytes &&
        repaired.state.group_scan==old.state.group_scan,"read-funded repair reused forfeited input or changed collective state");
  }
  for(const auto reopen_utc:std::array<std::uint64_t,2>{start-1,start+28ULL*24*60*60*10'000'000}) {
    auto session=open(advice);
    require(session.scrub_indexed_payload(reopen_utc).object_id==first,
        "clock rollback or expired checkpoint skipped early records");
  }
  // The current wire type fits the old tables and has a fixed cursor offset.
  constexpr std::size_t scrub_at=64+32*56;
  const auto resign=[](Ram& copy,unsigned slot) {
    auto bytes=std::span(copy.live).subspan(slot*kStoreCommitUnit,kStoreCommitUnit);
    const auto digest=detail::record_digest(bytes.first(kStoreCommitUnit-32));
    std::copy(digest.begin(),digest.end(),bytes.end()-32);
    copy.stable=copy.live;
  };
  for(bool wrong_image:{false,true}) {
    auto unrelated=image(after);
    for(unsigned slot=0;slot<2;++slot) {
      const auto at=slot*kStoreCommitUnit+scrub_at;
      if(wrong_image)unrelated->live.at(at)^=std::byte{1};
      else put(unrelated->live,at+16,position.generation+1);
      resign(*unrelated,slot);
    }
    auto session=open(unrelated);
    require(session.scrub_indexed_payload(start+1).object_id==first,
        "unrelated or future image progress skipped unverified records");
  }
  {
    auto malformed=image(after);
    for(unsigned slot=0;slot<2;++slot) {
      put(malformed->live,slot*kStoreCommitUnit+scrub_at+32,position.after);
      resign(*malformed,slot);
    }
    require(read_optimization_state(*malformed,next_id()).code==OptimizationReadCode::invalid,
        "malformed cursor escaped wire bounds");
    auto session=open(malformed);
    require(session.scrub_indexed_payload(start+1).object_id==first,
        "malformed checkpoint interfered with normal verification");
  }
  {
    auto uncharged=image(after);
    std::copy_n(legacy.begin()+kStoreCommitUnit,kStoreCommitUnit,uncharged->live.begin()+kStoreCommitUnit);
    put(uncharged->live,64+32,2);
    resign(*uncharged,0);
    require(read_optimization_state(*uncharged,next_id()).code==OptimizationReadCode::invalid,
        "cursor transition accepted an unpaid profile change");
  }
  {
    auto session=open(advice);
    const auto file=session.open("payload",{true,true,true});
    expected[0]^=std::byte{1};
    session.write(file,0,std::span<const std::byte>(expected).first(1));
    session.cleanup(file,false);session.close(file);
  }
  {
    auto session=open(advice);
    const auto continued=session.scrub_indexed_payload(start+2);
    require(continued.object_id==next(position.after) && continued.resumable &&
        continued.resumable->generation>position.generation,
        "foreground commit invalidated checked progress in the same image");
  }
  verify_data();
  std::cout<<"durable_payload_scrub advisory_cuts=12 read_earned=1 legacy_max_tables=1 repaired_peer=1"
      " resume_after_commit=1 wrong_image_age_and_future_refused=1 no_uncharged_policy_change=1 malformed_refused=1"
      <<" skipped_records="<<cold.first-resumed.first<<" skipped_encoded_verification_bytes="
      <<cold.second-resumed.second<<'\n';
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

void compressed_replica_verification(bool profile) {
  const bool zstd=store_zstd_available();
  require(!profile || zstd,"compressed audit profile requires the Zstandard build");
  const auto pattern=data(zstd?8192U:8U,932);
  Bytes bytes(192*1024);
  for(std::size_t offset=0;offset<bytes.size();offset+=pattern.size())
    std::copy(pattern.begin(),pattern.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset));
  constexpr unsigned batch=16;
  const unsigned objects=profile?96:1;
  auto ram=std::make_shared<Ram>();
  ram->failure_unit=4096;
  {
    auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,
        {StoreCompressionMode::automatic,3,zstd});
    for(unsigned first=1;first<=objects;) {
      std::vector<StoreMutation> changes;
      for(;first<=objects && changes.size()<batch;++first)changes.push_back({first,false,bytes});
      writer.commit(changes);
    }
  }
  const auto records=locations(*ram);
  require(records.size()==objects,"compressed fixture object inventory");
  for(const auto& record:records)
    require(record.stored<bytes.size() && get(ram->live,record.primary+12,4)==
        (zstd?detail::kStoreZstd:detail::kStorePattern),"compressed fixture used the wrong codec");
  auto snapshot=PagedObjectStoreSnapshot::open(ram,limits);
  const unsigned audits=profile?24:1;
  const auto mutations=ram->calls;
  const auto start_bytes=ram->read_bytes,start_calls=ram->read_calls;
  const auto started=std::chrono::steady_clock::now();
  for(unsigned pass=0;pass<audits;++pass)snapshot.verify_all();
  const auto audit_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-started).count();
  const auto read_bytes=ram->read_bytes-start_bytes,read_calls=ram->read_calls-start_calls;
  require(!snapshot.info().degraded_payloads,"healthy compressed replicas did not verify");
  for(unsigned id=1;id<=objects;++id)require(snapshot.read(id)==bytes,"compressed CPU readback differs");
  const auto& record=records.front();
  require(record.primary%ram->failure_unit+128<ram->failure_unit,"compressed fixture body is not in the header stripe");
  damage_record(*ram,record.peer+128);
  snapshot.verify_all();
  require(snapshot.info().degraded_payloads && snapshot.read(1)==bytes,
      "compressed peer damage escaped notice or discarded its survivor");
  damage_record(*ram,record.primary+128);
  rejects([&]{snapshot.verify_all();},"compressed audit accepted two damaged copies");
  damage_record(*ram,record.peer+128);
  auto recovered=PagedObjectStoreSnapshot::open(ram,limits);
  recovered.verify_all();
  require(recovered.info().degraded_payloads && recovered.read(1)==bytes,
      "compressed primary damage did not recover from the verified peer");
  require(ram->calls==mutations,"replica verification mutated the fixture");
  std::cout<<"compressed_replica_audit zstd="<<zstd<<" objects="<<objects<<" passes="<<audits
      <<" logical_bytes_per_pass="<<objects*bytes.size()<<" backing_read_bytes="<<read_bytes
      <<" backing_read_calls="<<read_calls<<" audit_ns="<<audit_ns
      <<" independent_cpu_readback=1 peer_damage_detected=1 primary_damage_recovered=1 double_damage_refused=1\n";
}

constexpr unsigned scrub_payload_bytes=64;
constexpr auto scrub_work_per_record=2ULL*(128+2*scrub_payload_bytes);
detail::NativeObjectWriter seed_scrub_records(const std::shared_ptr<Ram>& ram,unsigned total) {
  auto writer=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  for(unsigned first=1;first<=total;) {
    std::vector<StoreMutation> changes;
    for(;first<=total && changes.size()<64;++first)changes.push_back({first,false,data(scrub_payload_bytes,first)});
    writer.commit(changes);
  }
  return detail::NativeObjectWriter(std::move(writer));
}
struct ScrubPass {
  std::uint64_t objects=0;
  std::optional<PagedPayloadCheckpointObservation> checkpoint;
};
ScrubPass finish_scrub(detail::NativeObjectWriter& writer,const PagedPayloadScrubBudget& budget,unsigned limit) {
  ScrubPass pass;
  for(unsigned i=0;i<=limit;++i) {
    const auto result=writer.scrub_payload(budget);
    pass.objects+=result.examined_objects;
    if(result.state==PagedPayloadScrubState::complete) {
      pass.checkpoint=result.verified_checkpoint;
      return pass;
    }
    require(result.state==PagedPayloadScrubState::checked && result.examined_objects &&
        result.examined_objects<=budget.maximum_objects,"scrub did not make bounded verified progress");
  }
  throw std::runtime_error("scrub exceeded its population bound");
}
void bounded_scrub_provenance() {
  using S=PagedPayloadScrubState;
  constexpr unsigned total=96;
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  auto writer=seed_scrub_records(ram,total);
  const auto calls=ram->calls,reads=ram->read_calls;
  require(writer.scrub_payload({16,kPagedScrubRecordWorkBytes,[](auto){return true;}}).state==S::idle &&
      ram->read_calls==reads,"foreground refusal reached backing");
  require(writer.scrub_payload({16,scrub_work_per_record-1,{}}).state==S::idle,
      "scrub exceeded the first record's work limit");
  const auto first=writer.scrub_payload({16,kPagedScrubRecordWorkBytes,[](auto checked){return checked==3;}});
  require(first.state==S::checked && first.examined_objects==3 && first.first_examined_object_id==1 && first.object_id==3 &&
      first.verified_record_bytes==3*2*(128+scrub_payload_bytes) && first.resumable && !first.verified_checkpoint,
      "scrub did not yield between verified records");
  const auto next=writer.scrub_payload({16,2*scrub_work_per_record-1,{}});
  require(next.state==S::checked && next.object_id==4 && next.examined_objects==1,
      "scrub exceeded the batch work limit or skipped a record");
  const PagedPayloadScrubBudget full{16,kPagedScrubRecordWorkBytes,{}};
  const auto clean=finish_scrub(writer,full,total);
  require(clean.objects==total-4 && clean.checkpoint && clean.checkpoint->image==identity() &&
      clean.checkpoint->generation==writer.snapshot().info().generation && ram->calls==calls,
      "complete unchanged checkpoint did not verify its exact inventory");
  require(writer.restore_payload_scrub(*first.resumable),"saved advisory cursor was refused");
  const auto restored=finish_scrub(writer,full,total);
  require(restored.objects==total-3 && !restored.checkpoint,"saved progress became checkpoint evidence");

  require(writer.scrub_payload({3,kPagedScrubRecordWorkBytes,{}}).object_id==3,"new audit did not start from the first key");
  const std::array<StoreMutation,2> changed{{{1,false,data(scrub_payload_bytes,979)},{total+1,false,data(scrub_payload_bytes,980)}}};
  writer.paged_if()->commit(changed);
  const auto mixed=finish_scrub(writer,full,total+1);
  require(mixed.objects==total-3 && !mixed.checkpoint,"superseded or grown checkpoint gained exact evidence");
  const auto renewed=finish_scrub(writer,full,total+1);
  require(renewed.objects==total+1 && renewed.checkpoint && renewed.checkpoint->generation>clean.checkpoint->generation &&
      renewed.checkpoint->roots!=clean.checkpoint->roots,"new complete checkpoint retained the old identity");

  const auto records=locations(*ram);
  const auto target=records.at(4);
  damage_record(*ram,target.peer+128);
  const auto before_fault=ram->calls;
  const auto fault=writer.scrub_payload(full);
  require(fault.state==S::repair_queued && fault.object_id==5 && fault.examined_objects==5 &&
      fault.verified_record_bytes==4*2*(128+scrub_payload_bytes) && !fault.resumable &&
      !fault.verified_checkpoint && ram->calls==before_fault,"scanner went past a fault or tried to write a repair");
  require(writer.repair_payload()==PagedPayloadRepairProgress::repaired &&
      writer.snapshot().read(5)==data(scrub_payload_bytes,5),"repair lost the verified bytes");
  const auto repaired=finish_scrub(writer,full,total+1);
  require(!repaired.checkpoint,"repaired mixed-generation pass was certified");
  const auto last=writer.scrub_payload({total+1,kPagedScrubRecordWorkBytes,{}});
  require(last.state==S::checked && last.object_id==total+1,"final-fence fixture missed the end of the inventory");
  ram->corrupt_read_offset=0;ram->corrupt_read_number=2;ram->matching_reads=0;
  const auto final=writer.scrub_payload(full);
  require(final.state==S::unverified_index && !final.verified_checkpoint && ram->matching_reads==2,
      "checkpoint evidence ignored a new fault at its final root read");
  ram->corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)();
  auto independent=PagedObjectStoreSnapshot::open(ram,limits);
  independent.verify_all();
  require(independent.read(1)==changed[0].bytes && independent.read(total+1)==changed[1].bytes,
      "independent CPU reader disagreed after foreground changes");
  std::cout<<"bounded_scrub_provenance yielded=1 frame_budget=1 saved_cursor_advisory=1"
      <<" generation_change_refused=1 high_water_reached=1 fault_stopped_and_repaired=1 final_root_rechecked=1\n";
}
void profile_batched_scrub() {
  constexpr unsigned total=16384;
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;ram->capacity=96ULL*1024*1024;
  auto writer=seed_scrub_records(ram,total);
  const auto run=[&](bool batched) {
    const auto reads=ram->read_calls,bytes=ram->read_bytes,calls=ram->calls;
    const auto start=std::chrono::steady_clock::now();
    const auto pass=finish_scrub(writer,{batched?64U:1U,kPagedScrubRecordWorkBytes,{}},total);
    const auto time=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    require(pass.objects==total && pass.checkpoint && ram->calls==calls,"profile changed or failed to cover its checkpoint");
    std::cout<<"scrub_sample batched="<<batched<<" objects="<<pass.objects<<" backing_calls="<<ram->read_calls-reads
        <<" backing_bytes="<<ram->read_bytes-bytes<<" elapsed_ns="<<time<<" exact_checkpoint=1 writes=0\n";
    return *pass.checkpoint;
  };
  for(unsigned pair=0;pair<3;++pair) {
    const bool batch_first=pair%2!=0;
    const auto first=run(batch_first),second=run(!batch_first);
    require(first.image==second.image && first.generation==second.generation && first.roots==second.roots,
        "batch width changed the verified checkpoint");
    std::cout<<"scrub_pair pair="<<pair+1<<" batched_first="<<batch_first<<" identical_roots=1\n";
  }
  const auto cpu=PagedObjectStoreSnapshot::open(ram,limits);
  cpu.verify_all();
  for(unsigned id=1;id<=total;++id)require(cpu.read(id)==data(scrub_payload_bytes,id),"profile changed an independently read object");
  std::cout<<"scrub_profile_readback objects="<<total<<" exact=1\n";
}
}
int main(int argc,char** argv){try {
  if(argc==2 && std::string_view(argv[1])=="--scrub-batch-profile"){
    profile_batched_scrub();
    return 0;
  }
  if(argc==2 && std::string_view(argv[1])=="--compressed-audit-profile"){
    compressed_replica_verification(true);
    return 0;
  }
  require(argc==1,"unknown mirrored payload test option");
  compressed_replica_verification(false);
  forged_descriptors();native_shared_lifetime();payload_repair_rechecks();
  retained_indexed_reopen_fences(false);
  if(store_zstd_available())retained_indexed_reopen_fences(true);
  durable_payload_scrub();bounded_scrub_provenance();
  for(auto unit:{4096U,65536U}) {
    proactive_payload_scrub(unit);
    startup_payload_repair(unit);
    packing_and_boundaries(unit);survivor_and_rebuild(unit);fresh_replica_readback(unit);
    append_only_payload_repair(unit);
    for(unsigned layout=0;layout<3;++layout){native_recovery(layout,unit);fault_matrix(layout,unit);}
  }
  std::cout<<"PASS: "<<checks<<" mirrored payload checks; "<<faults<<" injected write/barrier cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
