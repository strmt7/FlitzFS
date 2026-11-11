#include "native_cleanup.hpp"
#include "native_fs_fixture.hpp"
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned cases=0,clock_calls=0;
constexpr std::uint64_t stamp=133800000000000099ULL;
std::uint64_t fixed_clock() noexcept {++clock_calls;return stamp;}
std::uint64_t invalid_clock() noexcept {++clock_calls;return UINT64_MAX;}
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
NativeBasicMetadata basic() {
  return {133700000000000011ULL,133700000000000022ULL,133700000000000033ULL,
      133700000000000044ULL,2};
}
struct Seed {
  std::shared_ptr<Ram> ram;
  NativeFileSession session;
  NativeFileHandle handle;
};
Seed seed() {
  auto ram=std::make_shared<Ram>();
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity()));
  const std::array<NativeFileMutation,2> ops{{file_create("f"),file_write("f",0,data(73,61))}};
  const NativeBasicMutation value{0,1,basic()};writer.apply_with_basic(ops,{&value,1});
  auto session=NativeFileSession::create(std::move(writer));
  const auto handle=session.open("f",{true,true,true});
  return {ram,std::move(session),handle};
}
struct Harness {
  NativeFileSession& session;
  const NativeFileHandle& handle;
  NativeCleanupClock clock=fixed_clock;
  NativeCleanupOutcome result=NativeCleanupOutcome::fenced_engine;
  ULONG seen=0;
  unsigned calls=0;
  static void callback(FSP_FILE_SYSTEM* fs,PVOID context,PWSTR,ULONG flags) noexcept {
    auto& self=*static_cast<Harness*>(fs->UserContext);
    if(context!=&self.handle) {
      self.session.fence();self.result=NativeCleanupOutcome::fenced_engine;return;
    }
    ++self.calls;self.seen=flags;
    self.result=dispatch_native_cleanup(self.session,self.handle,flags,self.clock);
  }
};
void dispatch(Harness& h,ULONG flags) {
  FSP_FILE_SYSTEM_INTERFACE table{};table.Cleanup=Harness::callback;
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof(fs));fs.UserContext=&h;fs.Interface=&table;
  fs.UmFileContextIsUserContext2=TRUE;
  // These selected-SDK C structs end in Buffer[]. MSVC rejects C++
  // aggregate {} initialization of that extension; initialize their fixed
  // headers explicitly, without changing the SDK layout or disabling checks.
  FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
  static_assert(std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_REQ> &&
      std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_RSP>);
  ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
  request.Kind=FspFsctlTransactCleanupKind;
  request.Req.Cleanup.UserContext2=reinterpret_cast<std::uintptr_t>(&h.handle);
  request.Req.Cleanup.Delete=(flags&FspCleanupDelete)!=0;
  request.Req.Cleanup.SetAllocationSize=(flags&FspCleanupSetAllocationSize)!=0;
  request.Req.Cleanup.SetArchiveBit=(flags&FspCleanupSetArchiveBit)!=0;
  request.Req.Cleanup.SetLastAccessTime=(flags&FspCleanupSetLastAccessTime)!=0;
  request.Req.Cleanup.SetLastWriteTime=(flags&FspCleanupSetLastWriteTime)!=0;
  request.Req.Cleanup.SetChangeTime=(flags&FspCleanupSetChangeTime)!=0;
  // The actual vendor dispatcher reports success even if its VOID callback
  // fenced. No filesystem object, volume handle, mount or driver is created.
  require(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS &&
      h.calls==1 && h.seen==flags,"actual SDK Cleanup dispatch/VOID mapping differs");
}
void normal() {
  auto s=seed();Harness h{s.session,s.handle};
  clock_calls=0;
  constexpr ULONG flags=FspCleanupSetArchiveBit|FspCleanupSetLastAccessTime|
      FspCleanupSetLastWriteTime|FspCleanupSetChangeTime;
  dispatch(h,flags);
  const auto value=s.session.basic_metadata(s.handle);
  require(h.result==NativeCleanupOutcome::applied && value &&
      value->creation_time==basic().creation_time && value->attributes==(basic().attributes|FILE_ATTRIBUTE_ARCHIVE) &&
      value->last_access_time==stamp && value->last_write_time==stamp && value->change_time==stamp &&
      clock_calls==1 && s.session.stats().contexts==1 && s.session.stats().cleaned==1,
      "SDK Cleanup did not apply one implicit metadata clock without attribute-write grant");
  s.session.close(s.handle);++cases;
}
void archive_no_clock() {
  auto s=seed();Harness h{s.session,s.handle};h.clock=nullptr;clock_calls=0;
  dispatch(h,FspCleanupSetArchiveBit);
  require(h.result==NativeCleanupOutcome::applied && !clock_calls &&
      s.session.basic_metadata(s.handle)->last_write_time==basic().last_write_time,
      "archive-only Cleanup sampled/required a clock");
  s.session.close(s.handle);++cases;
}
void deletion() {
  auto s=seed();s.session.set_delete(s.handle,true);auto old=s.session.snapshot();
  Harness h{s.session,s.handle};const auto barriers=s.ram->flushes;
  dispatch(h,FspCleanupDelete|FspCleanupSetLastWriteTime);
  require(h.result==NativeCleanupOutcome::applied && s.ram->flushes==barriers+3 &&
      s.session.info(s.handle).unlinked && s.session.basic_metadata(s.handle)->last_write_time==stamp &&
      s.session.read(s.handle,0,100)==data(73,61) && old.basic_metadata(2)==basic(),
      "SDK Cleanup split unlink from automatic metadata/destroyed retained bytes");
  s.session.close(s.handle);++cases;
}
void failed(unsigned kind) {
  auto s=seed();const auto before=s.ram->live;const auto calls=s.ram->calls;
  const auto reads=s.ram->read_calls;Harness h{s.session,s.handle};clock_calls=0;
  if(kind==0) {
    dispatch(h,FspCleanupSetAllocationSize|FspCleanupSetLastAccessTime);
    require(h.result==NativeCleanupOutcome::fenced_unsupported && !clock_calls,
        "unimplemented allocation Cleanup silently accepted");
  } else if(kind==1) {
    h.clock=nullptr;dispatch(h,FspCleanupSetLastAccessTime);
    require(h.result==NativeCleanupOutcome::fenced_clock,"missing Cleanup clock not fenced");
  } else if(kind==2) {
    h.clock=invalid_clock;dispatch(h,FspCleanupSetLastAccessTime);
    require(h.result==NativeCleanupOutcome::fenced_clock && clock_calls==1,"invalid clock not fenced");
  } else if(kind==3) {
    h.result=dispatch_native_cleanup(s.session,s.handle,4,fixed_clock);
    require(h.result==NativeCleanupOutcome::fenced_unsupported && !clock_calls,"unknown flags acknowledged");
  } else {
    s.ram->calls=0;s.ram->fail_at=0;s.ram->failure=0;
    dispatch(h,FspCleanupSetLastWriteTime);
    require(h.result==NativeCleanupOutcome::fenced_engine,"backend failure crossed VOID bridge");
  }
  require(s.session.poisoned() && s.session.stats().contexts==1 && !s.session.stats().cleaned,
      "failed SDK Cleanup released/advanced context");
  if(kind<4)require(s.ram->live==before && s.ram->calls==calls && s.ram->read_calls==reads,
      "unsupported/clock Cleanup touched backing data");
  else require(NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(s.ram->stable))).basic_metadata(2)==basic(),
      "failed SDK Cleanup lost acknowledged tuple");
  const auto blocked=s.ram->calls;
  require(dispatch_native_cleanup(s.session,s.handle,0)==NativeCleanupOutcome::fenced_engine &&
      s.ram->calls==blocked,"fenced SDK Cleanup retried I/O");
  ++cases;
}
void real_clock() {
  auto s=seed();Harness h{s.session,s.handle};h.clock=native_cleanup_utc;
  dispatch(h,FspCleanupSetLastAccessTime);
  const auto value=s.session.basic_metadata(s.handle);
  require(h.result==NativeCleanupOutcome::applied && value &&
      value->last_access_time>0 && value->last_access_time<=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
      value->creation_time==basic().creation_time && value->last_write_time==basic().last_write_time &&
      value->change_time==basic().change_time,"actual Windows Cleanup clock corrupted untouched fields");
  s.session.close(s.handle);++cases;
}
}
int main(){try {
  static_assert(noexcept(dispatch_native_cleanup(std::declval<NativeFileSession&>(),
      std::declval<const NativeFileHandle&>(),ULONG{})));
  static_assert(FspCleanupDelete==1 && FspCleanupSetAllocationSize==2 &&
      FspCleanupSetArchiveBit==16 && FspCleanupSetLastAccessTime==32 &&
      FspCleanupSetLastWriteTime==64 && FspCleanupSetChangeTime==128);
  normal();archive_no_clock();deletion();for(unsigned kind=0;kind<5;++kind)failed(kind);real_clock();
  std::cout<<"WinFsp Cleanup evidence: {\"schema\":1,\"cases\":"<<cases<<",\"complete\":true}\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"WinFsp Cleanup failure: "<<error.what()<<"\n";return 1;}}
