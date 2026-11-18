#include "native_fs_fixture.hpp"
#include "native_group_codec.hpp"
#include <iostream>
#include <set>
namespace {
using namespace flitzfs;using namespace native_fixture;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
unsigned checks=0,faults=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why){++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);}
StoreIdentity next_id(unsigned value){auto id=identity();id[7]=static_cast<std::byte>(value);return id;}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,bool indexed_layout=false) {
  auto objects=PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,{StoreCompressionMode::raw,1,false});
  return indexed_layout?NativeFilesystemWriter::initialize_indexed(std::move(objects)):
      NativeFilesystemWriter::initialize_shared_bases(std::move(objects));
}
Bytes family(unsigned member) {
  auto bytes=data(kNativeExtentBytes,714);
  if(member){bytes[(member*719)%bytes.size()]^=std::byte{41};bytes[(member*1409+170)%bytes.size()]^=std::byte{77};}
  return bytes;
}
Bytes shifted_family(unsigned member) {
  auto bytes=family(0);
  std::rotate(bytes.begin(),bytes.begin()+member*37,bytes.end());
  return bytes;
}
void populate(NativeFilesystemWriter& writer,unsigned count,Bytes(*content)(unsigned)=family) {
  for(unsigned i=0;i<count;++i)writer.apply(std::array{file_create("f"+std::to_string(i)),file_write("f"+std::to_string(i),0,content(i))});
}
std::vector<NativeExtentSelection> selections(const NativeFilesystemSnapshot& snapshot,unsigned count) {
  std::vector<NativeExtentSelection> result;
  for(unsigned i=0;i<count;++i)result.push_back({snapshot.stat("f"+std::to_string(i)).inode,0});return result;
}
NativeFilesystemSnapshot view(const std::shared_ptr<Ram>& ram){return NativeFilesystemSnapshot::open(PagedObjectStoreSnapshot::open(ram,limits));}
void verify_family(const NativeFilesystemSnapshot& snapshot,unsigned count,Bytes(*content)(unsigned)=family) {
  for(unsigned i=0;i<count;++i)require(snapshot.read("f"+std::to_string(i),0,kNativeExtentBytes)==content(i),"shared-base file bytes differ");
  snapshot.verify_all();
}
void anchor_copy_codec() {
  const auto base=shifted_family(0);const auto id=detail::kNativeGroupBaseBit+1;
  const detail::NativeGroupAnchorIndex index(base);
  for(unsigned shift:{1U,16U,37U,71U,1000U}) {
    auto member=base;
    std::rotate(member.begin(),member.begin()+shift,member.end());
    require(!detail::encode_native_group_member(member,base,id,17),"aligned patches accepted a shifted control");
    const auto encoded=index.encode(member,base,id,17,kNativeExtentBytes);
    require(encoded && encoded->size()<512 && detail::decode_native_group_member(*encoded,base,true)==member,
        "shifted member did not round trip in a bounded frame");
    rejects([&]{(void)detail::decode_native_group_member(*encoded,base);},"legacy capability accepted anchor copies");
    require(!index.encode(member,base,id,17,encoded->size()),"anchor copy exceeded the competing size limit");
    auto rebound=*encoded;
    detail::bind_native_group_member(rebound,id+1,19,true);
    const auto reference=detail::parse_native_group_member(rebound,true);
    require(reference.method==detail::NativeGroupMethod::anchor_copies && reference.base_id==id+1 &&
        reference.base_generation==19 && detail::decode_native_group_member(rebound,base,true)==member,
        "rebinding affected the copy frame");
  }
  auto member=base;
  std::rotate(member.begin(),member.begin()+32,member.end());
  const auto encoded=index.encode(member,base,id,17,kNativeExtentBytes);
  require(encoded && ((*encoded)[131]&std::byte{128})!=std::byte{},"copy control fixture did not start with a copy");
  for(unsigned variant=0;variant<8;++variant) {
    auto corrupt=*encoded;
    if(variant==0)std::fill_n(corrupt.begin()+132,4,std::byte{255});
    if(variant==1){std::fill_n(corrupt.begin()+128,3,std::byte{});corrupt[131]=std::byte{128};}
    if(variant==2)corrupt[131]=std::byte{};
    if(variant==3)corrupt[96]=std::byte{};
    if(variant==4)corrupt[128]^=std::byte{1};
    if(variant==5)corrupt[32]^=std::byte{1};
    if(variant==6)corrupt[64]^=std::byte{1};
    if(variant==7)corrupt.pop_back();
    rejects([&]{(void)detail::decode_native_group_member(corrupt,base,true);},"malformed anchor copy decoded");
  }
}
void codec() {
  const auto base=family(0),member=family(1);const auto id=detail::kNativeGroupBaseBit+1;
  const auto encoded=detail::encode_native_group_member(member,base,id,17);
  require(encoded && encoded->size()<256 && detail::decode_native_group_member(*encoded,base)==member,"private codec round trip");
  const auto equal=detail::encode_native_group_member(base,base,id,17);
  require(equal && equal->size()==128 && detail::decode_native_group_member(*equal,base)==base,"zero-patch member");
  auto dense=base;for(auto& byte:dense)byte^=std::byte{255};
  require(!detail::encode_native_group_member(dense,base,id,17),"expanding dense patch selected");
  for(unsigned variant=0;variant<14;++variant) {
    auto bytes=*encoded;
    if(variant==0)bytes[0]^=std::byte{1};if(variant==1)bytes[8]=std::byte{255};
    if(variant==2)bytes[12]=std::byte{1};if(variant==3)bytes[23]=std::byte{0};
    if(variant==4)std::fill_n(bytes.begin()+24,8,std::byte{0});
    if(variant==5)bytes[32]^=std::byte{1};if(variant==6)bytes[64]^=std::byte{1};
    if(variant==7)bytes[96]=std::byte{255};if(variant==8)bytes[100]^=std::byte{1};
    if(variant==9)bytes[104]=std::byte{1};if(variant==10)std::fill_n(bytes.begin()+128,4,std::byte{255});
    if(variant==11)std::fill_n(bytes.begin()+132,4,std::byte{0});
    if(variant==12)bytes.pop_back();if(variant==13)bytes.push_back(std::byte{0});
    rejects([&]{detail::decode_native_group_member(bytes,base);},"malformed member decoded");
  }
  for(unsigned i=0;i<96;++i) {
    auto varied=base;
    for(unsigned j=0;j<i;++j)varied[(i*127+j*313)%varied.size()]^=static_cast<std::byte>(j+1);
    const auto result=detail::encode_native_group_member(varied,base,id,17);
    require(result && detail::decode_native_group_member(*result,base)==varied,"varied sparse patch mismatch");
  }
}
void lifecycle_and_sizes(std::uint32_t unit,bool indexed_layout=false) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=unit;
  std::optional<NativeFilesystemSnapshot> before;NativeSharedBaseCommit grouped;Bytes baseline_bytes;
  {
    auto writer=fresh(ram,indexed_layout);populate(writer,8);before=writer.snapshot();const auto keys=selections(*before,8);
    if(indexed_layout)require(!before->shared_base_info().enabled,"ordinary indexed creation promoted its root");
    const auto workload=before->workload_info();const auto old_file=before->stat("f1");
    baseline_bytes=ram->stable;grouped=writer.compress_shared_base(keys.front(),keys);
    require(grouped.applied && grouped.members==8 && grouped.after_compacted_upper_bound<grouped.before_compacted_lower_bound,"beneficial family declined");
    auto after=writer.snapshot();verify_family(after,8);
    require(after.shared_base_info().enabled && after.shared_base_info().members==8 && after.shared_base_info().bases==1,"group inventory differs");
    require(after.workload_info()==workload && after.stat("f1").modified_sequence==old_file.modified_sequence,"optimization changed workload/mtime provenance");
    const auto writes=ram->writes,generation=after.storage_info().generation;
    require(!writer.compress_shared_base(keys.front(),keys).applied && ram->writes==writes && writer.snapshot().storage_info().generation==generation,
        "same group repeatedly rewrote data");
  }
  auto after=view(ram);const auto source=ram->live;const auto calls=ram->calls;
  auto baseline=image(baseline_bytes,unit);
  auto old_image=std::make_shared<Ram>(),new_image=std::make_shared<Ram>();old_image->failure_unit=new_image->failure_unit=unit;
  auto old_current=NativeFilesystemWriter::compact(view(baseline),old_image,next_id(90),{StoreCompressionMode::raw,1,false});
  verify_family(old_current.snapshot(),8);const auto actual_before=old_image->extent();
  auto current=NativeFilesystemWriter::compact(after,new_image,next_id(91),{StoreCompressionMode::raw,1,false});
  verify_family(current.snapshot(),8);
  const auto actual_after=new_image->extent();
  require(actual_before>=grouped.before_compacted_lower_bound && actual_after<actual_before,"complete compacted image did not shrink");
  require(new_image->extent()<=grouped.after_compacted_upper_bound && ram->live==source && ram->calls==calls,"compacted upper bound/source preservation failed");
  verify_family(*before,8);
  auto retained=current.snapshot();
  file_apply(current,file_remove("f0"));
  require(current.snapshot().read("f7",0,kNativeExtentBytes)==family(7),"deleting base filename lost dependents");
  const auto detached=current.snapshot().stat("f1").inode;
  current.set_delete_pending(detached,true);current.unlink_inode(detached);
  require(current.snapshot().read_inode(detached,0,kNativeExtentBytes)==family(1),"detached grouped file unreadable");
  current.retire_inode(detached);
  file_apply(current,file_write("f2",19,Bytes{std::byte{19}}));auto changed=family(2);changed[19]=std::byte{19};
  require(current.snapshot().read("f2",0,kNativeExtentBytes)==changed,"grouped write did not detach");
  for(unsigned i=2;i<8;++i)file_apply(current,file_remove("f"+std::to_string(i)));
  const auto empty_volume=current.snapshot();
  empty_volume.verify_all();
  require(empty_volume.shared_base_info().members==0 && empty_volume.shared_base_info().bases==0 &&
      empty_volume.storage_info().objects==3,"last group dependency was not collected");
  verify_family(retained,8);
  std::cout<<"shared_base_sizes indexed="<<indexed_layout<<" unit="<<unit<<" before_lower="<<grouped.before_compacted_lower_bound
      <<" after_upper="<<grouped.after_compacted_upper_bound<<" actual_before="<<actual_before<<" actual_after="<<actual_after<<'\n';
}
void independent_and_decline() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);populate(writer,2);
  auto bytes=family(0);for(auto& byte:bytes)byte^=std::byte{255};file_apply(writer,file_write("f1",0,bytes));
  const auto keys=selections(writer.snapshot(),2);const auto before=ram->writes;
  require(!writer.compress_shared_base(keys.front(),keys).applied && ram->writes==before,"unrelated data rewritten");
  rejects([&]{writer.compress_shared_base({1,0},keys);},"directory selected as base");
  rejects([&]{writer.compress_shared_base({keys.front().inode,999},keys);},"sparse hole selected as base");
  auto legacy=std::make_shared<Ram>();legacy->failure_unit=4096;
  const auto start=legacy->calls;
  rejects([&]{NativeFilesystemWriter::initialize_shared_bases(PagedObjectStoreWriter::create(legacy,identity(),limits));},"unprotected shared-base namespace admitted");
  require(legacy->calls>start,"legacy fixture did not create standalone empty storage");
}
void fault_matrix(std::uint32_t unit,bool indexed_layout=false,bool shifted=false) {
  auto original=std::make_shared<Ram>();original->failure_unit=unit;
  const auto content=shifted?shifted_family:family;
  {auto writer=fresh(original,indexed_layout);populate(writer,4,content);}
  const auto keys=selections(view(original),4);
  auto resume=[&](const std::shared_ptr<Ram>& ram) {
    const auto info=PagedObjectStoreSnapshot::open(ram,limits).info();
    return NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(ram,identity(),info.generation,limits,{StoreCompressionMode::raw,1,false}));
  };
  auto probe=image(original->stable,unit);std::size_t cuts=0;
  {auto writer=resume(probe);const auto start=probe->calls;require(writer.compress_shared_base(keys.front(),keys).applied,"fault probe no-op");cuts=probe->calls-start;}
  require(cuts>3 && cuts<256,"shared-base fault envelope");
  unsigned prior=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto ram=image(original->stable,unit);
    {auto writer=resume(ram);ram->failure=mode;ram->fail_at=ram->calls+cut;
      rejects([&]{writer.compress_shared_base(keys.front(),keys);},"faulted shared-base operation acknowledged");require(writer.poisoned(),"indeterminate group writer active");}
    ram->live=ram->stable;ram->fail_at=(std::numeric_limits<std::size_t>::max)();
    const auto recovered=view(ram);verify_family(recovered,4,content);const auto info=recovered.shared_base_info();
    require((info.members==0 && info.bases==0) || (info.members==4 && info.bases==1),"partial shared group published");
    if(shifted) {
      const auto root=PagedObjectStoreSnapshot::open(ram,limits).read(1);
      require(root[8]==(info.members?std::byte{4}:std::byte{2}),"anchor copy capability changed outside the group transaction");
    }
    if(info.members)++complete;else ++prior;
    auto continued=resume(ram);file_apply(continued,file_write("f1",0,Bytes{std::byte{79}}));
    auto expected=content(1);expected[0]=std::byte{79};require(continued.snapshot().read("f1",0,kNativeExtentBytes)==expected,"recovered group could not continue");++faults;
  }
  require(prior && complete,"shared-base faults did not recover both roots");
  std::cout<<"shared_base_faults indexed="<<indexed_layout<<" shifted="<<shifted<<" unit="<<unit<<" cuts="<<cuts
      <<" modes=3 prior="<<prior<<" complete="<<complete<<'\n';
}
void indexed_extension_and_reclamation() {
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;
  NativeSharedBaseCommit extended;
  {
    auto writer=fresh(ram,true);populate(writer,4);
    auto keys=selections(writer.snapshot(),4);
    require(writer.compress_shared_base(keys.front(),keys).applied,"indexed first cohort");
    const auto first=writer.snapshot();
    const auto anchor=[&] {
      const auto objects=PagedObjectStoreSnapshot::open(ram,limits);
      for(const auto& record:objects.list_page(0,128)) {
        if(record.logical_bytes==8+kNativeExtentBytes)return record;
      }
      throw std::runtime_error("missing first indexed shared base");
    }();
    writer.apply(std::array{file_create("f4"),file_write("f4",0,family(4))});
    keys=selections(writer.snapshot(),5);
    extended=writer.compress_shared_base(keys.front(),keys);
    require(extended.applied && writer.snapshot().shared_base_info().members==5 &&
        writer.snapshot().shared_base_info().bases==1,"indexed family did not extend");
    const auto preserved=PagedObjectStoreSnapshot::open(ram,limits).stat(anchor.id);
    require(preserved && preserved->generation==anchor.generation,"extension rewrote its immutable base");
    verify_family(first,4);
  }
  const auto original=view(ram);
  verify_family(original,5);
  const std::array removals{file_remove("f0"),file_remove("f1"),file_remove("f2"),file_remove("f3"),file_remove("f4")};
  const auto stable=ram->stable;
  const auto resume=[&](const std::shared_ptr<Ram>& scope) {
    return NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(scope,identity(),
        extended.transaction.storage.generation,limits,{StoreCompressionMode::raw,1,false}));
  };
  auto probe=image(stable,4096);std::size_t cuts=0;
  {
    auto writer=resume(probe);
    const auto calls=probe->calls;
    writer.apply(removals);
    cuts=probe->calls-calls;
    const auto retired=writer.snapshot();
    retired.verify_all();
    require(retired.shared_base_info().bases==0 && retired.storage_info().objects==3,"indexed final base survived");
  }
  require(cuts>3 && cuts<256,"indexed final retirement fault envelope");
  unsigned previous=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<cuts;++cut) {
    auto copy=image(stable,4096);
    {
      auto victim=resume(copy);
      copy->failure=mode;copy->fail_at=copy->calls+cut;
      rejects([&]{victim.apply(removals);},"faulted indexed final retirement acknowledged");
      require(victim.poisoned(),"faulted indexed final retirement left a writer active");
    }
    const auto recovered=view(image(copy->stable,4096));
    recovered.verify_all();
    if(recovered.shared_base_info().members==5) {verify_family(recovered,5);++previous;}
    else {
      require(recovered.shared_base_info().bases==0 && recovered.shared_base_info().members==0 &&
          recovered.storage_info().objects==3 && recovered.list().empty(),"partial indexed final retirement");
      ++complete;
    }
    ++faults;
  }
  require(previous && complete && ram->stable==stable,"indexed fault campaign missed a root or changed its source");
  std::cout<<"indexed_base_retirement_faults="<<previous+complete<<" previous="<<previous<<" complete="<<complete<<'\n';
}

void malformed_storage() {
  auto original=std::make_shared<Ram>();original->failure_unit=4096;
  {auto writer=fresh(original);populate(writer,4);const auto keys=selections(writer.snapshot(),4);
    require(writer.compress_shared_base(keys.front(),keys).applied,"malformed fixture group");}
  auto objects=PagedObjectStoreSnapshot::open(original,limits);std::uint64_t member=0,anchor=0;
  for(const auto& object:objects.list_page(0,1024)) {
    if(object.id>=2 && object.id<=256 && object.logical_bytes<kNativeExtentBytes)member=object.id;
    if(object.id>detail::kNativeGroupBaseBit && object.id<2*detail::kNativeGroupBaseBit)anchor=object.id;
  }
  require(member && anchor,"malformed fixture dependency identities");
  const auto body=objects.read(member),base=objects.read(anchor);
  for(unsigned variant=0;variant<12;++variant) {
    auto ram=image(original->stable,4096);auto pg=PagedObjectStoreWriter::resume(ram,identity(),objects.info().generation,limits,{StoreCompressionMode::raw,1,false});
    auto bytes=body;StoreMutation change{member,false,bytes};
    if(variant==0)change.bytes[16]^=std::byte{64};
    if(variant==1)change.bytes[24]^=std::byte{1};
    if(variant==2)change.bytes[32]^=std::byte{1};
    if(variant==3)change.bytes[64]^=std::byte{1};
    if(variant==4)change={anchor,true,{}};
    if(variant==5){change={anchor,false,base};change.bytes[100]^=std::byte{1};}
    if(variant==6)change={anchor,false,Bytes(128)};
    if(variant==7)change={anchor+1,false,base};
    if(variant==8){change={1,false,objects.read(1)};change.bytes[8]=std::byte{2};}
    if(variant==9)change.bytes.resize(127);
    if(variant==10)change.bytes[104]=std::byte{1};
    if(variant==11)change.bytes.resize(65535);
    pg.commit(std::span<const StoreMutation>(&change,1));pg.snapshot().verify_all();
    rejects([&]{view(ram);},"checksum-valid malformed dependency exposed");
  }
  // A raw full extent beginning with the group magic remains opaque file data.
  auto ram=std::make_shared<Ram>();ram->failure_unit=4096;auto writer=fresh(ram);auto plain=family(0);
  std::copy_n(body.begin(),8,plain.begin());writer.apply(std::array{file_create("opaque"),file_write("opaque",0,plain)});
  require(writer.snapshot().read("opaque",0,kNativeExtentBytes)==plain && writer.snapshot().shared_base_info().members==0,
      "plain magic prefix misidentified as group");
}
void staged_group(std::uint32_t unit,bool reencode,bool indexed_layout=false) {
  auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=unit;
  auto writer=fresh(source,indexed_layout);populate(writer,8);const auto keys=selections(writer.snapshot(),8);
  require(writer.compress_shared_base(keys.front(),keys).applied,"staged fixture group");
  const auto original=source->live;const auto calls=source->calls;
  auto candidate=writer.prepare_paged_recompression(target,next_id(81),{StoreCompressionMode::raw,1,false},reencode);
  unsigned steps=0;
  while(candidate.progress().phase!=PagedRewritePhase::ready){writer.advance_paged_recompression(candidate);require(++steps<256,"staged group stalled");}
  auto copied=writer.finish_paged_recompression(candidate);verify_family(copied.snapshot(),8);verify_family(view(target),8);
  require(copied.snapshot().shared_base_info().members==8 && copied.snapshot().shared_base_info().bases==1 && source->live==original && source->calls==calls,
      "staged rebuild changed dependency closure/source");
  file_apply(copied,file_truncate("f2",1));require(copied.snapshot().read("f2",0,2)==Bytes{family(2).front()},"group truncate mismatch");
  verify_family(writer.snapshot(),8);
}
void lost_base_bank(std::uint32_t unit,bool indexed_layout=false) {
  auto source=std::make_shared<Ram>(),compact=std::make_shared<Ram>();source->failure_unit=compact->failure_unit=unit;
  {auto writer=fresh(source,indexed_layout);populate(writer,4);const auto keys=selections(writer.snapshot(),4);
    require(writer.compress_shared_base(keys.front(),keys).applied,"base-loss fixture group");}
  {auto copied=NativeFilesystemWriter::compact(view(source),compact,next_id(83),{StoreCompressionMode::raw,1,false});verify_family(copied.snapshot(),4);}
  const auto base=family(0);std::set<std::uint64_t> banks;std::vector<std::size_t> base_bytes;
  for(auto at=compact->live.begin();;) {
    at=std::search(at,compact->live.end(),base.begin(),base.begin()+32);if(at==compact->live.end())break;
    const auto offset=static_cast<std::size_t>(at-compact->live.begin());
    banks.insert(offset/unit*unit);base_bytes.push_back(offset+17);++at;
  }
  require(banks.size()==2,"unique mirrored base-bank fixture");
  for(const auto bank:banks) {
    auto damaged=compact->stable;std::fill_n(damaged.begin()+bank,unit,std::byte{0});auto ram=image(damaged,unit);
    auto readable=view(ram);verify_family(readable,4);require(readable.storage_info().degraded_checkpoint,"base loss not reported degraded");
    auto target=std::make_shared<Ram>();target->failure_unit=unit;
    auto repaired=NativeFilesystemWriter::compact(readable,target,next_id(84),{StoreCompressionMode::raw,1,false});verify_family(repaired.snapshot(),4);
    require(!repaired.snapshot().storage_info().degraded_checkpoint && ram->live==damaged,"base repair changed source or stayed degraded");
  }
  auto destroyed=compact->stable;for(const auto bank:banks)std::fill_n(destroyed.begin()+bank,unit,std::byte{0});
  rejects([&]{(void)view(image(destroyed,unit)).read("f1",0,kNativeExtentBytes);},
      "both missing base copies exposed data");
  auto active=image(compact->stable,unit);const auto objects=PagedObjectStoreSnapshot::open(active,limits);
  auto writer=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(active,next_id(83),objects.info().generation,limits,{StoreCompressionMode::raw,1,false}));
  const auto keys=selections(writer.snapshot(),4);const auto writes=active->writes;
  // Corrupt only anchor payload bytes, retaining namespace bytes in its bank.
  auto bad_anchor=compact->stable;for(const auto offset:base_bytes)bad_anchor[offset]^=std::byte{1};
  active->live=active->stable=bad_anchor;
  rejects([&]{writer.compress_shared_base(keys.front(),keys);},"live lost dependency accepted");
  require(writer.poisoned(),"dependency read failure did not fence writer");
  rejects([&]{file_apply(writer,file_create("later"));},"fenced dependency writer accepted later write");
  require(active->writes==writes,"failed dependency work performed writes");
}
void layout_accounting(std::uint32_t unit,bool mirrored,unsigned count) {
  auto source=std::make_shared<Ram>(),target=std::make_shared<Ram>();source->failure_unit=target->failure_unit=unit;
  {
    auto writer=mirrored?PagedObjectStoreWriter::create_mirrored(source,identity(),limits,{StoreCompressionMode::raw,1,false}):
        PagedObjectStoreWriter::create(source,identity(),limits,{StoreCompressionMode::raw,1,false});
    for(unsigned at=0;at<count;) {
      std::vector<StoreMutation> changes;
      for(unsigned i=0;i<256 && at<count;++i,++at)changes.push_back({at+1,false,data(at%89,at)});
      writer.commit(changes);
    }
  }
  auto objects=PagedObjectStoreSnapshot::open(source,limits);auto records=objects.list_page(0,4096);
  const auto reads=source->read_calls,calls=source->calls;
  const auto expected=objects.rebuilt_size(records);
  require(source->read_calls==reads && source->calls==calls,"pure layout estimate performed I/O");
  std::vector<StoreMutation> changes{{count+100,false,data(197,91)}};
  if(count)changes.push_back({1,false,data(unit-128,533)});
  if(count>1)changes.push_back({count,true,{}});
  const auto estimate_start=source->read_bytes;
  const auto estimated=objects.rebuilt_size_after(changes);
  const auto estimate_bytes=source->read_bytes-estimate_start;
  require(estimated.before==expected && source->calls==calls,"size estimate baseline or source differs");
  require(estimate_bytes<=changes.size()*(objects.info().height+1)*8192ULL,
      "size estimate read unrelated metadata");
  auto rebuilt=PagedObjectStoreWriter::rebuild(objects,target,next_id(82),limits,{StoreCompressionMode::raw,1,false});rebuilt.snapshot().verify_all();
  require(target->extent()==expected && rebuilt.snapshot().info().objects==count,"layout estimate differs at tree boundary");
  for(const auto& record:records)require(rebuilt.snapshot().read(record.id)==objects.read(record.id),"generic rebuild changed bytes");
  for(const auto& change:changes) {
    const auto at=std::lower_bound(records.begin(),records.end(),change.id,
        [](const StoreObjectInfo& object,std::uint64_t id){return object.id<id;});
    if(change.erase)records.erase(at);
    else {
      const auto length=static_cast<std::uint32_t>(change.bytes.size());
      const StoreObjectInfo record{change.id,0,length,length};
      if(at!=records.end() && at->id==change.id)*at=record;
      else records.insert(at,record);
    }
  }
  require(estimated.after_upper==objects.rebuilt_size(records),"affected object size proof differs from complete layout");
  if(count==1926) {
    {
      auto writer=PagedObjectStoreWriter::resume(source,identity(),objects.info().generation,limits,
          {StoreCompressionMode::raw,1,false});
      writer.commit(changes);
    }
    auto changed=PagedObjectStoreSnapshot::open(source,limits);
    auto revised=std::make_shared<Ram>();revised->failure_unit=unit;
    auto compacted=PagedObjectStoreWriter::rebuild(changed,revised,next_id(85),limits,{StoreCompressionMode::raw,1,false});
    compacted.snapshot().verify_all();
    require(revised->extent()==estimated.after_upper && compacted.snapshot().info().objects==records.size(),
        "affected object size proof differs from actual compacted image");
  }
  if(records.size()>1){std::swap(records[0],records[1]);rejects([&]{objects.rebuilt_size(records);},"unordered hypothetical inventory accepted");}
  std::cout<<"layout_accounting unit="<<unit<<" mirrored="<<mirrored<<" objects="<<count
      <<" before="<<expected<<" after="<<estimated.after_upper<<" proposal_read_bytes="<<estimate_bytes<<'\n';
}
void indexed_group_on_populated_volume() {
  constexpr unsigned unrelated=1024,group_size=8,batch=32;
  constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};
  auto ram=std::make_shared<Ram>();
  ram->failure_unit=4096;
  const auto name=[](unsigned file){return "separate-"+std::to_string(file);};
  Bytes original;
  std::uint64_t proposal_reads=0;
  NativeSharedBaseCommit result;
  {
    auto writer=fresh(ram,true);
    for(unsigned first=0;first<unrelated;first+=batch) {
      std::vector<NativeFileMutation> operations;
      operations.reserve(batch*2);
      for(unsigned file=first;file<first+batch;++file) {
        const auto path=name(file);
        operations.push_back(file_create(path));
        operations.push_back(file_write(path,0,data(17,file)));
      }
      require(writer.apply(operations).created_inodes.size()==batch,"populated grouping fixture creation");
    }
    populate(writer,group_size);
    auto prior=writer.snapshot();
    auto keys=selections(prior,group_size);
    original=ram->stable;
    const auto begin=ram->read_bytes;
    result=writer.compress_shared_base(keys.front(),keys);
    proposal_reads=ram->read_bytes-begin;
    require(result.applied && result.members==group_size &&
        writer.snapshot().shared_base_info().members==group_size && !prior.shared_base_info().enabled,
        "populated indexed volume rejected a related family or changed the prior root");
  }
  auto baseline=image(std::move(original),4096);
  auto before=std::make_shared<Ram>(),after=std::make_shared<Ram>();
  before->failure_unit=after->failure_unit=4096;
  auto a=NativeFilesystemWriter::compact(view(baseline),before,next_id(86),raw);
  auto b=NativeFilesystemWriter::compact(view(ram),after,next_id(87),raw);
  const auto prior=a.snapshot();
  const auto current=b.snapshot();
  require(after->extent()<=result.after_compacted_upper_bound &&
      result.before_compacted_lower_bound<=before->extent() && after->extent()<before->extent(),
      "populated indexed compacted image did not shrink within proven bounds");
  verify_family(prior,group_size);
  verify_family(current,group_size);
  for(unsigned file=0;file<unrelated;++file) {
    const auto expected=data(17,file);
    require(prior.read(name(file),0,17)==expected && current.read(name(file),0,17)==expected,
        "grouped compaction changed an unrelated populated file");
  }
  std::cout<<"indexed_group_accounting files="<<unrelated+group_size<<" members="<<group_size
      <<" proposal_read_bytes="<<proposal_reads<<" before="<<before->extent()<<" after="<<after->extent()<<'\n';
}
void layouts(std::uint32_t unit) {
  for(bool mirrored:{false,true})for(unsigned count:{0U,55U,56U,1926U})layout_accounting(unit,mirrored,count);
}
}
int main(int argc,char** argv){try {
  if(argc==2 && std::string_view(argv[1])=="--layout") {
    for(auto unit:{4096U,65536U})layouts(unit);
    return 0;
  }
  require(argc==1,"unknown shared-base test mode");
  codec();anchor_copy_codec();independent_and_decline();malformed_storage();indexed_extension_and_reclamation();
  indexed_group_on_populated_volume();
  for(auto unit:{4096U,65536U}){
    for(const auto indexed_layout:{false,true}) {
      lifecycle_and_sizes(unit,indexed_layout);fault_matrix(unit,indexed_layout);
      lost_base_bank(unit,indexed_layout);
      for(bool reencode:{false,true})staged_group(unit,reencode,indexed_layout);
    }
    fault_matrix(unit,true,true);
    layouts(unit);
  }
  std::cout<<"PASS: "<<checks<<" native shared-base checks; "<<faults<<" injected write/barrier cases; memory CPU only\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
