#include "native_cleanup.hpp"
#include "native_close.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include <iostream>
#include <string_view>
#include <type_traits>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,cases=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t stamp=133800000000000123ULL;
std::uint64_t clock_value() noexcept{return stamp;}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Outcome {NativeCleanupOutcome cleanup=NativeCleanupOutcome::fenced_engine;
  NativeCloseOutcome close=NativeCloseOutcome::fenced_engine;unsigned callbacks=0;};
struct Dispatch {
  NativeFileSession& session;const NativeFileHandle& handle;Outcome& outcome;
  static void cleanup(FSP_FILE_SYSTEM* fs,PVOID ctx,PWSTR,ULONG flags) noexcept {
    auto& h=*static_cast<Dispatch*>(fs->UserContext);
    if(ctx!=&h.handle){h.session.fence();return;}
    ++h.outcome.callbacks;h.outcome.cleanup=dispatch_native_cleanup(h.session,h.handle,flags,clock_value);
  }
  static void close(FSP_FILE_SYSTEM* fs,PVOID ctx) noexcept {
    auto& h=*static_cast<Dispatch*>(fs->UserContext);
    if(ctx!=&h.handle){h.session.fence();return;}
    ++h.outcome.callbacks;h.outcome.close=dispatch_native_close(h.session,h.handle);
  }
};
Outcome dispatch(NativeFileSession& s,const NativeFileHandle& handle,bool close,ULONG flags=0) {
  Outcome result;Dispatch h{s,handle,result};FSP_FILE_SYSTEM_INTERFACE table{};
  table.Cleanup=Dispatch::cleanup;table.Close=Dispatch::close;
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof(fs));fs.UserContext=&h;
  fs.Interface=&table;fs.UmFileContextIsUserContext2=TRUE;
  FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
  static_assert(std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_REQ> &&
      std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_RSP>);
  ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
  const auto context=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(&handle));
  if(close) {request.Kind=FspFsctlTransactCloseKind;request.Req.Close.UserContext2=context;}
  else {
    request.Kind=FspFsctlTransactCleanupKind;request.Req.Cleanup.UserContext2=context;
    request.Req.Cleanup.Delete=(flags&FspCleanupDelete)!=0;
    request.Req.Cleanup.SetArchiveBit=(flags&FspCleanupSetArchiveBit)!=0;
    request.Req.Cleanup.SetLastWriteTime=(flags&FspCleanupSetLastWriteTime)!=0;
    request.Req.Cleanup.SetChangeTime=(flags&FspCleanupSetChangeTime)!=0;
  }
  const auto status=close?FspFileSystemOpClose(&fs,&request,&response):
      FspFileSystemOpCleanup(&fs,&request,&response);
  require(status==STATUS_SUCCESS && result.callbacks==1,"actual SDK VOID callback/status/context mapping differs");
  return result;
}
enum class Kind {single,pair,empty_dir,nonempty_dir,nonempty_pair,cancel_open,cancel_create,failed,unauthorized,overwrite,supersede};
void run(StoreCompressionMode mode,Kind kind) {
  const bool dir=kind==Kind::empty_dir || kind==Kind::nonempty_dir || kind==Kind::nonempty_pair ||
      kind==Kind::cancel_create;
  const bool child=kind==Kind::nonempty_dir || kind==Kind::nonempty_pair;
  auto ram=std::make_shared<Ram>();
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
  std::vector<NativeFileMutation> ops{file_create("target",dir),file_create("keep"),file_write("keep",0,data(37,61))};
  if(child){ops.push_back(file_create("target/child"));ops.push_back(file_write("target/child",0,data(19,62)));}
  else if(!dir)ops.push_back(file_write("target",0,data(73,63)));
  const NativeBasicMetadata basic{11,22,33,44,dir?0x10U:2U};
  const NativeBasicMutation value{0,1,basic};const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&value,1},{&security,1});
  auto s=NativeFileSession::create(std::move(writer));
  NativeFileHandle h;
  if(kind==Kind::cancel_create) {
    NativePreparedCreateOptions options;options.initial_delete={true};
    h=s.create_prepared("new",true,basic,sec::descriptor(),options).handle;
  } else if(kind==Kind::overwrite || kind==Kind::supersede) {
    const bool replace=kind==Kind::supersede;
    h=s.prepare_overwrite("target",{true,false,false},{},{},{!replace,replace},{true});
    auto result=s.overwrite_prepared_result(h,{stamp,4,replace});
    require(!result.info.delete_pending,"prepared overwrite persisted its initial flag");
  } else h=s.prepare_open("target",{true,false,false},dir?NativeOpenKind::directory:NativeOpenKind::file,
      {},{true,false},{kind!=Kind::unauthorized});
  if(kind==Kind::cancel_open || kind==Kind::cancel_create) {
    const auto bytes=ram->live;const auto calls=ram->calls,reads=ram->read_calls;
    const auto outcome=dispatch(s,h,true);
    require(outcome.close==NativeCloseOutcome::retired && ram->live==bytes && ram->calls==calls &&
        ram->read_calls==reads && !s.stats().contexts && !s.snapshot().needs_session_recovery(),
        "actual SDK canceled initial option performed deletion/Close I/O");
    require(s.snapshot().stat(kind==Kind::cancel_create?"new":"target").directory==dir,
        "canceled initial option removed named inode");++cases;return;
  }
  s.adopt_prepared(h);
  require(!s.info(h).delete_pending && !s.snapshot().needs_session_recovery(),"initial option armed before SDK Cleanup");
  // A final GrantedAccess mask without DELETE does not authorize an explicit
  // SetDelete, even though the initial request was independently checked.
  try{s.set_delete(h,true);throw std::runtime_error("initial option granted explicit DELETE");}
  catch(const NativeSessionError& e){require(e.code()==NativeSessionErrorCode::access_denied,"wrong explicit-grant denial");}
  const bool paired=kind==Kind::pair || kind==Kind::nonempty_pair;
  NativeFileHandle peer;if(paired)peer=s.open("target",{true,false,false});
  if(kind==Kind::failed){ram->calls=0;ram->fail_at=0;}
  const auto first=dispatch(s,h,false,(paired?0U:FspCleanupDelete)|FspCleanupSetLastWriteTime|FspCleanupSetArchiveBit);
  if(kind==Kind::failed || kind==Kind::unauthorized) {
    require(first.cleanup==NativeCleanupOutcome::fenced_engine && s.poisoned() &&
        s.stats().contexts==1 && !s.stats().cleaned,"actual VOID Cleanup hid failure or released context");
    const auto calls=ram->calls,reads=ram->read_calls;auto closed=dispatch(s,h,true);
    require(closed.close==NativeCloseOutcome::fenced_engine && s.stats().contexts==1 &&
        ram->calls==calls && ram->read_calls==reads,"fenced SDK Close retried/released initial context");
    auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
    require(recovered.read("target",0,100)==data(73,63),"failed/unauthorized Cleanup lost acknowledged contents");
    ++cases;return;
  }
  require(first.cleanup==NativeCleanupOutcome::applied && s.stats().cleaned==1,
      "actual initial Cleanup did not apply to owned context");
  if(paired) {
    require(s.info(peer).delete_pending && s.stats().latched_inodes==1 &&
        s.snapshot().inode_state(2).delete_pending==!child,"paired initial pending policy differs from storage");
    require(dispatch(s,h,true).close==NativeCloseOutcome::retired,"first initial context failed Close");
    require(dispatch(s,peer,false,FspCleanupDelete|FspCleanupSetChangeTime).cleanup==NativeCleanupOutcome::applied,
        "last peer Cleanup lost initial authority");
    require(dispatch(s,peer,true).close==NativeCloseOutcome::retired,"last peer Close failed");
  } else require(dispatch(s,h,true).close==NativeCloseOutcome::retired,"single initial Close failed");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  require(!s.stats().contexts && !s.stats().latched_inodes && !s.poisoned() &&
      view.read("keep",0,100)==data(37,61),"initial SDK lifecycle leaked/lost unrelated bytes");
  if(child)require(view.read("target/child",0,100)==data(19,62) && !view.needs_session_recovery(),
      "actual nonempty directory Cleanup deleted contents or stranded pending state");
  else {
    bool missing=false;try{view.stat("target");}catch(const NativeFsError& e){missing=e.code()==NativeFsErrorCode::not_found;}
    require(missing,"actual initial Cleanup did not remove eligible inode");
  }
  ++cases;
}
}
int main(){try {
  wchar_t action[16]{},environment[32]{};
  if(GetEnvironmentVariableW(L"GITHUB_ACTIONS",action,16)!=4 || std::wstring_view(action)!=L"true" ||
      GetEnvironmentVariableW(L"RUNNER_ENVIRONMENT",environment,32)!=13 ||
      std::wstring_view(environment)!=L"github-hosted")throw std::runtime_error("hosted-only SDK fixture");
  for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
    for(const auto kind:{Kind::single,Kind::pair,Kind::empty_dir,Kind::nonempty_dir,Kind::nonempty_pair,
        Kind::cancel_open,Kind::cancel_create,Kind::failed,Kind::unauthorized,Kind::overwrite,Kind::supersede})run(mode,kind);
  require(cases==22,"initial SDK case omitted");
  std::cout<<"Windows initial-delete evidence: {\"schema\":1,\"cases\":"<<cases<<",\"checks\":"<<checks
      <<",\"complete\":true,\"mount\":false,\"scope\":\"actual user-mode SDK Cleanup/Close; not initial-option kernel/client qualification\"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Windows initial-delete failure: "<<e.what()<<"\n";return 1;}}
