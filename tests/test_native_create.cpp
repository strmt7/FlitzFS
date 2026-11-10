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
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected native creation rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected owned creation rejection");
}
constexpr NativeFileAccess all{true,true,true};
NativeSessionCreateOptions options(bool pending=false) {
  return {all,{true},{true,true},pending};
}
NativeBasicMetadata tuple(bool directory=false) {return {101,202,303,404,directory?0x10U:0x20U};}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer) {
  const std::array<NativeFileMutation,4> ops{{file_create("keep"),file_write("keep",0,data(31,11)),
      file_create("stable"),file_write("stable",0,data(37,12))}};
  writer.apply(ops);
}
void unchanged(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(31,11) && view.stat("keep").inode==2 &&
      view.read("stable",0,100)==data(37,12) && view.stat("stable").inode==3 &&
      !view.security_descriptor(2) && !view.basic_metadata(2),
      "creation changed previously acknowledged unrelated bytes/identity/absence");
}
void created(const NativeFilesystemSnapshot& view,bool directory,bool pending) {
  const auto info=view.stat("new");const auto state=view.inode_state(info.inode);
  require(info.inode==4 && !info.logical_bytes && info.directory==directory &&
      view.basic_metadata(4)==tuple(directory) && view.security_descriptor(4)==sec::descriptor(2) &&
      state.delete_pending==pending && !state.unlinked,
      "new inode exposed without atomic tuple/descriptor/delete intent");
  require(view.needs_session_recovery()==pending,"new pending creation has wrong recovery requirement");
}
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();
  bool sealed=false;mutable unsigned blocked=0;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  void allowed() const {if(sealed){++blocked;throw std::runtime_error("post-publication backend unavailable");}}
  std::uint64_t extent() const override{allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t bytes) override{allowed();ram->admit_write(bytes);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{allowed();ram->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override{allowed();ram->write_exact(at,in);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
  void arm(){seal_at=ram->flushes+3;}
};
void sealed_result(StoreCompressionMode mode,bool compact,bool directory,bool pending) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));
  seed(writer);auto session=NativeFileSession::create(std::move(writer));
  auto old=session.snapshot();const auto sequence=old.sequence(),barriers=io->ram->flushes;
  unchanged(old);io->arm();
  auto result=session.create_open("new",directory,tuple(directory),sec::descriptor(2),options(pending));
  require(io->sealed && !io->blocked && io->ram->flushes==barriers+3,
      "creation/open result queried storage after final barrier or used extra publication");
  require(result.info.inode==4 && !result.info.logical_bytes && result.info.directory==directory &&
      result.info.basic==tuple(directory) && result.info.delete_pending==pending &&
      !result.info.unlinked && result.transaction.sequence==sequence+1 &&
      result.info.modified_sequence==sequence+1 && result.transaction.created_inodes==std::vector<std::uint64_t>{4},
      "creation returned incomplete or incorrect committed facts");
  require(session.stats().contexts==1 && !session.stats().cleaned &&
      session.stats().delete_requests==(pending?1U:0U) && !session.stats().fenced,
      "creation failed to install its owned context without a query");
  require(old.list().size()==2 && !old.needs_session_recovery(),"creation changed retained namespace snapshot");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  unchanged(view);created(view,directory,pending);
  require(view.storage_info().generation==result.transaction.storage.generation &&
      view.stat_inode(4).modified_sequence==result.info.modified_sequence,
      "result disagrees with independent CPU recovery");
  // Prove the returned handle is real by using it against deliberately denied
  // storage. It must reach the backend, fence, and retain the owned context.
  rejects([&]{session.info(result.handle);},NativeFsErrorCode::storage_failure);
  require(io->blocked && session.poisoned() && session.stats().contexts==1 &&
      result.info.basic==tuple(directory),"later failure invalidated creation result/ownership");
  const auto blocked=io->blocked;
  denies([&]{session.create_open("other",false,tuple(),sec::descriptor());},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked,"terminal creation retried backend");
  ++sealed_cases;
}
void lifecycle(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));
  auto result=session.create_open("new",false,tuple(),sec::descriptor(2),options());
  auto alias=result.handle;
  require(session.read(result.handle,0,1).empty() && session.basic_metadata(result.handle)==tuple() &&
      session.security_descriptor(result.handle)==sec::descriptor(2),"created handle has wrong access/metadata");
  session.write(result.handle,7,data(23,13));session.rename(result.handle,"moved");
  require(session.info(alias).info.path=="moved" && session.read(alias,0,30)==
      ([] {Bytes b(30);const auto d=data(23,13);std::copy(d.begin(),d.end(),b.begin()+7);return b;}()),
      "created context did not retain stable inode through write/rename");
  session.set_basic_metadata(alias,{11,22,33,44,2});session.set_security_descriptor(alias,sec::empty_dacl());
  session.cleanup(result.handle,false);session.close(result.handle);
  denies([&]{session.info(alias);},NativeSessionErrorCode::invalid_handle);
  auto dir=session.create_open("dir",true,tuple(true),sec::descriptor(),options());
  auto child=session.create_open("dir/child",false,tuple(),sec::descriptor(),options());
  require(session.list(dir.handle).size()==1,"created directory cannot own a child");
  session.set_delete(child.handle,true);session.cleanup(child.handle,true);session.close(child.handle);
  session.set_delete(dir.handle,true);session.cleanup(dir.handle,true);session.close(dir.handle);
  for(bool directory:{false,true}) {
    auto deleted=session.create_open("temporary",directory,tuple(directory),sec::descriptor(2),options(true));
    require(session.info(deleted.handle).delete_pending && session.stats().delete_requests==1,
        "delete-on-close creation did not bind persisted intent to its own context");
    denies([&]{session.open("temporary",all);},NativeSessionErrorCode::delete_pending);
    session.set_delete(deleted.handle,false);auto second=session.open("temporary",all);
    require(!session.info(second).delete_pending,"created disposition could not be canceled");
    session.set_delete(deleted.handle,true);session.cleanup(deleted.handle,true);
    if(!directory)session.write(deleted.handle,0,data(7,14)); // Paging context lives until Close.
    require(session.info(deleted.handle).unlinked,"created delete context detached incorrectly");
    session.close(deleted.handle);session.cleanup(second,false);session.close(second);
    rejects([&]{session.snapshot().stat("temporary");},NativeFsErrorCode::not_found);
    auto replacement=session.create_open("temporary",directory,tuple(directory),sec::descriptor(),options());
    require(replacement.info.inode!=deleted.info.inode,"retired creation inode was reused");
    denies([&]{session.info(deleted.handle);},NativeSessionErrorCode::invalid_handle);
    session.set_delete(replacement.handle,true);session.cleanup(replacement.handle,true);session.close(replacement.handle);
  }
  auto reader=session.create_open("readonly",false,tuple(),sec::descriptor());
  const auto reads=ram->read_calls,calls=ram->calls;
  denies([&]{session.write(reader.handle,0,data(1,1));},NativeSessionErrorCode::access_denied);
  denies([&]{session.set_basic_metadata(reader.handle,tuple());},NativeSessionErrorCode::access_denied);
  denies([&]{session.security_descriptor(reader.handle);},NativeSessionErrorCode::access_denied);
  denies([&]{session.set_security_descriptor(reader.handle,sec::descriptor());},NativeSessionErrorCode::access_denied);
  require(ram->calls==calls && ram->read_calls==reads,"creation invented unauthorized grants");
  session.cleanup(reader.handle,false);session.close(reader.handle);
  require(!session.stats().contexts && !session.poisoned(),"created context lifecycle leaked ownership");
}
void admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));
  const auto calls=ram->calls,reads=ram->read_calls,barriers=ram->flushes;
  auto bad=tuple();bad.creation_time=UINT64_MAX;
  rejects([&]{session.create_open("new",false,bad,sec::descriptor());},NativeFsErrorCode::invalid_operation);
  rejects([&]{session.create_open("new",true,tuple(),sec::descriptor());},NativeFsErrorCode::invalid_operation);
  rejects([&]{session.create_open("new",false,tuple(),{});},NativeFsErrorCode::invalid_operation);
  rejects([&]{session.create_open(std::string(241,'x'),false,tuple(),sec::descriptor());},NativeFsErrorCode::invalid_path);
  rejects([&]{session.create_open("",false,tuple(),sec::descriptor());},NativeFsErrorCode::invalid_path);
  auto denied=options(true);denied.access.erase=false;
  denies([&]{session.create_open("new",false,tuple(),sec::descriptor(),denied);},NativeSessionErrorCode::access_denied);
  require(ram->calls==calls && ram->read_calls==reads && ram->flushes==barriers &&
      !session.stats().contexts && !session.poisoned(),"invalid creation reached I/O or leaked context");
  rejects([&]{session.create_open("KEEP",false,tuple(),sec::descriptor());},NativeFsErrorCode::already_exists);
  rejects([&]{session.create_open("missing/new",false,tuple(),sec::descriptor());},NativeFsErrorCode::not_found);
  require(ram->calls==calls && !session.stats().contexts && !session.poisoned(),
      "failed creation modified data or installed context");
  auto root=session.open("",all);
  std::array<NativeFileHandle,NativeFileSession::max_contexts-1> handles;
  for(auto& h:handles)h=session.open("",all);
  session.cleanup(root,false);const auto full_calls=ram->calls,full_reads=ram->read_calls;
  denies([&]{session.create_open("new",false,tuple(),sec::descriptor());},NativeSessionErrorCode::limit);
  require(ram->calls==full_calls && ram->read_calls==full_reads &&
      session.stats().contexts==NativeFileSession::max_contexts,
      "full/cleaned context table admitted durable creation");
  session.close(root);auto accepted=session.create_open("new",false,tuple(),sec::descriptor());
  require(accepted.info.inode==4 && session.stats().contexts==NativeFileSession::max_contexts,
      "failed preflights consumed an inode/context or closed slot was not reusable");
  session.cleanup(accepted.handle,false);session.close(accepted.handle);
  for(const auto& h:handles){session.cleanup(h,false);session.close(h);}
  unchanged(session.snapshot());
  // NS04 metadata remains bounded at 64 KiB, even for a framed descriptor.
  auto large=sec::descriptor();large.resize(kNativeMaxSecurityBytes);
  for(unsigned i=0;i<3;++i) {
    auto item=session.create_open("large"+std::to_string(i),false,tuple(),large);
    session.cleanup(item.handle,false);session.close(item.handle);
  }
  const auto before=ram->calls;
  rejects([&]{session.create_open("overflow",false,tuple(),large);},NativeFsErrorCode::limit);
  require(ram->calls==before && !session.stats().contexts && !session.poisoned(),
      "metadata capacity failure published creation or leaked context");
}
void closed_recovery() {
  auto ram=std::make_shared<Ram>();std::uint64_t generation=0;
  {auto writer=fresh(ram,StoreCompressionMode::raw,true);seed(writer);
    auto session=NativeFileSession::create(std::move(writer));
    auto result=session.create_open("new",false,tuple(),sec::descriptor(2),options(true));
    session.write(result.handle,0,data(9,19));generation=session.snapshot().storage_info().generation;}
  auto recovered=image(ram->stable,ram->failure_unit);
  rejects([&]{NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),generation));},
      NativeFsErrorCode::invalid_operation);
  auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(recovered,identity(),generation));
  auto view=writer.snapshot();unchanged(view);
  rejects([&]{view.stat("new");},NativeFsErrorCode::not_found);
  require(!view.needs_session_recovery(),"confirmed closed-session creation cleanup retained pending inode");
}
void faults(StoreCompressionMode mode,bool compact,bool directory,bool pending) {
  std::size_t steps=0;
  {auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
    auto session=NativeFileSession::create(std::move(writer));ram->calls=0;
    session.create_open("new",directory,tuple(directory),sec::descriptor(2),options(pending));steps=ram->calls;}
  require(steps>3 && steps<100,"creation fault trace outside existing bounds");
  std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;auto ram=std::make_shared<Ram>();std::uint64_t sequence=0;
    {auto writer=fresh(ram,mode,compact);seed(writer);
      auto session=NativeFileSession::create(std::move(writer));auto keep=session.open("keep",all);
      sequence=session.snapshot().sequence();const auto before=session.stats();
      ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{session.create_open("new",directory,tuple(directory),sec::descriptor(2),options(pending));},
          NativeFsErrorCode::storage_failure);
      const auto after=session.stats();
      require(session.poisoned() && after.contexts==before.contexts &&
          after.cleaned==before.cleaned && after.delete_requests==before.delete_requests,
          "failed creation exposed/installed/released a context");
      const auto calls=ram->calls;
      denies([&]{session.info(keep);},NativeSessionErrorCode::terminal);
      denies([&]{session.create_open("retry",false,tuple(),sec::descriptor());},NativeSessionErrorCode::terminal);
      require(ram->calls==calls,"failed creation retried poisoned storage");}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));unchanged(view);
    require(view.sequence()==sequence || view.sequence()==sequence+1,"creation recovery lost/fabricated acknowledged history");
    if(view.sequence()==sequence+1)created(view,directory,pending);
    else rejects([&]{view.stat("new");},NativeFsErrorCode::not_found);
    // The old owning session is destroyed; this synthetic single-control-thread
    // fixture independently knows there are no remaining external contexts.
    auto writer=NativeFilesystemWriter::resume_closed_session(ObjectStoreWriter::resume(
        recovered,identity(),view.storage_info().generation,{mode,6,false}));
    auto session=NativeFileSession::create(std::move(writer));
    auto next=session.create_open("continued",false,tuple(),sec::empty_dacl(),options());
    session.write(next.handle,0,data(17,21));
    auto continued=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(recovered->stable,recovered->failure_unit)));
    unchanged(continued);
    require(continued.read("continued",0,100)==data(17,21) &&
        continued.security_descriptor(next.info.inode)==sec::empty_dacl() &&
        !continued.needs_session_recovery(),"CPU continuation after creation failure lost contents/metadata");
    if(pending)rejects([&]{continued.stat("new");},NativeFsErrorCode::not_found);
  }
  require(cases==3*steps,"creation fault point omitted");
  std::cout<<"Native create evidence: {\"schema\":1,\"directory\":"<<(directory?"true":"false")
      <<",\"delete_on_close\":"<<(pending?"true":"false")<<",\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"layout\":\""
      <<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  static_assert(std::is_nothrow_move_constructible_v<NativeSessionCreateCommit>);
  static_assert(std::is_nothrow_move_constructible_v<NativeInodeCreateCommit>);
  if(argc!=2)throw std::runtime_error("one bounded creation group required");
  const std::string_view group=argv[1];
  if(group=="--invariants") {
    admission();closed_recovery();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true}) {
        lifecycle(mode,compact);
        for(bool directory:{false,true})for(bool pending:{false,true})sealed_result(mode,compact,directory,pending);
      }
    require(sealed_cases==16,"creation sealed cases omitted");
    std::cout<<"Native create invariants: {\"schema\":1,\"sealed_cases\":"<<sealed_cases
        <<",\"checks\":"<<checks<<",\"complete\":true}\n";
  } else {
    const bool directory=group=="--directory-faults" || group=="--delete-directory-faults";
    const bool pending=group=="--delete-file-faults" || group=="--delete-directory-faults";
    if(group!="--file-faults" && group!="--directory-faults" && !pending)
      throw std::runtime_error("unknown bounded creation group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,directory,pending);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native create failure: "<<e.what()<<"\n";return 1;}}
