#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>

namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t stamp=133800000000000123ULL;
NativeBasicMetadata original(){return {11,22,33,44,0x2002};}
NativeOverwriteMetadata event(bool replace=false,std::uint64_t time=stamp){return {time,4,replace};}
NativeBasicMetadata updated(bool replace=false,std::uint64_t time=stamp){return {11,time,time,time,replace?0x24U:0x2026U};}
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected native overwrite rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected owned overwrite rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer,bool descriptor=true) {
  const std::array<NativeFileMutation,4> ops{{file_create("f"),file_write("f",0,data(65577,17)),
      file_create("keep"),file_write("keep",0,data(37,18))}};
  const NativeBasicMutation basic{0,1,original()};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&basic,1},descriptor?std::span<const NativeSecurityMutation>(&security,1):
      std::span<const NativeSecurityMutation>{});
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.stat("keep").inode==3 && view.read("keep",0,100)==data(37,18) &&
      !view.basic_metadata(3) && !view.security_descriptor(3),"overwrite lost acknowledged unrelated bytes/metadata");
}
void expected(const NativeFilesystemSnapshot& view,bool changed,bool replace,bool descriptor=true) {
  keep(view);const auto info=view.stat("f");
  require(info.inode==2 && !info.directory && info.logical_bytes==(changed?0U:65577U) &&
      view.basic_metadata(2)==(changed?updated(replace):original()) &&
      view.security_descriptor(2)==(descriptor?std::optional<NativeSecurityDescriptor>{sec::descriptor()}:
          std::optional<NativeSecurityDescriptor>{}) && !view.needs_session_recovery(),
      "overwrite recovery mixed identity/data/basic/descriptor/lifetime");
  require(view.read("f",0,100000)==(changed?Bytes{}:data(65577,17)),"overwrite returned partial old/new data");
}
NativeFileHandle target(NativeFileSession& session,bool replace,NativeFileAccess access={true,false,false}) {
  // Model the source-verified FSD case: implicit overwrite rights were checked,
  // then removed from returned GrantedAccess because the caller did not ask
  // to retain them. Only the separate operation grant authorizes truncation.
  return session.open("f",access,NativeOpenKind::file,{},{},{!replace,replace});
}
void normal(StoreCompressionMode mode,bool compact,bool replace,bool descriptor) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer,descriptor);
  auto session=NativeFileSession::create(std::move(writer));
  auto handle=target(session,replace),other=session.open("f",all);
  auto old=session.snapshot();const auto sequence=old.sequence(),barriers=ram->flushes;
  const auto reads=ram->read_calls,calls=ram->calls;
  denies([&]{session.write(handle,0,data(1,1));},NativeSessionErrorCode::access_denied);
  denies([&]{session.set_basic_metadata(handle,original());},NativeSessionErrorCode::access_denied);
  denies([&]{session.overwrite_result(handle,event(!replace));},NativeSessionErrorCode::access_denied);
  require(ram->read_calls==reads && ram->calls==calls,"overwrite grant inferred data/basic/wrong-mode authority");
  const auto result=session.overwrite_result(handle,event(replace));
  require(!result.bytes && !result.offset && result.info.inode==2 && !result.info.logical_bytes &&
      result.info.basic==updated(replace) && !result.info.directory && !result.info.unlinked &&
      !result.info.delete_pending && result.info.modified_sequence==sequence+1 &&
      result.transaction.sequence==sequence+1 && ram->flushes==barriers+3,
      "overwrite did not return one complete durable metadata/data event");
  expected(session.snapshot(),true,replace,descriptor);expected(old,false,replace,descriptor);
  require(session.info(other).info.logical_bytes==0 && session.stats().contexts==2 &&
      !session.stats().cleaned,"overwrite changed other handle identity or released contexts");
  // No old data may reappear when the empty file regrows.
  session.truncate(other,65577);
  require(session.read(other,0,100000)==Bytes(65577),"overwrite/regrow exposed discarded old bytes");
  session.write(other,65530,data(29,19));auto bytes=Bytes(65577);const auto patch=data(29,19);
  std::copy(patch.begin(),patch.end(),bytes.begin()+65530);
  require(session.read(handle,0,100000)==bytes,"post-overwrite context no longer addresses same inode");
  session.rename(other,"renamed");
  session.overwrite_result(handle,event(replace,0));
  require(session.info(handle).info.path=="renamed" && session.basic_metadata(handle)==updated(replace,0) &&
      !session.info(handle).info.logical_bytes,"overwrite redirected stable inode after rename or rejected real zero UTC");
  // Same size, tuple and sampled clock must still acknowledge an overwrite event.
  const auto before=session.snapshot().sequence(),flushes=ram->flushes;
  session.overwrite_result(handle,event(replace,0));
  require(session.snapshot().sequence()==before+1 && ram->flushes==flushes+3,
      "already empty identical overwrite silently skipped its publication");
  session.cleanup(handle,false);
  const auto cleaned_calls=ram->calls,cleaned_reads=ram->read_calls;
  denies([&]{session.overwrite_result(handle,event(replace));},NativeSessionErrorCode::invalid_options);
  require(ram->calls==cleaned_calls && ram->read_calls==cleaned_reads,"cleaned overwrite context reached storage");
  session.close(handle);
  denies([&]{session.overwrite_result(handle,event(replace));},NativeSessionErrorCode::invalid_handle);
  session.cleanup(other,false);session.close(other);
  require(!session.stats().contexts && !session.poisoned(),"overwrite context lifecycle leaked/fenced healthy state");
}
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();bool sealed=false;mutable unsigned blocked=0;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  void allowed() const{if(sealed){++blocked;throw std::runtime_error("post-publication backend unavailable");}}
  std::uint64_t extent() const override{allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t bytes) override{allowed();ram->admit_write(bytes);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{allowed();ram->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override{allowed();ram->write_exact(at,in);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
};
void sealed_result(StoreCompressionMode mode,bool compact,bool replace) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));
  seed(writer);auto session=NativeFileSession::create(std::move(writer));
  auto handle=target(session,replace),other=session.open("f",all);
  io->seal_at=io->ram->flushes+3;
  auto result=session.overwrite_result(handle,event(replace));
  require(io->sealed && !io->blocked && result.info.inode==2 && !result.info.logical_bytes &&
      result.info.basic==updated(replace) && session.stats().contexts==2 &&
      !session.stats().cleaned,"overwrite result needed post-barrier backing access or lost ownership");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  expected(view,true,replace);
  require(view.stat_inode(2).modified_sequence==result.info.modified_sequence &&
      view.storage_info().generation==result.transaction.storage.generation,"sealed result differs from recovered commit");
  rejects([&]{session.info(other);},NativeFsErrorCode::storage_failure);
  require(io->blocked && session.poisoned() && session.stats().contexts==2 &&
      result.info.basic==updated(replace),"later storage denial damaged committed result/context");
  const auto blocked=io->blocked;
  denies([&]{session.overwrite_result(handle,event(replace));},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked,"fenced overwrite retried storage");++sealed_cases;
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  const auto calls=ram->calls,reads=ram->read_calls;
  for(const auto effect:{NativeOverwriteMetadata{UINT64_MAX,0,false},
      NativeOverwriteMetadata{1,0x80,false},NativeOverwriteMetadata{1,0x10,false},
      NativeOverwriteMetadata{1,0xffffffffU,true}}) {
    require(!native_overwrite_metadata_valid(effect),"invalid overwrite metadata validator admitted sentinels");
    rejects([&]{writer.overwrite_inode_result(2,effect);},NativeFsErrorCode::invalid_operation);
  }
  rejects([&]{writer.overwrite_inode_result(1,event());},NativeFsErrorCode::is_directory);
  rejects([&]{writer.overwrite_inode_result(999,event());},NativeFsErrorCode::not_found);
  require(ram->calls==calls && ram->read_calls==reads && !writer.poisoned(),"invalid core overwrite reached storage");
  auto session=NativeFileSession::create(std::move(writer));
  const auto no_grant=session.open("f",all,NativeOpenKind::file,{true},{true,true});
  const auto granted=target(session,false);
  const auto before_reads=ram->read_calls,before_calls=ram->calls;
  denies([&]{session.overwrite_result(no_grant,event());},NativeSessionErrorCode::access_denied);
  denies([&]{session.overwrite_result(granted,{UINT64_MAX,0,false});},NativeSessionErrorCode::invalid_options);
  require(ram->calls==before_calls && ram->read_calls==before_reads,"data/delete/basic/security grants bypassed overwrite admission");
  session.set_delete(no_grant,true);
  denies([&]{session.overwrite_result(granted,event());},NativeSessionErrorCode::delete_pending);
  session.cleanup(no_grant,true);
  denies([&]{session.overwrite_result(granted,event());},NativeSessionErrorCode::delete_pending);
  session.close(no_grant);session.cleanup(granted,false);session.close(granted);
  require(!session.stats().contexts,"rejected pending/detached overwrite released wrong contexts");
  auto legacy=std::make_shared<Ram>();auto old=fresh(legacy,StoreCompressionMode::raw,false);
  const auto op=file_create("legacy");old.apply({&op,1});const auto old_calls=legacy->calls,old_reads=legacy->read_calls;
  rejects([&]{old.overwrite_inode_result(2,event());},NativeFsErrorCode::invalid_operation);
  require(legacy->calls==old_calls && legacy->read_calls==old_reads &&
      !old.snapshot().basic_metadata(2) && !old.poisoned(),"overwrite invented original legacy times or published");
}
void faults(StoreCompressionMode mode,bool compact,bool replace) {
  std::size_t steps=0;
  {auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
    auto session=NativeFileSession::create(std::move(writer));auto h=target(session,replace);
    ram->calls=0;session.overwrite_result(h,event(replace));steps=ram->calls;}
  require(steps>3 && steps<100,"overwrite trace outside existing bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::uint64_t sequence=0;
    {auto writer=fresh(ram,mode,compact);seed(writer);
      auto session=NativeFileSession::create(std::move(writer));
      auto h=target(session,replace),other=session.open("f",all);auto old=session.snapshot();
      sequence=old.sequence();const auto before=session.stats();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{session.overwrite_result(h,event(replace));},NativeFsErrorCode::storage_failure);
      const auto after=session.stats();
      require(session.poisoned() && before.contexts==after.contexts && before.cleaned==after.cleaned &&
          before.delete_requests==after.delete_requests,"failed overwrite released/changed owned contexts");
      const auto calls=ram->calls;
      denies([&]{session.overwrite_result(h,event(replace));},NativeSessionErrorCode::terminal);
      denies([&]{session.read(other,0,1);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls,"poisoned overwrite/session retried backing");
      expected(old,false,replace);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"overwrite recovery lost/fabricated acknowledged history");
    const bool changed=view.sequence()==sequence+1;expected(view,changed,replace);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,23))}};
    writer.apply(continuation);
    auto continued=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable,recovered->failure_unit)));
    expected(continued,changed,replace);
    require(continued.read("continued",0,100)==data(17,23),"independent CPU continuation failed after overwrite fault");
  }
  require(cases==steps*3,"overwrite fault point omitted");
  std::cout<<"Native overwrite evidence: {\"schema\":1,\"replace_attributes\":"<<(replace?"true":"false")
      <<",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  static_assert(std::is_nothrow_move_constructible_v<NativeInodeMutationCommit>);
  if(argc!=2)throw std::runtime_error("one bounded overwrite group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    admission();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})for(bool replace:{false,true}) {
        for(bool descriptor:{false,true})normal(mode,compact,replace,descriptor);
        sealed_result(mode,compact,replace);
      }
    require(sealed_cases==8,"overwrite sealed case omitted");
    std::cout<<"Native overwrite invariants: {\"schema\":1,\"sealed_cases\":"<<sealed_cases
        <<",\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    if(group!="--merge-faults" && group!="--replace-faults")throw std::runtime_error("unknown overwrite group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,group=="--replace-faults");
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native overwrite failure: "<<e.what()<<"\n";return 1;}}
