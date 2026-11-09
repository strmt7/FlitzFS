#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include "record_digest.hpp"
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,NativeFsErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeFsError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
template<class F>void rejects_session(F&& action,NativeSessionErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeSessionError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
NativeBasicMetadata basic(unsigned n=1,bool directory=false) {
  const std::uint64_t base=133700000000000000ULL+100*n;
  return {base+1,base+2,base+3,base+4,directory?0x10U:0x2022U};
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
}
std::uint64_t get(const Bytes& bytes,std::size_t at,unsigned width=8) {
  std::uint64_t value=0;for(unsigned i=0;i<width;++i)
    value|=std::uint64_t(std::to_integer<unsigned char>(bytes.at(at+i)))<<(8*i);
  return value;
}
void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned width=8) {
  for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::byte>(value>>(8*i));
}
std::map<std::uint64_t,std::size_t> fields(const Bytes& bytes) {
  std::map<std::uint64_t,std::size_t> out;std::size_t cursor=64;
  const auto header=get(bytes,8,4)==3?88U:64U;
  for(std::uint64_t i=0;i<get(bytes,12,4);++i) {
    out.emplace(get(bytes,cursor),cursor);
    cursor+=header+static_cast<std::size_t>(get(bytes,cursor+48,2))+
        16*static_cast<std::size_t>(get(bytes,cursor+50,2));
  }
  require(cursor==bytes.size(),"basic namespace fixture cursor");return out;
}
void normal(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemWriter> owner(fresh(ram,mode));
  auto& writer=*owner;
  require(!writer.inspect_basic_metadata(1),"legacy root invented timestamps");
  const std::array<NativeFileMutation,4> files{{file_create("D",true),file_create("D/f"),
      file_write("D/f",0,data(73,11)),file_create("plain")}};
  auto legacy=writer.apply(files);auto old=writer.snapshot();
  require(!old.basic_metadata(3) && !old.basic_metadata(4),"legacy file invented Windows fields");
  const auto legacy_ns=ObjectStoreSnapshot::open(ram).read(1);
  require(get(legacy_ns,8,4)==1 && legacy_ns[7]==std::byte{'1'},"ordinary legacy encoding changed");
  const std::array<NativeFileMutation,2> ops{{file_write("D/f",7,data(19,12)),file_create("created")}};
  const std::array<NativeBasicMutation,4> metadata{{{1,0,basic(1,true)},
      {2,0,basic(2,true)},{3,0,basic(3)},{0,1,basic(4)}}};
  const auto barriers=ram->flushes;
  const auto committed=writer.apply_with_basic(ops,metadata);
  require(committed.sequence==legacy.sequence+1 && committed.created_inodes.size()==1 &&
      ram->flushes==barriers+3,"compound basic update did not use one three-barrier transaction");
  auto view=writer.snapshot();const auto id=committed.created_inodes.front();
  require(view.basic_metadata(1)==basic(1,true) && view.basic_metadata(2)==basic(2,true) &&
      view.basic_metadata(3)==basic(3) && view.basic_metadata(id)==basic(4) &&
      !view.basic_metadata(4),"full basic replacements/creation selector lost fields");
  require(!old.basic_metadata(3) && old.read("D/f",0,100)==data(73,11),"metadata upgrade changed retained snapshot");
  auto bytes=data(73,11);const auto patch=data(19,12);std::copy(patch.begin(),patch.end(),bytes.begin()+7);
  require(view.read("D/f",0,100)==bytes,"compound metadata/data not atomic");
  const auto ns=ObjectStoreSnapshot::open(ram).read(1);const auto map=fields(ns);
  require(get(ns,8,4)==3 && ns[7]==std::byte{'3'} &&
      get(ns,map.at(3)+20,4)==4 && get(ns,map.at(3)+52,4)==basic(3).attributes &&
      get(ns,map.at(3)+56)==basic(3).creation_time && get(ns,map.at(3)+80)==basic(3).change_time,
      "basic namespace explicit capability/fields");
  const auto calls=ram->calls;
  const auto unchanged=writer.set_basic_metadata(3,basic(3));
  require(unchanged.sequence==committed.sequence && ram->calls==calls,"identical basic tuple published");
  writer.set_basic_metadata(3,basic(5));
  require(writer.inspect_inode(3).info.modified_sequence==committed.sequence+1 &&
      writer.inspect_basic_metadata(3)==basic(5),"basic-only update did not advance logical metadata");
  file_apply(writer,file_rename("D/f","renamed"));
  require(writer.inspect_basic_metadata(3)==basic(5),"rename lost inode basic tuple");
  // Raw file I/O preserves explicit times; an OS adapter supplies authorized
  // automatic timestamps in apply_with_basic, never by an implicit library clock.
  writer.write_inode(3,0,data(3,13));
  require(writer.inspect_basic_metadata(3)==basic(5),"generic write invented a Windows timestamp");
  writer.set_delete_pending(3,true);writer.set_delete_pending(3,false);
  require(get(ObjectStoreSnapshot::open(ram).read(1),8,4)==3,"lifetime transition downgraded basic capability");
  writer.set_delete_pending(3,true);writer.unlink_inode(3);
  writer.set_basic_metadata(3,basic(6));
  require(writer.inspect_inode(3).unlinked && writer.inspect_basic_metadata(3)==basic(6) &&
      view.basic_metadata(3)==basic(3),"detached inode/old snapshot lost separate basic identity");
  writer.retire_inode(3);
  rejects([&]{(void)writer.inspect_basic_metadata(3);},NativeFsErrorCode::not_found,"retired metadata redirected");
  auto zero=NativeBasicMetadata{};writer.set_basic_metadata(id,zero);
  require(writer.inspect_basic_metadata(id)==zero,"explicit zero times were treated as request sentinels");
  auto maximum=zero;maximum.creation_time=maximum.last_access_time=maximum.last_write_time=
      maximum.change_time=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  writer.set_basic_metadata(id,maximum);
  require(writer.snapshot().basic_metadata(id)==maximum,"valid nonnegative timestamp maximum rejected");
  auto target=std::make_shared<Ram>();auto other=identity();other[3]=std::byte{9};
  const auto before_copy=writer.snapshot();owner.reset();
  auto copy=NativeFilesystemWriter::compact(before_copy,target,other,{mode,6,false});
  require(copy.snapshot().basic_metadata(id)==maximum &&
      copy.snapshot().basic_metadata(1)==basic(1,true),"compaction lost canonical Windows tuple");
  auto independent=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  require(independent.basic_metadata(id)==maximum && independent.basic_metadata(1)==basic(1,true),
      "independent CPU reopen lost basic metadata");
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  file_apply(writer,file_create("f"));const auto original=ram->live;
  for(const auto attributes:{0x80U,0x200U,0x400U,0x800U,0x4000U,0x80000000U}) {
    auto value=basic();value.attributes=attributes;const auto reads=ram->read_calls;
    rejects([&]{writer.set_basic_metadata(2,value);},NativeFsErrorCode::invalid_operation,"unsupported attribute persisted");
    require(ram->read_calls==reads && ram->live==original && !writer.poisoned(),"invalid attribute reached I/O/fenced");
  }
  for(unsigned field=0;field<4;++field)for(const auto sentinel:std::array<std::uint64_t,3>{UINT64_MAX,UINT64_MAX-1,std::uint64_t{1}<<63}) {
    auto value=basic();
    switch(field){case 0:value.creation_time=sentinel;break;case 1:value.last_access_time=sentinel;break;
      case 2:value.last_write_time=sentinel;break;default:value.change_time=sentinel;}
    const auto reads=ram->read_calls;
    rejects([&]{writer.set_basic_metadata(2,value);},NativeFsErrorCode::invalid_operation,"negative/request time persisted");
    require(ram->read_calls==reads && ram->live==original,"invalid time reached backing I/O");
  }
  auto directory=basic(1,true);
  rejects([&]{writer.set_basic_metadata(2,directory);},NativeFsErrorCode::invalid_operation,"file kind changed via attributes");
  rejects([&]{writer.set_basic_metadata(1,basic());},NativeFsErrorCode::invalid_operation,"root lost directory attribute");
  directory.attributes|=0x100;
  const auto reads=ram->read_calls;
  rejects([&]{writer.set_basic_metadata(1,directory);},NativeFsErrorCode::invalid_operation,"temporary directory metadata");
  require(ram->read_calls==reads,"temporary directory tuple reached I/O");
  for(const auto selector:std::array<NativeBasicMutation,4>{{{0,0,basic()},{2,1,basic()},
      {0,17,basic()},{0,1,basic()}}}) {
    const auto before=ram->calls;
    rejects([&]{writer.apply_with_basic({},{&selector,1});},NativeFsErrorCode::invalid_operation,"bad creation selector");
    require(ram->calls==before && ram->live==original,"bad selector caused writes");
  }
  rejects([&]{writer.set_basic_metadata(99,basic());},NativeFsErrorCode::not_found,"missing metadata target created");
  std::array<NativeBasicMutation,17> excess{};
  rejects([&]{writer.apply_with_basic({},excess);},NativeFsErrorCode::limit,"metadata operation cap ignored");
  std::array<NativeFileMutation,16> sixteen{};for(auto& op:sixteen)op=file_create("x");
  const NativeBasicMutation one{2,0,basic()};
  rejects([&]{writer.apply_with_basic(sixteen,{&one,1});},NativeFsErrorCode::limit,"combined operation cap increased");
  const std::array<NativeFileMutation,2> vanished{{file_create("vanished"),file_remove("vanished")}};
  const NativeBasicMutation created{0,1,basic()};
  rejects([&]{writer.apply_with_basic(vanished,{&created,1});},NativeFsErrorCode::not_found,"removed created selector rebound");
  require(!writer.poisoned() && ram->live==original,"failed private basic staging changed image/fenced healthy writer");
}
void session() {
  auto ram=std::make_shared<Ram>();auto owned=NativeFileSession::create(fresh(ram,StoreCompressionMode::raw));
  const auto created=owned.create_entry("f",false,basic());
  require(created.created_inodes.size()==1,"owning basic creation count");
  const auto data_writer=owned.open("f",{true,true,true});
  rejects_session([&]{owned.set_basic_metadata(data_writer,basic(2));},NativeSessionErrorCode::access_denied,
      "data-write access silently granted attribute write");
  const auto metadata_writer=owned.open("f",{false,false,true},NativeOpenKind::file,{true});
  require(owned.basic_metadata(metadata_writer)==basic(),"owning context lost basic tuple");
  owned.set_basic_metadata(metadata_writer,basic(2));
  rejects_session([&]{owned.write(metadata_writer,0,data(1,4));},NativeSessionErrorCode::access_denied,
      "attribute-write access silently granted data write");
  owned.set_delete(metadata_writer,true);owned.cleanup(metadata_writer,true);
  owned.set_basic_metadata(metadata_writer,basic(3));owned.close(metadata_writer);
  require(owned.basic_metadata(data_writer)==basic(3),"detached surviving context lost basic tuple");
  const auto id=created.created_inodes.front();owned.cleanup(data_writer,false);owned.close(data_writer);
  const auto reads=ram->read_calls;
  rejects_session([&]{owned.set_basic_metadata(metadata_writer,basic());},NativeSessionErrorCode::invalid_handle,
      "closed metadata context changed replacement");
  require(ram->read_calls==reads && owned.stats().contexts==0,"closed metadata token reached I/O/retained context");
  rejects([&]{(void)owned.snapshot().stat_inode(id);},NativeFsErrorCode::not_found,"final Close retained detached basic inode");
}
void failed_session_update() {
  for(unsigned failure=0;failure<3;++failure) {
    auto ram=std::make_shared<Ram>();auto owned=NativeFileSession::create(fresh(ram,StoreCompressionMode::raw));
    owned.create_entry("f",false,basic());
    const auto handle=owned.open("f",{true,false,false},NativeOpenKind::file,{true});
    auto old=owned.snapshot();ram->calls=0;ram->fail_at=0;ram->failure=failure;
    rejects([&]{owned.set_basic_metadata(handle,basic(2));},NativeFsErrorCode::storage_failure,
        "failed owning basic metadata update acknowledged");
    require(owned.poisoned() && owned.stats().contexts==1 && old.basic_metadata(2)==basic(),
        "failed basic metadata update released context/changed retained snapshot");
    const auto calls=ram->calls;
    rejects_session([&]{owned.set_basic_metadata(handle,basic(3));},NativeSessionErrorCode::terminal,
        "failed owning basic update retried");
    require(ram->calls==calls,"failed owning basic context performed more I/O");
    auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
    require(recovered.basic_metadata(2)==basic(),"failed first write lost earlier acknowledged tuple");
  }
}
void malformed() {
  auto ram=std::make_shared<Ram>();
  {auto writer=fresh(ram,StoreCompressionMode::raw);file_apply(writer,file_create("f"));
    writer.set_basic_metadata(2,basic());}
  const auto source=ram->stable;const auto objects=ObjectStoreSnapshot::open(ram);
  const auto original=objects.read(1);const auto map=fields(original);const auto at=map.at(2);
  const auto corrupt=[&](const std::function<void(Bytes&)>& mutate) {
    auto clone=image(source);auto bytes=original;mutate(bytes);
    {auto object_writer=ObjectStoreWriter::resume(clone,identity(),objects.info().generation);
      const StoreMutation edit{1,false,std::move(bytes)};object_writer.commit({&edit,1});}
    rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(clone));},
        NativeFsErrorCode::corrupt,"digest-valid malformed basic namespace admitted");
  };
  corrupt([](auto& b){b[7]=std::byte{'2'};});
  corrupt([](auto& b){put(b,8,2,4);});
  corrupt([](auto& b){put(b,8,4,4);});
  corrupt([&](auto& b){put(b,at+20,8,4);});
  corrupt([&](auto& b){put(b,at+20,0,4);});
  corrupt([&](auto& b){put(b,map.at(1)+52,1,4);});
  corrupt([&](auto& b){put(b,at+52,0x80,4);});
  corrupt([&](auto& b){put(b,at+52,0x10,4);});
  for(const auto offset:{56U,64U,72U,80U})
    corrupt([&](auto& b){put(b,at+offset,UINT64_MAX);});
  corrupt([&](auto& b){b.erase(b.begin()+static_cast<std::ptrdiff_t>(at+87));});
  auto legacy=source;
  for(unsigned slot=0;slot<2;++slot) {
    const auto offset=static_cast<std::size_t>(slot*kStoreCommitUnit);
    put(legacy,offset+8,1,4);std::fill_n(legacy.begin()+static_cast<std::ptrdiff_t>(offset+96),16,std::byte{0});
    const auto digest=detail::record_digest(std::span<const std::byte>(legacy).subspan(offset,4064));
    std::copy(digest.begin(),digest.end(),legacy.begin()+static_cast<std::ptrdiff_t>(offset+4064));
  }
  require(!ObjectStoreSnapshot::open(image(legacy)).info().replicated_checkpoint,"legacy root malformed fixture invalid");
  rejects([&]{(void)NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(legacy)));},
      NativeFsErrorCode::corrupt,"basic namespace admitted nonreplicated root downgrade");
  // Either current root/manifest may fail; repair preserves the latest tuple.
  const std::array<std::uint64_t,4> offsets{{0,kStoreCommitUnit,get(source,32),
      get(source,static_cast<std::size_t>(kStoreCommitUnit+32))}};
  require(offsets[2]!=offsets[3] && offsets[2]>=2*kStoreCommitUnit &&
      offsets[3]>=2*kStoreCommitUnit,"basic current manifest units not independent");
  for(const auto offset:offsets) {
    auto bytes=source;std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(offset),kStoreCommitUnit,std::byte{0});
    auto clone=image(std::move(bytes));auto degraded=ObjectStoreSnapshot::open(clone);
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(clone)).basic_metadata(2)==basic(),
        "current metadata-unit loss rolled back basic tuple");
    auto repair=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(clone,identity(),degraded.info().generation));
    require(repair.snapshot().basic_metadata(2)==basic() && !repair.snapshot().storage_info().degraded_checkpoint,
        "same-current checkpoint repair lost Windows tuple");
  }
}
void capacity() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  writer.set_basic_metadata(1,basic(1,true));
  for(unsigned first=0;first<255;first+=5) {
    std::vector<NativeFileMutation> ops;std::vector<NativeBasicMutation> updates;
    for(unsigned i=first;i<(std::min)(first+5,255U);++i) {
      auto name="f"+std::to_string(i);name.append(100-name.size(),'x');
      ops.push_back(file_create(name));ops.push_back(file_write(name,0,Bytes{std::byte{1}}));
      updates.push_back({0,i-first+1,basic(i+1)});
    }
    writer.apply_with_basic(ops,updates);
  }
  auto view=writer.snapshot();require(view.list().size()==255,"combined basic capacity lost inodes");
  const auto metadata=ObjectStoreSnapshot::open(ram).read(1);
  require(metadata.size()==52172 && metadata.size()<=kNativeExtentBytes &&
      view.storage_info().objects==kStoreMaxObjects,"basic maximum namespace/extent budget wrong");
  for(unsigned i=0;i<255;++i)
    require(view.basic_metadata(i+2)==basic(i+1),"maximum basic capacity lost tuple");
  const auto calls=ram->calls;const auto op=file_create("overflow");
  rejects([&]{writer.apply({&op,1});},NativeFsErrorCode::limit,"basic namespace silently expanded node cap");
  require(ram->calls==calls,"basic capacity rejection wrote backing");
}
enum class Phase {upgrade,update,compound};
const char* phase_name(Phase p){return p==Phase::upgrade?"upgrade":p==Phase::update?"update":"compound";}
NativeFilesystemWriter seed(std::shared_ptr<Ram> ram,StoreCompressionMode mode,Phase phase) {
  auto writer=fresh(ram,mode);
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(73,21)),
      file_create("keep"),file_write("keep",0,data(91,22))}};
  writer.apply(ops);
  if(phase!=Phase::upgrade)writer.set_basic_metadata(2,basic());
  return writer;
}
void action(NativeFilesystemWriter& writer,Phase phase) {
  if(phase!=Phase::compound){writer.set_basic_metadata(2,basic(2));return;}
  const std::array<NativeFileMutation,3> ops{{file_write("f",7,data(19,23)),
      file_rename("f","renamed"),file_create("new")}};
  const std::array<NativeBasicMutation,2> updates{{{2,0,basic(2)},{0,1,basic(3)}}};
  writer.apply_with_basic(ops,updates);
}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool complete) {
  require(view.read("keep",0,100)==data(91,22) && !view.basic_metadata(3),
      "basic interruption lost unrelated acknowledged bytes/metadata");
  auto bytes=data(73,21);
  if(phase==Phase::compound && complete){const auto patch=data(19,23);std::copy(patch.begin(),patch.end(),bytes.begin()+7);}
  require(view.read_inode(2,0,100)==bytes,"basic interruption returned mixed data");
  const auto metadata=view.basic_metadata(2);
  require(complete?metadata==basic(2):(phase==Phase::upgrade?!metadata:metadata==basic()),
      "basic interruption returned mixed/lost acknowledged Windows tuple");
  if(phase==Phase::compound) {
    require(view.stat(complete?"renamed":"f").inode==2,"compound namespace not atomic with tuple");
    if(complete)require(view.basic_metadata(view.stat("new").inode)==basic(3),"new inode lacked atomic basic tuple");
    else rejects([&]{(void)view.stat("new");},NativeFsErrorCode::not_found,"uncommitted stamped creation exposed");
  }
}
void interruption(StoreCompressionMode mode,Phase phase) {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto writer=seed(probe,mode,phase);probe->calls=0;action(writer,phase);steps=probe->calls;}
  require(steps>3 && steps<80,"basic fault trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> old;std::uint64_t sequence=0;
    {
      auto writer=seed(ram,mode,phase);old.emplace(writer.snapshot());sequence=old->sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(writer,phase);},NativeFsErrorCode::storage_failure,"basic I/O error hidden");
      require(writer.poisoned(),"basic I/O failure did not fence");
      const auto calls=ram->calls;
      rejects([&]{writer.set_basic_metadata(3,basic());},NativeFsErrorCode::storage_failure,"fenced metadata writer retried");
      require(ram->calls==calls,"fenced basic writer touched backing");
    }
    expected(*old,phase,false);
    auto recovered=image(ram->stable);auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"basic recovery lost/fabricated acknowledged sequence");
    expected(view,phase,view.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    writer.set_basic_metadata(3,basic(9));
    auto continued=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable)));
    require(continued.basic_metadata(3)==basic(9) && continued.read("keep",0,100)==data(91,22),
        "independent CPU basic recovery could not continue");
  }
  require(cases==steps*3,"basic fault campaign omitted cases");
  std::cout<<"Native basic evidence: {\"schema\":1,\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"phase\":\""<<phase_name(phase)
      <<"\",\"io_steps\":"<<steps<<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one bounded basic test group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})normal(mode);
    admission();session();failed_session_update();malformed();capacity();
    std::cout<<"Native basic invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    const auto phase=group=="--upgrade-faults"?Phase::upgrade:group=="--update-faults"?Phase::update:
        group=="--compound-faults"?Phase::compound:throw std::runtime_error("unknown basic test group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})interruption(mode,phase);
  }
  return 0;
}catch(const std::exception& error){std::cerr<<"Native basic failure: "<<error.what()<<"\n";return 1;}}
