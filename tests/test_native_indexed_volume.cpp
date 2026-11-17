#include "native_indexed_volume.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/volume_head.hpp"
#include <iostream>

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

void populated_native_snapshot() {
  constexpr unsigned files=1024,batch=32;
  constexpr std::uint64_t sparse=3+files;
  constexpr auto sparse_size=(std::numeric_limits<std::uint64_t>::max)()-7;
  constexpr auto last_block=(sparse_size-1)/kNativeExtentBytes;
  auto ram=empty();
  IndexedVolumeBuilder builder(PagedObjectStoreWriter::create_mirrored(ram,identity(),limits,raw),
      2,sparse+1,{true,files*17});
  const std::array<IndexedInodeInput,2> root{{{1,0,0,1,1,true,""},
      {2,1,0,2,2,true,"Data"}}};
  builder.append(root);
  refuses(NativeFsErrorCode::storage_failure,[&]{(void)NativeFilesystemSnapshot::open_image(ram,indexed);});
  for(unsigned first=0;first<files;first+=batch) {
    std::vector<Bytes> contents;
    std::vector<IndexedInodeInput> inodes;
    std::vector<IndexedExtentInput> extents;
    contents.reserve(batch);inodes.reserve(batch);extents.reserve(batch);
    for(unsigned i=first;i<first+batch;++i) {
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
  const auto start_open=ram->read_bytes;
  const auto view=NativeFilesystemSnapshot::open_image(ram,indexed);
  const auto open_bytes=ram->read_bytes-start_open;
  require(view.image_storage()==indexed && view.sequence()==2 && view.identity()==identity() &&
      view.allocation_usage().live_extents==files+3,"indexed native root counts");
  const auto start_lookup=ram->read_bytes;
  const auto last=view.stat("DATA/"+file(files-1));
  const auto lookup_bytes=ram->read_bytes-start_lookup;
  require(last.inode==files+2 && last.path=="Data/"+file(files-1) && last.logical_bytes==17,
      "indexed path did not resolve persisted data");
  const auto start_scrub=ram->read_bytes;
  view.verify_all();
  const auto scrub_bytes=ram->read_bytes-start_scrub;
  require(open_bytes<scrub_bytes/16 && lookup_bytes<scrub_bytes/16,"indexed volume loaded whole namespace for lookup");
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
    (void)NativeFilesystemSnapshot::open_image(ram,{NativeImageKind::paged,limits.maximum_physical_bytes});
  });
  auto head=empty();
  refuses(NativeFsErrorCode::invalid_operation,[&]{(void)NativeVolumeHeadWriter::create(head,identity_for(92),view);});
  require(head->extent()==0,"draft capability wrote an incompatible volume head");
  require(view.read_inode(sparse,127ULL*kNativeExtentBytes,17)==a &&
      view.read_inode(sparse,128ULL*kNativeExtentBytes,17)==b &&
      view.read_inode(sparse,last_block*kNativeExtentBytes,17)==c &&
      view.read_inode(sparse,sparse_size-3,8)==Bytes(3),"sparse block indexing or high-offset EOF");
  auto cross=Bytes(2);
  cross.insert(cross.end(),b.begin(),b.begin()+3);
  require(view.read_inode(sparse,128ULL*kNativeExtentBytes-2,5)==cross,"indexed range across sparse blocks");
  std::cout<<"indexed_volume files="<<files<<" image_bytes="<<view.storage_info().physical_end
      <<" open_read_bytes="<<open_bytes<<" lookup_read_bytes="<<lookup_bytes
      <<" scrub_read_bytes="<<scrub_bytes<<" sparse_size="<<sparse_size<<'\n';

  // Re-seal a semantically wrong owner through the ordinary object store.
  // Object integrity succeeds here, but the indexed reader must refuse it.
  const auto objects=PagedObjectStoreSnapshot::open(ram,limits);
  const NativeNameIndex names(objects);
  const auto object=names.find(native_extent_key(3,0));
  require(object.has_value(),"indexed source extent not found");
  auto frame=objects.read(*object);frame.at(8)^=std::byte{2};
  {
    auto writer=PagedObjectStoreWriter::resume(ram,identity(),objects.info().generation,limits,raw);
    const StoreMutation wrong{*object,false,std::move(frame)};
    writer.commit({&wrong,1});
  }
  const auto malformed=NativeFilesystemSnapshot::open_image(ram,indexed);
  refuses(NativeFsErrorCode::corrupt,[&]{(void)malformed.read_inode(3,0,17);});
  require(malformed.read_inode(4,0,17)==payload(1) && view.read_inode(3,0,17)==payload(0),
      "bad extent crossed ownership or changed an older root");
  refuses(NativeFsErrorCode::corrupt,[&]{malformed.verify_all();});
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
  const auto stable=roomy->calls;
  refuses(Error::invalid_path,[&]{file_apply(writer,file_rename("p",std::string(99,'q')));});
  require(roomy->calls==stable && writer.inspect_path(old+"/"+long_leaf).info.logical_bytes==0,
      "invalid directory growth changed the indexed graph");
  writer.snapshot().verify_all();
}

void verify_recovered_batch(const NativeFilesystemSnapshot& snapshot,bool committed) {
  using Error=NativeFsErrorCode;
  require(snapshot.sequence()==(committed?3U:2U) &&
      snapshot.read("stable.bin",0,100)==data(31,18),"indexed recovery lost acknowledged stable data");
  const auto entries=snapshot.list_inode_page(1).entries;
  if(committed) {
    require(entries.size()==2 && snapshot.read("target.bin",0,100000)==final_bytes() &&
        snapshot.stat("target.bin").inode==3 && snapshot.allocation_usage().live_extents==3,
        "indexed recovery published a partial replacement");
    refuses(Error::not_found,[&]{(void)snapshot.stat("A");});
    refuses(Error::not_found,[&]{(void)snapshot.stat("B");});
  } else {
    require(entries.size()==3 && snapshot.read("target.bin",0,100)==data(31,2) &&
        snapshot.read("A/file.bin",0,100)==data(17,1) && snapshot.stat("target.bin").inode==4 &&
        snapshot.allocation_usage().live_extents==3,"indexed recovery mixed checkpoint metadata and bytes");
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
  const auto operations=update();
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
}

int main() {
  try { populated_native_snapshot();migration(false);migration(true);growing_native_writer();indexed_capacity_and_metadata();indexed_transaction_recovery(); }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
