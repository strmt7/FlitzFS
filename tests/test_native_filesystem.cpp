#include "native_fs_fixture.hpp"
#include "record_digest.hpp"
#include <functional>
#include <iostream>
#include <optional>
#include <string>
namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
template<class F>void rejects(F&& f,const char* reason){
  ++checks;try{f();}catch(const NativeFsError&){return;}throw std::runtime_error(reason);
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode=StoreCompressionMode::automatic) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
void normal_operations() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram);
  require(fs.snapshot().stat("").inode==1 && fs.snapshot().stat("").directory,"persistent root");
  const std::array<NativeFileMutation,4> setup{{file_create("Docs",true),file_create("Scratch",true),file_create("Docs/report.bin"),file_create("Scratch/other.bin")}};
  const auto created=fs.apply(setup);require(created.sequence==2 && created.created_inodes.size()==4,"atomic inode allocation");
  const auto inode=fs.snapshot().stat("docs/REPORT.bin").inode;
  auto content=Bytes(65530+512);const auto payload=data(512,7);std::copy(payload.begin(),payload.end(),content.begin()+65530);
  file_apply(fs,file_write("Docs/report.bin",65530,payload));auto old=fs.snapshot();
  require(old.read("DOCS/REPORT.BIN",65500,1000)==Bytes(content.begin()+65500,content.end()),"cross-extent short read");
  const auto prefix=data(70,8);file_apply(fs,file_write("Docs/report.bin",0,prefix));std::copy(prefix.begin(),prefix.end(),content.begin());
  require(fs.snapshot().read("Docs/report.bin",0,100000)==content,"partial overwrite preserved other bytes");
  require(old.read("Docs/report.bin",0,70)==Bytes(70),"old snapshot changed");
  file_apply(fs,file_rename("Docs","Moved"));file_apply(fs,file_rename("Moved/report.bin","Moved/REPORT.bin"));
  require(fs.snapshot().stat("moved/report.bin").inode==inode && fs.snapshot().stat("moved/report.bin").path=="Moved/REPORT.bin","rename identity/case");
  file_apply(fs,file_truncate("Moved/REPORT.bin",31));content.resize(31);
  file_apply(fs,file_truncate("Moved/REPORT.bin",3*65536+17));content.resize(3*65536+17);
  require(fs.snapshot().read("Moved/report.bin",0,300000)==content,"truncate/regrow exposed discarded bytes");
  rejects([&]{file_apply(fs,file_remove("Moved"));},"nonempty directory removal accepted");
  rejects([&]{file_apply(fs,file_rename("Moved","Moved/inside"));},"directory cycle accepted");
  file_apply(fs,file_write("Scratch/other.bin",0,data(23,9)));auto replaced=fs.snapshot();
  file_apply(fs,file_rename("Moved/REPORT.bin","Scratch/other.bin",true));file_apply(fs,file_remove("Moved"));
  require(fs.snapshot().stat("scratch/other.bin").inode==inode,"replacement changed source inode");
  require(replaced.read("Scratch/other.bin",0,100)==data(23,9),"replaced inode snapshot changed");
  const auto deleted=fs.snapshot().stat("Scratch/other.bin").inode;
  file_apply(fs,file_remove("Scratch/other.bin"));const auto next=file_apply(fs,file_create("Scratch/new"));
  require(next.created_inodes[0]>deleted,"inode ID reused");
  auto moved=std::move(old);require(moved.stat("Docs/report.bin").inode==inode,"snapshot move lost identity");
  rejects([&]{(void)old.stat("");},"moved-from snapshot operation accepted");
  auto writer=std::move(fs);rejects([&]{file_apply(fs,file_create("bad"));},"moved-from writer accepted operation");
  file_apply(writer,file_create("empty"));writer.snapshot().verify_all();
}
void sparse_and_admission() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram);
  file_apply(fs,file_create("large"));file_apply(fs,file_truncate("large",kNativeMaxFileBytes));
  require(fs.snapshot().storage_info().objects==1,"sparse size allocated extents");
  require(fs.snapshot().read("large",kNativeMaxFileBytes-99,1000)==Bytes(99),"sparse tail short read");
  file_apply(fs,file_write("large",kNativeMaxFileBytes-17,data(17,3)));
  require(fs.snapshot().read("large",kNativeMaxFileBytes-31,64)==[] {
    Bytes out(31);const auto d=data(17,3);std::copy(d.begin(),d.end(),out.begin()+14);return out;}(),"sparse far write");
  const auto bytes=ram->live;const auto calls=ram->calls;const auto seq=fs.snapshot().sequence();
  for(const auto* path:{"../bad","/bad","C:bad","bad/","CON","a//b","a/./b","a\\b"})
    rejects([&]{file_apply(fs,file_create(path));},"unsafe virtual path");
  rejects([&]{file_apply(fs,file_write("large",kNativeMaxFileBytes,data(1,1)));},"write beyond logical cap");
  rejects([&]{file_apply(fs,file_truncate("large",kNativeMaxFileBytes+1));},"truncate beyond logical cap");
  rejects([&]{file_apply(fs,file_write("missing",0,data(1,1)));},"missing file write");
  rejects([&]{file_apply(fs,file_write("large",0,Bytes(kStoreMaxObjectBytes+1)));},"oversized write");
  rejects([&]{(void)fs.snapshot().read("large",0,kNativeMaxReadBytes+1);},"oversized read");
  rejects([&]{(void)fs.apply({});},"empty batch accepted");
  NativeFileMutation unknown=file_create("large");unknown.operation=static_cast<NativeFileOperation>(99);
  rejects([&]{file_apply(fs,unknown);},"unknown operation accepted");
  require(ram->live==bytes && ram->calls==calls && fs.snapshot().sequence()==seq && !fs.poisoned(),"admission changed durable state");
  const auto noop=file_apply(fs,file_write("large",123,{}));require(noop.sequence==seq && ram->calls==calls,"zero write changed file size or generation");
  file_apply(fs,file_truncate("large",0));require(fs.snapshot().storage_info().objects==1,"truncate did not discard extent reference");
  file_apply(fs,file_write("large",0,Bytes(65537)));require(fs.snapshot().storage_info().objects==1,"zero write not sparse");
}
void capacity_and_rollback() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram);
  for(unsigned first=0;first<kNativeMaxNodes-1;) {
    std::vector<NativeFileMutation> ops;
    for(unsigned i=0;i<16 && first<kNativeMaxNodes-1;++i,++first)ops.push_back(file_create("n"+std::to_string(first)));
    fs.apply(ops);
  }
  const auto before=ram->live;const auto calls=ram->calls;
  rejects([&]{file_apply(fs,file_create("overflow"));},"node cap ignored");
  require(before==ram->live && calls==ram->calls && !fs.poisoned(),"node cap partially committed");
  const std::array<NativeFileMutation,2> replace{{file_remove("n0"),file_create("replacement")}};fs.apply(replace);
  auto another=std::make_shared<Ram>();auto volume=fresh(another);file_apply(volume,file_create("f"));
  // Reach the combined namespace/extent ceiling with maximum-length names.
  // Each write remains <=1 MiB; no batch bypasses the staging allowance.
  for(std::size_t first=2;first<kNativeMaxNodes;) {
    std::vector<NativeFileMutation> ops;
    for(unsigned i=0;i<16 && first<kNativeMaxNodes;++i,++first) {
      auto name="node"+std::to_string(first);name.resize(100,'x');ops.push_back(file_create(name));
    }
    volume.apply(ops);
  }
  const auto full=data(kNativeMaxExtents*kNativeExtentBytes,33);
  for(std::size_t at=0;at<full.size();at+=kStoreMaxObjectBytes) {
    const auto end=(std::min)(at+std::size_t(kStoreMaxObjectBytes),full.size());
    file_apply(volume,file_write("f",at,Bytes(full.begin()+static_cast<std::ptrdiff_t>(at),full.begin()+static_cast<std::ptrdiff_t>(end))));
  }
  auto populated=volume.snapshot();
  require(populated.storage_info().objects==kStoreMaxObjects && populated.list().size()==kNativeMaxNodes-1,"combined capacity not reached");
  for(std::size_t at=0;at<full.size();at+=kNativeMaxReadBytes) {
    const auto end=(std::min)(at+std::size_t(kNativeMaxReadBytes),full.size());
    require(populated.read("f",at,kNativeMaxReadBytes)==Bytes(full.begin()+static_cast<std::ptrdiff_t>(at),full.begin()+static_cast<std::ptrdiff_t>(end)),"combined-capacity byte readback");
  }
  const auto baseline=another->live;const auto operations=another->calls;
  rejects([&]{file_apply(volume,file_write("f",kNativeMaxExtents*kNativeExtentBytes,data(1,7)));},"extent cap ignored");
  require(baseline==another->live && operations==another->calls && !volume.poisoned(),"extent cap partially committed");
  file_apply(volume,file_truncate("f",0));file_apply(volume,file_write("f",65537,data(13,11)));
  require(volume.snapshot().read("f",65500,100)==[] {
    Bytes out(50);const auto d=data(13,11);std::copy(d.begin(),d.end(),out.begin()+37);return out;}(),"freed object reused incorrectly");
  const auto stable=another->live;
  const std::array<NativeFileMutation,3> invalid{{file_remove("node2"+std::string(95,'x')),file_create("new"),file_create("missing/child")}};
  rejects([&]{volume.apply(invalid);},"partial failed batch accepted");
  require(another->live==stable && volume.snapshot().list().size()==kNativeMaxNodes-1,"failed batch leaked namespace");
}
void rename_depth_admission() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram);
  const std::array<NativeFileMutation,2> setup{{file_create("A",true),file_create("A/f")}};fs.apply(setup);
  std::string parent;
  for(unsigned i=0;i<15;++i){if(!parent.empty())parent+="/";parent+="d"+std::to_string(i);file_apply(fs,file_create(parent,true));}
  const auto bytes=ram->live;const auto calls=ram->calls;
  rejects([&]{file_apply(fs,file_rename("A",parent+"/A"));},"subtree moved beyond depth cap");
  require(!fs.poisoned() && ram->calls==calls && ram->live==bytes,"depth rejection poisoned writer or wrote data");
  file_apply(fs,file_create("still-healthy"));
}
void transactional_faults() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto probe=std::make_shared<Ram>();auto writer=fresh(probe,mode);writer.apply(baseline());
    probe->calls=0;writer.apply(update());const auto steps=probe->calls;
    for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
      auto ram=std::make_shared<Ram>();
      {
        auto fs=fresh(ram,mode);fs.apply(baseline());auto retained=fs.snapshot();
        ram->calls=0;ram->fail_at=step;ram->failure=fault;
        rejects([&]{fs.apply(update());},"native I/O failure not propagated");
        require(fs.poisoned(),"failed native writer not terminal");
        expected(retained,false);expected(fs.snapshot(),false);
        const auto calls=ram->calls;
        rejects([&]{file_apply(fs,file_create("must-not-write"));},"poisoned native writer retried");
        require(ram->calls==calls,"poisoned native writer touched I/O");
      }
      auto recovered=image(ram->stable);
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
      require(snapshot.sequence()==2 || snapshot.sequence()==3,"native acknowledged sequence lost/fabricated");
      const bool updated=snapshot.sequence()==3;expected(snapshot,updated);
      auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),snapshot.storage_info().generation,{mode,6,false}));
      const std::array<NativeFileMutation,2> more{{file_create("continued"),file_write("continued",0,data(3073,91))}};
      fs.apply(more);expected(fs.snapshot(),updated,true);
      expected(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable))),updated,true);
    }
  }
}
std::uint64_t get(const Bytes& b,std::size_t at,unsigned width=8) {
  std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return value;
}
void set(Bytes& b,std::size_t at,std::uint64_t value,unsigned width=8) {
  for(unsigned i=0;i<width;++i)b[at+i]=static_cast<std::byte>(value>>(8*i));
}
struct Fields {std::size_t header,name,extents;};
std::map<std::uint64_t,Fields> fields(const Bytes& b) {
  std::map<std::uint64_t,Fields> result;std::size_t cursor=64;
  for(std::uint64_t i=0;i<get(b,12,4);++i) {
    const auto begin=cursor,names=static_cast<std::size_t>(get(b,cursor+48,2)),count=static_cast<std::size_t>(get(b,cursor+50,2));
    result.emplace(get(b,cursor),Fields{begin,begin+64,begin+64+names});cursor+=64+names+count*16;
  }
  return result;
}
void malicious_metadata() {
  auto ram=std::make_shared<Ram>();
  {auto fs=fresh(ram);const std::array<NativeFileMutation,4> ops{{file_create("D",true),file_create("D/F"),file_write("D/F",3,data(17,4)),file_create("E")}};fs.apply(ops);}
  const auto original=ram->stable;
  const auto snapshot=ObjectStoreSnapshot::open(image(original));
  const auto metadata=snapshot.read(1);const auto positions=fields(metadata);
  auto corrupt=[&](std::function<void(Bytes&)> edit) {
    auto backend=image(original);
    {auto writer=ObjectStoreWriter::resume(backend,identity(),snapshot.info().generation);
      auto bytes=metadata;edit(bytes);const StoreMutation mutation{1,false,std::move(bytes)};writer.commit({&mutation,1});}
    rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(backend));},"intact malformed namespace accepted");
  };
  corrupt([](auto& b){set(b,8,2,4);});corrupt([](auto& b){set(b,12,kNativeMaxNodes+1,4);});
  corrupt([](auto& b){set(b,24,1);});corrupt([](auto& b){set(b,16,(std::numeric_limits<std::uint64_t>::max)());});
  corrupt([](auto& b){set(b,32,4096,4);});corrupt([](auto& b){b[56]=std::byte{1};});
  corrupt([](auto& b){set(b,64+8,1);});corrupt([&](auto& b){set(b,positions.at(3).header+8,3);});
  corrupt([&](auto& b){set(b,positions.at(3).header+8,4);});
  corrupt([&](auto& b){set(b,positions.at(3).header+24,kNativeMaxFileBytes+1);});
  corrupt([&](auto& b){b[positions.at(3).name]=std::byte{0};});
  corrupt([&](auto& b){set(b,positions.at(3).extents,1);});
  corrupt([&](auto& b){set(b,positions.at(3).extents+8,1);});
  corrupt([](auto& b){b[40]^=std::byte{1};});
  corrupt([&](auto& b){set(b,positions.at(3).header+40,0);});
  corrupt([](auto& b){b.push_back(std::byte{0});});
  corrupt([](auto& b){b.resize(65537);});
  auto bad_extent=[&](Bytes bytes) {
    auto backend=image(original);const auto object=get(metadata,positions.at(3).extents+8);
    {auto writer=ObjectStoreWriter::resume(backend,identity(),snapshot.info().generation);
      const StoreMutation m{object,false,std::move(bytes)};writer.commit({&m,1});}
    rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(backend));},"noncanonical extent content accepted");
  };
  bad_extent(Bytes(65536));auto tail=snapshot.read(get(metadata,positions.at(3).extents+8));tail[100]=std::byte{1};bad_extent(tail);
  {
    auto backend=image(original);
    {auto writer=ObjectStoreWriter::resume(backend,identity(),snapshot.info().generation);const StoreMutation extra{16,false,data(1,8)};writer.commit({&extra,1});}
    rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(backend));},"foreign object accepted");
  }
}

void scalable_staging_and_integrity() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,StoreCompressionMode::raw);
  file_apply(fs,file_create("large"));
  constexpr std::size_t extents=200;
  const auto original=data(extents*kNativeExtentBytes,117);
  for(std::size_t at=0;at<original.size();at+=kStoreMaxObjectBytes) {
    const auto end=(std::min)(at+std::size_t(kStoreMaxObjectBytes),original.size());
    file_apply(fs,file_write("large",at,Bytes(original.begin()+static_cast<std::ptrdiff_t>(at),original.begin()+static_cast<std::ptrdiff_t>(end))));
  }
  const auto replacement=data(128,118);const auto position=77ULL*kNativeExtentBytes+17;
  ram->read_bytes=ram->read_calls=0;
  file_apply(fs,file_write("large",position,replacement));
  const auto small_write_reads=ram->read_bytes;
  require(small_write_reads<3ULL*kNativeExtentBytes && small_write_reads>=kNativeExtentBytes,"partial write reread unrelated volume data");
  ram->read_bytes=ram->read_calls=0;
  file_apply(fs,file_rename("large","renamed"));
  require(ram->read_bytes<kNativeExtentBytes+128,"namespace-only operation reread data extents");
  ram->read_bytes=ram->read_calls=0;
  auto snapshot=fs.snapshot();
  require(ram->read_bytes>=extents*std::uint64_t(kNativeExtentBytes),"public snapshot skipped full verification");
  std::cout<<"Native staging evidence: raw_live_bytes="<<original.size()
           <<" partial_write_bytes=128 backing_read_bytes="<<small_write_reads
           <<" public_snapshot_backing_read_bytes="<<ram->read_bytes<<'\n';
  require(snapshot.read("renamed",position,128)==replacement,"incremental write bytes wrong");
  require(snapshot.read("renamed",0,128)==Bytes(original.begin(),original.begin()+128),"untouched extent changed");
  // Corrupt an untouched record AFTER admission. Write staging deliberately
  // avoids background full-volume scrubbing; exposure still performs it.
  auto corrupt_record=[&](std::uint64_t id) {
    const auto slot=static_cast<std::size_t>((snapshot.storage_info().generation%2)*kStoreCommitUnit);
    const auto index=static_cast<std::size_t>(get(ram->live,slot+32));
    const auto count=get(ram->live,index+12,4);
    for(std::size_t i=0;i<count;++i)if(get(ram->live,index+64+64*i)==id) {
      const auto record=static_cast<std::size_t>(get(ram->live,index+64+64*i+16));
      ram->live[record+128]^=std::byte{1};return;
    }
    throw std::runtime_error("missing corruption fixture record");
  };
  corrupt_record(3); // Block 1; the next write touches only block 0.
  file_apply(fs,file_write("renamed",0,data(9,119)));
  rejects([&]{(void)snapshot.read("renamed",kNativeExtentBytes,1);},"per-read corrupt extent exposed");
  const auto calls=ram->calls;
  rejects([&]{file_apply(fs,file_write("renamed",kNativeExtentBytes,data(9,120)));},"staging used corrupt touched data");
  require(fs.poisoned() && ram->calls==calls,"corrupt read did not fence before backing writes");
  rejects([&]{(void)fs.snapshot();},"public snapshot exposed corrupt untouched data");

  auto metadata_ram=std::make_shared<Ram>();auto metadata_fs=fresh(metadata_ram,StoreCompressionMode::raw);
  file_apply(metadata_fs,file_create("f"));
  const auto metadata_snapshot=metadata_fs.snapshot();
  const auto slot=static_cast<std::size_t>((metadata_snapshot.storage_info().generation%2)*kStoreCommitUnit);
  const auto index=static_cast<std::size_t>(get(metadata_ram->live,slot+32));
  const auto record=static_cast<std::size_t>(get(metadata_ram->live,index+64+16));
  metadata_ram->live[record+128]^=std::byte{1};
  const auto metadata_calls=metadata_ram->calls;
  rejects([&]{file_apply(metadata_fs,file_rename("f","g"));},"namespace staging skipped metadata integrity");
  require(metadata_fs.poisoned() && metadata_ram->calls==metadata_calls,"metadata corruption changed backing data");
  auto snapshot_ram=std::make_shared<Ram>();auto snapshot_fs=fresh(snapshot_ram,StoreCompressionMode::raw);
  file_apply(snapshot_fs,file_create("f"));file_apply(snapshot_fs,file_write("f",0,data(19,121)));
  const auto info=snapshot_fs.snapshot().storage_info();
  const auto root=static_cast<std::size_t>((info.generation%2)*kStoreCommitUnit);
  const auto catalog=static_cast<std::size_t>(get(snapshot_ram->live,root+32));
  const auto data_record=static_cast<std::size_t>(get(snapshot_ram->live,catalog+128+16));
  snapshot_ram->live[data_record+128]^=std::byte{1};const auto snapshot_calls=snapshot_ram->calls;
  try{(void)snapshot_fs.snapshot();throw std::runtime_error("snapshot corruption accepted");}
  catch(const NativeFsError& e){require(e.code()==NativeFsErrorCode::storage_failure,"snapshot leaked non-filesystem storage error");}
  require(snapshot_fs.poisoned(),"failed full snapshot verification did not fence writer");
  rejects([&]{file_apply(snapshot_fs,file_create("must-not-write"));},"snapshot-fenced writer retried");
  require(snapshot_ram->calls==snapshot_calls,"snapshot failure permitted backing writes");
}

// Inspect the actual immutable record identities/offsets, not just decoded bytes.
using RecordLocations=std::map<std::uint64_t,std::pair<std::uint64_t,std::uint64_t>>;
RecordLocations records(const std::shared_ptr<Ram>& ram) {
  const auto objects=ObjectStoreSnapshot::open(ram);const auto info=objects.info();
  const auto root=static_cast<std::size_t>((info.generation%2)*kStoreCommitUnit);
  const auto catalog=static_cast<std::size_t>(get(ram->live,root+32));
  RecordLocations result;
  for(std::uint64_t i=0;i<get(ram->live,catalog+12,4);++i) {
    const auto at=catalog+64+static_cast<std::size_t>(i)*64,id=get(ram->live,at);
    if(id!=1)result.emplace(id,std::pair{get(ram->live,at+8),get(ram->live,at+16)});
  }
  return result;
}
void unchanged_extent_reuse() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();auto fs=fresh(ram,mode);
    const auto content=data(2*kNativeExtentBytes,301);
    const std::array<NativeFileMutation,3> setup{{file_create("early"),file_create("f"),file_write("f",0,content)}};
    fs.apply(setup);auto retained=fs.snapshot();
    const auto original=records(ram);const auto inode=retained.stat("f").inode;
    const auto before=retained.storage_info().physical_end,written=ram->write_bytes,flushes=ram->flushes;
    const auto identical=file_apply(fs,file_write("f",0,content));
    const auto identical_appended=identical.storage.physical_end-before;
    const auto identical_written=ram->write_bytes-written;
    require(records(ram)==original,"identical full write replaced payload records");
    require(identical.sequence==retained.sequence()+1 && identical.storage.generation>retained.storage_info().generation &&
            fs.snapshot().stat("f").modified_sequence==identical.sequence,"identical write skipped namespace publication");
    require(ram->flushes-flushes==3 && identical.storage.replicated_checkpoint &&
            !identical.storage.degraded_checkpoint,"identical write skipped durable replicated barriers");
    const auto partial=Bytes(content.begin()+65520,content.begin()+65570);
    file_apply(fs,file_write("f",65520,partial));
    require(records(ram)==original,"identical cross-extent write replaced payload records");
    const std::array<NativeFileMutation,3> reversible{{file_write("f",13,data(71,302)),
        file_rename("f","renamed"),file_write("renamed",13,Bytes(content.begin()+13,content.begin()+84))}};
    fs.apply(reversible);
    require(records(ram)==original && fs.snapshot().stat("renamed").inode==inode,"reverted renamed extent not reused");
    // A no-op after a real edit must retain the earlier changed buffer.
    const auto edit=data(19,303);
    const std::array<NativeFileMutation,2> repeated{{file_write("renamed",0,edit),file_write("renamed",0,edit)}};
    fs.apply(repeated);auto expected=content;std::copy(edit.begin(),edit.end(),expected.begin());
    require(fs.snapshot().read("renamed",0,200000)==expected,"repeated changed write discarded pending bytes");
    file_apply(fs,file_write("renamed",0,content));
    const auto restored=records(ram);
    // Inode 2 needs a new ID BEFORE inode 3 in allocation order. Its new object
    // must not steal an original ID restored from a temporarily sparse block.
    const auto early=data(31,304);
    const std::array<NativeFileMutation,3> zero_restore{{file_write("renamed",0,Bytes(content.size())),
        file_write("early",0,early),file_write("renamed",0,content)}};
    fs.apply(zero_restore);const auto with_early=records(ram);
    for(const auto& [id,record]:restored)require(with_early.at(id)==record,"restored extent ID was reallocated first");
    require(with_early.size()==restored.size()+1 && fs.snapshot().read("early",0,100)==early,"reused record aliased new inode");
    require(fs.snapshot().read("renamed",0,200000)==content && retained.read("f",0,200000)==content,"reuse changed committed/retained bytes");
    const std::array<NativeFileMutation,3> shrink_restore{{file_truncate("renamed",7),
        file_truncate("renamed",content.size()),file_write("renamed",0,content)}};
    fs.apply(shrink_restore);
    require(records(ram)==with_early,"shrink/regrow/restoration rewrote original extents");
    const std::array<NativeFileMutation,2> shrink_grow{{file_truncate("renamed",7),file_truncate("renamed",content.size())}};
    fs.apply(shrink_grow);auto truncated=content;std::fill(truncated.begin()+7,truncated.end(),std::byte{0});
    require(fs.snapshot().read("renamed",0,200000)==truncated,"reuse resurrected discarded truncate bytes");
    const auto padded=records(ram);
    const std::array<NativeFileMutation,2> zero_tail{{file_truncate("renamed",9),file_truncate("renamed",content.size())}};
    fs.apply(zero_tail);
    require(records(ram)==padded && fs.snapshot().read("renamed",0,200000)==truncated,"already-zero truncate tail rewrote payload");
    const auto changed_before=fs.snapshot().storage_info().physical_end,changed_written=ram->write_bytes;
    file_apply(fs,file_write("renamed",0,content));
    const auto changed_appended=fs.snapshot().storage_info().physical_end-changed_before;
    require(changed_appended>identical_appended && ram->write_bytes-changed_written>identical_written,
            "identical write did not reduce actual appended/written bytes");
    const auto prior=records(ram);const auto old_inode=fs.snapshot().stat("renamed").inode;
    const std::array<NativeFileMutation,3> recreate{{file_remove("renamed"),file_create("renamed"),file_write("renamed",0,content)}};
    fs.apply(recreate);
    require(fs.snapshot().stat("renamed").inode>old_inode && records(ram)!=prior,"deleted inode inherited prior payload identity");
    file_apply(fs,file_create("sparse"));const auto allocated=records(ram);
    file_apply(fs,file_write("sparse",65529,Bytes(93)));
    require(records(ram)==allocated && fs.snapshot().read("sparse",65529,100)==Bytes(93),"identical sparse write allocated data");
    auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
    require(reopened.read("renamed",0,200000)==content && reopened.read("early",0,100)==early,"independent reopen lost reused bytes");
    std::cout<<"Native reuse evidence: mode="<<(mode==StoreCompressionMode::raw?"raw":"automatic")
             <<" identical_input_bytes="<<content.size()<<" identical_appended_bytes="<<identical_appended
             <<" identical_backing_write_bytes="<<identical_written<<" changed_appended_bytes="<<changed_appended
             <<" retained_payload_records="<<original.size()<<'\n';
  }
}
void reuse_read_failure() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,StoreCompressionMode::raw);
  const auto content=data(kNativeExtentBytes,305);
  const std::array<NativeFileMutation,2> setup{{file_create("f"),file_write("f",0,content)}};
  fs.apply(setup);const auto payload=records(ram).begin()->second.second;
  // First decode stages the record; second decode independently checks reuse.
  ram->corrupt_read_offset=payload+128;ram->corrupt_read_number=2;ram->matching_reads=0;
  const auto live=ram->live;const auto calls=ram->calls;
  const std::array<NativeFileMutation,2> reversible{{file_write("f",11,data(7,306)),
      file_write("f",11,Bytes(content.begin()+11,content.begin()+18))}};
  rejects([&]{fs.apply(reversible);},"reuse comparison accepted corrupt backing bytes");
  require(ram->matching_reads==2 && fs.poisoned() && ram->calls==calls && ram->live==live,
          "reuse verification failed to fence before writes");
  rejects([&]{file_apply(fs,file_create("never"));},"reuse-fenced writer retried");
  ram->corrupt_read_number=0;
  require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable))).read("f",0,100000)==content,
          "reuse comparison failure damaged acknowledged bytes");
}
void reuse_transaction_faults() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    const auto content=data(kNativeExtentBytes,307);
    const std::array<NativeFileMutation,2> setup{{file_create("f"),file_write("f",0,content)}};
    const std::array<NativeFileMutation,3> reverted{{file_write("f",0,Bytes(content.size())),
        file_write("f",0,content),file_rename("f","g")}};
    auto probe=std::make_shared<Ram>();auto writer=fresh(probe,mode);writer.apply(setup);
    const auto locations=records(probe);probe->calls=0;writer.apply(reverted);const auto steps=probe->calls;
    require(records(probe)==locations,"fault probe did not actually elide payload");
    for(unsigned failure=0;failure<3;++failure)for(std::size_t step=0;step<steps;++step) {
      auto ram=std::make_shared<Ram>();
      {
        auto fs=fresh(ram,mode);fs.apply(setup);auto retained=fs.snapshot();
        ram->calls=0;ram->fail_at=step;ram->failure=failure;
        rejects([&]{fs.apply(reverted);},"elided transaction hid injected I/O failure");
        require(fs.poisoned() && retained.read("f",0,100000)==content,"elided failed transaction changed retained bytes");
        const auto calls=ram->calls;
        rejects([&]{file_apply(fs,file_create("never"));},"elided poisoned transaction retried");
        require(ram->calls==calls,"elided poisoned writer performed I/O");
      }
      auto disk=image(ram->stable);auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(disk));
      const bool newer=snapshot.sequence()==3;
      require(snapshot.sequence()==2 || newer,"elided transaction lost acknowledged sequence");
      const auto path=newer?"g":"f";
      require(snapshot.list().size()==1 && snapshot.read(path,0,100000)==content &&
              records(disk)==locations,"elided interruption damaged acknowledged payload identity");
      auto resumed=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(disk,identity(),snapshot.storage_info().generation,{mode,6,false}));
      file_apply(resumed,file_write(path,0,content));
      require(resumed.snapshot().read(path,0,100000)==content && records(disk)==locations,"elided recovery continuation rewrote payload");
    }
  }
}

void dirty_budget() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();auto fs=fresh(ram,mode);file_apply(fs,file_create("f"));
    const auto block=data(kNativeExtentBytes,222);
    std::vector<NativeFileMutation> maximum;
    std::size_t done=0;
    while(done<kNativeMaxDirtyExtents) {
      const auto count=(std::min)(std::size_t{16},kNativeMaxDirtyExtents-done);
      Bytes payload(count*kNativeExtentBytes);
      for(std::size_t i=0;i<count;++i)std::copy(block.begin(),block.end(),payload.begin()+static_cast<std::ptrdiff_t>(i*kNativeExtentBytes));
      maximum.push_back(file_write("f",done*kNativeExtentBytes,std::move(payload)));done+=count;
    }
    fs.apply(maximum);
    require(fs.snapshot().storage_info().objects==1+kNativeMaxDirtyExtents,"maximum dirty batch rejected");
    const auto before=ram->live;const auto calls=ram->calls;
    // Same input allowance, but 64 distinct dirty records leave no full
    // metadata reserve. Rejection must occur entirely before backing writes.
    // Existing identical bytes no longer consume dirty slots. Make every
    // old block genuinely different before testing the unchanged hard cap.
    for(auto& op:maximum)for(auto& b:op.data)b^=std::byte{1};
    maximum.back().data.insert(maximum.back().data.end(),block.begin(),block.end());
    rejects([&]{fs.apply(maximum);},"dirty decoded staging cap exceeded");
    require(!fs.poisoned() && before==ram->live && calls==ram->calls,"dirty-cap rejection partly committed");
    auto snapshot=fs.snapshot();
    for(std::size_t i=0;i<kNativeMaxDirtyExtents;++i)
      require(snapshot.read("f",i*kNativeExtentBytes,kNativeExtentBytes)==block,"maximum dirty batch readback");
  }
}


void native_compacted_copies() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> source,old;
    {
      auto writer=fresh(ram,mode);writer.apply(baseline());old.emplace(writer.snapshot());writer.apply(update());
      source.emplace(writer.snapshot());auto rejected=std::make_shared<Ram>();auto id=identity();id[2]=std::byte{11};
      rejects([&]{(void)NativeFilesystemWriter::compact(*source,rejected,id,{mode,6,false});},"active native source copied");
      require(rejected->live.empty(),"active native source changed target");
    }
    const auto untouched=ram->live;const auto calls=ram->calls;
    const auto info=source->stat("target.bin");auto id=identity();id[2]=std::byte{11};
    auto target=std::make_shared<Ram>();Bytes compacted_baseline;
    rejects([&]{(void)NativeFilesystemWriter::compact(*source,target,identity());},"same native volume identity copied");
    rejects([&]{(void)NativeFilesystemWriter::compact(*source,target,{});},"zero native volume identity copied");
    rejects([&]{(void)NativeFilesystemWriter::compact(*old,target,id);},"stale native snapshot copied");
    require(target->live.empty(),"native compaction admission wrote bytes");
    {
      auto copy=NativeFilesystemWriter::compact(*source,target,id,{mode,6,false});
      auto snapshot=copy.snapshot();expected(snapshot,true);
      const auto got=snapshot.stat("target.bin");
      require(snapshot.identity()==id && snapshot.sequence()==source->sequence() &&
          got.inode==info.inode && got.created_sequence==info.created_sequence && got.modified_sequence==info.modified_sequence &&
          snapshot.storage_info().generation==source->storage_info().generation+2,"native compaction identity/sequence metadata");
      require(snapshot.storage_info().physical_end<source->storage_info().physical_end,"native history not compacted");
      compacted_baseline=target->stable;
      const std::array<NativeFileMutation,2> more{{file_create("continued"),file_write("continued",0,data(3073,91))}};
      copy.apply(more);expected(copy.snapshot(),true,true);
    }
    expected(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(target->stable))),true,true);
    require(ram->live==untouched && ram->calls==calls,"native source modified");
    expected(*source,true);expected(*old,false);
    for(unsigned slot=0;slot<2;++slot) {
      // Both freshly compacted roots name IDENTICAL acknowledged contents.
      // Damage either one before adding a later, different transaction.
      auto damaged=image(compacted_baseline);
      std::fill_n(damaged->live.begin()+static_cast<std::ptrdiff_t>(slot*kStoreCommitUnit),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      auto survivor=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(damaged));expected(survivor,true);
      {
        auto repaired=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(damaged,id,survivor.storage_info().generation,{mode,6,false}));
        expected(repaired.snapshot(),true);repaired.snapshot().verify_all();
        const std::array<NativeFileMutation,2> more{{file_create("continued"),file_write("continued",0,data(3073,91))}};
        repaired.apply(more);expected(repaired.snapshot(),true,true);
      }
      expected(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(damaged->stable))),true,true);
    }
    // After a different acknowledged transaction, fault only the obsolete
    // root's failure unit. Never count rollback of continued as safe repair.
    auto current=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(target->stable)));
    auto damaged=image(target->stable);const auto obsolete=(current.storage_info().generation+1)%2;
    std::fill_n(damaged->live.begin()+static_cast<std::ptrdiff_t>(obsolete*kStoreCommitUnit),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
    auto survivor=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(damaged));expected(survivor,true,true);
    auto repaired=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(damaged,id,survivor.storage_info().generation,{mode,6,false}));
    expected(repaired.snapshot(),true,true);repaired.snapshot().verify_all();
  }
}


void native_replicated_acknowledgements() {
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    auto ram=std::make_shared<Ram>();
    {auto files=fresh(ram,mode);files.apply(baseline());files.apply(update());}
    const auto acknowledged=ram->stable;
    const auto original=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
    const auto info=original.storage_info();require(info.replicated_checkpoint && !info.degraded_checkpoint,"native writes are not replicated");
    const auto at=static_cast<std::size_t>((info.generation%2)*kStoreCommitUnit);
    const std::array<std::uint64_t,4> units{{0,kStoreCommitUnit,get(acknowledged,at+32),get(acknowledged,at+96)}};
    for(const auto unit:units) {
      auto disk=image(acknowledged);
      std::fill_n(disk->live.begin()+static_cast<std::ptrdiff_t>(unit),static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
      auto snapshot=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(disk));
      require(snapshot.sequence()==3 && snapshot.storage_info().degraded_checkpoint,"latest native acknowledgement rolled back");
      expected(snapshot,true);
      {
        auto files=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(disk,identity(),snapshot.storage_info().generation,{mode,6,false}));
        expected(files.snapshot(),true);require(!files.snapshot().storage_info().degraded_checkpoint,"native replica repair incomplete");
        const std::array<NativeFileMutation,2> more{{file_create("continued"),file_write("continued",0,data(3073,91))}};
        files.apply(more);
      }
      expected(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(disk->stable))),true,true);
    }
    // A legacy native namespace remains readable. Writable admission upgrades
    // its publication protocol without changing inode/content/sequence bytes.
    auto legacy=acknowledged;
    for(unsigned slot=0;slot<2;++slot) {
      const auto root=static_cast<std::size_t>(slot*kStoreCommitUnit);
      set(legacy,root+8,1,4);std::fill_n(legacy.begin()+static_cast<std::ptrdiff_t>(root+96),16,std::byte{0});
      const auto digest=detail::record_digest(std::span<const std::byte>(legacy).subspan(root,4064));
      std::copy(digest.begin(),digest.end(),legacy.begin()+static_cast<std::ptrdiff_t>(root+4064));
    }
    auto disk=image(legacy);const auto read=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(disk));
    require(!read.storage_info().replicated_checkpoint,"legacy fixture not legacy");expected(read,true);
    {
      auto files=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(disk,identity(),read.storage_info().generation,{mode,6,false}));
      auto upgraded=files.snapshot();expected(upgraded,true);
      require(upgraded.sequence()==read.sequence() && upgraded.stat("target.bin").inode==read.stat("target.bin").inode &&
          upgraded.storage_info().replicated_checkpoint && !upgraded.storage_info().degraded_checkpoint,"legacy native upgrade changed logical state");
    }
    expected(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(disk->stable))),true);
  }
}

void independent_model() {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram);std::map<std::string,Bytes> expected_bytes;
  for(const auto* name:{"a","b","c"}){file_apply(fs,file_create(name));expected_bytes[name]={};}
  std::uint64_t rng=0xbe5466cf34e90c6cULL;
  for(unsigned step=0;step<80;++step) {
    rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
    const std::string name(1,static_cast<char>('a'+rng%3));auto& oracle=expected_bytes[name];
    if(step%4==0) {
      const auto size=static_cast<std::size_t>(rng%180001);file_apply(fs,file_truncate(name,size));oracle.resize(size);
    } else {
      const auto offset=static_cast<std::size_t>(rng%170001),count=static_cast<std::size_t>(rng%97+1);
      const auto bytes=step%5?data(count,step):Bytes(count);
      file_apply(fs,file_write(name,offset,bytes));if(oracle.size()<offset+count)oracle.resize(offset+count);
      std::copy(bytes.begin(),bytes.end(),oracle.begin()+static_cast<std::ptrdiff_t>(offset));
    }
    auto snapshot=fs.snapshot();
    for(const auto& [path,bytes]:expected_bytes) {
      require(snapshot.stat(path).logical_bytes==bytes.size(),"independent size oracle");
      require(snapshot.read(path,0,200000)==bytes,"independent byte-vector oracle");
    }
  }
}
}
int main(){
  try{normal_operations();sparse_and_admission();capacity_and_rollback();rename_depth_admission();transactional_faults();malicious_metadata();scalable_staging_and_integrity();unchanged_extent_reuse();reuse_read_failure();reuse_transaction_faults();dirty_budget();native_compacted_copies();native_replicated_acknowledgements();independent_model();
    std::cout<<"PASS: "<<checks<<" native filesystem assertions; directories, stable IDs, sparse ranges, truncation, atomic rename/data and crash recovery.\n";return 0;
  }catch(const std::exception& e){std::cerr<<"native filesystem failure after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
