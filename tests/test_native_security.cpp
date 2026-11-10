#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <functional>
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected native security rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected owned security rejection");
}
NativeBasicMetadata tuple(unsigned n=1){return {100+n,200+n,300+n,400+n,0x20};}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact=false) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void framing() {
  for(auto value:{sec::descriptor(),sec::null_dacl(),sec::null_dacl(false),sec::empty_dacl()})
    require(native_security_descriptor_framed(value),"valid bounded descriptor framing rejected");
  const auto invalid=[](auto mutate) {
    auto value=sec::descriptor();mutate(value);
    require(!native_security_descriptor_framed(value),"malformed descriptor framing accepted");
  };
  for(std::size_t size=0;size<72;++size) {
    auto value=sec::descriptor();value.resize(size);
    require(!native_security_descriptor_framed(value),"truncated descriptor admitted");
  }
  invalid([](auto& b){b.resize(kNativeMaxSecurityBytes+4);});
  invalid([](auto& b){sec::put(b,0,2,1);});
  invalid([](auto& b){sec::put(b,2,4,2);});
  invalid([](auto& b){sec::put(b,2,0x80c4,2);});
  invalid([](auto& b){sec::put(b,1,1,1);});
  for(const auto at:{4U,8U,12U,16U}) {
    invalid([&](auto& b){sec::put(b,at,16);});
    invalid([&](auto& b){sec::put(b,at,21);});
    invalid([&](auto& b){sec::put(b,at,UINT32_MAX);});
  }
  invalid([](auto& b){sec::put(b,21,16,1);});
  invalid([](auto& b){sec::put(b,20,2,1);});
  invalid([](auto& b){sec::put(b,2,0x8000,2);});
  invalid([](auto& b){sec::put(b,44,1,1);});
  invalid([](auto& b){sec::put(b,46,4,2);});
  invalid([](auto& b){sec::put(b,46,32,2);});
  invalid([](auto& b){sec::put(b,48,8,2);});
  invalid([](auto& b){sec::put(b,50,1,2);});
  invalid([](auto& b){sec::put(b,54,0,2);});
  invalid([](auto& b){sec::put(b,54,24,2);});
  auto shared=sec::descriptor();sec::put(shared,8,20);
  require(native_security_descriptor_framed(shared),"shared owner/group SID framing rejected");
  auto rm=sec::descriptor();sec::put(rm,2,0xc004,2);sec::put(rm,1,37,1);
  require(native_security_descriptor_framed(rm),"opaque RM control byte lost");
}
std::map<std::uint64_t,std::size_t> fields(const Bytes& bytes) {
  std::map<std::uint64_t,std::size_t> out;std::size_t at=64;
  for(std::size_t i=0;i<sec::get(bytes,12);++i) {
    out.emplace(sec::get(bytes,at,8),at);
    at+=96+static_cast<std::size_t>(sec::get(bytes,at+48,2))+
        static_cast<std::size_t>(sec::get(bytes,at+88))+16*static_cast<std::size_t>(sec::get(bytes,at+50,2));
  }
  require(at==bytes.size(),"security namespace fixture cursor");return out;
}
void normal(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemWriter> owner(fresh(ram,mode,compact));
  auto& writer=*owner;
  const std::array<NativeFileMutation,4> initial{{file_create("f"),file_write("f",0,data(73,1)),
      file_create("keep"),file_write("keep",0,data(91,2))}};
  writer.apply(initial);auto old=writer.snapshot();const auto before=ram->flushes;
  require(!old.security_descriptor(2),"legacy descriptor invented");
  const std::array<NativeFileMutation,3> ops{{file_write("f",7,data(19,3)),file_rename("f","renamed"),file_create("new")}};
  const std::array<NativeBasicMutation,2> basic{{{2,0,tuple(2)},{0,1,tuple(3)}}};
  const std::array<NativeSecurityMutation,3> security{{{1,0,sec::empty_dacl()},
      {2,0,sec::descriptor(2)},{0,1,sec::null_dacl()}}};
  const auto result=writer.apply_with_metadata(ops,basic,security);
  require(ram->flushes==before+3 && result.sequence==old.sequence()+1,"security compound publication not atomic");
  auto view=writer.snapshot();auto expected=data(73,1);const auto patch=data(19,3);
  std::copy(patch.begin(),patch.end(),expected.begin()+7);
  require(view.read("renamed",0,100)==expected && view.stat("renamed").inode==2 &&
      view.basic_metadata(2)==tuple(2) && view.security_descriptor(2)==sec::descriptor(2),
      "security/basic/data/rename mixture");
  require(view.security_descriptor(1)==sec::empty_dacl() &&
      view.security_descriptor(result.created_inodes.at(0))==sec::null_dacl() &&
      !view.security_descriptor(3),"new/root/unrelated security presence wrong");
  require(!old.security_descriptor(2) && old.read("f",0,100)==data(73,1),"retained security snapshot changed");
  const auto encoded=ObjectStoreSnapshot::open(ram).read(1);
  require(sec::get(encoded,8)==4 && encoded[7]==std::byte{'4'},"security namespace capability absent");
  auto noop=writer.set_security_descriptor(2,sec::descriptor(2));
  require(noop.sequence==result.sequence && ram->flushes==before+3,"identical descriptor published");
  writer.set_basic_metadata(2,tuple(9));
  require(sec::get(ObjectStoreSnapshot::open(ram).read(1),8)==4 &&
      writer.snapshot().security_descriptor(2)==sec::descriptor(2),"basic update downgraded security capability");
  file_apply(writer,file_truncate("renamed",1000));
  require(writer.snapshot().security_descriptor(2)==sec::descriptor(2),"data mutation lost descriptor");
  const auto source=ram->live;auto destination=std::make_shared<Ram>();destination->failure_unit=ram->failure_unit;
  auto id=identity();id[2]=std::byte{97};
  auto selected=writer.snapshot();
  rejects([&]{NativeFilesystemWriter::compact(selected,destination,id,{mode,6,false});},
      NativeFsErrorCode::storage_failure);
  require(destination->live.empty() && ram->live==source,"live writer compaction did not preserve ownership/source");
  owner.reset(); // The existing compaction contract requires a quiescent source.
  auto copied=NativeFilesystemWriter::compact(selected,destination,id,{mode,6,false});
  require(copied.snapshot().security_descriptor(2)==sec::descriptor(2) &&
      copied.snapshot().security_descriptor(1)==sec::empty_dacl() && ram->live==source,
      "new-identity copy lost security or altered source");
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(destination->stable,destination->failure_unit)));
  require(reopened.security_descriptor(2)==sec::descriptor(2),"independent copy security reopen");
}
void admission_and_capacity() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  for(const auto* path:{"a","b","c","d"})file_apply(writer,file_create(path));
  auto count=ram->calls;const auto reads=ram->read_calls;
  rejects([&]{writer.set_security_descriptor(2,Bytes{});},NativeFsErrorCode::invalid_operation);
  NativeSecurityMutation bad{2,1,sec::descriptor()};
  rejects([&]{writer.apply_with_metadata({},{},{&bad,1});},NativeFsErrorCode::invalid_operation);
  std::array<NativeSecurityMutation,17> many{};for(auto& item:many)item={2,0,sec::descriptor()};
  rejects([&]{writer.apply_with_metadata({},{},many);},NativeFsErrorCode::limit);
  require(ram->calls==count && ram->read_calls==reads,"pure descriptor admission touched backing");
  bad={0,2,sec::descriptor()};
  rejects([&]{writer.apply_with_metadata({},{},{&bad,1});},NativeFsErrorCode::invalid_operation);
  require(ram->calls==count && !writer.poisoned(),"bad creation selector wrote/fenced");
  auto large=sec::descriptor();large.resize(kNativeMaxSecurityBytes);
  for(unsigned id=2;id<=4;++id)writer.set_security_descriptor(id,large);
  const auto saved=ram->live;count=ram->calls;
  rejects([&]{writer.set_security_descriptor(5,large);},NativeFsErrorCode::limit);
  require(ram->calls==count && ram->live==saved && !writer.poisoned() &&
      !writer.inspect_security_descriptor(5),"whole namespace security cap not atomic");
  writer.set_security_descriptor(2,sec::descriptor());writer.set_security_descriptor(5,large);
  require(ObjectStoreSnapshot::open(ram).read(1).size()<=kNativeExtentBytes,"security enlarged metadata cap");
}
void malformed() {
  auto ram=std::make_shared<Ram>();
  {auto writer=fresh(ram,StoreCompressionMode::raw);file_apply(writer,file_create("f"));
    writer.set_security_descriptor(2,sec::descriptor());}
  const auto original=ObjectStoreSnapshot::open(ram).read(1);const auto map=fields(original);const auto at=map.at(2);
  const auto payload=at+96+1;const auto generation=ObjectStoreSnapshot::open(ram).info().generation;
  const auto corrupt=[&](auto mutation) {
    auto clone=image(ram->stable);auto bytes=original;mutation(bytes);
    {auto objects=ObjectStoreWriter::resume(clone,identity(),generation);
      const StoreMutation edit{1,false,std::move(bytes)};objects.commit_replicated({&edit,1});}
    rejects([&]{NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(clone));},NativeFsErrorCode::corrupt);
  };
  corrupt([](auto& b){b[7]=std::byte{'3'};});
  corrupt([](auto& b){sec::put(b,8,3);});
  corrupt([&](auto& b){sec::put(b,at+20,0);});
  corrupt([&](auto& b){sec::put(b,at+88,0);});
  corrupt([&](auto& b){sec::put(b,at+88,UINT32_MAX);});
  corrupt([&](auto& b){sec::put(b,at+92,1);});
  corrupt([&](auto& b){sec::put(b,at+52,1);});
  corrupt([&](auto& b){sec::put(b,map.at(1)+88,20);});
  corrupt([&](auto& b){sec::put(b,payload,2,1);});
  corrupt([&](auto& b){sec::put(b,payload+4,UINT32_MAX);});
  corrupt([&](auto& b){b.pop_back();});
}
void owned() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  auto session=NativeFileSession::create(std::move(writer));
  session.create_entry("f",false,tuple(),sec::descriptor());
  const auto denied=session.open("f",{true,true,true},NativeOpenKind::file,{true});
  auto count=ram->calls;const auto reads=ram->read_calls;
  denies([&]{session.security_descriptor(denied);},NativeSessionErrorCode::access_denied);
  denies([&]{session.set_security_descriptor(denied,sec::descriptor(2));},NativeSessionErrorCode::access_denied);
  require(ram->calls==count && ram->read_calls==reads,"data/basic grants allowed security I/O");
  auto handle=session.open("f",{true,true,true},NativeOpenKind::file,{},{true,true});
  require(session.security_descriptor(handle)==sec::descriptor(),"owned security readback");
  session.set_security_descriptor(handle,sec::descriptor(2));session.rename(handle,"renamed");
  require(session.security_descriptor(handle)==sec::descriptor(2),"stable inode rename lost descriptor");
  auto stale=handle;session.cleanup(handle,false);session.close(handle);
  denies([&]{session.security_descriptor(stale);},NativeSessionErrorCode::invalid_handle);
  auto late=session.open("renamed",{true,true,true},NativeOpenKind::file,{},{true,true});
  session.set_delete(late,true);session.cleanup(late,true);
  require(session.security_descriptor(late)==sec::descriptor(2),"detached owned descriptor disappeared");
  session.close(late);session.cleanup(denied,false);session.close(denied);
  require(session.stats().contexts==0 && !session.poisoned(),"security context lifecycle failed");
  require(sec::get(ObjectStoreSnapshot::open(ram).read(1),8)==4,"retirement downgraded security capability");
}
void failed_owned_update() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw);
  auto session=NativeFileSession::create(std::move(writer));
  session.create_entry("f",false,tuple(),sec::descriptor());
  auto handle=session.open("f",{true,true,true},NativeOpenKind::file,{},{true,true});
  const auto before=session.stats();ram->calls=0;ram->fail_at=0;
  rejects([&]{session.set_security_descriptor(handle,sec::descriptor(2));},NativeFsErrorCode::storage_failure);
  require(session.poisoned() && session.stats().contexts==before.contexts &&
      session.stats().cleaned==before.cleaned,"failed owned security update released context");
  const auto calls=ram->calls;
  denies([&]{session.security_descriptor(handle);},NativeSessionErrorCode::terminal);
  require(ram->calls==calls,"fenced descriptor context retried backing");
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  require(recovered.security_descriptor(2)==sec::descriptor(),"failed first-write update lost acknowledged descriptor");
}
enum class Phase {upgrade,update,compound};
const char* phase_name(Phase phase){return phase==Phase::upgrade?"upgrade":phase==Phase::update?"update":"compound";}
NativeFilesystemWriter seed(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact,Phase phase) {
  auto writer=fresh(ram,mode,compact);
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(73,11)),
      file_create("keep"),file_write("keep",0,data(91,12))}};
  writer.apply(ops);writer.set_basic_metadata(2,tuple());
  if(phase!=Phase::upgrade)writer.set_security_descriptor(2,sec::descriptor());
  return writer;
}
void action(NativeFilesystemWriter& writer,Phase phase) {
  if(phase!=Phase::compound){writer.set_security_descriptor(2,sec::descriptor(2));return;}
  const std::array<NativeFileMutation,3> ops{{file_write("f",7,data(19,13)),file_rename("f","renamed"),file_create("new")}};
  const std::array<NativeBasicMutation,2> basic{{{2,0,tuple(2)},{0,1,tuple(3)}}};
  const std::array<NativeSecurityMutation,2> security{{{2,0,sec::descriptor(2)},{0,1,sec::empty_dacl()}}};
  writer.apply_with_metadata(ops,basic,security);
}
void expected(const NativeFilesystemSnapshot& view,Phase phase,bool changed) {
  require(view.read("keep",0,100)==data(91,12) && !view.security_descriptor(3),"acknowledged unrelated contents/security lost");
  auto bytes=data(73,11);
  if(phase==Phase::compound && changed){const auto patch=data(19,13);std::copy(patch.begin(),patch.end(),bytes.begin()+7);}
  require(view.read_inode(2,0,100)==bytes && view.security_descriptor(2)==
      (changed?std::optional<NativeSecurityDescriptor>{sec::descriptor(2)}:
       phase==Phase::upgrade?std::optional<NativeSecurityDescriptor>{}:std::optional<NativeSecurityDescriptor>{sec::descriptor()}),
      "security interruption produced mixed data/descriptor");
  require(view.basic_metadata(2)==tuple(phase==Phase::compound && changed?2:1),"security interruption lost/mixed basic tuple");
  if(phase==Phase::compound) {
    require(view.stat(changed?"renamed":"f").inode==2,"security rename not atomic");
    if(changed)require(view.security_descriptor(view.stat("new").inode)==sec::empty_dacl() &&
        view.basic_metadata(view.stat("new").inode)==tuple(3),"new inode exposed without complete basic/security metadata");
    else rejects([&]{view.stat("new");},NativeFsErrorCode::not_found);
  }
}
void interruption(StoreCompressionMode mode,bool compact,Phase phase) {
  auto probe=std::make_shared<Ram>();std::size_t steps=0;
  {auto writer=seed(probe,mode,compact,phase);probe->calls=0;action(writer,phase);steps=probe->calls;}
  require(steps>3 && steps<100,"security fault trace outside bound");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemSnapshot> old;std::uint64_t sequence=0;
    {auto writer=seed(ram,mode,compact,phase);old.emplace(writer.snapshot());sequence=old->sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(writer,phase);},NativeFsErrorCode::storage_failure);
      require(writer.poisoned(),"failed security publication did not fence");const auto calls=ram->calls;
      rejects([&]{writer.set_security_descriptor(3,sec::descriptor());},NativeFsErrorCode::storage_failure);
      require(ram->calls==calls,"fenced security writer retried");}
    expected(*old,phase,false);
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"security recovery lost/fabricated sequence");
    expected(view,phase,view.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),view.storage_info().generation,{mode,6,false}));
    writer.set_security_descriptor(3,sec::descriptor(9));
    auto continued=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable,recovered->failure_unit)));
    require(continued.security_descriptor(3)==sec::descriptor(9) &&
        continued.read("keep",0,100)==data(91,12),"independent CPU security continuation failed");
  }
  require(cases==3*steps,"security fault point omitted");
  std::cout<<"Native security evidence: {\"schema\":1,\"phase\":\""<<phase_name(phase)
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one bounded security group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    framing();admission_and_capacity();malformed();owned();failed_owned_update();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})normal(mode,compact);
    std::cout<<"Native security invariants: {\"schema\":1,\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    const auto phase=group=="--upgrade-faults"?Phase::upgrade:group=="--update-faults"?Phase::update:
        group=="--compound-faults"?Phase::compound:throw std::runtime_error("unknown security group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})interruption(mode,compact,phase);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native security failure: "<<e.what()<<"\n";return 1;}}
