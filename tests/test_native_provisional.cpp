#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t stamp=133800000000000123ULL;
NativeBasicMetadata original(){return {11,22,33,44,2};}
NativeOverwriteMetadata event(bool replace=false){return {stamp,4,replace};}
NativeBasicMetadata updated(bool replace){return {11,stamp,stamp,stamp,replace?0x24U:0x26U};}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected native provisional failure");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected provisional ownership rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer) {
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(73,31)),
      file_create("keep"),file_write("keep",0,data(37,32))}};
  const NativeBasicMutation basic{0,1,original()};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&basic,1},{&security,1});
}
NativeFileHandle prepare(NativeFileSession& s,bool replace=false) {
  return s.prepare_overwrite("f",all,{true},{true,true},{!replace,replace});
}
void expected(const NativeFilesystemSnapshot& view,bool changed,bool replace) {
  require(view.read("keep",0,100)==data(37,32) && view.stat("keep").inode==3 &&
      !view.basic_metadata(3) && !view.security_descriptor(3),"provisional path lost unrelated acknowledged bytes");
  require(view.stat("f").inode==2 && view.read("f",0,100)==(changed?Bytes{}:data(73,31)) &&
      view.basic_metadata(2)==(changed?updated(replace):original()) &&
      view.security_descriptor(2)==sec::descriptor() && !view.needs_session_recovery(),
      "provisional recovery mixed inode/data/basic/security/lifetime");
}
void blocked(NativeFileSession& s,const NativeFileHandle& h,Ram& ram) {
  const auto calls=ram.calls,reads=ram.read_calls;
  denies([&]{s.read(h,0,1);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.list(h);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.write(h,0,data(1,1));},NativeSessionErrorCode::invalid_options);
  denies([&]{s.write_result(h,0,data(1,1));},NativeSessionErrorCode::invalid_options);
  denies([&]{s.truncate(h,0);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.truncate_result(h,0);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.set_basic_metadata(h,original());},NativeSessionErrorCode::invalid_options);
  denies([&]{s.set_security_descriptor(h,sec::descriptor());},NativeSessionErrorCode::invalid_options);
  denies([&]{s.rename(h,"other");},NativeSessionErrorCode::invalid_options);
  denies([&]{s.set_delete(h,true);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.cleanup(h,false);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.flush(h);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.overwrite_result(h,event());},NativeSessionErrorCode::invalid_options);
  require(ram.calls==calls && ram.read_calls==reads,"unadopted I/O reached backing");
}
void normal(StoreCompressionMode mode,bool compact,bool replace) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto peer=s.open("f",all);
  auto h=prepare(s,replace);const auto sequence=s.snapshot().sequence();
  require(s.info(h).info.inode==2 && s.basic_metadata(h)==original() &&
      s.security_descriptor(h)==sec::descriptor(),"provisional Open metadata unavailable");
  blocked(s,h,*ram);
  const auto calls=ram->calls,reads=ram->read_calls;
  denies([&]{s.adopt_prepared(h);},NativeSessionErrorCode::invalid_options);
  denies([&]{s.set_delete(peer,true);},NativeSessionErrorCode::sharing_violation);
  denies([&]{s.cleanup(peer,true);},NativeSessionErrorCode::sharing_violation);
  denies([&]{s.overwrite_prepared_result(h,event(!replace));},NativeSessionErrorCode::access_denied);
  denies([&]{s.overwrite_prepared_result(h,{UINT64_MAX,0,replace});},NativeSessionErrorCode::invalid_options);
  require(ram->calls==calls && ram->read_calls==reads,"invalid phase/grant/peer deletion reached backing");
  const auto result=s.overwrite_prepared_result(h,event(replace));
  require(result.info.inode==2 && !result.info.logical_bytes && result.info.basic==updated(replace) &&
      result.transaction.sequence==sequence+1 && !result.bytes && !result.offset,
      "prepared overwrite did not return its committed result");
  blocked(s,h,*ram);
  const auto after=ram->calls,after_reads=ram->read_calls;
  denies([&]{s.overwrite_prepared_result(h,event(replace));},NativeSessionErrorCode::invalid_options);
  denies([&]{s.set_delete(peer,true);},NativeSessionErrorCode::sharing_violation);
  s.adopt_prepared(h);
  require(ram->calls==after && ram->read_calls==after_reads,"phase adoption/rejection performed I/O");
  require(s.read(h,0,1).empty(),"adopted handle is not usable");
  s.set_delete(peer,true);s.set_delete(peer,false); // Unresolved-open guard must be gone.
  s.write(peer,7,data(13,33));s.rename(peer,"renamed");
  require(s.info(h).info.path=="renamed" && s.read(h,7,13)==data(13,33),
      "adoption lost stable inode through peer write/rename");
  denies([&]{s.adopt_prepared(h);},NativeSessionErrorCode::invalid_options);
  s.cleanup(h,false);s.close(h);s.cleanup(peer,false);s.close(peer);
  require(!s.stats().contexts && !s.poisoned(),"adopted context lifecycle leaked");
}
void cancellation(StoreCompressionMode mode,bool compact,bool replace,bool overwritten) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto peer=s.open("f",all);auto h=prepare(s,replace),alias=h;
  if(overwritten) {
    s.overwrite_prepared_result(h,event(replace));
    // Another live context may publish after the callback. Cancellation must
    // never undo that later acknowledged write.
    s.write(peer,0,data(19,34));
  }
  const auto bytes=ram->live;
  const auto calls=ram->calls,reads=ram->read_calls,flushes=ram->flushes;
  s.close(h);
  require(ram->live==bytes && ram->calls==calls && ram->read_calls==reads && ram->flushes==flushes &&
      s.stats().contexts==1 && !s.stats().cleaned && !s.poisoned(),
      "healthy provisional cancellation performed storage I/O/cleanup or released peer");
  denies([&]{s.info(alias);},NativeSessionErrorCode::invalid_handle);
  require(s.read(peer,0,100)==(overwritten?data(19,34):data(73,31)),
      "provisional cancellation undid overwrite/later acknowledged data");
  s.set_delete(peer,true);s.set_delete(peer,false);
  s.cleanup(peer,false);s.close(peer);
}
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();bool sealed=false;mutable unsigned blocked=0;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  void allowed() const{if(sealed){++blocked;throw std::runtime_error("post-publication unavailable");}}
  std::uint64_t extent() const override{allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t n) override{allowed();ram->admit_write(n);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{allowed();ram->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override{allowed();ram->write_exact(at,in);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
};
void sealed_result(StoreCompressionMode mode,bool compact,bool adopt) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));
  seed(writer);auto s=NativeFileSession::create(std::move(writer));auto peer=s.open("keep",all);auto h=prepare(s);
  io->seal_at=io->ram->flushes+3;auto result=s.overwrite_prepared_result(h,event());
  require(io->sealed && !io->blocked && result.info.basic==updated(false),"prepared result queried after publication");
  if(adopt) {
    s.adopt_prepared(h);
    denies([&]{s.close(h);},NativeSessionErrorCode::invalid_options);
    require(s.stats().contexts==2 && !io->blocked,"adoption queried storage or bypassed active Cleanup");
  } else {
    s.close(h);
    require(s.stats().contexts==1 && !io->blocked,"post-overwrite cancellation queried storage or lost peer");
    denies([&]{s.info(h);},NativeSessionErrorCode::invalid_handle);
  }
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  expected(view,true,false);
  rejects([&]{s.info(peer);},NativeFsErrorCode::storage_failure);
  const auto contexts=s.stats().contexts,blocked=io->blocked;
  denies([&]{s.close(adopt?h:peer);},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked && s.stats().contexts==contexts && s.poisoned(),
      "poisoned Close released context/retried backing");
  ++sealed_cases;
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));const auto calls=ram->calls,reads=ram->read_calls;
  for(const auto grant:{NativeOverwriteAccess{},NativeOverwriteAccess{true,true}})
    denies([&]{s.prepare_overwrite("f",all,{},{},grant);},NativeSessionErrorCode::invalid_options);
  require(ram->calls==calls && ram->read_calls==reads,"invalid preparation grants reached backing");
  rejects([&]{s.prepare_overwrite("",all,{},{},{true,false});},NativeFsErrorCode::is_directory);
  std::array<NativeFileHandle,NativeFileSession::max_contexts> handles;
  for(auto& h:handles)h=prepare(s);
  const auto full_calls=ram->calls,full_reads=ram->read_calls;
  denies([&]{prepare(s);},NativeSessionErrorCode::limit);
  require(ram->calls==full_calls && ram->read_calls==full_reads,"full provisional table reached backing");
  for(const auto& h:handles)s.close(h);
  require(!s.stats().contexts && ram->calls==full_calls && ram->read_calls==full_reads,
      "bulk provisional retirement performed I/O or leaked context");
  auto stale=handles.front();auto fresh_handle=prepare(s);
  denies([&]{s.adopt_prepared(stale);},NativeSessionErrorCode::invalid_handle);s.close(fresh_handle);
  auto other_ram=std::make_shared<Ram>();auto other_writer=fresh(other_ram,StoreCompressionMode::raw,false);seed(other_writer);
  auto other=NativeFileSession::create(std::move(other_writer));auto foreign=prepare(other);
  denies([&]{s.close(foreign);},NativeSessionErrorCode::invalid_handle);other.close(foreign);
  auto legacy_ram=std::make_shared<Ram>();auto legacy_writer=fresh(legacy_ram,StoreCompressionMode::raw,false);
  const auto op=file_create("f");legacy_writer.apply({&op,1});
  auto legacy=NativeFileSession::create(std::move(legacy_writer));auto h=prepare(legacy);
  const auto before=legacy_ram->live;
  rejects([&]{legacy.overwrite_prepared_result(h,event());},NativeFsErrorCode::invalid_operation);
  const auto legacy_calls=legacy_ram->calls,legacy_reads=legacy_ram->read_calls;
  legacy.close(h);
  require(legacy_ram->live==before && legacy_ram->calls==legacy_calls &&
      legacy_ram->read_calls==legacy_reads && !legacy.stats().contexts && !legacy.poisoned(),
      "failed unstamped overwrite could not close without fabricated Cleanup");
}
void faults(StoreCompressionMode mode,bool compact,bool replace) {
  std::size_t steps=0;
  {auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
    auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s,replace);ram->calls=0;
    s.overwrite_prepared_result(h,event(replace));steps=ram->calls;}
  require(steps>3 && steps<100,"prepared overwrite trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::uint64_t sequence=0;
    {auto writer=fresh(ram,mode,compact);seed(writer);auto s=NativeFileSession::create(std::move(writer));
      auto peer=s.open("f",all),h=prepare(s,replace);auto old=s.snapshot();sequence=old.sequence();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{s.overwrite_prepared_result(h,event(replace));},NativeFsErrorCode::storage_failure);
      require(s.poisoned() && s.stats().contexts==2 && !s.stats().cleaned,
          "failed prepared publication lost/cleaned contexts");
      const auto calls=ram->calls,reads=ram->read_calls;
      denies([&]{s.close(h);},NativeSessionErrorCode::terminal);
      denies([&]{s.adopt_prepared(h);},NativeSessionErrorCode::terminal);
      denies([&]{s.read(peer,0,1);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls && ram->read_calls==reads && s.stats().contexts==2,
          "poisoned provisional close/adoption retried/released ownership");expected(old,false,replace);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"provisional recovery lost/fabricated history");
    expected(view,view.sequence()==sequence+1,replace);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    auto s=NativeFileSession::create(std::move(writer));auto h=prepare(s,replace);
    s.overwrite_prepared_result(h,event(replace));s.adopt_prepared(h);s.write(h,0,data(17,35));
    auto continued=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable,recovered->failure_unit)));
    require(continued.read("f",0,100)==data(17,35) && continued.read("keep",0,100)==data(37,32) &&
        continued.security_descriptor(2)==sec::descriptor(),"independent prepared CPU continuation failed");
  }
  require(cases==steps*3,"prepared fault point omitted");
  std::cout<<"Native provisional evidence: {\"schema\":1,\"replace_attributes\":"<<(replace?"true":"false")
      <<",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one provisional group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    admission();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      for(bool replace:{false,true}) {normal(mode,compact,replace);for(bool overwritten:{false,true})cancellation(mode,compact,replace,overwritten);}
      for(bool adopt:{false,true})sealed_result(mode,compact,adopt);
    }
    require(sealed_cases==8,"provisional sealed case omitted");
    std::cout<<"Native provisional invariants: {\"schema\":1,\"checks\":"<<checks
        <<",\"sealed_cases\":"<<sealed_cases<<",\"complete\":true}\n";
  } else {
    if(group!="--merge-faults" && group!="--replace-faults")throw std::runtime_error("unknown provisional group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,group=="--replace-faults");
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native provisional failure: "<<e.what()<<"\n";return 1;}}
