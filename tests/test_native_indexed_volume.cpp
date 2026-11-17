#include "native_indexed_volume.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
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
  try { populated_native_snapshot();migration(false);migration(true);growing_native_writer();indexed_capacity_and_metadata();indexed_stable_inode_io();indexed_session_lifetimes();indexed_detach_recovery();
    indexed_large_retirement();indexed_transaction_recovery(); }
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
