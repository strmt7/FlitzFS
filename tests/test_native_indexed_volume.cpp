#include "native_indexed_volume.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "native_lz4_read.hpp"
#include <utility>
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/volume_head.hpp"
#include "flitzfs/read_only_file.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <map>

namespace {
using namespace native_fixture;
using namespace flitzfs::detail;
constexpr PagedStoreLimits limits{kStoreMaxPhysicalBytes};
constexpr NativeImageStorage indexed{NativeImageKind::indexed_paged,limits.maximum_physical_bytes};
constexpr StoreCompressionOptions raw{StoreCompressionMode::raw,1,false};

void require(bool ok,const char* message) {
  if(!ok)throw std::runtime_error(message);
}
template<class Operation>
void refuses(NativeFsErrorCode code,const Operation& operation) {
  try { operation(); }
  catch(const NativeFsError& error) {
    if(error.code()==code)return;
    throw;
  }
  throw std::runtime_error("invalid indexed operation completed");
}
std::shared_ptr<Ram> empty() {
  auto result=std::make_shared<Ram>();
  result->failure_unit=4096;
  return result;
}
StoreIdentity identity_for(unsigned role) {
  auto result=identity();
  result[1]=static_cast<std::byte>(role);
  return result;
}
std::string file(unsigned index){return "f"+std::to_string(100000+index);}
Bytes payload(unsigned index) {
  auto bytes=data(17,index);
  bytes.back()=std::byte{0x55};
  return bytes;
}

template<class Verify>
void profile_indexed_reopen(const std::shared_ptr<Ram>& image,const NativeFilesystemSnapshot& source,unsigned files,
    std::uint64_t logical_bytes,std::string_view workload,const Verify& verify,StoreCompressionOptions encoding=raw) {
  const auto volume=identity_for(82);
  auto head=empty();
  {auto owner=NativeVolumeHeadWriter::create(head,volume,source);}
  const auto layout=PagedObjectStoreSnapshot::open(image,{source.image_storage().maximum_physical_bytes}).info();
  const auto read_before=image->read_bytes,calls_before=image->read_calls;
  const auto image_fences=image->flushes,head_fences=head->flushes;
  const auto bytes_written=image->write_bytes,head_written=head->write_bytes;
  const auto resolve=[&](const auto& id)->std::shared_ptr<StoreByteIo> {
    require(id==source.identity(),"scale reopen selected a different image");
    return image;
  };
  const auto started=std::chrono::steady_clock::now();
  const auto session=NativeFileSession::resume_closed_indexed_volume(head,volume,1,resolve,encoding);
  const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
  const auto read_after=image->read_bytes-image->read_bytes_at_flush;
  std::cout<<"indexed_scale_reopen files="<<files
      <<" image_pre_flush_bytes="<<image->read_bytes_at_flush-read_before
      <<" image_post_flush_bytes="<<read_after
      <<" image_read_calls="<<image->read_calls-calls_before
      <<" image_flushes="<<image->flushes-image_fences
      <<" head_flushes="<<head->flushes-head_fences
      <<" backing_write_bytes="<<image->write_bytes-bytes_written+head->write_bytes-head_written
      <<" reopen_ns="<<elapsed<<" logical_bytes="<<logical_bytes<<" workload="<<workload
      <<" objects="<<layout.objects<<" object_tree_height="<<layout.height
      <<" stored_payload_bytes="<<layout.stored_payload_bytes<<'\n';
  require(image->write_bytes==bytes_written && head->write_bytes==head_written,
      "clean indexed reopen rewrote the image or selector");
  const auto restored=NativeFilesystemSnapshot::open_image(image,source.image_storage());
  require(restored.sequence()==source.sequence(),"scaled writable reopen selected a different namespace");
  verify(restored);
  std::cout<<"indexed_reopen_readback files="<<files<<" workload="<<workload<<" exact=1\n";
}

void indexed_reopen_checks_batched_replicas() {
  const auto image=empty(),head=empty();
  const auto first=payload(9027),second=payload(9028);
  IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(image,identity(),limits,raw),
      1,4,{true,first.size()+second.size()},2);
  const std::array<IndexedInodeInput,3> inodes{{
      {1,0,0,1,1,true,""},{2,1,first.size(),1,1,false,"first"},{3,1,second.size(),1,1,false,"second"}}};
  const std::array<IndexedExtentInput,2> extents{{{2,0,first},{3,0,second}}};
  builder.append(inodes,extents);
  const auto source=builder.finish();
  const auto volume=identity_for(83);
  {auto owner=NativeVolumeHeadWriter::create(head,volume,source);}
  const auto offset_of=[&](const Bytes& payload) {
    const auto match=std::ranges::search(image->live,payload);
    require(!match.empty(),"batched-replica fixture did not store its raw bytes");
    return static_cast<std::uint64_t>(match.begin()-image->live.begin());
  };
  const auto first_at=offset_of(first),second_at=offset_of(second);
  require(first_at<second_at && first_at/image->failure_unit==second_at/image->failure_unit,
      "batched-replica fixture does not share a bank");
  const auto resolve=[&](const auto& id)->std::shared_ptr<StoreByteIo> {
    return id==source.identity()?image:nullptr;
  };
  const auto reject_after_fences=[&](std::uint64_t bank,bool both) {
    const auto writes=image->write_bytes+head->write_bytes;
    const auto image_fences=image->flushes,head_fences=head->flushes;
    image->corrupt_read_offset=second_at+bank+1;
    image->corrupt_read_number=2;
    image->corrupt_followup_offset=both?second_at+image->failure_unit+1:Ram::no_read_fault;
    image->matching_reads=0;
    image->corrupted_read_start=Ram::no_read_fault;
    refuses(NativeFsErrorCode::invalid_operation,[&] {
      (void)NativeFileSession::resume_closed_indexed_volume(head,volume,1,resolve,raw);
    });
    const auto last_bank=both?image->failure_unit:bank;
    const auto first_peer_at=first_at+last_bank;
    require(image->corrupted_read_start/image->failure_unit==first_peer_at/image->failure_unit &&
        image->corrupted_read_start<=first_peer_at && image->flushes==image_fences+1 &&
        head->flushes==head_fences+1 && image->write_bytes+head->write_bytes==writes,
        "writable reopen did not reject an interior replica observation after both fences");
  };
  reject_after_fences(0,false);
  reject_after_fences(image->failure_unit,false);
  // Equality between two identically damaged observations is insufficient.
  reject_after_fences(0,true);
  image->corrupt_read_offset=Ram::no_read_fault;
  const auto session=NativeFileSession::resume_closed_indexed_volume(head,volume,1,resolve,raw);
  const auto restored=NativeFilesystemSnapshot::open_image(image,source.image_storage());
  require(restored.read_inode(2,0,first.size())==first && restored.read_inode(3,0,second.size())==second,
      "failed batched replica check changed the recoverable files");
}

void populated_native_snapshot(unsigned files=1024,bool reopen_only=false) {
  constexpr unsigned batch=32;
  const std::uint64_t sparse=3+files;
  constexpr auto sparse_size=(std::numeric_limits<std::uint64_t>::max)()-7;
  constexpr auto last_block=(sparse_size-1)/kNativeExtentBytes;
  auto ram=empty();
  if(files>1024)ram->capacity=192ULL*1024*1024;
  const PagedStoreLimits image_limits{ram->capacity};
  const NativeImageStorage storage{indexed.kind,ram->capacity};
  IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(ram,identity(),image_limits,raw),
      2,sparse+1,{true,files*17},(reopen_only || files>1024)?2:1);
  const std::array<IndexedInodeInput,2> root{{{1,0,0,1,1,true,""},
      {2,1,0,2,2,true,"Data"}}};
  builder.append(root);
  refuses(NativeFsErrorCode::storage_failure,[&]{(void)NativeFilesystemSnapshot::open_image(ram,storage);});
  for(unsigned first=0;first<files;first+=batch) {
    std::vector<Bytes> contents;
    std::vector<IndexedInodeInput> inodes;
    std::vector<IndexedExtentInput> extents;
    contents.reserve(batch);inodes.reserve(batch);extents.reserve(batch);
    for(unsigned i=first;i<(std::min)(first+batch,files);++i) {
      contents.push_back(payload(i));const auto inode=std::uint64_t{3}+i;
      inodes.push_back({inode,2,contents.back().size(),2,2,false,file(i)});
      extents.push_back({inode,0,contents.back()});
    }
    builder.append(inodes,extents);
  }
  const IndexedInodeInput large{sparse,2,sparse_size,2,2,false,"z-sparse"};
  const auto a=payload(7001),b=payload(7002),c=payload(7003);
  const std::array<IndexedExtentInput,3> sparse_extents{{{sparse,127,a},{sparse,128,b},{sparse,last_block,c}}};
  builder.append({&large,1},sparse_extents);
  const auto published=builder.finish();
  const auto reopen=[&] {
    profile_indexed_reopen(ram,published,files,files*17ULL,"short_files",[&](const auto& snapshot) {
      for(unsigned i=0;i<files;++i)
        require(snapshot.read_inode(3+i,0,17)==payload(i),"scaled writable reopen changed a named file");
      require(snapshot.read_inode(sparse,127ULL*kNativeExtentBytes,17)==a &&
          snapshot.read_inode(sparse,last_block*kNativeExtentBytes,17)==c,"scaled writable reopen changed sparse data");
    });
  };
  if(reopen_only) {
    reopen();
    return;
  }
  const auto start_open=ram->read_bytes;
  const auto view=NativeFilesystemSnapshot::open_image(ram,storage);
  const auto open_bytes=ram->read_bytes-start_open;
  require(view.image_storage()==storage && view.sequence()==2 && view.identity()==identity() &&
      view.allocation_usage().live_extents==files+3,"indexed native root counts");
  const auto start_lookup=ram->read_bytes;
  const auto last=view.stat("DATA/"+file(files-1));
  const auto lookup_bytes=ram->read_bytes-start_lookup;
  require(last.inode==files+2 && last.path=="Data/"+file(files-1) && last.logical_bytes==17,
      "indexed path did not resolve persisted data");
  const auto start_scrub=ram->read_bytes,scrub_calls=ram->read_calls;
  view.verify_all();
  const auto scrub_bytes=ram->read_bytes-start_scrub;
  const auto scrub_reads=ram->read_calls-scrub_calls;
  const auto audit_read_budget=files>1024?5000ULL*files:4*view.storage_info().physical_end;
  require(scrub_bytes<audit_read_budget,
      "indexed scrub repeated object and metadata reads on the short-file fixture");
  const auto objects=PagedObjectStoreSnapshot::open(ram,image_limits);
  const auto layout=objects.info();
  // A complete raw inventory would read at least the stored payload in one
  // replica. This bound is independent of the full audit's read algorithm.
  const auto one_copy_bytes=layout.stored_payload_bytes/2;
  require(layout.mirrored_payloads && open_bytes<one_copy_bytes && lookup_bytes<one_copy_bytes,
      "indexed volume loaded whole namespace for lookup");
  std::optional<NativeDirectoryCursor> cursor;
  unsigned seen=0;
  do {
    const auto page=view.list_inode_page(2,cursor.value_or(NativeDirectoryCursor{}),29);
    for(const auto& entry:page.entries) {
      if(seen<files) {
        require(entry.path=="Data/"+file(seen) && entry.inode==3+seen &&
            view.read_inode(entry.inode,0,25)==payload(seen),"scaled native enumeration/readback");
      }else require(entry.inode==sparse && entry.logical_bytes==sparse_size,"large sparse inode metadata");
      ++seen;
    }
    cursor=page.next;
  }while(cursor);
  require(seen==files+1,"indexed directory lost a named file");
  const auto first=view.list_inode_page(2,{},1);
  require(first.next.has_value(),"missing indexed snapshot cursor");
  refuses(NativeFsErrorCode::invalid_operation,[&]{(void)published.list_inode_page(2,*first.next);});
  refuses(NativeFsErrorCode::corrupt,[&] {
    (void)NativeFilesystemSnapshot::open_image(ram,{NativeImageKind::paged,ram->capacity});
  });
  require(view.read_inode(sparse,127ULL*kNativeExtentBytes,17)==a &&
      view.read_inode(sparse,128ULL*kNativeExtentBytes,17)==b &&
      view.read_inode(sparse,last_block*kNativeExtentBytes,17)==c &&
      view.read_inode(sparse,sparse_size-3,8)==Bytes(3),"sparse block indexing or high-offset EOF");
  auto cross=Bytes(2);
  cross.insert(cross.end(),b.begin(),b.begin()+3);
  require(view.read_inode(sparse,128ULL*kNativeExtentBytes-2,5)==cross,"indexed range across sparse blocks");
  std::cout<<"indexed_volume files="<<files<<" image_bytes="<<view.storage_info().physical_end
      <<" open_read_bytes="<<open_bytes<<" lookup_read_bytes="<<lookup_bytes
      <<" scrub_read_bytes="<<scrub_bytes<<" sparse_size="<<sparse_size
      <<" scrub_read_calls="<<scrub_reads<<" object_tree_height="<<layout.height
      <<" one_copy_payload_bytes="<<one_copy_bytes<<'\n';

  if(files>1024)reopen();

  // Re-seal a semantically wrong owner through the ordinary object store.
  // Object integrity succeeds here, but the indexed reader must refuse it.
  const NativeNameIndex names(objects);
  const auto object=names.find(native_extent_key(3,0));
  require(object.has_value(),"indexed source extent not found");
  auto frame=objects.read(*object);frame.at(8)^=std::byte{2};
  {
    auto writer=PagedObjectStoreWriter::resume(ram,identity(),objects.info().generation,image_limits,raw);
    const StoreMutation wrong{*object,false,std::move(frame)};
    writer.commit({&wrong,1});
  }
  const auto malformed=NativeFilesystemSnapshot::open_image(ram,storage);
  refuses(NativeFsErrorCode::corrupt,[&]{(void)malformed.read_inode(3,0,17);});
  require(malformed.read_inode(4,0,17)==payload(1) && view.read_inode(3,0,17)==payload(0),
      "bad extent crossed ownership or changed an older root");
  refuses(NativeFsErrorCode::corrupt,[&]{malformed.verify_all();});
}

void indexed_unreachable_name_page() {
  auto image=empty();
  const auto contents=payload(943);
  {
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(image,identity(),limits,raw));
    writer.apply(std::array{file_create("document"),file_write("document",0,contents)});
  }
  const auto original=NativeFilesystemSnapshot::open_image(image,indexed);
  original.verify_all();
  const auto store=PagedObjectStoreSnapshot::open(image,limits);
  auto root=store.read(kNativeNameIndexObject,40);
  const auto field=[&](std::size_t offset) {
    std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)value|=std::to_integer<std::uint64_t>(root.at(offset+i))<<(8*i);
    return value;
  };
  const auto live_page=field(16),unused_ordinal=field(24);
  require(live_page && unused_ordinal && unused_ordinal<kNativeIndexedInodeLimit-1,
      "name page inventory fixture");
  const auto orphan=kNativeNameIndexObject|unused_ordinal;
  for(unsigned i=0;i<8;++i)root.at(24+i)=static_cast<std::byte>((unused_ordinal+1)>>(8*i));
  {
    auto writer=PagedObjectStoreWriter::resume(image,identity(),store.info().generation,limits,raw);
    const std::array changes{StoreMutation{kNativeNameIndexObject,false,std::move(root)},
        StoreMutation{orphan,false,store.read(live_page,4096)}};
    writer.commit(changes);
  }
  const auto protected_objects=PagedObjectStoreSnapshot::open(image,limits);
  protected_objects.verify_all();
  const auto with_orphan=NativeFilesystemSnapshot::open_image(image,indexed);
  refuses(NativeFsErrorCode::corrupt,[&]{NativeNameIndex(protected_objects).verify_all();});
  refuses(NativeFsErrorCode::corrupt,[&]{with_orphan.verify_all();});
  require(original.read("document",0,contents.size())==contents &&
      with_orphan.read("document",0,contents.size())==contents,"orphan changed selected payload bytes");
  std::cout<<"indexed_name_page_inventory protected_orphan_refused=1 earlier_snapshot_exact=1\n";
}

void compare(const NativeFilesystemSnapshot& source,const NativeFilesystemSnapshot& indexed_copy) {
  require(source.sequence()==indexed_copy.sequence() && source.workload_info()==indexed_copy.workload_info(),
      "migration changed namespace sequence or accounting");
  const auto source_dir=source.stat("A"),target_dir=indexed_copy.stat("A");
  require(source_dir.inode==target_dir.inode && indexed_copy.list().size()==source.list().size(),
      "migration changed parent or root names");
  for(const std::string_view name:{"A/file.bin","target.bin","stable.bin"}) {
    const auto before=source.stat(name),after=indexed_copy.stat(name);
    require(before.inode==after.inode && before.parent==after.parent &&
        before.created_sequence==after.created_sequence && before.modified_sequence==after.modified_sequence &&
        before.path==after.path && before.logical_bytes==after.logical_bytes &&
        source.basic_metadata(before.inode)==indexed_copy.basic_metadata(after.inode) &&
        source.security_descriptor(before.inode)==indexed_copy.security_descriptor(after.inode) &&
        source.allocation_info(before.inode).reserved_bytes==indexed_copy.allocation_info(after.inode).reserved_bytes &&
        source.read(name,0,kNativeMaxReadBytes)==indexed_copy.read(name,0,kNativeMaxReadBytes),
        "migration changed a file, allocation or metadata");
  }
}

void migration(bool paged) {
  auto medium=empty();
  auto old=paged?NativeFilesystemWriter::initialize_shared_bases(
      PagedObjectStoreWriter::create_mirrored(medium,identity(),limits,raw)):
      NativeFilesystemWriter::initialize(ObjectStoreWriter::create(medium,identity(),raw));
  old.enable_workload_accounting();
  old.apply(baseline());
  file_apply(old,file_write("A/file.bin",kNativeExtentBytes-2,payload(12)));
  const auto target_inode=old.inspect_path("target.bin").info.inode;
  old.set_security_descriptor(target_inode,native_security_fixture::descriptor());
  old.reserve_inode_result(target_inode,8192);
  for(std::uint64_t i=1;i<=12;++i)old.set_basic_metadata(target_inode,{i,i,i,i,0x20});
  constexpr unsigned grouped_files=6;
  if(paged) {
    std::vector<NativeExtentSelection> selections;
    const auto base=data(kNativeExtentBytes,215);
    for(unsigned i=0;i<grouped_files;++i) {
      const auto path="grouped-"+std::to_string(i);
      auto bytes=base;bytes[i*731]^=std::byte{47};
      const auto result=old.apply(std::array{file_create(path),file_write(path,0,std::move(bytes))});
      selections.push_back({result.created_inodes.at(0),0});
    }
    const auto result=old.compress_shared_base(selections.front(),selections);
    require(result.applied && result.members==grouped_files,"legacy group migration fixture");
  }
  const auto source=old.snapshot();
  const auto source_bytes=medium->stable;
  const auto source_writes=medium->writes;
  auto target=empty();
  const auto compression=paged?StoreCompressionOptions{StoreCompressionMode::automatic,6,false}:raw;
  const auto copy=source.create_indexed_copy(target,identity_for(88),limits,compression);
  const auto operations=target->calls;
  copy.verify_all();
  compare(source,copy);
  require(medium->stable==source_bytes && medium->writes==source_writes &&
      copy.identity()!=source.identity(),"migration altered its source");
  if(paged) {
    require(source.shared_base_info().members==grouped_files,"legacy copy skipped shared-base source");
    for(unsigned i=0;i<grouped_files;++i) {
      const auto path="grouped-"+std::to_string(i);
      require(copy.stat(path).inode==source.stat(path).inode &&
          copy.read(path,0,kNativeExtentBytes)==source.read(path,0,kNativeExtentBytes),
          "indexed migration lost CPU-decoded shared contents");
    }
  }
  {
    const auto published=copy.storage_info().generation;
    auto candidate=image(target->stable,4096);
    auto resumed=NativeFilesystemWriter::resume(
        PagedObjectStoreWriter::resume(candidate,copy.identity(),published,limits,compression));
    const auto amount=2*kNativeExtentBytes+6U;
    file_apply(resumed,file_truncate("target.bin",amount));
    const auto info=resumed.inspect_allocation(target_inode);
    require(info.info.logical_bytes==amount && info.reserved_bytes==2*kNativeExtentBytes+kNativeAllocationUnit &&
        info.info.basic==copy.basic_metadata(target_inode) &&
        resumed.inspect_security_descriptor(target_inode)==copy.security_descriptor(target_inode),
        "first indexed transaction lost imported allocation or metadata");
    require(resumed.read_inode(target_inode,0,31)==copy.read_inode(target_inode,0,31) &&
        resumed.read_inode(target_inode,amount-6,6)==Bytes(6) && copy.stat("target.bin").logical_bytes==31,
        "first indexed transaction lost migrated input or changed its source");
    resumed.snapshot().verify_all();
  }
  // Faults in the synthetic backend cover every write and flush. The only
  // native namespace this image may expose is the complete verified copy.
  if(paged)return;
  unsigned incomplete=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<operations;++cut) {
    auto failed=empty();failed->failure=mode;failed->fail_at=cut;
    refuses(NativeFsErrorCode::storage_failure,[&] {
      (void)source.create_indexed_copy(failed,identity_for(88),limits,raw);
    });
    auto recovered=image(failed->stable,4096);
    std::optional<NativeFilesystemSnapshot> published;
    try { published.emplace(NativeFilesystemSnapshot::open_image(recovered,indexed)); }
    catch(const std::runtime_error&) {}
    if(published){published->verify_all();compare(source,*published);++complete;}
    else ++incomplete;
    require(medium->stable==source_bytes && medium->writes==source_writes,"faulted migration altered source");
  }
  require(incomplete>0 && complete>0,"interruption matrix did not reach both publication states");
  std::cout<<"indexed_import_faults="<<complete+incomplete<<" unpublished="<<incomplete
      <<" verified_published="<<complete<<'\n';
}

NativeFilesystemWriter new_writer(const std::shared_ptr<Ram>& ram,PagedStoreLimits capacity=limits) {
  return NativeFilesystemWriter::initialize_indexed(
      PagedObjectStoreWriter::create_mirrored(ram,identity(),capacity,raw));
}

NativeFilesystemWriter resume_writer(const std::shared_ptr<Ram>& ram,std::uint64_t generation) {
  return NativeFilesystemWriter::resume(
      PagedObjectStoreWriter::resume(ram,identity(),generation,limits,raw));
}

void indexed_query_page_lifetime() {
  auto backing=empty();
  auto writer=new_writer(backing);
  const auto base=data(kNativeExtentBytes,2067);
  auto expected=base;
  expected.insert(expected.end(),base.begin(),base.end());
  expected[kNativeExtentBytes+721]^=std::byte{17};
  const auto created=writer.apply(std::array{file_create("D",true),file_create("D/E",true),
      file_create("D/E/file"),file_write("D/E/file",0,expected),
      file_create("D/E/peer"),file_write("D/E/peer",0,base)});
  const auto file=created.created_inodes.at(2),peer=created.created_inodes.at(3);
  const std::array<NativeExtentSelection,3> group{{{file,0},{file,1},{peer,0}}};
  require(writer.compress_shared_base(group.front(),group).members==group.size(),
      "scoped query fixture must read shared-base references");

  const auto snapshot=writer.snapshot();
  const auto index=PagedObjectStoreSnapshot::open(backing,limits);
  const auto descriptor=index.read(kNativeNameIndexObject,40);
  std::uint64_t page=0;
  for(unsigned i=0;i<8;++i)page|=std::to_integer<std::uint64_t>(descriptor[16+i])<<(8*i);
  const auto frame=index.read(page,4096);
  const auto prefix=std::span(frame).first(40);
  auto& bytes=backing->live;
  const auto primary=std::ranges::search(bytes,prefix).begin();
  require(primary!=bytes.end(),"scoped query fixture did not locate its name page");
  const auto start=static_cast<std::size_t>(primary-bytes.begin());
  const auto replica=start+backing->failure_unit;
  require(replica+prefix.size()<=bytes.size() &&
      std::ranges::equal(std::span(bytes).subspan(replica,prefix.size()),prefix),
      "scoped query fixture did not locate the second name page");
  backing->corrupt_read_offset=start;
  constexpr auto length=static_cast<std::uint32_t>(2*kNativeExtentBytes);
  require(snapshot.read("D/E/file",0,length)==expected && backing->matching_reads==1,
      "independent read should verify the name page once across path and grouped extents");
  require(writer.read_inode(file,0,length)==expected && backing->matching_reads==2,
      "foreground read should verify the name page once across grouped extents");

  refuses(NativeFsErrorCode::not_found,[&]{(void)snapshot.read("D/E/missing",0,1);});
  require(backing->matching_reads==3,"a later query reused a previously verified page");
  bytes[start]^=std::byte{1};
  require(snapshot.read("D/E/file",0,length)==expected && snapshot.storage_info().degraded_checkpoint,
      "cache survived a failed query or hid a single damaged name page");
  bytes[replica]^=std::byte{1};
  refuses(NativeFsErrorCode::storage_failure,[&]{(void)snapshot.read("D/E/file",0,length);});
  bool scrub_refused=false;
  try {snapshot.verify_all();}
  catch(const std::runtime_error&) {scrub_refused=true;}
  require(scrub_refused,"independent scrub accepted two damaged name page copies");
  std::cout<<"indexed_query_pages verified_once=true cache_released_after_error=true"
      <<" survivor_verified=true both_copies_rejected=true\n";
}

void indexed_extent_advice() {
  constexpr std::uint64_t block=1ULL<<20,offset=block*kNativeExtentBytes;
  constexpr std::uint32_t header_bytes=24;
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  const auto plain=native_lz4_fixture::source();
  require(plain.back()!=std::byte{},"indexed advice fixture must retain a complete extent");
  const auto alternative=native_lz4_fixture::alternative_block(plain);
  const NativeLz4ExtentCandidate valid{block,alternative};
  const auto run=[&](std::span<const std::byte> bytes,std::span<const NativeLz4ExtentCandidate> candidates,
      StoreCompressionOptions compression) {
    auto ram=empty();
    auto writer=NativeFilesystemWriter::initialize_indexed(
        PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,compression));
    const auto created=file_create("target");
    writer.apply({&created,1});
    auto session=NativeFileSession::create(std::move(writer));
    const auto handle=session.open("target",{true,true,true});
    const auto prior=session.snapshot();
    const auto written=session.write_storage_with_lz4_result(handle,offset,bytes,candidates);
    require(written.offset==offset && written.bytes==bytes.size() &&
        written.state.info.logical_bytes==offset+bytes.size(),"indexed advice changed committed write facts");
    session.cleanup(handle,false);
    session.close(handle);
    const auto reopened=NativeFilesystemSnapshot::open_image(ram,indexed);
    reopened.verify_all();
    require(reopened.read("target",offset,static_cast<std::uint32_t>(bytes.size()))==Bytes(bytes.begin(),bytes.end()) &&
        prior.read("target",offset,1).empty(),"indexed advice changed current or retained bytes");
    const auto objects=PagedObjectStoreSnapshot::open(ram,limits);
    const auto extent=objects.stat(kNativeIndexedDataObject|1);
    require(extent && extent->logical_bytes==header_bytes+bytes.size(),"indexed advice changed data framing");
    return std::pair{extent->stored_payload_bytes,std::move(ram->stable)};
  };

  const auto baseline=run(plain,{},fast);
  const auto advised=run(plain,{&valid,1},fast);
  if(store_lz4_available()) {
    require(advised.first==alternative.size()+header_bytes && advised.first!=baseline.first,
        "indexed store did not select the independently supplied LZ4 representation");
  }else require(advised==baseline,"missing LZ4 changed CPU fallback");
  const auto malformed=std::array{std::byte{0xf0},std::byte{0xff}};
  const NativeLz4ExtentCandidate invalid{block,malformed},unrelated{block+1,alternative};
  const std::array duplicate{valid,NativeLz4ExtentCandidate{block,{}}};
  require(run(plain,{&invalid,1},fast)==baseline && run(plain,{&unrelated,1},fast)==baseline &&
      run(plain,duplicate,fast)==baseline,"invalid, misaddressed or ambiguous indexed advice changed CPU bytes");
  auto changed=plain;
  changed[8193]^=std::byte{0x21};
  require(run(changed,{&valid,1},fast)==run(changed,{},fast) &&
      run(plain,{&valid,1},raw)==run(plain,{},raw),
      "stale indexed advice or raw mode changed the accepted CPU representation");
  const auto partial=std::span<const std::byte>(plain).first(plain.size()-3);
  require(run(partial,{&valid,1},fast)==run(partial,{},fast),"full extent advice overrode a partial indexed extent");
  std::cout<<"indexed_lz4_advice decoder="<<store_lz4_available()<<" offset="<<offset
      <<" CPU_payload="<<baseline.first<<" advised_payload="<<advised.first<<'\n';
}
void indexed_extent_read_route() {
  constexpr StoreCompressionOptions fast{StoreCompressionMode::fast,1,false};
  auto ram=empty();
  auto writer=NativeFilesystemWriter::initialize_indexed(
      PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,fast));
  const auto created=file_create("read-target");
  writer.apply({&created,1});
  auto session=NativeFileSession::create(std::move(writer));
  const auto reader=session.open("read-target",{true,true,true});
  const auto plain=native_lz4_fixture::source();
  const auto encoded=native_lz4_fixture::alternative_block(plain);
  const NativeLz4ExtentCandidate hint{1,encoded};
  const auto offset=kNativeExtentBytes;
  (void)session.write_storage_with_lz4_result(reader,offset,plain,{&hint,1});

  struct ReadyRoute final:detail::NativeLz4ReadRoute {
    std::optional<Bytes> ready;
    detail::NativeLz4ReadKey key{};
    std::size_t offers=0,rejections=0,encoded_bytes=0;
    std::optional<Bytes> try_ready(const detail::NativeLz4ReadKey& offered,
        std::span<const std::byte> bytes) noexcept override {
      key=offered;
      ++offers;
      encoded_bytes=bytes.size();
      return std::exchange(ready,std::nullopt);
    }
    void reject(const detail::NativeLz4ReadKey& rejected) noexcept override {
      if(rejected==key)++rejections;
    }
  } route;

  require(session.read(reader,0,8,&route)==Bytes(8) && route.offers==0,
      "sparse reads offered backing bytes");
  require(session.read(reader,offset,static_cast<std::uint32_t>(plain.size()),&route)==plain,
      "unavailable route changed the CPU read");
  if(!store_lz4_available()) {
    require(route.offers==0,"codec-free recovery offered LZ4");
    return;
  }

  const auto store=PagedObjectStoreSnapshot::open(ram,limits);
  const auto frame=store.read(kNativeIndexedDataObject|1);
  require(route.offers==1 && route.key.image==identity() && route.key.object==(kNativeIndexedDataObject|1) &&
      route.key.generation>0 && route.key.decoded_bytes==frame.size() &&
      route.key.decoded_digest==detail::record_digest(frame) && route.encoded_bytes==encoded.size()+24,
      "wrong exact native LZ4 frame binding");
  const auto before=route.key;
  route.ready=frame;
  require(session.read(reader,offset-3,7,&route)==Bytes({std::byte{},std::byte{},std::byte{},
      plain[0],plain[1],plain[2],plain[3]}) && route.offers==2 && route.rejections==0,
      "correct ready frame or sparse-bounded range failed");

  route.ready=Bytes(1);
  require(session.read(reader,offset,1,&route)==Bytes{plain[0]} && route.rejections==1,
      "short completion changed CPU fallback");
  route.ready=frame;
  route.ready->back()^=std::byte{1};
  require(session.read(reader,offset,1,&route)==Bytes{plain[0]} && route.rejections==2,
      "wrong bytes outside the requested range bypassed decoded verification");

  const std::byte after=plain.front()^std::byte{1};
  (void)session.write(reader,offset,{&after,1});
  route.ready=frame;
  require(session.read(reader,offset,1,&route)==Bytes{after} && route.rejections==3 && route.key!=before,
      "late frame from the old object generation changed the current file");
  const auto offers=route.offers;
  require(session.read(reader,offset+plain.size(),1,&route).empty() && route.offers==offers,
      "end of file offered a decode");
  session.snapshot().verify_all();
  std::cout<<"indexed_ready_read offers="<<route.offers<<" rejected="<<route.rejections
      <<" CPU_fallback_exact=1\n";
}
void growing_native_writer() {
  using Error=NativeFsErrorCode;
  constexpr unsigned files=1024,batch=32;
  auto ram=empty();
  auto writer=new_writer(ram);
  require(writer.image_kind()==NativeImageKind::indexed_paged&&writer.snapshot().sequence()==1,
      "indexed writer did not initialize a clean empty volume");
  file_apply(writer,file_create("Data",true));
  const auto before=writer.snapshot();
  for(unsigned first=0;first<files;first+=batch) {
    std::vector<NativeFileMutation> operations;
    operations.reserve(batch*2);
    for(unsigned i=first;i<first+batch;++i) {
      const auto path="Data/"+file(i);
      operations.push_back(file_create(path));
      operations.push_back(file_write(path,0,payload(i)));
    }
    require(writer.apply(operations).created_inodes.size()==batch,"indexed writer lost a batch creation");
  }
  require(before.list_inode_page(2).entries.empty(),"indexed writer changed a retained empty directory");
  const auto written=writer.snapshot();
  const auto scrub_start=ram->read_bytes;
  written.verify_all();
  const auto scrub_bytes=ram->read_bytes-scrub_start;
  const auto update_start=ram->read_bytes;
  const auto update=file_write("data/"+file(files-1),0,payload(3000));
  const auto append=writer.apply({&update,1});
  const auto update_bytes=ram->read_bytes-update_start;
  require(update_bytes<scrub_bytes/16,"single path write read the complete indexed volume");
  require(written.read_inode(2+files,0,17)==payload(files-1) &&
      writer.read_inode(2+files,0,17)==payload(3000),"indexed update changed preceding snapshot bytes");
  auto current=writer.snapshot();
  unsigned seen=0;
  std::optional<NativeDirectoryCursor> cursor;
  do {
    const auto page=current.list_inode_page(2,cursor.value_or(NativeDirectoryCursor{}),41);
    for(const auto& entry:page.entries) {
      const auto expected_index=seen+1==files?3000:seen;
      require(entry.path=="Data/"+file(seen) && entry.inode==3+seen &&
          current.read_inode(entry.inode,0,100)==payload(expected_index),"indexed writer enumeration or byte readback");
      ++seen;
    }
    cursor=page.next;
  }while(cursor);
  require(seen==files && current.allocation_usage().live_extents==files &&
      writer.workload_info().committed_input_bytes==(files+1)*17 &&
      writer.flush().sequence==append.sequence,"scaled indexed writer accounting or flush");
  const auto stable=ram->stable;
  auto isolated=image(stable,4096);
  auto reopened=resume_writer(isolated,append.storage.generation);
  const auto fresh=file_apply(reopened,file_create("resumed"));
  require(fresh.created_inodes==std::vector<std::uint64_t>{3+files} &&
      reopened.inspect_path("DATA/"+file(files-1)).info.inode==2+files,
      "resumed indexed writer lost the allocator or prior names");

  constexpr auto high=(std::numeric_limits<std::uint64_t>::max)()-7;
  const auto high_data=payload(444);
  const auto sequence=
      reopened.apply(std::array{file_write("resumed",high-high_data.size(),high_data)}).sequence;
  require(reopened.read_inode(fresh.created_inodes.front(),high-high_data.size(),100)==high_data &&
      reopened.inspect_allocation(fresh.created_inodes.front()).info.logical_bytes==high,
      "indexed write at high offset did not preserve sparse EOF");
  const auto calls=isolated->calls;
  refuses(Error::limit,[&] {
    file_apply(reopened,file_write("resumed",high,high_data));
  });
  refuses(Error::not_empty,[&]{file_apply(reopened,file_remove("Data"));});
  refuses(Error::invalid_operation,[&] {
    file_apply(reopened,file_rename("Data","Data/cycle"));
  });
  const std::array rollback{file_create("rollback"),file_write("missing",0,payload(123))};
  refuses(Error::not_found,[&]{reopened.apply(rollback);});
  require(isolated->calls==calls && reopened.snapshot().sequence()==sequence && !reopened.poisoned(),
      "rejected indexed transaction reached backing or fenced healthy writer");
  refuses(Error::not_found,[&]{(void)reopened.inspect_path("rollback");});
  const std::array reset{file_truncate("resumed",high-8),file_truncate("resumed",high),
      file_truncate("resumed",3),file_truncate("resumed",high),
      file_rename("Data","Longer-Directory")};
  reopened.apply(reset);
  require(reopened.read_inode(fresh.created_inodes.front(),high-high_data.size(),100)==Bytes(high_data.size()) &&
      reopened.inspect_path("longer-directory/"+file(0)).info.inode==3,
      "indexed shrink/re-extension leaked bytes or directory rename lost descendants");
  reopened.snapshot().verify_all();
  require(ram->stable==stable,"isolated resume changed the original backing");
  std::cout<<"indexed_writer files="<<files<<" image_bytes="<<current.storage_info().physical_end
      <<" single_write_read_bytes="<<update_bytes<<" full_scrub_read_bytes="<<scrub_bytes<<'\n';
}

void indexed_capacity_and_metadata() {
  using Error=NativeFsErrorCode;
  auto ram=empty();
  auto limited=new_writer(ram,{512U*1024});
  const auto created=file_apply(limited,file_create("large"));
  const auto calls=ram->calls;
  refuses(Error::limit,[&]{file_apply(limited,file_write("large",0,data(8*kNativeExtentBytes,13)));});
  require(ram->calls==calls && !limited.poisoned() && limited.snapshot().sequence()==created.sequence,
      "indexed capacity refusal crossed publication or fenced a healthy writer");
  file_apply(limited,file_write("large",0,payload(3)));
  require(limited.snapshot().read("large",0,17)==payload(3),"indexed writer did not continue after capacity refusal");

  auto roomy=empty();
  auto writer=new_writer(roomy);
  const auto contents=data(2*kStoreMaxObjectBytes+17,47);
  auto write=file_write("wide",0,contents);
  const NativeBasicMutation basic{0,1,{11,22,33,44,0x20}};
  const NativeSecurityMutation security{0,1,native_security_fixture::descriptor()};
  const std::array create{file_create("wide"),std::move(write)};
  const auto result=writer.apply_with_metadata(create,{&basic,1},{&security,1});
  const auto id=result.created_inodes.front();
  require(writer.read_inode(id,0,kNativeMaxReadBytes)==contents &&
      writer.inspect_basic_metadata(id)==basic.value && writer.inspect_security_descriptor(id)==security.value,
      "indexed writer did not publish a multimegabyte input and metadata together");
  const auto no_op=writer.enable_workload_accounting();
  require(no_op.sequence==result.sequence && no_op.storage.generation==result.storage.generation,
      "indexed accounting enable published an unchanged root");

  const std::string long_dir(99,'d'),long_leaf(99,'f');
  const std::string old="p/"+long_dir;
  writer.apply(std::array{file_create("p",true),file_create(old,true),file_create(old+"/"+long_leaf)});
  const std::string longer(99,'q');
  file_apply(writer,file_rename("p",longer));
  const auto enlarged=longer+"/"+long_dir+"/"+long_leaf;
  require(writer.inspect_path(enlarged).info.logical_bytes==0,
      "directory growth lost the indexed descendant");
  writer.snapshot().verify_all();

  auto tampered=image(roomy->stable,4096);
  const auto objects=PagedObjectStoreSnapshot::open(tampered,limits);
  auto root=objects.read(1);
  require(root.at(92)==std::byte{1},"expanding directory rename did not promote the name profile");
  root.at(92)=std::byte{0};
  auto store=PagedObjectStoreWriter::resume(tampered,identity(),objects.info().generation,limits,raw);
  const StoreMutation downgraded{1,false,std::move(root)};
  store.commit({&downgraded,1});
  refuses(Error::corrupt,[&]{(void)NativeFilesystemSnapshot::open_image(tampered,indexed).stat(enlarged);});
}

void indexed_directory_moves() {
  using Error=NativeFsErrorCode;
  constexpr unsigned files=384,batch=32;
  const std::string expanded="directory-with-a-longer-name";
  const auto contents=payload(101);
  const auto from_path=[](unsigned i){return "tree/branch/"+file(i);};
  auto old_medium=empty(),medium=empty();
  IndexedVolumeBuilder legacy(PagedObjectStoreWriter::create_mirrored(old_medium,identity(),limits,raw),
      1,files+4,{true,0},2);
  const std::array<IndexedInodeInput,3> dirs{{{1,0,0,1,1,true,""},{2,1,0,1,1,true,"tree"},
      {3,2,0,1,1,true,"branch"}}};
  legacy.append(dirs);
  auto modern=new_writer(medium);
  modern.apply(std::array{file_create("tree",true),file_create("tree/branch",true)});
  for(unsigned first=0;first<files;first+=batch) {
    std::vector<IndexedInodeInput> inodes;
    std::vector<NativeFileMutation> create;
    for(unsigned i=first;i<first+batch;++i) {
      inodes.push_back({i+4,3,i?0:contents.size(),1,1,false,file(i)});
      create.push_back(file_create(from_path(i)));
    }
    if(!first) {
      const IndexedExtentInput extent{4,0,contents};
      legacy.append(inodes,{&extent,1});
      create.push_back(file_write(from_path(0),0,contents));
    } else legacy.append(inodes);
    modern.apply(create);
  }
  const auto legacy_view=legacy.finish();
  auto old=resume_writer(old_medium,legacy_view.storage_info().generation);
  const auto measure=[&](auto& writer,const auto& backing) {
    const auto reads=backing->read_calls,bytes=backing->read_bytes;
    file_apply(writer,file_rename("tree",expanded));
    const auto cost=std::pair{backing->read_calls-reads,backing->read_bytes-bytes};
    const auto view=writer.snapshot();
    require(view.stat(expanded+"/branch/"+file(0)).inode==4 &&
        view.read(expanded+"/branch/"+file(0),0,100)==contents,"indexed subtree move changed identity or bytes");
    view.verify_all();
    return cost;
  };
  const auto prior_cost=measure(old,old_medium);
  const auto current_cost=measure(modern,medium);
  std::cout<<"indexed_directory_move descendants="<<files+1<<" old_read_calls="<<prior_cost.first
      <<" new_read_calls="<<current_cost.first<<" old_read_bytes="<<prior_cost.second
      <<" new_read_bytes="<<current_cost.second<<'\n';
  require(current_cost.first*4<prior_cost.first && current_cost.second*2<prior_cost.second,
      "indexed measured directory move did not reduce descendant reads");

  // Moving an older subtree into a measured one invalidates its dependent bounds.
  old.apply(std::array{file_create("holder",true),file_create("holder/nested",true),
      file_rename(expanded,"holder/nested/tree-with-a-longer-name-than-before")});
  auto mixed=old.snapshot();
  mixed.verify_all();
  require(mixed.read("holder/nested/tree-with-a-longer-name-than-before/branch/"+file(0),0,100)==contents,
      "indexed mixed-format move lost old subtree data");

  const auto dir1=expanded+"/"+std::string(255,'x');
  const auto dir2=dir1+"/"+std::string(255,'y');
  const auto dir3=dir2+"/"+std::string(255,'z');
  const auto leaf_units=1022-dir3.size()-1;
  const auto left=dir3+"/"+std::string(leaf_units,'a');
  const auto right=dir3+"/"+std::string(leaf_units,'b');
  modern.apply(std::array{file_create(dir1,true),file_create(dir2,true),file_create(dir3,true),
      file_create(left),file_create(right)});
  const auto grows=[&] {file_apply(modern,file_rename(expanded,expanded+"x"));};
  const auto reject_growth=[&] {
    const auto writes=medium->calls;
    refuses(Error::invalid_path,grows);
    require(medium->calls==writes && !modern.poisoned(),"indexed summary overflow reached publication");
  };
  reject_growth();
  file_apply(modern,file_remove(left));
  reject_growth();
  file_apply(modern,file_remove(right));
  grows();
  std::string deep="deep";
  file_apply(modern,file_create(deep,true));
  for(unsigned i=1;i<16;++i) {
    deep+="/d";
    file_apply(modern,file_create(deep,true));
  }
  refuses(Error::invalid_path,[&]{file_apply(modern,file_rename("deep",expanded+"x/deep"));});
  modern.snapshot().verify_all();

  auto tampered=image(medium->stable,4096);
  const auto objects=PagedObjectStoreSnapshot::open(tampered,limits);
  const IndexedVolumeView view(objects,objects.read(1));
  auto root=view.inode(1);
  root.shape->replace({},IndexedPathBound{1,1});
  auto store=PagedObjectStoreWriter::resume(tampered,identity(),objects.info().generation,limits,raw);
  const StoreMutation forged{(1ULL<<63)|1,false,encode_indexed_inode(root,view.root())};
  store.commit({&forged,1});
  const auto bad=NativeFilesystemSnapshot::open_image(tampered,indexed);
  refuses(Error::corrupt,[&]{bad.verify_all();});

  auto unreachable=empty();
  IndexedVolumeBuilder cyclic(PagedObjectStoreWriter::create_mirrored(unreachable,identity(),limits,raw),
      1,4,{true,0},2);
  const std::array<IndexedInodeInput,3> island{{{1,0,0,1,1,true,""},{2,3,0,1,1,true,"left"},
      {3,2,0,1,1,true,"right"}}};
  cyclic.append(island);
  refuses(Error::corrupt,[&]{(void)cyclic.finish();});
}

void indexed_directory_backfill() {
  using Step=NativeDirectoryBackfillStep;
  using Error=NativeFsErrorCode;
  constexpr unsigned files=130;
  const auto contents=payload(314);
  auto medium=empty();
  IndexedVolumeBuilder legacy(PagedObjectStoreWriter::create_mirrored(medium,identity(),limits,raw),
      1,files+5,{true,0},2);
  std::vector<IndexedInodeInput> inodes{{1,0,0,1,1,true,""},{2,1,0,1,1,true,"old"},
      {3,2,0,1,1,true,"nested"},{files+4,3,contents.size(),1,1,false,"member"}};
  for(unsigned i=0;i<files;++i)inodes.push_back({i+4,2,0,1,1,false,file(i)});
  const IndexedExtentInput extent{files+4,0,contents};
  legacy.append(inodes,{&extent,1});
  const auto before=legacy.finish();
  auto writer=resume_writer(medium,before.storage_info().generation);
  const auto initial_writes=medium->calls,initial_reads=medium->read_calls;
  require(writer.directory_backfill_pending() && medium->read_calls==initial_reads,"backfill demand reads storage");
  require(writer.advance_directory_backfill()==Step::scanning && writer.advance_directory_backfill()==Step::scanning &&
      medium->calls==initial_writes,"backfill scan produced an incomplete publication");
  file_apply(writer,file_rename("old/"+file(0),"old/a longer member name"));
  Step step=Step::scanning;
  for(unsigned i=0;i<20 && step==Step::scanning;++i)step=writer.advance_directory_backfill();
  require(step==Step::published && writer.directory_backfill_pending(),"backfill did not publish the first child");
  const auto durable=writer.snapshot();
  durable.verify_all();

  auto restarted=image(medium->stable,4096);
  auto resumed=resume_writer(restarted,durable.storage_info().generation);
  unsigned published=0,steps=0;
  while(resumed.directory_backfill_pending() && ++steps<30) {
    const auto progress=resumed.advance_directory_backfill();
    require(progress==Step::scanning || progress==Step::published,"reopened backfill stopped early");
    published+=progress==Step::published;
  }
  require(published==2 && !resumed.directory_backfill_pending(),"restart repeated a durable child summary");
  const auto reads=restarted->read_calls;
  require(resumed.advance_directory_backfill()==Step::idle && restarted->read_calls==reads,
      "finished directory backfill performed backing reads");
  file_apply(resumed,file_rename("old","directory-with-a-longer-prefix"));
  const auto final=resumed.snapshot();
  final.verify_all();
  require(final.stat("directory-with-a-longer-prefix/a longer member name").inode==4 &&
      final.read("directory-with-a-longer-prefix/nested/member",0,100)==contents &&
      durable.read("old/nested/member",0,100)==contents,
      "backfill or resumed rename changed acknowledged bytes or live identity");

  auto power_source=empty();
  IndexedVolumeBuilder root_only(PagedObjectStoreWriter::create_mirrored(power_source,identity(),limits,raw),
      1,3,{true,0},2);
  const std::array<IndexedInodeInput,2> small{{{1,0,0,1,1,true,""},{2,1,contents.size(),1,1,false,"data"}}};
  const IndexedExtentInput root_extent{2,0,contents};
  root_only.append(small,{&root_extent,1});
  const auto selected=root_only.finish().storage_info();
  const auto stable=power_source->stable;
  auto good=image(stable,4096);
  auto completed=resume_writer(good,selected.generation);
  require(completed.advance_directory_backfill()==Step::scanning,"backfill did not prepare publication");
  const auto before_calls=good->calls;
  require(completed.advance_directory_backfill()==Step::published,"backfill root was not published");
  const auto boundaries=good->calls-before_calls;
  unsigned prior=0,committed=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut) {
    auto target=image(stable,4096);
    auto interrupted=resume_writer(target,selected.generation);
    require(interrupted.advance_directory_backfill()==Step::scanning,"backfill fault setup");
    target->failure=mode;target->fail_at=target->calls+cut;
    refuses(Error::storage_failure,[&]{(void)interrupted.advance_directory_backfill();});
    require(interrupted.poisoned(),"uncertain directory summary publication left the writer active");
    const auto recovered=NativeFilesystemSnapshot::open_image(image(target->stable,4096),indexed);
    recovered.verify_all();
    require(recovered.read("data",0,100)==contents,"summary recovery lost acknowledged payload");
    if(recovered.sequence()==1)++prior;
    else if(recovered.sequence()==2)++committed;
    else throw std::runtime_error("summary recovery selected a partial transaction");
  }
  require(prior && committed,"backfill recovery did not exercise both atomic outcomes");

  auto full=image(stable,4096);
  auto limited=NativeFilesystemWriter::resume(PagedObjectStoreWriter::resume(full,identity(),selected.generation,
      {selected.physical_end},raw));
  require(limited.advance_directory_backfill()==Step::scanning,"full-volume summary scan rejected");
  const auto calls=full->calls;
  require(limited.advance_directory_backfill()==Step::capacity && !limited.poisoned() &&
      !limited.directory_backfill_pending() && limited.advance_directory_backfill()==Step::idle && full->calls==calls,
      "capacity refusal repeated or touched a healthy checkpoint");
  std::cout<<"indexed_directory_backfill restarted_publications="<<published<<" cuts="<<prior+committed
      <<" previous="<<prior<<" committed="<<committed<<'\n';
}

void indexed_windows_names() {
  using Error=NativeFsErrorCode;
  const auto utf8=[](std::u8string_view value){return std::string(reinterpret_cast<const char*>(value.data()),value.size());};
  const auto read_root=[](const auto& medium){return PagedObjectStoreSnapshot::open(medium,limits).read(1);};
  auto ram=empty();
  auto writer=new_writer(ram);
  file_apply(writer,file_create("Legacy"));
  const auto previous=image(ram->stable,4096);
  require(read_root(previous).at(92)==std::byte{0},"legacy-compatible names changed the root profile");

  const auto folder=utf8(u8"Caf\u00e9 \u2c62");
  const auto folder_alias=utf8(u8"CAF\u00c9 \u026b");
  const auto leaf=utf8(u8"\u2c62 \u6f22 \U0001f600.txt");
  const auto leaf_alias=utf8(u8"\u026b \u6f22 \U0001f600.TXT");
  const auto next=utf8(u8"\u767d.txt");
  const auto decomposed=utf8(u8"Cafe\u0301 \u2c62");
  const auto contents=payload(256);
  writer.apply(std::array{file_create(folder,true),file_create(folder_alias+"/"+leaf),
      file_write(folder_alias+"/"+leaf_alias,0,contents),file_create(folder+"/"+next),file_create(decomposed)});
  const auto directory=writer.inspect_path(folder).info.inode;
  const auto file_id=writer.inspect_path(folder_alias+"/"+leaf_alias).info.inode;
  require(writer.inspect_path(folder_alias+"/"+leaf_alias).info.path==folder+"/"+leaf &&
      writer.inspect_path(decomposed).info.inode!=directory && read_root(ram).at(92)==std::byte{1},
      "indexed Unicode aliases lost original spelling or collapsed distinct Unicode names");
  const auto listing=writer.snapshot();
  const auto first_page=listing.list_inode_page(directory,{},1);
  require(first_page.entries.size()==1 && first_page.entries.front().path==folder+"/"+leaf && first_page.next,
      "indexed first directory page changed the Unicode display name");
  const auto page=listing.list_inode_page(directory,*first_page.next,1);
  require(page.entries.size()==1 && page.entries.front().path==folder+"/"+next && !page.next,
      "indexed Unicode directory cursor repeated or skipped a name");
  writer.apply(std::array{file_rename(folder,folder_alias),file_rename(folder+"/"+leaf,folder+"/"+leaf_alias)});
  require(writer.inspect_path(folder+"/"+leaf).info.inode==file_id &&
      writer.inspect_path(folder+"/"+leaf).info.path==folder_alias+"/"+leaf_alias,
      "indexed case-only rename changed identity or truncated the display name");

  std::string full_name;
  for(unsigned i=0;i<255;++i)full_name+=utf8(u8"\u6f22");
  const auto first=full_name;
  const auto second=first+"/"+full_name;
  const auto third=second+"/"+full_name;
  const auto longest=third+"/"+full_name.substr(3);
  writer.apply(std::array{file_create(first,true),file_create(second,true),file_create(third,true),file_create(longest)});
  std::string supplementary;
  for(unsigned i=0;i<127;++i)supplementary+=utf8(u8"\U0001f600");
  file_apply(writer,file_create(supplementary+"a"));
  const auto calls=ram->calls;
  for(const auto& invalid:std::array{std::string("\xc0\xaf"),std::string("\xed\xa0\x80"),
        std::string("\xf4\x90\x80\x80"),std::string("a\0b",3),utf8(u8"COM\u00b9.txt"),
        utf8(u8"LpT\u00b2 .txt"),std::string("space "),std::string("a:b"),std::string("a/../b"),
        longest+"z",full_name+"z",supplementary+"ab"}) {
    refuses(Error::invalid_path,[&]{file_apply(writer,file_create(invalid));});
  }
  refuses(Error::already_exists,[&]{file_apply(writer,file_create(folder+"/"+leaf));});
  require(ram->calls==calls,"rejected Unicode publication modified backing storage");

  auto target=empty();
  auto candidate=writer.prepare_paged_recompression(target,identity_for(49),raw,false);
  unsigned steps=0;
  while(!writer.advance_paged_recompression(candidate))
    require(++steps<1000,"indexed Unicode rebuild did not finish");
  auto copy=writer.finish_paged_recompression(candidate);
  const auto restored=NativeFilesystemSnapshot::open_image(target,indexed);
  restored.verify_all();
  require(restored.read(folder+"/"+leaf,0,100)==contents && restored.list(third).front().path==longest &&
      read_root(target).at(92)==std::byte{1},"rebuild or CPU reopen lost the namespace profile or file bytes");
  const auto old=NativeFilesystemSnapshot::open_image(previous,indexed);
  old.verify_all();
  require(old.list().size()==1 && old.list().front().path=="Legacy","legacy root lost backward compatibility");

  auto frame=read_root(target);
  frame.at(92)=std::byte{2};
  const auto generation=restored.storage_info().generation;
  // Re-seal an unknown profile in an independent image.
  auto incompatible=image(target->stable,4096);
  auto store=PagedObjectStoreWriter::resume(incompatible,restored.identity(),generation,limits,raw);
  const StoreMutation unknown{1,false,std::move(frame)};
  store.commit({&unknown,1});
  refuses(Error::corrupt,[&]{(void)NativeFilesystemSnapshot::open_image(incompatible,indexed);});
  file_apply(copy,file_remove(folder+"/"+leaf));
  require(copy.snapshot().list(folder).size()==1 && restored.read(folder+"/"+leaf,0,100)==contents,
      "removing an aliased name affected a stable snapshot");
  std::cout<<"indexed_names Unicode UTF-16 limits and CPU recovery verified\n";
}

void indexed_stable_inode_io() {
  using Error=NativeFsErrorCode;
  auto ram=empty();
  auto writer=new_writer(ram);
  auto expected=payload(77);
  const auto unrelated=payload(78);
  const NativeBasicMutation basic{0,1,{11,22,33,44,2}};
  const std::array create{file_create("original"),file_write("original",0,expected)};
  const auto id=writer.apply_with_basic(create,{&basic,1}).created_inodes.front();
  const auto before=writer.snapshot();
  writer.apply(std::array{file_rename("original","renamed"),file_create("original"),
      file_write("original",0,unrelated)});

  const auto extra=data(9,546);
  const auto append=writer.write_inode_storage_result(id,(std::numeric_limits<std::uint64_t>::max)(),extra,
      {true,false},{501,true,false,true,true});
  require(append.offset==expected.size() && append.bytes==extra.size() &&
      append.state.info.inode==id && append.state.info.logical_bytes==expected.size()+extra.size() &&
      append.state.info.basic==NativeBasicMetadata{11,22,501,501,0x22},
      "indexed append did not return selected EOF and committed metadata");
  expected.insert(expected.end(),extra.begin(),extra.end());

  const auto replacement=data(5,999);
  const auto constrained=writer.write_inode_storage_result(id,expected.size()-2,replacement,{false,true});
  std::copy_n(replacement.begin(),2,expected.end()-2);
  require(constrained.bytes==2 && constrained.state.info.logical_bytes==expected.size() &&
      writer.read_inode(id,0,100)==expected && writer.snapshot().read("original",0,100)==unrelated,
      "indexed constrained write extended EOF or changed a reused name");
  const auto stable=ram->calls;
  const auto no_op=writer.write_inode_storage_result(id,(std::numeric_limits<std::uint64_t>::max)(),
      replacement,{false,true},{502,true,false,true,true});
  require(no_op.bytes==0 && no_op.transaction.sequence==constrained.transaction.sequence &&
      no_op.state.info.basic==constrained.state.info.basic && ram->calls==stable,
      "indexed constrained no-op committed data or metadata");

  constexpr auto high=(std::numeric_limits<std::uint64_t>::max)()-7;
  const auto truncate=writer.truncate_inode_storage_result(id,high,{503,false,true,false,false});
  require(truncate.state.info.logical_bytes==high && truncate.state.info.basic->last_access_time==503 &&
      writer.read_inode(id,high-5,25)==Bytes(5),"indexed truncate did not publish sparse EOF and metadata");
  writer.truncate_inode(id,5);
  writer.truncate_inode(id,expected.size());
  expected.resize(5);
  expected.resize(append.state.info.logical_bytes);
  require(writer.read_inode(id,0,100)==expected && before.read_inode(id,0,100)==payload(77),
      "indexed stable inode writes exposed discarded tails or altered pinned bytes");

  file_apply(writer,file_remove("renamed"));
  const auto call_count=ram->calls;
  refuses(Error::not_found,[&]{(void)writer.write_inode_storage_result(id,0,replacement);});
  require(ram->calls==call_count && !writer.poisoned() && writer.snapshot().read("original",0,100)==unrelated,
      "stale indexed inode wrote to a different file");
  writer.snapshot().verify_all();
}

void indexed_session_lifetimes() {
  using Error=NativeFsErrorCode;
  using SessionError=NativeSessionErrorCode;
  auto ram=empty();
  auto session=NativeFileSession::create(new_writer(ram),NativeDispositionPolicy::windows_global);
  const NativeFileAccess access{true,true,true};
  const auto security=native_security_fixture::descriptor();
  const NativeBasicMetadata file_basic{11,22,33,44,0x20},directory_basic{11,22,33,44,0x10};
  const auto made=session.create_open("target",false,file_basic,security,{access,{true},{true,true},false});
  const auto id=made.info.inode;
  const auto peer=session.open("TARGET",access,NativeOpenKind::file,{true},{true,true});
  const auto initial=payload(518),extra=payload(529);
  session.write(made.handle,0,initial);
  const auto unreserved=session.allocation_usage();
  require(unreserved.live_extents==1 && unreserved.charged_extents==1,
      "indexed unreserved file accounting");
  constexpr std::uint64_t reserved=32U*1024*1024;
  session.reserve_result(made.handle,reserved);
  require(session.allocation_usage().charged_extents==reserved/kNativeExtentBytes &&
      session.allocation_info(peer).reserved_bytes==reserved,"indexed reservation count for a populated file");
  session.reserve_result(made.handle,8192);
  require(session.allocation_usage().charged_extents==1,"indexed shrink left stale reservation charges");

  session.set_delete(made.handle,true);
  const auto marked=session.snapshot();
  require(marked.needs_session_recovery() && marked.inode_state(id).delete_pending &&
      session.lookup("target").state.info.delete_pending,"indexed pending intent not visible to the owning session");
  try {
    (void)session.open("target",access);
    throw std::runtime_error("indexed delete-pending file reopened");
  }catch(const NativeSessionError& error){if(error.code()!=SessionError::delete_pending)throw;}
  session.set_delete(peer,false);
  require(!session.snapshot().needs_session_recovery(),"indexed WinFsp-global cancellation retained old intent");
  session.set_delete(made.handle,true);
  session.cleanup(made.handle,true,{88,false,true,false,false});
  const auto detached=session.snapshot();
  require(detached.needs_session_recovery() && detached.inode_state(id).unlinked &&
      detached.inode_state(id).info.path.empty() && detached.basic_metadata(id)->last_access_time==88,
      "indexed Cleanup did not publish detach and metadata together");
  refuses(Error::not_found,[&]{(void)session.lookup("target");});

  const auto replacement=session.create_open("target",false,file_basic,security,{access,{},{},false});
  const auto other=payload(1000);
  session.write(replacement.handle,0,other);
  session.write(peer,0,extra,{true,false});
  auto combined=initial;
  combined.insert(combined.end(),extra.begin(),extra.end());
  session.close(made.handle);
  require(session.read(peer,0,100)==combined && session.read(replacement.handle,0,100)==other &&
      session.security_descriptor(peer)==security,"indexed detach lost peer I/O or changed the replacement");
  const auto root=session.open("",{},NativeOpenKind::directory);
  const auto page=session.list_page(root,{},1);
  require(page.entries.size()==1 && page.entries.front().name=="target" &&
      page.entries.front().state.info.inode==replacement.info.inode && !page.more,
      "indexed session directory exposed a detached inode");
  const auto still_owned=session.snapshot();
  session.cleanup(peer,false);
  session.close(peer);
  const auto closed=session.snapshot();
  require(!closed.needs_session_recovery() && !session.reclaim_retired_inode(),
      "small indexed inode retirement left residual references");
  refuses(Error::not_found,[&]{(void)closed.read_inode(id,0,100);});
  require(still_owned.read_inode(id,0,100)==combined && marked.read_inode(id,0,100)==initial &&
      detached.read_inode(id,0,100)==initial,"retirement changed older indexed snapshots");

  const auto directory=session.create_open("temp",true,directory_basic,security,{access,{},{},false});
  session.set_delete(directory.handle,true);
  refuses(Error::invalid_operation,[&]{(void)session.create_entry("temp/child");});
  session.set_delete(directory.handle,false);
  session.create_entry("temp/child");
  refuses(Error::not_empty,[&]{session.set_delete(directory.handle,true);});
  session.cleanup(directory.handle,false);
  session.close(directory.handle);
  session.cleanup(root,false);
  session.close(root);
  session.cleanup(replacement.handle,false);
  session.close(replacement.handle);
  require(session.stats().contexts==0 && !session.poisoned(),"indexed session did not retire all contexts");
  session.snapshot().verify_all();
}

void indexed_detach_recovery() {
  using Error=NativeFsErrorCode;
  auto original=empty();
  auto writer=new_writer(original);
  const auto seed=writer.apply(std::array{file_create("target"),file_write("target",0,payload(995)),
      file_create("keep"),file_write("keep",0,payload(996))});
  const auto id=seed.created_inodes.front();
  const auto marked=writer.set_delete_pending(id,true);
  const auto stable=original->stable;
  auto successful=image(stable,4096);
  std::size_t boundaries=0;
  {
    auto paged=PagedObjectStoreWriter::resume(successful,identity(),marked.storage.generation,limits,raw);
    const auto start=successful->calls;
    auto recovered=NativeFilesystemWriter::resume_closed_session(std::move(paged));
    boundaries=successful->calls-start;
    require(!recovered.snapshot().needs_session_recovery(),"indexed closed-session recovery retained old intent");
  }
  unsigned old=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut) {
    auto failed=image(stable,4096);
    auto paged=PagedObjectStoreWriter::resume(failed,identity(),marked.storage.generation,limits,raw);
    failed->failure=mode;failed->fail_at=failed->calls+cut;
    refuses(Error::storage_failure,[&] {
      (void)NativeFilesystemWriter::resume_closed_session(std::move(paged));
    });
    auto selected=image(failed->stable,4096);
    const auto prior=NativeFilesystemSnapshot::open_image(selected,indexed);
    prior.verify_all();
    require(prior.read("keep",0,100)==payload(996),"indexed interrupted recovery lost an unrelated file");
    if(prior.needs_session_recovery()) {
      ++old;
      require(prior.sequence()==marked.sequence && prior.inode_state(id).delete_pending &&
          prior.read("target",0,100)==payload(995),"indexed recovery exposed mixed pending state");
    } else {
      ++complete;
      require(prior.sequence()==marked.sequence+1,"indexed recovery published an unexpected sequence");
      refuses(Error::not_found,[&]{(void)prior.stat("target");});
    }
    auto reopened=NativeFilesystemWriter::resume_closed_session(PagedObjectStoreWriter::resume(
        selected,identity(),prior.storage_info().generation,limits,raw));
    const auto final=reopened.snapshot();
    final.verify_all();
    require(!final.needs_session_recovery() && final.read("keep",0,100)==payload(996),
        "indexed recovery could not finish after an interrupted attempt");
    refuses(Error::not_found,[&]{(void)final.read_inode(id,0,100);});
  }
  require(old && complete && original->stable==stable,"indexed closed recovery missed a checkpoint or changed its source");
  std::cout<<"indexed_closed_recovery_faults="<<old+complete<<" previous="<<old<<" complete="<<complete<<'\n';
}

void check_retirement_cuts(const Bytes& stable,std::uint64_t generation,std::size_t boundaries,
    std::uint64_t inode,std::size_t before,std::size_t after) {
  using Error=NativeFsErrorCode;
  unsigned previous=0,advanced=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut) {
    auto failed=image(stable,4096);
    auto writer=resume_writer(failed,generation);
    failed->failure=mode;failed->fail_at=failed->calls+cut;
    refuses(Error::storage_failure,[&]{(void)writer.reclaim_retired_inode();});
    require(writer.poisoned(),"interrupted indexed reclamation did not fence writer");
    const auto recovered=NativeFilesystemSnapshot::open_image(image(failed->stable,4096),indexed);
    const auto mapped=recovered.allocation_usage().live_extents;
    if(mapped==before)++previous;
    else if(mapped==after)++advanced;
    else throw std::runtime_error("indexed interruption exposed partially reclaimed extent references");
    require(!recovered.needs_session_recovery() && recovered.read("keep",0,100)==payload(888),
        "indexed reclamation affected a live name or unrelated file bytes");
    refuses(Error::not_found,[&]{(void)recovered.read_inode(inode,0,17);});
    recovered.verify_all();
  }
  require(previous && advanced,"indexed reclamation did not exercise both durability outcomes");
  std::cout<<"indexed_reclamation_faults="<<previous+advanced<<" previous="<<previous
      <<" advanced="<<advanced<<'\n';
}

void indexed_large_retirement() {
  using Error=NativeFsErrorCode;
  auto ram=empty();
  constexpr unsigned blocks=273,write_batch=48;
  Bytes stable;
  std::uint64_t generation=0;
  {
    auto writer=new_writer(ram);
    const auto created=writer.apply(std::array{file_create("spanned"),file_create("keep"),
        file_write("keep",0,payload(888))});
    const auto id=created.created_inodes.front();
    for(unsigned first=0;first<blocks;first+=write_batch) {
      std::vector<NativeFileMutation> operations;
      for(unsigned i=first;i<(std::min)(first+write_batch,blocks);++i)
        operations.push_back(file_write("spanned",std::uint64_t(i)*kNativeExtentBytes,payload(i)));
      writer.apply(operations);
    }
    auto session=NativeFileSession::create(std::move(writer));
    const auto handle=session.open("spanned",{true,true,true});
    session.reserve_result(handle,64U*1024*1024);
    session.set_delete(handle,true);
    session.cleanup(handle,true);
    const auto detached=session.snapshot();
    const auto read_offset=std::uint64_t(blocks-1)*kNativeExtentBytes;
    require(detached.read_inode(id,read_offset,17)==payload(blocks-1) &&
        detached.allocation_usage().live_extents==blocks+1,"indexed large detach lost data");
    auto pending=image(ram->stable,4096);
    auto prior_objects=PagedObjectStoreWriter::resume(pending,identity(),detached.storage_info().generation,limits,raw);
    const auto calls=pending->calls;
    refuses(Error::invalid_operation,[&] {
      (void)NativeFilesystemWriter::resume(std::move(prior_objects));
    });
    require(pending->calls==calls,"ordinary indexed native admission tried to retire old contexts");
    auto recovered=NativeFilesystemWriter::resume_closed_session(PagedObjectStoreWriter::resume(
        pending,identity(),detached.storage_info().generation,limits,raw));
    require(!recovered.snapshot().needs_session_recovery() && recovered.reclaim_retired_inode(),
        "closed-session admission did not queue large detached recovery");
    session.close(handle);
    const auto retiring=session.snapshot();
    require(!retiring.needs_session_recovery() && retiring.allocation_usage().live_extents==blocks+1 &&
        retiring.read("keep",0,100)==payload(888),
        "first large retirement touched extent data or recovery predicate");
    refuses(Error::not_found,[&]{(void)retiring.read_inode(id,read_offset,17);});
    retiring.verify_all();
    const auto retiring_bytes=ram->stable;
    const auto before=ram->calls;
    require(session.reclaim_retired_inode(),"large indexed file did not enqueue reaping");
    const auto after=session.snapshot();
    require(after.allocation_usage().live_extents>1 && after.allocation_usage().live_extents<blocks+1 &&
        ram->calls>before && detached.read_inode(id,read_offset,17)==payload(blocks-1),
        "indexed reaping exceeded one bounded quantum or invalidated an earlier root");
    check_retirement_cuts(retiring_bytes,retiring.storage_info().generation,ram->calls-before,id,
        blocks+1,after.allocation_usage().live_extents);
    generation=after.storage_info().generation;
    stable=ram->stable;
  }
  auto reopened=resume_writer(ram,generation);
  unsigned remaining=0;
  while(reopened.reclaim_retired_inode()) {
    ++remaining;
    require(remaining<=blocks,"indexed retirement did not make progress");
  }
  const auto clean=reopened.snapshot();
  clean.verify_all();
  require(remaining>1 && clean.allocation_usage().live_extents==1 && clean.allocation_usage().charged_extents==1 &&
      clean.read("keep",0,100)==payload(888) && ram->stable!=stable,
      "indexed reopened reaping did not release extents and reservation exactly");
  std::cout<<"indexed_large_retirement_blocks="<<blocks<<" quanta="<<remaining+1<<'\n';
}

void indexed_head_independent_sequence() {
  constexpr auto native_sequence=std::uint64_t{1}<<40;
  const auto volume_id=identity_for(76);
  auto head=empty();
  auto backing=empty();
  IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(backing,identity(),limits,raw),
      native_sequence,3,{true,0},2);
  const auto contents=payload(763);
  const std::array<IndexedInodeInput,2> inodes{{{1,0,0,1,1,true,""},
      {2,1,contents.size(),native_sequence,native_sequence,false,"picked"}}};
  const IndexedExtentInput extent{2,0,contents};
  builder.append(inodes,{&extent,1});
  const auto source=builder.finish();
  auto owner=NativeVolumeHeadWriter::create(head,volume_id,source);
  const auto select=[&](const auto& id)->std::shared_ptr<StoreByteIo> {
    require(id==identity(),"indexed head called the wrong image resolver");
    return backing;
  };
  const auto view=NativeVolumeHeadView::open(head,volume_id,select);
  const auto selected=view.info().selection;
  require(selected.storage==indexed && selected.namespace_sequence==native_sequence &&
      selected.minimum_checkpoint<native_sequence && view.filesystem().read("picked",0,100)==contents,
      "indexed head confused checkpoint and imported namespace sequence");
  require(head->stable.at(8)==std::byte{3} && head->stable.at(96)==std::byte{2},
      "indexed head did not publish distinct wire kind");
  refuses(NativeFsErrorCode::corrupt,[&] {
    (void)NativeVolumeHeadView::open(head,volume_id,[&](const auto&){return head;});
  });
}

void indexed_staged_rebuild() {
  auto backing=empty(),target=empty();
  auto writer=new_writer(backing);
  const auto contents=payload(672);
  writer.apply(std::array{file_create("data"),file_write("data",0,contents)});
  const auto source=writer.snapshot();
  const auto source_bytes=backing->stable;
  const auto image_id=identity_for(48);
  auto candidate=writer.prepare_paged_recompression(target,image_id,raw,false);
  unsigned steps=0;
  while(!writer.advance_paged_recompression(candidate))
    require(++steps<1000,"indexed staged rebuild did not finish");
  auto copied=writer.finish_paged_recompression(candidate);
  const auto result=copied.snapshot();
  result.verify_all();
  require(result.identity()==image_id && result.sequence()==source.sequence() &&
      result.read("data",0,100)==contents && backing->stable==source_bytes,
      "indexed staged rebuild did not rebind its own root exactly");
  file_apply(copied,file_write("data",0,payload(673)));
  require(source.read("data",0,100)==contents,"new indexed writer changed its source snapshot");
}

void indexed_reconciled_rebuild() {
  auto backing=empty(),target=empty();
  auto writer=new_writer(backing);
  const auto name=[](unsigned i){return "sample"+std::to_string(i);};
  std::map<std::string,Bytes> expected;
  const auto create=[&](unsigned i) {
    const auto path=name(i);
    auto bytes=data(251+i,92+i);
    writer.apply(std::array{file_create(path),file_write(path,0,bytes)});
    expected.emplace(path,std::move(bytes));
  };
  for(unsigned i=0;i<84;++i)create(i);
  const auto pinned=writer.snapshot();
  const auto image_id=identity_for(51);
  auto candidate=writer.prepare_paged_recompression(target,image_id,raw,false,PagedRewriteMode::reconcile_foreground);
  std::uint64_t source_reads=0,target_reads=0,read_bytes=0;
  const auto advance=[&] {
    const auto source_before=backing->read_calls,target_before=target->read_calls;
    const auto bytes_before=backing->read_bytes+target->read_bytes;
    const auto complete=writer.advance_paged_recompression(candidate);
    source_reads+=backing->read_calls-source_before;
    target_reads+=target->read_calls-target_before;
    read_bytes+=backing->read_bytes+target->read_bytes-bytes_before;
    return complete;
  };
  require(!advance(),"indexed candidate completed before first scan");
  // Delete enough adjoining keys to remove old leaves and insert new keys
  // beyond the old tree's end. Neither change should restart the private copy.
  for(unsigned i=0;i<31;++i) {
    file_apply(writer,file_remove(name(i)));
    expected.erase(name(i));
  }
  for(unsigned i=84;i<96;++i)create(i);
  unsigned steps=0;
  while(candidate.progress().phase!=PagedRewritePhase::reconciling) {
    require(!advance() && ++steps<2000,
        "indexed rebuild did not start reconciliation against committed edits");
  }
  // Admit another foreground commit after the delta cursor captured its root.
  create(96);
  unsigned catchup=0;
  while(!advance())
    require(++catchup<500,"indexed reconciliation did not converge");
  require(writer.paged_recompression_ready(candidate),"reconciled image did not bind to the current source");

  const auto former_path=name(65);
  file_apply(writer,file_rename(former_path,"renamed"));
  auto bytes=std::move(expected.at(former_path));
  expected.erase(former_path);
  expected.emplace("renamed",std::move(bytes));
  const auto previous_target_extent=target->extent();
  require(!writer.paged_recompression_ready(candidate),"late commit left the staged target publishable");
  refuses(NativeFsErrorCode::storage_failure,[&]{(void)writer.finish_paged_recompression(candidate);});
  require(target->extent()==previous_target_extent,"stale publication touched the private image");
  unsigned late_steps=0;
  while(!advance())
    require(++late_steps<100,"late foreground commit restarted the full copy");

  const auto source=writer.snapshot();
  auto rebuilt=writer.finish_paged_recompression(candidate);
  const auto observed=NativeFilesystemSnapshot::open_image(target,indexed);
  observed.verify_all();
  require(observed.sequence()==source.sequence() && observed.identity()==image_id &&
      observed.storage_info().objects==source.storage_info().objects &&
      observed.storage_info().physical_end<source.storage_info().physical_end &&
      late_steps*4<steps && pinned.read(name(0),0,512)==data(251,92),
      "incremental reconciliation lost source identity, space or a pinned reader");
  const auto prior_records=PagedObjectStoreSnapshot::open(backing,limits).list_page(0,4096);
  const auto new_records=PagedObjectStoreSnapshot::open(target,limits).list_page(0,4096);
  require(prior_records.size()==new_records.size(),"reconciled object inventory count differs");
  for(std::size_t i=0;i<prior_records.size();++i) {
    const auto& original=prior_records[i];
    const auto& copied=new_records[i];
    require(original.id==copied.id && original.generation==copied.generation &&
        original.logical_bytes==copied.logical_bytes,
        "reconciled object identity or version differs");
  }
  for(const auto& [path,value]:expected)
    require(observed.read(path,0,512)==value,"reconciled reopen lost file bytes");
  for(unsigned i=0;i<31;++i)
    refuses(NativeFsErrorCode::not_found,[&]{(void)observed.stat(name(i));});
  refuses(NativeFsErrorCode::not_found,[&]{(void)observed.stat(former_path);});
  require(rebuilt.snapshot().sequence()==source.sequence(),"reconciled writer cached a stale namespace");
  // This fixed workload previously used 7,990 reads during staged advances.
  // Leave margin for layout changes while catching a fresh walk per record.
  require(source_reads+target_reads<5000,"reconciled rebuild repeated checked backing reads");
  std::cout<<"indexed_reconciliation retained_files="<<expected.size()<<" deleted_files=31 baseline_steps="<<steps
      <<" catchup_steps="<<catchup<<" late_steps="<<late_steps
      <<" source_read_calls="<<source_reads<<" target_read_calls="<<target_reads
      <<" read_bytes="<<read_bytes<<'\n';
}

void advance_to_compaction_ready(NativeFileSession& session) {
  using Progress=NativeFileSession::RecompressionProgress;
  for(unsigned step=0;step<2000;++step) {
    const auto result=session.advance_recompression();
    if(result==Progress::ready)return;
    require(result==Progress::copying,"unchanged staged compaction became stale");
  }
  throw std::runtime_error("staged compaction did not finish");
}

void indexed_managed_staged_compaction() {
  using Progress=NativeFileSession::RecompressionProgress;
  const auto volume_id=identity_for(63),source_id=identity_for(64),target_id=identity_for(65);
  auto head=empty(),source=empty(),stale=empty(),target=empty();
  auto session=NativeFileSession::create_indexed_volume(head,volume_id,source,source_id,limits,raw);
  session.create_entry("retain");
  const auto handle=session.open("retain",{true,true,true});
  auto expected=data(16*kNativeExtentBytes,735);
  session.write(handle,0,expected);
  const auto pinned=session.snapshot();
  session.write(handle,0,payload(159));
  auto overwrite=payload(160);
  require(session.begin_idle_compaction(stale,identity_for(66)),"could not start initial indexed compaction");
  advance_to_compaction_ready(session);
  session.write(handle,0,overwrite);
  std::copy(overwrite.begin(),overwrite.end(),expected.begin());
  const auto stale_reads=stale->read_bytes;
  const auto stale_calls=head->calls;
  const auto source_reads=source->read_bytes;
  require(session.advance_recompression()==Progress::stale && head->calls==stale_calls &&
      stale->read_bytes==stale_reads && source->read_bytes==source_reads,
      "stale staged compaction touched its image or selector");
  const auto prior=source->stable;
  const auto previous_free=session.indexed_volume_space().free_bytes;
  require(session.begin_idle_compaction(target,target_id),"could not start fresh indexed compaction");
  advance_to_compaction_ready(session);
  const auto reads=source->read_bytes+target->read_bytes;
  const auto old_calls=source->calls,new_calls=target->calls,head_calls=head->calls;
  const auto selected=session.publish_idle_recompression();
  const auto switch_reads=source->read_bytes+target->read_bytes-reads;
  require(source->calls==old_calls && target->calls==new_calls && head->calls==head_calls+4 &&
      switch_reads<expected.size()/2,"indexed staged selection repeated a body scan or write");
  require(selected && selected->selection.epoch==2 && selected->selection.image_identity==target_id &&
      selected->selection.parent_identity==source_id && source->stable==prior &&
      session.indexed_volume_space().free_bytes>previous_free &&
      session.read(handle,0,static_cast<std::uint32_t>(expected.size()))==expected,
      "staged selection lost a context or failed to reclaim committed space");
  require(!session.begin_idle_compaction({},identity_for(67)),"unchanged compacted image retried");
  overwrite=payload(161);
  session.write(handle,0,overwrite);
  std::copy(overwrite.begin(),overwrite.end(),expected.begin());
  require(session.read(handle,0,static_cast<std::uint32_t>(expected.size()))==expected &&
      pinned.read("retain",0,static_cast<std::uint32_t>(expected.size()))==data(expected.size(),735),
      "old CPU reader or new writer lost acknowledged data");
  session.cleanup(handle,false);
  session.close(handle);
  std::cout<<"indexed_compaction_switch_image_read_bytes="<<switch_reads
           <<" selected_live_file_bytes="<<expected.size()<<'\n';
}

void indexed_managed_staged_faults() {
  using Error=NativeFsErrorCode;
  constexpr unsigned head_boundaries=4;
  unsigned previous=0,advanced=0;
  for(unsigned mode=0;mode<3;++mode)for(unsigned cut=0;cut<head_boundaries;++cut) {
    const auto volume_id=identity_for(68),source_id=identity_for(69),target_id=identity_for(70);
    auto head=empty(),source=empty(),target=empty();
    auto session=NativeFileSession::create_indexed_volume(head,volume_id,source,source_id,limits,raw);
    session.create_entry("data");
    const auto handle=session.open("data",{true,true,true});
    for(unsigned i=0;i<6;++i)session.write(handle,0,payload(i));
    const auto expected=payload(185);
    session.write(handle,0,expected);
    require(session.begin_idle_compaction(target,target_id),"could not begin head-fault fixture");
    advance_to_compaction_ready(session);
    const auto source_before=source->stable;
    head->failure=mode;head->fail_at=head->calls+cut;
    refuses(Error::storage_failure,[&]{(void)session.publish_idle_recompression();});
    require(session.poisoned() && source->stable==source_before,"ambiguous staged selection modified the source");
    const auto recovered=session.recover_volume();
    if(recovered.selection.epoch==1 && recovered.selection.image_identity==source_id)++previous;
    else if(recovered.selection.epoch==2 && recovered.selection.image_identity==target_id)++advanced;
    else throw std::runtime_error("staged recovery selected an unrelated image");
    require(session.read(handle,0,100)==expected,"staged selector recovery lost acknowledged bytes");
    session.cleanup(handle,false);
    session.close(handle);
  }
  require(previous && advanced,"staged selector faults did not exercise both owned images");

  auto head=empty(),source=empty(),target=empty();
  auto session=NativeFileSession::create_indexed_volume(
      head,identity_for(71),source,identity_for(72),limits,raw);
  session.create_entry("data");
  const auto handle=session.open("data",{true,true,true});
  const auto expected=payload(186);
  session.write(handle,0,expected);
  require(session.begin_idle_compaction(target,identity_for(73)),"could not begin anchor fixture");
  advance_to_compaction_ready(session);
  target->live.at(0)^=std::byte{1};
  const auto head_calls=head->calls;
  refuses(Error::invalid_operation,[&]{(void)session.publish_idle_recompression();});
  require(!session.poisoned() && head->calls==head_calls &&
      session.volume_info().selection.epoch==1 && session.read(handle,0,100)==expected,
      "rejected private anchor affected the live original selection");
  session.cleanup(handle,false);
  session.close(handle);
  std::cout<<"indexed_compaction_head_cuts="<<previous+advanced
           <<" original="<<previous<<" candidate="<<advanced<<'\n';
}

void indexed_managed_handoff() {
  using Error=NativeFsErrorCode;
  const auto volume=identity_for(60),source_id=identity_for(61),candidate_id=identity_for(62);
  auto head=empty(),backing=empty(),candidate=empty();
  const auto contents=payload(1914);
  Bytes old_head;
  std::uint64_t before_size=0,after_size=0;
  {
    auto session=NativeFileSession::create_indexed_volume(head,volume,backing,source_id,limits,raw);
    session.create_entry("data");
    const auto handle=session.open("data",{true,true,true});
    for(unsigned i=0;i<12;++i)session.write(handle,0,payload(i));
    session.write(handle,0,contents);
    const auto pinned=session.snapshot();
    const auto before=backing->stable;
    before_size=pinned.storage_info().physical_end;
    old_head=head->stable;
    const auto selected=session.compact_volume(candidate,candidate_id,raw);
    after_size=session.snapshot().storage_info().physical_end;
    require(selected.selection.epoch==2 && selected.selection.storage==indexed &&
        selected.selection.image_identity==candidate_id && selected.selection.parent_identity==source_id &&
        session.read(handle,0,100)==contents && pinned.read("data",0,100)==contents &&
        backing->stable==before && after_size<before_size,
        "indexed compacted selection lost an owned handle or altered its source");
    session.cleanup(handle,false);
    session.close(handle);
  }

  const NativeVolumeImageResolver resolve=[&](const auto& id)->std::shared_ptr<StoreByteIo> {
    if(id==source_id)return backing;
    if(id==candidate_id)return candidate;
    throw std::runtime_error("unexpected indexed resolver identity");
  };
  auto reopened=NativeFileSession::resume_volume(head,volume,2,resolve,raw);
  require(reopened.snapshot().read("data",0,100)==contents &&
      reopened.volume_info().selection.image_identity==candidate_id,
      "indexed managed reopen did not select the durable compacted image");
  auto old=NativeFilesystemSnapshot::open_image(backing,indexed);
  auto newer=NativeFilesystemSnapshot::open_image(candidate,indexed);
  // The live reopened writer must retire before the candidate can be leased
  // again. Move it into a short scope and retain only immutable snapshots.
  {auto closing=std::move(reopened);}
  unsigned previous=0,advanced=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<4;++cut) {
    auto torn=image(old_head,4096);
    auto owner=NativeVolumeHeadWriter::resume(torn,volume,1,old);
    torn->failure=mode;torn->fail_at=torn->calls+cut;
    refuses(Error::storage_failure,[&]{(void)owner.publish_compacted(old,newer);});
    require(owner.poisoned(),"failed indexed head publication retained write authority");
    unsigned resolutions=0;
    const auto observed=NativeVolumeHeadView::open(image(torn->stable,4096),volume,[&](const auto& id) {
      ++resolutions;return resolve(id);
    });
    require(resolutions==1 && observed.filesystem().read("data",0,100)==contents,
        "indexed interrupted head selected fallback or wrong bytes");
    const auto selection=observed.info().selection;
    if(selection.epoch==1 && selection.image_identity==source_id)++previous;
    else if(selection.epoch==2 && selection.image_identity==candidate_id)++advanced;
    else throw std::runtime_error("indexed interrupted head used a partial selection");
  }
  require(previous && advanced,"indexed head fault sweep missed a selection");
  std::cout<<"indexed_managed_compaction source_bytes="<<before_size<<" target_bytes="<<after_size
      <<" head_faults="<<previous+advanced<<" previous="<<previous<<" advanced="<<advanced<<'\n';
}

constexpr auto recovered_name="targ\xc3\xa9t.bin";
constexpr auto recovered_alias="TARG\xc3\x89T.BIN";
void verify_recovered_batch(const NativeFilesystemSnapshot& snapshot,bool committed) {
  using Error=NativeFsErrorCode;
  require(snapshot.sequence()==(committed?3U:2U) &&
      snapshot.read("stable.bin",0,100)==data(31,18),"indexed recovery lost acknowledged stable data");
  const auto entries=snapshot.list_inode_page(1).entries;
  if(committed) {
    require(entries.size()==2 && snapshot.read(recovered_alias,0,100000)==final_bytes() &&
        snapshot.stat(recovered_name).inode==3 && snapshot.allocation_usage().live_extents==3,
        "indexed recovery published a partial replacement");
    refuses(Error::not_found,[&]{(void)snapshot.stat("target.bin");});
    refuses(Error::not_found,[&]{(void)snapshot.stat("A");});
    refuses(Error::not_found,[&]{(void)snapshot.stat("B");});
  } else {
    require(entries.size()==3 && snapshot.read("target.bin",0,100)==data(31,2) &&
        snapshot.read("A/file.bin",0,100)==data(17,1) && snapshot.stat("target.bin").inode==4 &&
        snapshot.allocation_usage().live_extents==3,"indexed recovery mixed checkpoint metadata and bytes");
    refuses(Error::not_found,[&]{(void)snapshot.stat(recovered_name);});
  }
  snapshot.verify_all();
}

void indexed_transaction_recovery() {
  using Error=NativeFsErrorCode;
  auto original=empty();
  auto writer=new_writer(original);
  const auto baseline_commit=writer.apply(baseline());
  const auto original_snapshot=writer.snapshot();
  const auto stable=original->stable;
  auto operations=update();
  operations.push_back(file_rename("target.bin",recovered_name));
  auto success=image(stable,4096);
  std::size_t boundaries=0;
  {
    auto candidate=resume_writer(success,baseline_commit.storage.generation);
    const auto calls=success->calls;
    candidate.apply(operations);
    boundaries=success->calls-calls;
    verify_recovered_batch(candidate.snapshot(),true);
  }

  unsigned old=0,complete=0;
  for(unsigned mode=0;mode<3;++mode)for(std::size_t cut=0;cut<boundaries;++cut) {
    auto backing=image(stable,4096);
    auto candidate=resume_writer(backing,baseline_commit.storage.generation);
    const auto retained=candidate.snapshot();
    backing->failure=mode;
    backing->fail_at=backing->calls+cut;
    refuses(Error::storage_failure,[&]{candidate.apply(operations);});
    require(candidate.poisoned(),"ambiguous indexed publication did not fence writer");
    const auto calls=backing->calls;
    refuses(Error::storage_failure,[&]{candidate.apply(operations);});
    require(backing->calls==calls,"indexed writer retried after failed publication");
    verify_recovered_batch(retained,false);
    const auto recovered=NativeFilesystemSnapshot::open_image(image(backing->stable,4096),indexed);
    const auto committed=recovered.sequence()==3;
    verify_recovered_batch(recovered,committed);
    if(committed)++complete;
    else ++old;
  }
  verify_recovered_batch(original_snapshot,false);
  require(original->stable==stable && old && complete,"indexed recovery did not exercise both valid checkpoints");
  std::cout<<"indexed_writer_faults="<<old+complete<<" previous="<<old<<" committed="<<complete<<'\n';
}

std::uint32_t fixture_get(std::span<const std::byte> raw,std::size_t offset,unsigned width) {
  std::uint32_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::to_integer<std::uint32_t>(raw[offset+i])<<(8*i);
  return value;
}

void profile_cpython_reopen(const char* path,bool compressed=false) {
  if(compressed)require(store_zstd_available(),"compressed CPython profile requires Zstandard");
  const auto encoding=compressed?StoreCompressionOptions{StoreCompressionMode::automatic,3,true}:raw;
  const auto input=ReadOnlyFile::open(path);
  constexpr std::uint64_t maximum_input=96ULL*1024*1024;
  require(input.size()>=12 && input.size()<=maximum_input,"source tree fixture length");
  const auto take=[&](std::uint64_t offset,std::size_t count) {
    require(offset<=input.size() && count<=input.size()-offset,"truncated source tree fixture");
    Bytes bytes(count);
    if(count)input.read_exact_at(offset,bytes.data(),count);
    return bytes;
  };
  const auto header=take(0,12);
  constexpr std::string_view magic="FLSTREE1";
  require(std::string_view(reinterpret_cast<const char*>(header.data()),magic.size())==magic,"source tree fixture marker");
  const auto count=fixture_get(header,8,4);
  require(count && count<=4096,"source tree fixture file count");
  struct File {
    std::string path;
    std::uint64_t first,bytes;
  };
  std::vector<File> files;
  std::map<std::string,std::uint64_t> directories{{"",1}};
  std::uint64_t offset=header.size(),total=0;
  for(std::uint64_t i=0;i<count;++i) {
    const auto record=take(offset,8);
    offset+=8;
    const auto length=fixture_get(record,0,4),bytes=fixture_get(record,4,4);
    require(length && length<=1024 && bytes<=1024*1024,"source tree fixture entry bounds");
    const auto name=take(offset,static_cast<std::size_t>(length));
    offset+=length;
    require(bytes<=input.size()-offset,"truncated source tree fixture payload");
    auto& file=files.emplace_back(File{std::string(reinterpret_cast<const char*>(name.data()),name.size()),offset,bytes});
    for(auto end=file.path.find('/');end!=std::string::npos;end=file.path.find('/',end+1))
      directories.try_emplace(file.path.substr(0,end),0);
    total+=bytes;
    offset+=bytes;
  }
  require(offset==input.size(),"trailing source tree fixture data");

  auto backing=empty();
  backing->capacity=2*total+160ULL*1024*1024;
  IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(backing,identity(),
      {backing->capacity},encoding),2,directories.size()+files.size()+1,{},2);
  std::vector<IndexedInodeInput> directory_batch;
  std::uint64_t inode=1;
  for(auto& [path_id,key]:directories) {
    key=inode++;
    const auto separator=path_id.rfind('/');
    const auto parent=separator==std::string::npos?std::string{}:path_id.substr(0,separator);
    const auto leaf=separator==std::string::npos?path_id:path_id.substr(separator+1);
    const bool root=path_id.empty();
    directory_batch.push_back({key,root?0:directories.at(parent),0,root?1U:2U,2,true,leaf});
    if(directory_batch.size()==32) {
      builder.append(directory_batch);
      directory_batch.clear();
    }
  }
  if(!directory_batch.empty())builder.append(directory_batch);

  std::vector<IndexedInodeInput> inode_batch;
  std::vector<IndexedExtentInput> extents;
  std::vector<Bytes> payloads;
  const auto publish=[&] {
    builder.append(inode_batch,extents);
    inode_batch.clear();
    extents.clear();
    payloads.clear();
  };
  for(const auto& file:files) {
    if(inode_batch.size()==32 || extents.size()+(file.bytes+kNativeExtentBytes-1)/kNativeExtentBytes>48)publish();
    const auto separator=file.path.rfind('/');
    const auto parent=separator==std::string::npos?std::string{}:file.path.substr(0,separator);
    const auto leaf=separator==std::string::npos?file.path:file.path.substr(separator+1);
    const auto id=inode++;
    inode_batch.push_back({id,directories.at(parent),file.bytes,2,2,false,leaf});
    const auto& payload=payloads.emplace_back(take(file.first,static_cast<std::size_t>(file.bytes)));
    for(std::size_t first=0;first<payload.size();first+=kNativeExtentBytes) {
      auto bytes=std::span(payload).subspan(first,(std::min<std::size_t>)(payload.size()-first,kNativeExtentBytes));
      while(!bytes.empty() && bytes.back()==std::byte{})bytes=bytes.first(bytes.size()-1);
      if(!bytes.empty())extents.push_back({id,first/kNativeExtentBytes,bytes});
    }
  }
  if(!inode_batch.empty())publish();
  const auto published=builder.finish();
  profile_indexed_reopen(backing,published,static_cast<unsigned>(files.size()),total,
      compressed?"cpython_compressed":"cpython_lib",
      [&](const NativeFilesystemSnapshot& restored) {
    for(const auto& file:files) {
      require(restored.stat(file.path).logical_bytes==file.bytes &&
          restored.read(file.path,0,static_cast<std::size_t>(file.bytes))==
              take(file.first,static_cast<std::size_t>(file.bytes)),
          "CPython source file differs after indexed reopen");
    }
  },encoding);
}

}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string_view(argv[1])=="--reconciled-rebuild"){
      indexed_reconciled_rebuild();
      return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--scale-audit"){
      populated_native_snapshot(16384);
      return 0;
    }
    if(argc==3 && std::string_view(argv[1])=="--profile-reopen-count") {
      const std::string_view value(argv[2]);
      unsigned files=0;
      const auto result=std::from_chars(value.data(),value.data()+value.size(),files);
      require(result.ec==std::errc{} && result.ptr==value.data()+value.size() && files && files<=16384,
          "invalid bounded reopen profile size");
      populated_native_snapshot(files,true);
      return 0;
    }
    if(argc==3 && (std::string_view(argv[1])=="--profile-cpython-reopen" ||
        std::string_view(argv[1])=="--profile-cpython-compressed-reopen")) {
      profile_cpython_reopen(argv[2],std::string_view(argv[1])=="--profile-cpython-compressed-reopen");
      return 0;
    }
    require(argc==1,"unknown indexed volume test option");
    populated_native_snapshot();indexed_reopen_checks_batched_replicas();indexed_unreachable_name_page();migration(false);migration(true);growing_native_writer();indexed_capacity_and_metadata();indexed_stable_inode_io();indexed_session_lifetimes();indexed_detach_recovery();
    indexed_large_retirement();indexed_head_independent_sequence();indexed_staged_rebuild();indexed_reconciled_rebuild();
    indexed_managed_handoff();indexed_managed_staged_compaction();indexed_managed_staged_faults();
    indexed_transaction_recovery();indexed_query_page_lifetime();indexed_extent_advice();indexed_extent_read_route();
    indexed_directory_moves();indexed_directory_backfill();indexed_windows_names(); }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
