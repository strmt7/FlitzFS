#include "native_fs_fixture.hpp"
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
  for(unsigned first=0;first<63;) {
    std::vector<NativeFileMutation> ops;
    for(unsigned i=0;i<16 && first<63;++i,++first)ops.push_back(file_create("n"+std::to_string(first)));
    fs.apply(ops);
  }
  const auto before=ram->live;const auto calls=ram->calls;
  rejects([&]{file_apply(fs,file_create("overflow"));},"node cap ignored");
  require(before==ram->live && calls==ram->calls && !fs.poisoned(),"node cap partially committed");
  const std::array<NativeFileMutation,2> replace{{file_remove("n0"),file_create("replacement")}};fs.apply(replace);
  auto another=std::make_shared<Ram>();auto volume=fresh(another);file_apply(volume,file_create("f"));
  Bytes full(15*65536);
  for(std::size_t i=0;i<full.size();++i)full[i]=static_cast<std::byte>(i%251+1);
  file_apply(volume,file_write("f",0,full));require(volume.snapshot().storage_info().objects==16,"extent capacity not reached");
  const auto baseline=another->live;const auto operations=another->calls;
  rejects([&]{file_apply(volume,file_write("f",15*65536,data(1,7)));},"extent cap ignored");
  require(baseline==another->live && operations==another->calls && !volume.poisoned(),"extent cap partially committed");
  file_apply(volume,file_truncate("f",0));file_apply(volume,file_write("f",65537,data(13,11)));
  require(volume.snapshot().read("f",65500,100)==[] {
    Bytes out(50);const auto d=data(13,11);std::copy(d.begin(),d.end(),out.begin()+37);return out;}(),"freed object reused incorrectly");
  const auto stable=another->live;
  const std::array<NativeFileMutation,2> invalid{{file_create("new"),file_create("missing/child")}};
  rejects([&]{volume.apply(invalid);},"partial failed batch accepted");
  require(another->live==stable && volume.snapshot().list().size()==1,"failed batch leaked namespace");
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
  corrupt([](auto& b){set(b,8,2,4);});corrupt([](auto& b){set(b,12,65,4);});
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
  try{normal_operations();sparse_and_admission();capacity_and_rollback();rename_depth_admission();transactional_faults();malicious_metadata();independent_model();
    std::cout<<"PASS: "<<checks<<" native filesystem assertions; directories, stable IDs, sparse ranges, truncation, atomic rename/data and crash recovery.\n";return 0;
  }catch(const std::exception& e){std::cerr<<"native filesystem failure after "<<checks<<": "<<e.what()<<'\n';return 1;}
}
