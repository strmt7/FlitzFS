#include "native_close.hpp"
#include "native_cleanup.hpp"
#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include <iostream>
#include <type_traits>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,cases=0;
constexpr std::uint64_t stamp=133800000000000123ULL;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
enum class Case {cancel_before,invalid_clock,unstamped,allocation_reject,storage_failure,success_cancel,success_adopt,active_close_reject};
struct Harness {
  NativeFileSession& session;
  NativeFileHandle handle;
  Ram& ram;
  Case test;
  unsigned overwrites=0,closes=0,cleanups=0;
  std::uint64_t close_io=0;
  NativeCloseOutcome close_outcome=NativeCloseOutcome::fenced_engine;
  NativeCleanupOutcome cleanup_outcome=NativeCleanupOutcome::fenced_engine;
  static NTSTATUS overwrite(FSP_FILE_SYSTEM* fs,PVOID context,UINT32 attributes,
      BOOLEAN replace,UINT64 allocation,FSP_FSCTL_FILE_INFO* info) noexcept {
    auto& h=*static_cast<Harness*>(fs->UserContext);
    if(context!=&h.handle || !info){h.session.fence();return STATUS_INVALID_HANDLE;}
    ++h.overwrites;
    if(allocation)return STATUS_INVALID_PARAMETER; // Test-only unsupported-reservation rejection.
    try {
      const NativeOverwriteMetadata effect{h.test==Case::invalid_clock?UINT64_MAX:stamp,attributes,replace!=FALSE};
      auto result=h.session.overwrite_prepared_result(h.handle,effect);
      // This fixture has zero requested allocation/EOF. These bounded copies
      // exercise the actual SDK output ABI, not a production FileInfo policy.
      *info={};info->FileAttributes=result.info.basic->attributes;
      info->CreationTime=result.info.basic->creation_time;
      info->LastAccessTime=result.info.basic->last_access_time;
      info->LastWriteTime=result.info.basic->last_write_time;
      info->ChangeTime=result.info.basic->change_time;info->IndexNumber=result.info.inode;
      return STATUS_SUCCESS;
    }catch(const NativeSessionError&){return STATUS_INVALID_PARAMETER;}
    catch(const NativeFsError& e){return e.code()==NativeFsErrorCode::storage_failure?
        STATUS_IO_DEVICE_ERROR:STATUS_INVALID_PARAMETER;}
    catch(...){h.session.fence();return STATUS_INTERNAL_ERROR;}
  }
  static void close(FSP_FILE_SYSTEM* fs,PVOID context) noexcept {
    auto& h=*static_cast<Harness*>(fs->UserContext);
    if(context!=&h.handle){h.session.fence();return;}
    ++h.closes;const auto before=h.ram.calls+h.ram.read_calls;
    h.close_outcome=dispatch_native_close(h.session,h.handle);
    h.close_io+=h.ram.calls+h.ram.read_calls-before;
  }
  static void cleanup(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,ULONG flags) noexcept {
    auto& h=*static_cast<Harness*>(fs->UserContext);
    if(context!=&h.handle){h.session.fence();return;}
    ++h.cleanups;h.cleanup_outcome=dispatch_native_cleanup(h.session,h.handle,flags,nullptr);
  }
};
void run(Case test,StoreCompressionMode mode,bool replace) {
  auto ram=std::make_shared<Ram>();
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false}));
  const std::array<NativeFileMutation,2> ops{{file_create("f"),file_write("f",0,data(73,41))}};
  const NativeBasicMutation basic{0,1,{11,22,33,44,2}};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  if(test==Case::unstamped)writer.apply(ops);
  else writer.apply_with_metadata(ops,{&basic,1},{&security,1});
  auto s=NativeFileSession::create(std::move(writer));
  auto handle=s.prepare_overwrite("f",{true,false,false},{},{},{!replace,replace});
  Harness h{s,handle,*ram,test};
  FSP_FILE_SYSTEM_INTERFACE table{};table.Overwrite=Harness::overwrite;
  table.Close=Harness::close;table.Cleanup=Harness::cleanup;
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof(fs));fs.UserContext=&h;
  fs.Interface=&table;fs.UmFileContextIsUserContext2=TRUE;
  FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
  static_assert(std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_REQ> &&
      std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_RSP>);
  ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
  const auto context=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(&h.handle));
  const auto before=ram->live;
  if(test==Case::cancel_before) {
    request.Kind=FspFsctlTransactCloseKind;request.Req.Close.UserContext2=context;
    require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS,"SDK direct Close dispatch failed");
    require(!h.overwrites && h.closes==1 && !h.cleanups && !h.close_io &&
        h.close_outcome==NativeCloseOutcome::retired && !s.stats().contexts &&
        ram->live==before && !s.poisoned(),"canceled Open fabricated Cleanup/mutated file/retained healthy context");
    ++cases;return;
  }
  request.Kind=FspFsctlTransactOverwriteKind;request.Req.Overwrite.UserContext2=context;
  request.Req.Overwrite.FileAttributes=4;request.Req.Overwrite.Supersede=replace;
  request.Req.Overwrite.AllocationSize=test==Case::allocation_reject?4096:0;
  if(test==Case::storage_failure){ram->calls=0;ram->fail_at=0;}
  const auto status=FspFileSystemOpOverwrite(&fs,&request,&response);
  const bool success=test==Case::success_cancel || test==Case::success_adopt || test==Case::active_close_reject;
  if(!success) {
    require(status==(test==Case::storage_failure?STATUS_IO_DEVICE_ERROR:STATUS_INVALID_PARAMETER) &&
        h.overwrites==1 && h.closes==1 && !h.cleanups && !h.close_io,
        "actual SDK failed Overwrite did not call Close directly/preserve original status");
    if(test==Case::storage_failure) {
      require(h.close_outcome==NativeCloseOutcome::fenced_engine && s.poisoned() &&
          s.stats().contexts==1,"publication failure Close dropped poisoned ownership");
      auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
      require(recovered.read("f",0,100)==data(73,41),"failed first write lost original acknowledged data");
    } else require(h.close_outcome==NativeCloseOutcome::retired && !s.stats().contexts &&
        !s.poisoned() && ram->live==before,"healthy failed Overwrite Close mutated data/fenced/leaked");
    ++cases;return;
  }
  require(status==STATUS_SUCCESS && h.overwrites==1 && !h.closes && !h.cleanups &&
      response.Rsp.Overwrite.FileInfo.FileSize==0 && response.Rsp.Overwrite.FileInfo.AllocationSize==0 &&
      response.Rsp.Overwrite.FileInfo.IndexNumber==2 &&
      response.Rsp.Overwrite.FileInfo.CreationTime==11 &&
      response.Rsp.Overwrite.FileInfo.LastWriteTime==stamp &&
      response.Rsp.Overwrite.FileInfo.FileAttributes==(replace?0x24U:0x26U),
      "actual SDK success unexpectedly closed context or returned incomplete zero-size fixture facts");
  if(test!=Case::success_cancel)s.adopt_prepared(handle);
  if(test==Case::success_adopt) {
    request.Kind=FspFsctlTransactCleanupKind;ZeroMemory(&request.Req,sizeof(request.Req));
    request.Req.Cleanup.UserContext2=context;
    require(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS &&
        h.cleanup_outcome==NativeCleanupOutcome::applied && h.cleanups==1,
        "actual SDK adopted Cleanup did not reach engine");
  }
  const auto after=ram->live;
  request.Kind=FspFsctlTransactCloseKind;request.Req.Close.UserContext2=context;
  require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS && h.closes==1,
      "actual SDK final Close dispatch failed");
  if(test==Case::active_close_reject)
    require(h.close_outcome==NativeCloseOutcome::fenced_engine && s.poisoned() &&
        s.stats().contexts==1 && !h.cleanups && !h.close_io,
        "VOID Close concealed missing active Cleanup or released context");
  else require(h.close_outcome==NativeCloseOutcome::retired && !s.poisoned() &&
      !s.stats().contexts && (test!=Case::success_cancel || (!h.cleanups && !h.close_io)) &&
      ram->live==after,"SDK healthy Close fabricated Cleanup/undid commit/leaked context");
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable)));
  require(recovered.read("f",0,1).empty() && recovered.security_descriptor(2)==sec::descriptor(),
      "SDK Close path lost committed overwrite identity/security");++cases;
}
}
int main(){try {
  wchar_t action[16]{},environment[32]{};
  if(GetEnvironmentVariableW(L"GITHUB_ACTIONS",action,16)!=4 || std::wstring_view(action)!=L"true" ||
      GetEnvironmentVariableW(L"RUNNER_ENVIRONMENT",environment,32)!=13 ||
      std::wstring_view(environment)!=L"github-hosted")
    throw std::runtime_error("disposable hosted SDK fixture required");
  for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool replace:{false,true})
    for(const auto test:{Case::cancel_before,Case::invalid_clock,Case::unstamped,Case::allocation_reject,
        Case::storage_failure,Case::success_cancel,Case::success_adopt,Case::active_close_reject})run(test,mode,replace);
  require(cases==32,"SDK provisional case omitted");
  std::cout<<"Windows provisional evidence: {\"schema\":1,\"cases\":"<<cases<<",\"checks\":"<<checks
      <<",\"complete\":true,\"mount\":false,\"scope\":\"actual user-mode SDK Overwrite/Close/Cleanup; not kernel/client/writable qualification\"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Windows provisional failure: "<<e.what()<<"\n";return 1;}}
