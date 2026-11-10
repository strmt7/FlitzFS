#include "native_cleanup.hpp"
#include "native_close.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include <iostream>
#include <string_view>
#include <type_traits>
namespace {
using namespace flitzfs;using namespace native_fixture;namespace sec=native_security_fixture;
unsigned checks=0,cases=0,backend_retired=0,clock_calls=0;
constexpr std::uint64_t stamp=133800000000000131ULL;
std::uint64_t clock_value() noexcept{return stamp+clock_calls++;}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Outcome {NativeCleanupOutcome cleanup=NativeCleanupOutcome::fenced_engine;
  NativeCloseOutcome close=NativeCloseOutcome::fenced_engine;unsigned callbacks=0;};
struct Dispatch {
  NativeFileSession& session;const NativeFileHandle& handle;Outcome& outcome;
  static void cleanup(FSP_FILE_SYSTEM* fs,PVOID ctx,PWSTR,ULONG flags) noexcept {
    auto& h=*static_cast<Dispatch*>(fs->UserContext);
    if(ctx!=&h.handle){h.session.fence();return;}
    ++h.outcome.callbacks;
    h.outcome.cleanup=dispatch_native_allocated_cleanup(h.session,h.handle,flags,clock_value);
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
  if(close){request.Kind=FspFsctlTransactCloseKind;request.Req.Close.UserContext2=context;}
  else {
    request.Kind=FspFsctlTransactCleanupKind;request.Req.Cleanup.UserContext2=context;
    request.Req.Cleanup.Delete=(flags&FspCleanupDelete)!=0;
    request.Req.Cleanup.SetAllocationSize=(flags&FspCleanupSetAllocationSize)!=0;
    request.Req.Cleanup.SetArchiveBit=(flags&FspCleanupSetArchiveBit)!=0;
    request.Req.Cleanup.SetLastAccessTime=(flags&FspCleanupSetLastAccessTime)!=0;
    request.Req.Cleanup.SetLastWriteTime=(flags&FspCleanupSetLastWriteTime)!=0;
    request.Req.Cleanup.SetChangeTime=(flags&FspCleanupSetChangeTime)!=0;
  }
  const auto status=close?FspFileSystemOpClose(&fs,&request,&response):
      FspFileSystemOpCleanup(&fs,&request,&response);
  require(status==STATUS_SUCCESS && result.callbacks==1,"actual SDK VOID status/context dispatch differs");
  return result;
}
enum class Kind {trim,trim_only,duplicate,directory,untracked,failed,impossible};
void run(StoreCompressionMode mode,Kind kind) {
  std::weak_ptr<Ram> weak;
  {
    auto ram=std::make_shared<Ram>();weak=ram;
    auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
    const bool dir=kind==Kind::directory;
    std::vector<NativeFileMutation> ops{file_create("target",dir),file_create("keep"),file_write("keep",0,data(37,86))};
    if(dir){ops.push_back(file_create("target/child"));ops.push_back(file_write("target/child",0,data(19,87)));}
    else ops.push_back(file_write("target",0,data(7001,88)));
    const NativeBasicMetadata basic{11,22,33,44,dir?0x10U:2U};
    const NativeBasicMutation value{0,1,basic};const NativeSecurityMutation security{0,1,sec::descriptor()};
    writer.apply_with_metadata(ops,{&value,1},{&security,1});
    if(!dir && kind!=Kind::untracked)writer.reserve_inode_result(2,131072);
    auto s=NativeFileSession::create(std::move(writer));
    auto h=s.open("target",{true,false,false});clock_calls=0;
    ULONG flags=FspCleanupSetAllocationSize;
    if(kind!=Kind::trim_only)flags|=FspCleanupSetArchiveBit|FspCleanupSetLastAccessTime|
        FspCleanupSetLastWriteTime|FspCleanupSetChangeTime;
    if(kind==Kind::impossible)flags|=FspCleanupDelete;
    if(kind==Kind::failed){ram->calls=0;ram->fail_at=0;}
    const auto before_calls=ram->calls,before_reads=ram->read_calls;
    const auto first=dispatch(s,h,false,flags);
    const bool rejected=kind==Kind::untracked || kind==Kind::failed || kind==Kind::impossible;
    if(rejected) {
      require(first.cleanup==(kind==Kind::impossible?NativeCleanupOutcome::fenced_unsupported:NativeCleanupOutcome::fenced_engine) &&
          s.poisoned() && s.stats().contexts==1 && !s.stats().cleaned,
          "VOID allocated Cleanup hid failure/released ownership");
      require(clock_calls==(kind==Kind::impossible?0U:1U),"invalid flags sampled a clock or valid effects lost clock");
      if(kind==Kind::impossible)require(ram->calls==before_calls && ram->read_calls==before_reads,
          "impossible delete+trim touched backing");
      const auto calls=ram->calls,reads=ram->read_calls;const auto close=dispatch(s,h,true);
      require(close.close==NativeCloseOutcome::fenced_engine && ram->calls==calls &&
          ram->read_calls==reads && s.stats().contexts==1,"fenced SDK Close retried/released allocated context");
      auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
      require(view.read("target",0,10000)==data(7001,88) && view.basic_metadata(2)==basic &&
          view.allocation_info(2).reserved_bytes==(kind==Kind::untracked?std::optional<std::uint64_t>{}:131072),
          "rejected allocation Cleanup changed acknowledged data/metadata/reservation");
    } else {
      require(first.cleanup==NativeCleanupOutcome::applied && !s.poisoned() &&
          s.stats().contexts==1 && s.stats().cleaned==1,"SDK allocated Cleanup did not retain healthy paging ownership");
      require(clock_calls==(kind==Kind::trim_only?0U:1U),"trim sampled unnecessary/multiple clocks");
      require(s.allocation_info(h).reserved_bytes==(dir?std::optional<std::uint64_t>{}:8192),
          "SDK trim did not preserve directory absence/release native reservation");
      const auto expected=kind==Kind::trim_only?basic:
          NativeBasicMetadata{11,stamp,stamp,stamp,dir?0x10U:0x22U};
      require(s.basic_metadata(h)==expected,"SDK trim/basic tuple not atomic/first-clock exact");
      if(kind==Kind::duplicate) {
        const auto calls=ram->calls;const auto second=dispatch(s,h,false,flags);
        require(second.cleanup==NativeCleanupOutcome::applied && ram->calls==calls &&
            clock_calls==2 && s.basic_metadata(h)==expected,"duplicate SDK trim republished/replaced first clock");
      }
      if(dir)require(s.snapshot().read("target/child",0,100)==data(19,87),"directory trim lost child");
      else require(s.read(h,0,10000)==data(7001,88),"trim changed file bytes");
      const auto close=dispatch(s,h,true);
      require(close.close==NativeCloseOutcome::retired && !s.stats().contexts,
          "SDK allocated Close left a context");
    }
    require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable))).read("keep",0,100)==data(37,86),
        "SDK allocation lifecycle lost unrelated bytes");
  }
  require(weak.expired(),"allocated SDK fixture retained backing ownership after destruction");
  ++backend_retired;++cases;
}
}
int main(){try {
  wchar_t action[16]{},environment[32]{};
  if(GetEnvironmentVariableW(L"GITHUB_ACTIONS",action,16)!=4 || std::wstring_view(action)!=L"true" ||
      GetEnvironmentVariableW(L"RUNNER_ENVIRONMENT",environment,32)!=13 ||
      std::wstring_view(environment)!=L"github-hosted")throw std::runtime_error("hosted-only SDK fixture");
  DWORD initial=0,final=0;
  require(GetProcessHandleCount(GetCurrentProcess(),&initial)!=FALSE,"initial SDK handle accounting unavailable");
  for(unsigned round=0;round<2;++round)
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(const auto kind:{Kind::trim,Kind::trim_only,Kind::duplicate,Kind::directory,
          Kind::untracked,Kind::failed,Kind::impossible})run(mode,kind);
  require(GetProcessHandleCount(GetCurrentProcess(),&final)!=FALSE && initial==final,
      "allocated SDK repeated lifecycle leaked OS handles");
  require(cases==28 && backend_retired==cases,"allocated SDK lifecycle cases omitted");
  std::cout<<"Windows allocated-event evidence: {\"schema\":1,\"cases\":"<<cases<<",\"checks\":"<<checks
      <<",\"backend_retired\":"<<backend_retired<<",\"handles_initial\":"<<initial<<",\"handles_final\":"<<final
      <<",\"complete\":true,\"mount\":false,\"scope\":\"actual user-mode SDK Cleanup/Close; no writable/client/physical allocation qualification\"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Windows allocated-event failure: "<<e.what()<<"\n";return 1;}}
