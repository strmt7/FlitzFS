#include "native_file_info.hpp"
#include "native_fs_fixture.hpp"
#include "native_lz4_fixture.hpp"
#include "native_security_fixture.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <optional>
#include <string_view>
#include <type_traits>
namespace {
using namespace flitzfs;using namespace native_fixture;namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0,sdk_calls=0,retired=0,advice_cases=0,advice_sealed_cases=0;
constexpr auto mode=NativeFileInfoAllocation::native_reserved_cache;
constexpr std::uint64_t stamp=133800000000000137ULL;
constexpr NativeFileAccess all{true,true,true};
NativeBasicMetadata tuple(){return {11,22,33,44,2};}
NativeAutomaticMetadata effects(){return {stamp,true,true,true,true};}
NativeBasicMetadata updated(){return {11,stamp,stamp,stamp,0x22};}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
bool zero(const FSP_FSCTL_FILE_INFO& info) {
  const auto* b=reinterpret_cast<const unsigned char*>(&info);
  return std::all_of(b,b+sizeof(info),[](auto v){return v==0;});
}
void exact(const FSP_FSCTL_FILE_INFO& info,std::uint64_t size,std::uint64_t allocation,
    NativeBasicMetadata basic=tuple(),std::uint64_t inode=2) {
  require(info.FileAttributes==basic.attributes && !info.ReparseTag && info.AllocationSize==allocation &&
      info.FileSize==size && info.CreationTime==basic.creation_time && info.LastAccessTime==basic.last_access_time &&
      info.LastWriteTime==basic.last_write_time && info.ChangeTime==basic.change_time &&
      info.IndexNumber==inode && !info.HardLinks && !info.EaSize,"incomplete/fictional Windows FileInfo");
}
struct Result {
  NativeWindowsIoOutcome outcome;
  NTSTATUS status=STATUS_INVALID_DEVICE_STATE;
  std::uint64_t count=0;
  FSP_FSCTL_FILE_INFO info{};
  unsigned callbacks=0;
};
struct Dispatch {
  NativeFileSession& session;const NativeFileHandle& handle;Result& result;
  NativeAutomaticMetadata effect;NativeFileInfoAllocation policy=mode;
  std::span<const NativeLz4ExtentCandidate> candidates{};bool advice=false;
  bool valid(PVOID context) noexcept {
    if(context==&handle)return true;
    session.fence();result.outcome={STATUS_INVALID_HANDLE,true};return false;
  }
  static NTSTATUS write(FSP_FILE_SYSTEM* fs,PVOID ctx,PVOID buffer,UINT64 offset,ULONG length,
      BOOLEAN append,BOOLEAN constrained,PULONG count,FSP_FSCTL_FILE_INFO* info) noexcept {
    auto& d=*static_cast<Dispatch*>(fs->UserContext);++d.result.callbacks;
    if(!d.valid(ctx))return d.result.outcome.status;
    d.result.outcome=d.advice?
        dispatch_native_write_with_lz4(d.session,d.handle,buffer,offset,length,append!=FALSE,constrained!=FALSE,
            d.candidates,d.effect,d.policy,count,info):
        dispatch_native_write(d.session,d.handle,buffer,offset,length,append!=FALSE,constrained!=FALSE,
            d.effect,d.policy,count,info);
    return d.result.outcome.status;
  }
  static NTSTATUS size(FSP_FILE_SYSTEM* fs,PVOID ctx,UINT64 bytes,BOOLEAN allocation,FSP_FSCTL_FILE_INFO* info) noexcept {
    auto& d=*static_cast<Dispatch*>(fs->UserContext);++d.result.callbacks;
    if(!d.valid(ctx))return d.result.outcome.status;
    d.result.outcome=dispatch_native_set_size(d.session,d.handle,bytes,allocation!=FALSE,d.effect,d.policy,info);
    return d.result.outcome.status;
  }
  static NTSTATUS query(FSP_FILE_SYSTEM* fs,PVOID ctx,FSP_FSCTL_FILE_INFO* info) noexcept {
    auto& d=*static_cast<Dispatch*>(fs->UserContext);++d.result.callbacks;
    if(!d.valid(ctx))return d.result.outcome.status;
    d.result.outcome=dispatch_native_file_info(d.session,d.handle,d.policy,info);return d.result.outcome.status;
  }
  static NTSTATUS flush(FSP_FILE_SYSTEM* fs,PVOID ctx,FSP_FSCTL_FILE_INFO* info) noexcept {
    auto& d=*static_cast<Dispatch*>(fs->UserContext);++d.result.callbacks;
    if(ctx && !d.valid(ctx))return d.result.outcome.status;
    d.result.outcome=dispatch_native_flush(d.session,ctx?&d.handle:nullptr,d.policy,info);return d.result.outcome.status;
  }
};
enum class Op {write,eof,reserve,query,flush,volume};
Result dispatch(NativeFileSession& session,const NativeFileHandle& handle,Op op,
    std::uint64_t at=0,const Bytes& bytes={},bool constrained=false,
    NativeAutomaticMetadata effect={},NativeFileInfoAllocation policy=mode,
    std::span<const NativeLz4ExtentCandidate> candidates={},bool advice=false) {
  Result r;Dispatch d{session,handle,r,effect,policy,candidates,advice};FSP_FILE_SYSTEM_INTERFACE table{};
  table.Write=Dispatch::write;table.SetFileSize=Dispatch::size;table.GetFileInfo=Dispatch::query;table.Flush=Dispatch::flush;
  FSP_FILE_SYSTEM fs{};fs.Version=static_cast<UINT16>(sizeof(fs));fs.UserContext=&d;
  fs.Interface=&table;fs.UmFileContextIsUserContext2=TRUE;
  FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
  static_assert(std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_REQ> &&
      std::is_trivially_copyable_v<FSP_FSCTL_TRANSACT_RSP>);
  ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
  const auto ctx=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(&handle));
  switch(op) {
    case Op::write:
      request.Kind=FspFsctlTransactWriteKind;request.Req.Write.UserContext2=ctx;
      request.Req.Write.Address=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(bytes.data()));
      request.Req.Write.Offset=at;request.Req.Write.Length=static_cast<UINT32>(bytes.size());
      request.Req.Write.ConstrainedIo=constrained;
      r.status=FspFileSystemOpWrite(&fs,&request,&response);
      r.count=response.IoStatus.Information;r.info=response.Rsp.Write.FileInfo;break;
    case Op::eof:case Op::reserve:
      request.Kind=FspFsctlTransactSetInformationKind;request.Req.SetInformation.UserContext2=ctx;
      request.Req.SetInformation.FileInformationClass=op==Op::eof?20:19;
      if(op==Op::eof)request.Req.SetInformation.Info.EndOfFile.FileSize=at;
      else request.Req.SetInformation.Info.Allocation.AllocationSize=at;
      r.status=FspFileSystemOpSetInformation(&fs,&request,&response);r.info=response.Rsp.SetInformation.FileInfo;break;
    case Op::query:
      request.Kind=FspFsctlTransactQueryInformationKind;request.Req.QueryInformation.UserContext2=ctx;
      r.status=FspFileSystemOpQueryInformation(&fs,&request,&response);r.info=response.Rsp.QueryInformation.FileInfo;break;
    case Op::flush:case Op::volume:
      request.Kind=FspFsctlTransactFlushBuffersKind;request.Req.FlushBuffers.UserContext2=op==Op::volume?0:ctx;
      r.status=FspFileSystemOpFlushBuffers(&fs,&request,&response);r.info=response.Rsp.FlushBuffers.FileInfo;break;
  }
  require(r.callbacks==1 && r.status==r.outcome.status,"actual SDK callback/status mapping differs");
  ++sdk_calls;return r;
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode compression,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{compression,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer,bool basic=true,bool tracked=true) {
  const std::array<NativeFileMutation,4> ops{{file_create("target"),file_write("target",0,data(7001,91)),
      file_create("keep"),file_write("keep",0,data(37,92))}};
  const NativeBasicMutation b{0,1,tuple()};const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,basic?std::span<const NativeBasicMutation>(&b,1):std::span<const NativeBasicMutation>{},
      {&security,1});
  if(tracked)writer.reserve_inode_result(2,8192);
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(37,92) && view.stat("keep").inode==3,"Windows data bridge changed unrelated bytes");
}
void translation() {
  NativeInodeAllocationInfo state;state.info={2,7001,9,tuple(),false,false,false};state.reserved_bytes=8192;
  FSP_FSCTL_FILE_INFO info;std::memset(&info,0xa5,sizeof(info));
  require(encode_native_file_info(state,mode,info),"valid native FileInfo rejected");exact(info,7001,8192);
  const auto reject=[&](NativeInodeAllocationInfo invalid,NativeFileInfoAllocation policy=mode) {
    std::memset(&info,0xa5,sizeof(info));require(!encode_native_file_info(invalid,policy,info)&&zero(info),
        "invalid FileInfo left stale/partial ABI bytes");
  };
  reject(state,NativeFileInfoAllocation::unavailable);reject(state,static_cast<NativeFileInfoAllocation>(77));
  auto bad=state;bad.info.inode=0;reject(bad);bad=state;bad.info.modified_sequence=0;reject(bad);
  bad=state;bad.info.basic.reset();reject(bad);bad=state;bad.reserved_bytes.reset();reject(bad);
  bad=state;bad.reserved_bytes=4096;reject(bad);bad=state;bad.reserved_bytes=8193;reject(bad);
  bad=state;bad.reserved_bytes=kNativeAllocationCapacity+4096;reject(bad);
  bad=state;bad.info.basic->creation_time=UINT64_MAX;reject(bad);
  bad=state;bad.info.basic->attributes=FILE_ATTRIBUTE_NORMAL;reject(bad);
  bad=state;bad.info.unlinked=bad.info.delete_pending=true;reject(bad);
  bad=state;bad.info.directory=true;reject(bad);
  bad=state;bad.info.logical_bytes=0;bad.info.basic->attributes=0;bad.reserved_bytes=0;
  require(encode_native_file_info(bad,mode,info),"present zero reservation/attributes rejected");exact(info,0,0,{11,22,33,44,0});
  bad.info.directory=true;bad.info.basic->attributes=FILE_ATTRIBUTE_DIRECTORY;bad.reserved_bytes.reset();
  require(encode_native_file_info(bad,mode,info),"directory FileInfo rejected");
  exact(info,0,0,{11,22,33,44,FILE_ATTRIBUTE_DIRECTORY});
  bad.info.basic->attributes|=FILE_ATTRIBUTE_TEMPORARY;reject(bad);
}
void positive(StoreCompressionMode compression,bool compact) {
  std::weak_ptr<Ram> weak;
  {
    auto ram=std::make_shared<Ram>();weak=ram;auto writer=fresh(ram,compression,compact);seed(writer);
    auto s=NativeFileSession::create(std::move(writer));auto h=s.open("target",all);
    auto r=dispatch(s,h,Op::query);require(r.status==STATUS_SUCCESS&&!r.outcome.offline_required,"SDK query failed");exact(r.info,7001,8192);
    auto expected=data(7001,91);auto extra=data(73,93);expected.insert(expected.end(),extra.begin(),extra.end());
    r=dispatch(s,h,Op::write,UINT64_MAX,extra,false,effects());
    require(r.status==STATUS_SUCCESS && r.count==73 && !r.outcome.offline_required,"SDK append failed");exact(r.info,7074,8192,updated());
    require(s.read(h,0,10000)==expected,"SDK append data differs");
    r=dispatch(s,h,Op::write,7070,data(20,94),true);require(r.status==STATUS_SUCCESS&&r.count==4,"SDK constrained count incorrect");
    const auto clip=data(20,94);std::copy_n(clip.begin(),4,expected.begin()+7070);exact(r.info,7074,8192,updated());
    const auto calls=ram->calls;auto later=effects();later.utc_time++;
    r=dispatch(s,h,Op::write,UINT64_MAX,data(19,95),true,later);
    require(r.status==STATUS_SUCCESS&&!r.count&&ram->calls==calls,"SDK constrained EOF no-op published");exact(r.info,7074,8192,updated());
    r=dispatch(s,h,Op::write,kNativeMaxFileBytes+77,data(19,95),true,later);
    require(r.status==STATUS_SUCCESS&&!r.count&&ram->calls==calls,"SDK distant constrained no-op published");exact(r.info,7074,8192,updated());
    r=dispatch(s,h,Op::write,0,{},false,later);
    require(r.status==STATUS_SUCCESS&&!r.count&&ram->calls==calls,"SDK empty write published");exact(r.info,7074,8192,updated());
    r=dispatch(s,h,Op::eof,17);require(r.status==STATUS_SUCCESS,"SDK EOF shrink failed");exact(r.info,17,8192,updated());
    r=dispatch(s,h,Op::eof,7001);require(r.status==STATUS_SUCCESS,"SDK EOF regrowth failed");exact(r.info,7001,8192,updated());
    expected.resize(7001);std::fill(expected.begin()+17,expected.end(),std::byte{});
    require(s.read(h,0,10000)==expected,"SDK regrowth leaked truncated tail");
    r=dispatch(s,h,Op::reserve,4096);require(r.status==STATUS_SUCCESS,"SDK allocation shrink failed");exact(r.info,4096,4096,updated());
    expected.resize(4096);require(s.read(h,0,10000)==expected,"SDK allocation shrink data differs");
    const auto before=ram->live;const auto sequence=s.snapshot().sequence();const auto flushes=ram->flushes;
    r=dispatch(s,h,Op::flush);require(r.status==STATUS_SUCCESS && ram->live==before && ram->flushes==flushes+1 &&
        s.snapshot().sequence()==sequence,"SDK file Flush skipped barrier/published");exact(r.info,4096,4096,updated());
    r=dispatch(s,h,Op::volume);require(r.status==STATUS_SUCCESS && zero(r.info) && ram->flushes==flushes+2,
        "SDK volume Flush fabricated FileInfo/skipped barrier");
    s.set_delete(h,true);s.cleanup(h,true);
    r=dispatch(s,h,Op::write,UINT64_MAX,data(7,96));require(r.status==STATUS_SUCCESS&&r.count==7,"late paging append failed");
    exact(r.info,4103,8192,updated());r=dispatch(s,h,Op::flush);require(r.status==STATUS_SUCCESS,"detached Flush failed");
    s.close(h);require(!s.stats().contexts,"SDK positive lifecycle left a context");keep(s.snapshot());
  }
  require(weak.expired(),"SDK positive lifecycle retained RAM backing");++retired;
}
void rejection() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto h=s.open("target",all);
  FSP_FSCTL_FILE_INFO info;ULONG count=77;std::memset(&info,0xa5,sizeof(info));
  auto before=ram->live;auto calls=ram->calls,reads=ram->read_calls;
  auto out=dispatch_native_write(s,h,nullptr,0,1,false,false,{},mode,&count,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&!out.offline_required&&!count&&zero(info),"null SDK input accepted/leaked output");
  out=dispatch_native_write(s,h,nullptr,0,0,false,false,{},NativeFileInfoAllocation::unavailable,&count,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&!count&&zero(info),"missing allocation policy accepted");
  out=dispatch_native_write(s,h,nullptr,3,0,true,false,{},mode,&count,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&!count&&zero(info),"inconsistent append sentinel accepted");
  out=dispatch_native_set_size(s,h,1,true,{},mode,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&zero(info),"unaligned SDK allocation accepted");
  out=dispatch_native_write(s,h,nullptr,0,0,false,false,{1,false,false,false,false},mode,&count,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&!count&&zero(info),"invalid automatic clock accepted");
  out=dispatch_native_write(s,h,nullptr,0,0,false,false,{},mode,nullptr,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&zero(info),"null count output accepted");
  count=77;out=dispatch_native_write(s,h,nullptr,0,0,false,false,{},mode,&count,nullptr);
  require(out.status==STATUS_INVALID_PARAMETER&&!count,"null FileInfo output accepted");
  const std::byte dummy{};
  out=dispatch_native_write(s,h,&dummy,kNativeMaxFileBytes+77,kStoreMaxObjectBytes+1,false,true,{},mode,&count,&info);
  require(out.status==STATUS_INVALID_PARAMETER&&!count&&zero(info),"oversized SDK supplied input admitted");
  require(ram->live==before && ram->calls==calls && ram->read_calls==reads&&!s.poisoned(),"pure bridge rejection touched backing/fenced");
  auto denied=s.open("target",{true,false,false});calls=ram->calls;reads=ram->read_calls;
  auto r=dispatch(s,denied,Op::write,0,data(1,97));
  require(r.status==STATUS_ACCESS_DENIED&&!r.count&&zero(r.info)&&ram->calls==calls&&ram->read_calls==reads&&!s.poisoned(),
      "SDK denied data grant performed preflight I/O/mutation");
  s.cleanup(denied,false);s.close(denied);
  auto provisional=s.prepare_open("target",all,NativeOpenKind::file,{},{});
  r=dispatch(s,provisional,Op::write,0,data(1,97));
  require(r.status==STATUS_INVALID_DEVICE_STATE&&!r.outcome.offline_required&&!r.count&&zero(r.info),"SDK mutated provisional context");
  s.close(provisional);s.cleanup(h,false);s.close(h);
  r=dispatch(s,h,Op::query);require(r.status==STATUS_INVALID_HANDLE&&!r.outcome.offline_required&&zero(r.info),"stale SDK query accepted");
  for(bool basic:{false,true}) {
    auto image_ram=std::make_shared<Ram>();auto owner=fresh(image_ram,StoreCompressionMode::raw,false);seed(owner,basic,!basic);
    auto session=NativeFileSession::create(std::move(owner));auto handle=session.open("target",all);
    const auto image_bytes=image_ram->live;const auto n=image_ram->calls;
    auto failed=dispatch(session,handle,Op::write,0,data(1,98));
    require(failed.status==STATUS_INVALID_DEVICE_STATE&&!failed.outcome.offline_required&&!failed.count&&zero(failed.info)&&
        image_ram->live==image_bytes&&image_ram->calls==n&&!session.poisoned(),"legacy missing output prerequisite mutated/fabricated metadata");
    session.cleanup(handle,false);session.close(handle);
  }
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
void sealed(StoreCompressionMode compression,bool compact,unsigned kind) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{compression,6,false},{compact}));seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto h=s.open("target",all);
  io->seal_at=io->ram->flushes+(kind==6?1:3);
  Result result;
  if(kind==0)result=dispatch(s,h,Op::write,0,data(37,99),false,effects());
  if(kind==1)result=dispatch(s,h,Op::write,UINT64_MAX,data(37,99),false,effects());
  if(kind==2)result=dispatch(s,h,Op::write,6990,data(37,99),true,effects());
  if(kind==3)result=dispatch(s,h,Op::eof,70073,{},false,effects());
  if(kind==4)result=dispatch(s,h,Op::reserve,4096,{},false,effects());
  if(kind==5)result=dispatch(s,h,Op::write,0,data(7001,91),false,effects());
  if(kind==6)result=dispatch(s,h,Op::flush);
  require(io->sealed,"storage denial never activated");
  if(kind==6)require(result.status==STATUS_IO_DEVICE_ERROR&&result.outcome.offline_required&&zero(result.info)&&io->blocked,
      "Flush skipped required post-barrier verification");
  else {
    require(result.status==STATUS_SUCCESS&&!result.outcome.offline_required&&!io->blocked&&s.stats().contexts==1,
        "SDK return used post-commit backing or lost ownership");
    const auto size=kind==1?7038:kind==3?70073:kind==4?4096:7001;
    const auto allocation=kind==3?73728:kind==4?4096:8192;
    exact(result.info,size,allocation,updated());
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));keep(view);
    require(view.allocation_info(2).reserved_bytes==static_cast<std::uint64_t>(allocation)&&view.stat("target").logical_bytes==static_cast<std::uint64_t>(size),
        "sealed SDK output differs from independent stored state");
    auto failed=dispatch(s,h,Op::query);require(failed.status==STATUS_IO_DEVICE_ERROR&&failed.outcome.offline_required&&zero(failed.info),
        "active storage denial did not fence SDK query");
  }
  const auto blocked=io->blocked;
  auto denied=dispatch(s,h,Op::write,0,data(1,100));
  require(denied.status==STATUS_IO_DEVICE_ERROR&&denied.outcome.offline_required&&!denied.count&&zero(denied.info)&&
      io->blocked==blocked&&s.stats().contexts==1,"fenced SDK data call retried/released");
  ++sealed_cases;
}
void advisory(StoreCompressionMode compression,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,compression,compact);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));auto handle=session.open("target",all);
  const auto plain=native_lz4_fixture::source(),encoded=native_lz4_fixture::alternative_block(plain);
  const NativeLz4ExtentCandidate hint{0,encoded};
  auto old=session.snapshot();
  auto result=dispatch(session,handle,Op::write,0,plain,false,effects(),mode,{&hint,1},true);
  require(result.status==STATUS_SUCCESS && result.count==65536 && !result.outcome.offline_required,
      "SDK advised write lost durable count");
  exact(result.info,65536,65536,updated());
  require(session.read(handle,0,65536)==plain && old.read("target",0,10000)==data(7001,91),
      "SDK advice changed CPU-read current/prior bytes");
  auto independent=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(ram));
  require(independent.read("target",0,65536)==plain,"SDK advised write failed independent native recovery");
  auto objects=ObjectStoreSnapshot::open(ram);bool found=false;
  for(const auto& object:objects.list())if(object.logical_bytes==plain.size() && objects.read(object.id)==plain){
    found=true;
    require(object.stored_payload_bytes==(compression==StoreCompressionMode::fast && store_lz4_available()?
        encoded.size():plain.size()),"SDK advice did not reach native record");
  }
  require(found,"SDK advised extent absent");
  auto changed=plain;changed[19]^=std::byte{1};
  result=dispatch(session,handle,Op::write,0,changed,false,{},mode,{&hint,1},true);
  require(result.status==STATUS_SUCCESS && result.count==65536 && session.read(handle,0,65536)==changed,
      "stale SDK advice replaced current bytes");
  const std::array duplicates{hint,hint};
  result=dispatch(session,handle,Op::write,0,plain,false,{},mode,duplicates,true);
  require(result.status==STATUS_SUCCESS && session.read(handle,0,65536)==plain,
      "duplicate SDK advice corrupted CPU fallback");
  const auto patch=data(31,191);
  result=dispatch(session,handle,Op::write,65530,patch,false,{},mode,{&hint,1},true);
  auto expected=plain;expected.resize(65561);std::copy(patch.begin(),patch.end(),expected.begin()+65530);
  require(result.status==STATUS_SUCCESS && result.count==patch.size() && session.read(handle,0,100000)==expected,
      "SDK partial cross-extent advice corrupted merged data");
  exact(result.info,65561,69632,updated());
  const auto calls=ram->calls;
  result=dispatch(session,handle,Op::write,UINT64_MAX,patch,true,effects(),mode,{&hint,1},true);
  require(result.status==STATUS_SUCCESS && !result.count && ram->calls==calls,
      "zero accepted SDK advised write published metadata/data");
  exact(result.info,65561,69632,updated());
  result=dispatch(session,handle,Op::write,UINT64_MAX,patch,false,{},mode,{&hint,1},true);
  expected.insert(expected.end(),patch.begin(),patch.end());
  require(result.status==STATUS_SUCCESS && result.count==patch.size() &&
      session.read(handle,0,100000)==expected,"SDK advised append redirected EOF");
  exact(result.info,65592,69632,updated());
  // Closed, grant-denied and oversized advice must not touch backing.
  auto reader=session.open("target",{true,false,false});
  const auto unchanged=ram->live;const auto reads=ram->read_calls;const auto mutations=ram->calls;
  result=dispatch(session,reader,Op::write,0,plain,false,{},mode,{&hint,1},true);
  require(result.status==STATUS_ACCESS_DENIED && !result.count && zero(result.info) &&
      !result.outcome.offline_required && ram->read_calls==reads && ram->calls==mutations,
      "SDK advised write bypassed grant or read backing");
  session.cleanup(reader,false);session.close(reader);
  const auto before_reads=ram->read_calls,before_calls=ram->calls;
  result=dispatch(session,reader,Op::write,0,plain,false,{},mode,{&hint,1},true);
  require(result.status==STATUS_INVALID_HANDLE && !result.count && zero(result.info) &&
      ram->read_calls==before_reads && ram->calls==before_calls,"closed advised handle touched backing");
  std::vector<NativeLz4ExtentCandidate> too_many(kNativeMaxDirtyExtents+1);
  const Bytes oversized(kStoreMaxObjectBytes+1);
  const NativeLz4ExtentCandidate too_big{0,oversized};
  const Bytes megabyte(kStoreMaxObjectBytes);
  const std::array aggregate{NativeLz4ExtentCandidate{0,megabyte},NativeLz4ExtentCandidate{1,megabyte},
      NativeLz4ExtentCandidate{2,megabyte},NativeLz4ExtentCandidate{3,megabyte},NativeLz4ExtentCandidate{4,megabyte}};
  for(const auto hints:{std::span<const NativeLz4ExtentCandidate>(too_many),
      std::span<const NativeLz4ExtentCandidate>(&too_big,1),std::span<const NativeLz4ExtentCandidate>(aggregate)}){
    result=dispatch(session,handle,Op::write,0,plain,false,{},mode,hints,true);
    require(result.status==STATUS_INSUFFICIENT_RESOURCES && !result.count && zero(result.info) &&
        !result.outcome.offline_required && ram->read_calls==before_reads && ram->calls==before_calls &&
        ram->live==unchanged && !session.poisoned(),"SDK advised budget touched backing/health or returned output");
  }
  session.set_delete(handle,true);session.cleanup(handle,true);
  std::copy(plain.begin(),plain.end(),expected.begin());
  result=dispatch(session,handle,Op::write,0,plain,false,{},mode,{&hint,1},true);
  require(result.status==STATUS_SUCCESS && result.count==65536 &&
      session.read(handle,0,100000)==expected,"late detached SDK advised write lost context/EOF");
  session.close(handle);require(session.stats().contexts==0,"SDK advice retained native context");
  keep(session.snapshot());++advice_cases;
}
void advisory_sealed(StoreCompressionMode compression,bool compact) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{compression,6,false},{compact}));
  seed(writer);auto session=NativeFileSession::create(std::move(writer));auto handle=session.open("target",all);
  const auto source=native_lz4_fixture::source(),encoded=native_lz4_fixture::alternative_block(source);
  const NativeLz4ExtentCandidate hint{0,encoded};io->seal_at=io->ram->flushes+3;
  const auto result=dispatch(session,handle,Op::write,0,source,false,effects(),mode,{&hint,1},true);
  require(io->sealed && !io->blocked && result.status==STATUS_SUCCESS &&
      result.count==65536 && !result.outcome.offline_required && session.stats().contexts==1,
      "advised SDK return queried backing after durable publication");
  exact(result.info,65536,65536,updated());
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  require(recovered.read("target",0,65536)==source,"sealed advised SDK recovery differs");keep(recovered);
  ++advice_sealed_cases;
}
enum class Phase {write,eof,reserve,flush,lz4};
const char* name(Phase p){return p==Phase::write?"write":p==Phase::eof?"eof":p==Phase::reserve?"reserve":p==Phase::lz4?"lz4":"flush";}
struct Case {std::shared_ptr<Ram> ram;NativeFileSession session;NativeFileHandle handle;};
Case setup(StoreCompressionMode compression,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,compression,compact);seed(writer);
  auto s=NativeFileSession::create(std::move(writer));auto h=s.open("target",all);return {ram,std::move(s),h};
}
Result action(Case& c,Phase p) {
  if(p==Phase::lz4){
    const auto source=native_lz4_fixture::source(),encoded=native_lz4_fixture::alternative_block(source);
    const NativeLz4ExtentCandidate hint{1,encoded};
    return dispatch(c.session,c.handle,Op::write,65536,source,false,effects(),mode,{&hint,1},true);
  }
  if(p==Phase::write)return dispatch(c.session,c.handle,Op::write,70000,data(73,101),false,effects());
  if(p==Phase::eof)return dispatch(c.session,c.handle,Op::eof,70073,{},false,effects());
  if(p==Phase::reserve)return dispatch(c.session,c.handle,Op::reserve,4096,{},false,effects());
  return dispatch(c.session,c.handle,Op::flush);
}
void closure(const NativeFilesystemSnapshot& view,Phase p,bool changed) {
  keep(view);auto bytes=data(7001,91);
  if(changed && (p==Phase::write||p==Phase::eof))bytes.resize(70073);
  if(changed && p==Phase::lz4){
    bytes.resize(131072);const auto source=native_lz4_fixture::source();
    std::copy(source.begin(),source.end(),bytes.begin()+65536);
  }
  if(changed && p==Phase::write){const auto extra=data(73,101);std::copy(extra.begin(),extra.end(),bytes.begin()+70000);}
  if(changed && p==Phase::reserve)bytes.resize(4096);
  const auto allocation=changed?(p==Phase::reserve?4096:p==Phase::flush?8192:p==Phase::lz4?131072:73728):8192;
  require(view.stat("target").logical_bytes==bytes.size() &&
      view.read("target",0,static_cast<std::uint32_t>(bytes.size()))==bytes,
      "SDK fault changed complete acknowledged bytes/EOF");
  require(view.allocation_info(2).reserved_bytes==static_cast<std::uint64_t>(allocation) &&
      view.basic_metadata(2)==(changed&&p!=Phase::flush?updated():tuple()) &&
      view.security_descriptor(2)==sec::descriptor(),
      "SDK fault mixed acknowledged reservation/basic/security");
}
void faults(StoreCompressionMode compression,bool compact,Phase p) {
  std::size_t steps=0;
  {auto c=setup(compression,compact);c.ram->calls=0;auto r=action(c,p);
    require(r.status==STATUS_SUCCESS,"positive SDK fault trace failed");steps=c.ram->calls;
    closure(c.session.snapshot(),p,true);
  }
  require((p==Phase::flush?steps==1:steps>3&&steps<100),"SDK observed trace outside limits");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;std::shared_ptr<Ram> ram;std::uint64_t sequence=0;
    {auto c=setup(compression,compact);ram=c.ram;auto old=c.session.snapshot();sequence=old.sequence();
      const auto owned=c.session.stats();ram->calls=0;ram->fail_at=step;ram->failure=fault;
      auto result=action(c,p);
      require(result.status==STATUS_IO_DEVICE_ERROR&&result.outcome.offline_required&&!result.count&&zero(result.info)&&
          c.session.poisoned()&&c.session.stats().contexts==owned.contexts&&c.session.stats().cleaned==owned.cleaned,
          "failed SDK call acknowledged bytes/FileInfo or released context");
      const auto calls=ram->calls,reads=ram->read_calls;
      auto again=action(c,p);
      require(again.status==STATUS_IO_DEVICE_ERROR&&again.outcome.offline_required&&ram->calls==calls&&ram->read_calls==reads,
          "fenced SDK call retried backing");closure(old,p,false);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence||(p!=Phase::flush&&view.sequence()==sequence+1),"SDK recovery lost/fabricated sequence");
    closure(view,p,view.sequence()!=sequence);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{compression,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,102))}};
    writer.apply(continuation);keep(writer.snapshot());
    require(writer.snapshot().read("continued",0,100)==data(17,102),"SDK independent CPU continuation failed");
  }
  std::cout<<"Windows data-bridge faults: {\"schema\":1,\"phase\":\""<<name(p)
      <<"\",\"mode\":\""<<(compression==StoreCompressionMode::raw?"raw":compression==StoreCompressionMode::fast?"fast":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  wchar_t action_value[16]{},environment[32]{};
  if(GetEnvironmentVariableW(L"GITHUB_ACTIONS",action_value,16)!=4||std::wstring_view(action_value)!=L"true"||
      GetEnvironmentVariableW(L"RUNNER_ENVIRONMENT",environment,32)!=13||std::wstring_view(environment)!=L"github-hosted")
    throw std::runtime_error("hosted-only SDK data bridge fixture");
  if(argc!=2)throw std::runtime_error("one SDK data bridge group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    DWORD initial=0,final=0;require(GetProcessHandleCount(GetCurrentProcess(),&initial)!=FALSE,"initial handle accounting unavailable");
    translation();rejection();
    for(const auto compression:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      positive(compression,compact);for(unsigned kind=0;kind<7;++kind)sealed(compression,compact,kind);
    }
    for(const auto compression:{StoreCompressionMode::raw,StoreCompressionMode::fast})for(bool compact:{false,true}){
      advisory(compression,compact);advisory_sealed(compression,compact);
    }
    require(advice_cases==4 && advice_sealed_cases==4,"SDK advised profiles or sealed cases omitted");
    require(GetProcessHandleCount(GetCurrentProcess(),&final)!=FALSE&&initial==final,"SDK data bridge leaked OS handles");
    require(retired==4&&sealed_cases==28,"SDK data bridge lifecycle/sealed cases omitted");
    std::cout<<"Windows data-bridge invariants: {\"schema\":1,\"checks\":"<<checks<<",\"sealed_cases\":"<<sealed_cases
        <<",\"sdk_calls\":"<<sdk_calls<<",\"backend_retired\":"<<retired<<",\"handles_initial\":"<<initial
        <<",\"handles_final\":"<<final<<",\"advice_cases\":"<<advice_cases
        <<",\"advice_sealed_cases\":"<<advice_sealed_cases<<",\"complete\":true,\"mount\":false}\n";
  }else {
    const auto p=group=="--write-faults"?Phase::write:group=="--eof-faults"?Phase::eof:
        group=="--reserve-faults"?Phase::reserve:group=="--flush-faults"?Phase::flush:
        group=="--lz4-faults"?Phase::lz4:
        throw std::runtime_error("unknown SDK bridge group");
    for(const auto compression:std::array{StoreCompressionMode::raw,
        p==Phase::lz4?StoreCompressionMode::fast:StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(compression,compact,p);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Windows data-bridge failure: "<<e.what()<<"\n";return 1;}}
