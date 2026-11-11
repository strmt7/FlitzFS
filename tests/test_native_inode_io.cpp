#include "native_fs_fixture.hpp"
#include <iostream>
#include <string>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
template<class F>void rejects(F&& action,NativeFsErrorCode code,const char* reason) {
  ++checks;try{action();}catch(const NativeFsError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(reason);
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
void identity_and_paths(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
  const auto original=data(65530,41);
  const std::array<NativeFileMutation,3> setup{{file_create("Folder",true),
      file_create("Folder/original"),file_write("Folder/original",0,original)}};
  writer.apply(setup);auto retained=writer.snapshot();
  const auto directory=retained.stat("Folder").inode,inode=retained.stat("Folder/original").inode;
  require(retained.stat_inode(1).directory && retained.stat_inode(inode).path=="Folder/original",
      "inode metadata lookup differs from namespace");
  require(retained.list_inode(directory).size()==1 &&
      retained.list_inode(directory)[0].inode==inode,"directory inode enumeration");
  file_apply(writer,file_rename("Folder","Moved"));
  file_apply(writer,file_rename("Moved/original","Moved/renamed"));
  const std::array<NativeFileMutation,3> reused_name{{file_create("Folder",true),
      file_create("Folder/original"),file_write("Folder/original",0,data(19,42))}};
  writer.apply(reused_name);
  auto oracle=original;const auto suffix=data(29,43);
  auto result=writer.write_inode(inode,UINT64_MAX,suffix,{true,false});
  oracle.insert(oracle.end(),suffix.begin(),suffix.end());
  require(result.inode==inode && result.offset==original.size() && result.bytes==suffix.size(),
      "append did not report actual selected EOF/count");
  auto view=writer.snapshot();
  require(view.stat_inode(inode).path=="Moved/renamed" && view.read_inode(inode,0,100000)==oracle,
      "stable inode write followed reused path or lost renamed data");
  require(view.read("Folder/original",0,100)==data(19,42),"inode write damaged replacement at old name");
  require(retained.read_inode(inode,0,100000)==original &&
      retained.stat_inode(inode).path=="Folder/original","retained inode snapshot changed");
  require(view.list_inode(directory)[0].path=="Moved/renamed","renamed directory inode enumeration");
  // A second append must use the newly acknowledged EOF, not a saved initial size.
  const auto second=data(11,44);
  result=writer.write_inode(inode,0,second,{true,false});
  require(result.offset==oracle.size() && result.bytes==second.size(),"append reused stale EOF");
  oracle.insert(oracle.end(),second.begin(),second.end());
  // Constrained writes may cross EOF but must neither grow nor expose a gap.
  const auto patch=data(100,45);const auto before=oracle.size();
  const auto flushes=ram->flushes;
  result=writer.write_inode(inode,before-7,patch,{false,true});
  std::copy_n(patch.begin(),7,oracle.end()-7);
  require(result.offset==before-7 && result.bytes==7 && ram->flushes==flushes+3,
      "constrained write count or replicated barriers changed");
  require(writer.snapshot().stat_inode(inode).logical_bytes==before &&
      writer.snapshot().read_inode(inode,0,100000)==oracle,"constrained write extended logical EOF");
  // Nonempty identical accepted writes still publish metadata and all barriers.
  const auto sequence=writer.snapshot().sequence();const auto barriers=ram->flushes;
  result=writer.write_inode(inode,0,std::span<const std::byte>(oracle).first(23));
  require(result.bytes==23 && result.transaction.sequence==sequence+1 && ram->flushes==barriers+3,
      "identical inode write weakened durable publication");
  const auto stable=ram->live;const auto calls=ram->calls;
  for(const auto offset:{std::uint64_t(before),UINT64_MAX}) {
    result=writer.write_inode(inode,offset,patch,{false,true});
    require(!result.bytes && result.offset==offset && result.transaction.sequence==sequence+1,
        "beyond-EOF constrained no-op changed sequence/count");
  }
  result=writer.write_inode(inode,UINT64_MAX,patch,{true,true});
  require(!result.bytes && result.offset==before,"constrained append was allowed to grow");
  result=writer.write_inode(inode,UINT64_MAX,{}, {true,false});
  require(!result.bytes && result.offset==before && ram->live==stable && ram->calls==calls,
      "zero accepted inode write changed backing bytes");
  writer.truncate_inode(inode,31);oracle.resize(31);
  writer.truncate_inode(inode,65550);oracle.resize(65550);
  require(writer.snapshot().read_inode(inode,0,100000)==oracle,"inode truncate/regrow exposed old tail");
  const auto displaced=file_apply(writer,file_create("victim")).created_inodes[0];
  file_apply(writer,file_write("victim",0,data(5,46)));auto displaced_snapshot=writer.snapshot();
  file_apply(writer,file_rename("Moved/renamed","victim",true));
  require(writer.snapshot().stat_inode(inode).path=="victim","replace lost source inode identity");
  rejects([&]{(void)writer.write_inode(displaced,0,patch);},NativeFsErrorCode::not_found,
      "displaced inode redirected to replacement");
  require(displaced_snapshot.read_inode(displaced,0,100)==data(5,46),"displaced snapshot contents changed");
  file_apply(writer,file_remove("victim"));file_apply(writer,file_create("victim"));
  const auto replacement=writer.snapshot().stat("victim").inode;
  require(replacement>inode && replacement>displaced,"removed inode was reused");
  const auto removed=ram->live;const auto operations=ram->calls;
  rejects([&]{(void)writer.write_inode(inode,0,patch);},NativeFsErrorCode::not_found,
      "removed inode wrote to same-name replacement");
  rejects([&]{writer.truncate_inode(inode,0);},NativeFsErrorCode::not_found,"removed inode truncated replacement");
  require(ram->live==removed && ram->calls==operations && !writer.poisoned(),"stale inode rejection mutated/fenced healthy writer");
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  require(reopened.stat_inode(replacement).path=="victim" && reopened.read_inode(replacement,0,1).empty(),
      "independent inode reopen differs");
  require(retained.read_inode(inode,0,100000)==original,"removed retained inode lost its bytes");
}
void admission_and_sparse(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
  const auto inode=file_apply(writer,file_create("f")).created_inodes[0];
  file_apply(writer,file_create("Dir",true));const auto directory=writer.snapshot().stat("Dir").inode;
  const auto before=ram->live;const auto calls=ram->calls;const auto seq=writer.snapshot().sequence();
  const auto one=data(1,51),too_large=Bytes(kStoreMaxObjectBytes+1);
  rejects([&]{(void)writer.write_inode(inode,UINT64_MAX,one);},NativeFsErrorCode::limit,"ordinary offset overflow accepted");
  rejects([&]{(void)writer.write_inode(inode,kNativeMaxFileBytes,one);},NativeFsErrorCode::limit,"ordinary write beyond cap");
  rejects([&]{(void)writer.write_inode(inode,0,too_large,{false,true});},NativeFsErrorCode::limit,"clipping bypassed supplied input cap");
  rejects([&]{writer.truncate_inode(inode,kNativeMaxFileBytes+1);},NativeFsErrorCode::limit,"inode truncate beyond cap");
  rejects([&]{(void)writer.write_inode(0,0,one);},NativeFsErrorCode::not_found,"zero inode accepted");
  rejects([&]{(void)writer.write_inode(UINT64_MAX,0,one);},NativeFsErrorCode::not_found,"unknown inode accepted");
  rejects([&]{(void)writer.write_inode(directory,0,one);},NativeFsErrorCode::is_directory,"directory inode write accepted");
  rejects([&]{writer.truncate_inode(1,0);},NativeFsErrorCode::is_directory,"root inode truncate accepted");
  auto view=writer.snapshot();
  rejects([&]{(void)view.read_inode(directory,0,1);},NativeFsErrorCode::is_directory,"directory inode read accepted");
  rejects([&]{(void)view.list_inode(inode);},NativeFsErrorCode::not_directory,"file inode listed as directory");
  rejects([&]{(void)view.stat_inode(UINT64_MAX);},NativeFsErrorCode::not_found,"missing inode metadata fabricated");
  rejects([&]{(void)view.read_inode(inode,UINT64_MAX,kNativeMaxReadBytes+1);},NativeFsErrorCode::limit,"inode oversized EOF read bypass");
  require(ram->live==before && ram->calls==calls && writer.snapshot().sequence()==seq && !writer.poisoned(),
      "inode admission changed durable state");
  const auto maximum=data(kStoreMaxObjectBytes,53);
  const auto full_write=writer.write_inode(inode,0,maximum);
  require(full_write.bytes==kStoreMaxObjectBytes &&
      writer.snapshot().read_inode(inode,0,kStoreMaxObjectBytes)==maximum,
      "maximum admitted inode input was rejected or changed");
  writer.truncate_inode(inode,0);
  writer.truncate_inode(inode,kNativeMaxFileBytes-17);
  const auto suffix=data(17,52);const auto result=writer.write_inode(inode,0,suffix,{true,false});
  require(result.offset==kNativeMaxFileBytes-17 && result.bytes==17 &&
      writer.snapshot().stat_inode(inode).logical_bytes==kNativeMaxFileBytes,"sparse far append bounds");
  Bytes expected(31);std::copy(suffix.begin(),suffix.end(),expected.begin()+14);
  require(writer.snapshot().read_inode(inode,kNativeMaxFileBytes-31,64)==expected,"sparse inode tail bytes");
  const auto full=ram->live;const auto operations=ram->calls;
  rejects([&]{(void)writer.write_inode(inode,0,one,{true,false});},NativeFsErrorCode::limit,"append beyond logical cap");
  const auto noop=writer.write_inode(inode,0,one,{true,true});
  require(!noop.bytes && noop.offset==kNativeMaxFileBytes && ram->live==full && ram->calls==operations,
      "constrained append at logical cap mutated state");
  auto moved=std::move(view);
  require(moved.stat_inode(inode).logical_bytes==0,"old inode snapshot changed after sparse append");
  rejects([&]{(void)view.stat_inode(inode);},NativeFsErrorCode::invalid_operation,"moved-from inode stat");
  rejects([&]{(void)view.list_inode(1);},NativeFsErrorCode::invalid_operation,"moved-from inode list");
  rejects([&]{(void)view.read_inode(inode,0,1);},NativeFsErrorCode::invalid_operation,"moved-from inode read");
  auto active=std::move(writer);
  rejects([&]{(void)writer.write_inode(inode,0,one);},NativeFsErrorCode::storage_failure,"moved-from inode writer");
  rejects([&]{writer.truncate_inode(inode,0);},NativeFsErrorCode::storage_failure,"moved-from inode truncation");
  active.snapshot().verify_all();
}
void interruption(StoreCompressionMode mode) {
  const auto original=data(65530,61),addition=data(37,62);
  Bytes acknowledged=original;acknowledged.insert(acknowledged.end(),addition.begin(),addition.end());
  Bytes seeded;std::uint64_t inode=0,base_sequence=0;std::size_t steps=0;
  {
    auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode);
    file_apply(writer,file_create("f"));file_apply(writer,file_write("f",0,original));
    auto before=writer.snapshot();inode=before.stat("f").inode;base_sequence=before.sequence();seeded=ram->stable;
    ram->calls=0;const auto result=writer.write_inode(inode,UINT64_MAX,addition,{true,false});steps=ram->calls;
    require(result.bytes==addition.size() && ram->flushes>=3 && steps>3,"append fault trace missing publication");
  }
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    auto ram=image(seeded);
    {
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
      auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(ram,identity(),
          snapshot.storage_info().generation,{mode,6,false}));
      auto retained=writer.snapshot();ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{(void)writer.write_inode(inode,UINT64_MAX,addition,{true,false});},
          NativeFsErrorCode::storage_failure,"inode append hid injected I/O failure");
      require(writer.poisoned() && retained.read_inode(inode,0,100000)==original,
          "failed inode append changed acknowledged snapshot");
      const auto calls=ram->calls;
      rejects([&]{writer.truncate_inode(inode,0);},NativeFsErrorCode::storage_failure,"poisoned inode truncate retried");
      rejects([&]{(void)writer.write_inode(inode,0,addition);},NativeFsErrorCode::storage_failure,"poisoned inode write retried");
      require(ram->calls==calls,"poisoned inode method touched backing I/O");
    }
    auto recovered=image(ram->stable);
    auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(snapshot.sequence()==base_sequence || snapshot.sequence()==base_sequence+1,
        "interrupted append lost/fabricated acknowledged sequence");
    const auto content=snapshot.sequence()==base_sequence?original:acknowledged;
    require(snapshot.read_inode(inode,0,100000)==content,"recovered append is partial or lost prior bytes");
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        snapshot.storage_info().generation,{mode,6,false}));
    const auto suffix=data(3,63);const auto continued=writer.write_inode(inode,0,suffix,{true,false});
    require(continued.offset==content.size(),"recovered inode append used stale EOF");
    auto expected=content;expected.insert(expected.end(),suffix.begin(),suffix.end());
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable))).
        read_inode(inode,0,100000)==expected,"continued inode append did not independently recover");
  }
}
}
int main(){try {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    identity_and_paths(mode);admission_and_sparse(mode);interruption(mode);
  }
  std::cout<<"PASS: "<<checks<<" native inode I/O assertions; stable identity, atomic append, constrained writes, sparse bounds and interrupted recovery.\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"native inode I/O failure after "<<checks<<": "<<error.what()<<'\n';return 1;}}
