#include "winfsp_native.hpp"
#include "native_windows_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
namespace {
using namespace flitzfs;namespace nw=native_windows_fixture;
unsigned checks=0,dispatches=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Harness {
  nw::Fixture fixture;
  std::unique_ptr<WinFspNativeWritable> adapter;
  FSP_FILE_SYSTEM fs{};
  std::vector<std::byte> security;
  Harness(StoreCompressionMode mode):adapter(WinFspNativeWritable::create_new_hosted(
      fixture.image(),nw::identity(),{mode,6,false})) {
    fs.Version=static_cast<UINT16>(sizeof(fs));fs.UmFileContextIsUserContext2=TRUE;
    fs.Interface=&WinFspNativeWritable::callbacks();adapter->bind_callbacks(fs);
    SIZE_T size=0;UINT32 attributes=0;
    require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,nullptr,&size)==
        STATUS_BUFFER_OVERFLOW&&size>0&&size<=kNativeMaxSecurityBytes,"root security size negotiation");
    security.resize(size);
    require(fs.Interface->GetSecurityByName(&fs,const_cast<PWSTR>(L"\\"),&attributes,security.data(),&size)==
        STATUS_SUCCESS&&attributes==FILE_ATTRIBUTE_DIRECTORY&&IsValidSecurityDescriptor(security.data()),
        "root owner descriptor not published");
  }
  const FSP_FILE_SYSTEM_INTERFACE& cb() const{return *fs.Interface;}
  PVOID make(PCWSTR path,bool directory=false,UINT32 options=0,UINT64 allocation=0) {
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};++dispatches;
    const auto status=cb().Create(&fs,const_cast<PWSTR>(path),options|(directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE),
        FILE_ALL_ACCESS,directory?FILE_ATTRIBUTE_DIRECTORY:FILE_ATTRIBUTE_NORMAL,security.data(),allocation,&context,&info);
    require(status==STATUS_SUCCESS&&context&&info.IndexNumber>1&&info.CreationTime>0&&
        bool(info.FileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory,"native callback Create");
    return context;
  }
  PVOID open(PCWSTR path,UINT32 options=0,UINT32 access=FILE_ALL_ACCESS) {
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};++dispatches;
    require(cb().Open(&fs,const_cast<PWSTR>(path),(options&0xff000000U)?options:(options|(1U<<24)),access,&context,&info)==STATUS_SUCCESS&&context,
        "native callback Open");return context;
  }
  void finish(PVOID context,ULONG flags=0) {
    FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
    ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
    request.Kind=FspFsctlTransactCleanupKind;
    request.Req.Cleanup.UserContext2=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(context));
    request.Req.Cleanup.Delete=(flags&FspCleanupDelete)!=0;
    request.Req.Cleanup.SetAllocationSize=(flags&FspCleanupSetAllocationSize)!=0;
    ++dispatches;require(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS,"actual SDK Cleanup");
    ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));request.Kind=FspFsctlTransactCloseKind;
    request.Req.Close.UserContext2=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(context));
    ++dispatches;require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS,"actual SDK Close");
    require(!adapter->requires_offline(),"normal cleanup/close fenced adapter");
  }
  FSP_FSCTL_FILE_INFO info(PVOID context) {
    FSP_FSCTL_FILE_INFO result{};++dispatches;
    require(cb().GetFileInfo(&fs,context,&result)==STATUS_SUCCESS,"native callback GetFileInfo");return result;
  }
  void write(PVOID context,const std::vector<std::byte>& bytes,UINT64 offset=0) {
    FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
    ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
    request.Kind=FspFsctlTransactWriteKind;
    request.Req.Write.UserContext2=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(context));
    request.Req.Write.Address=reinterpret_cast<UINT64>(bytes.data());
    request.Req.Write.Offset=offset;request.Req.Write.Length=static_cast<ULONG>(bytes.size());++dispatches;
    require(FspFileSystemOpWrite(&fs,&request,&response)==STATUS_SUCCESS&&
        response.IoStatus.Information==bytes.size()&&response.Rsp.Write.FileInfo.FileSize>=offset+bytes.size(),
        "actual SDK Write lost accepted count/state");
  }
  void read(PVOID context,const std::vector<std::byte>& expected) {
    std::vector<std::byte> buffer(expected.size()+1,std::byte{0x76});
    FSP_FSCTL_TRANSACT_REQ request;FSP_FSCTL_TRANSACT_RSP response;
    ZeroMemory(&request,sizeof(request));ZeroMemory(&response,sizeof(response));
    request.Kind=FspFsctlTransactReadKind;
    request.Req.Read.UserContext2=static_cast<UINT64>(reinterpret_cast<std::uintptr_t>(context));
    request.Req.Read.Address=reinterpret_cast<UINT64>(buffer.data());
    request.Req.Read.Length=static_cast<ULONG>(expected.size());++dispatches;
    require(FspFileSystemOpRead(&fs,&request,&response)==STATUS_SUCCESS&&
        response.IoStatus.Information==expected.size()&&std::equal(expected.begin(),expected.end(),buffer.begin())&&
        buffer.back()==std::byte{0x76},"actual SDK Read bytes/bounds");
  }
  bool missing(std::string_view path) {
    try{(void)adapter->snapshot().stat(path);}catch(const NativeFsError& e){return e.code()==NativeFsErrorCode::not_found;}
    return false;
  }
  void close_owner() {
    require(!adapter->stats().contexts&&!adapter->stats().fenced,"healthy adapter left contexts");
    fs.UserContext=nullptr;adapter.reset();
    // A successful independent open proves the exclusive writable OS handle
    // was released. Full CPU verification runs after adapter ownership ends.
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(fixture.image()));view.verify_all();
    require(!view.needs_session_recovery(),"retired adapter left session-recovery work");
  }
};
void contents(StoreCompressionMode mode) {
  Harness h(mode);auto& cb=h.cb();auto* fs=&h.fs;
  const auto p=h.adapter->parameters();
  require(!p.ReadOnlyVolume&&p.UmFileContextIsUserContext2&&p.PersistentAcls&&p.FlushAndPurgeOnCleanup&&
      !p.FileInfoTimeout&&!p.PostDispositionWhenNecessaryOnly&&!p.PostCleanupWhenModifiedOnly&&
      !p.SupportsPosixUnlinkRename&&p.IrpCapacity==100,"writable protocol/cache parameters");
  FSP_FSCTL_VOLUME_INFO volume{};
  require(cb.GetVolumeInfo(fs,&volume)==STATUS_SUCCESS&&volume.TotalSize==kNativeAllocationCapacity&&
      volume.FreeSize==volume.TotalSize,"native quota initial accounting");
  auto dir=h.make(L"\\Data",true);h.finish(dir);
  auto file=h.make(L"\\Data\\sample.bin",false,0,8192);const auto inode=h.info(file).IndexNumber;
  const auto original=nw::data();h.write(file,original);h.read(file,original);
  FSP_FSCTL_FILE_INFO info{};
  require(cb.Flush(fs,file,&info)==STATUS_SUCCESS&&info.IndexNumber==inode&&info.FileSize==original.size(),
      "real callback Flush changed data result");
  require(cb.SetFileSize(fs,file,7001,FALSE,&info)==STATUS_SUCCESS&&info.FileSize==7001,"EOF shrink");
  auto short_data=std::vector<std::byte>(original.begin(),original.begin()+7001);h.read(file,short_data);
  require(cb.SetFileSize(fs,file,4096,TRUE,&info)==STATUS_SUCCESS&&info.FileSize==4096&&info.AllocationSize==4096,
      "reservation shrink did not atomically truncate EOF");
  short_data.resize(4096);h.read(file,short_data);
  const auto stamp=info.CreationTime+123;
  require(cb.SetBasicInfo(fs,file,FILE_ATTRIBUTE_HIDDEN,stamp,stamp+1,stamp+2,stamp+3,&info)==STATUS_SUCCESS&&
      info.FileAttributes==FILE_ATTRIBUTE_HIDDEN&&info.CreationTime==stamp&&info.ChangeTime==stamp+3,
      "native basic tuple callback");
  require(cb.SetBasicInfo(fs,file,INVALID_FILE_ATTRIBUTES,0,0,0,0,&info)==STATUS_SUCCESS&&
      info.CreationTime==stamp&&info.ChangeTime==stamp+3,"basic unchanged sentinels");
  SIZE_T security_size=0;
  require(cb.GetSecurity(fs,file,nullptr,&security_size)==STATUS_BUFFER_OVERFLOW&&security_size==h.security.size(),
      "owned descriptor size");
  std::vector<std::byte> security(security_size);
  require(cb.GetSecurity(fs,file,security.data(),&security_size)==STATUS_SUCCESS&&security==h.security,"owned descriptor bytes");
  require(cb.SetSecurity(fs,file,DACL_SECURITY_INFORMATION,security.data())==STATUS_INVALID_DEVICE_REQUEST&&
      cb.SetVolumeLabel(fs,const_cast<PWSTR>(L"other"),&volume)==STATUS_INVALID_DEVICE_REQUEST&&
      !h.adapter->requires_offline(),"unimplemented operations must not pretend success/fence healthy owner");
  require(cb.Rename(fs,file,const_cast<PWSTR>(L"\\Data\\sample.bin"),const_cast<PWSTR>(L"\\Data\\renamed.bin"),FALSE)==
      STATUS_SUCCESS&&h.info(file).IndexNumber==inode&&h.missing("Data/sample.bin"),"stable-handle rename");
  h.finish(file);
  auto reader=h.open(L"\\Data\\renamed.bin",0,FILE_READ_DATA);
  security_size=0;
  require(cb.GetSecurity(fs,reader,nullptr,&security_size)==STATUS_ACCESS_DENIED,"read data silently granted security read");
  h.read(reader,short_data);h.finish(reader);
  auto directory=h.open(L"\\Data",FILE_DIRECTORY_FILE);
  alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte,4096> packed{};ULONG count=0;
  require(cb.ReadDirectory(fs,directory,nullptr,nullptr,packed.data(),static_cast<ULONG>(packed.size()),&count)==
      STATUS_SUCCESS&&count>0&&count<=packed.size(),"directory packing");
  std::vector<std::wstring> names;
  for(std::size_t offset=0;offset+sizeof(UINT16)<=count;) {
    const auto* entry=reinterpret_cast<const FSP_FSCTL_DIR_INFO*>(packed.data()+offset);
    if(!entry->Size)break;
    require(entry->Size>=sizeof(FSP_FSCTL_DIR_INFO)&&offset+entry->Size<=count,"directory record bounds");
    const auto chars=(entry->Size-sizeof(FSP_FSCTL_DIR_INFO))/sizeof(wchar_t);
    names.emplace_back(entry->FileNameBuf,chars);offset+=(entry->Size+7U)&~std::size_t(7);
  }
  require(names==std::vector<std::wstring>{L".",L"..",L"renamed.bin"},"directory continuation ordering");
  count=0;
  require(cb.ReadDirectory(fs,directory,nullptr,const_cast<PWSTR>(L".."),packed.data(),static_cast<ULONG>(packed.size()),&count)==
      STATUS_SUCCESS&&count>sizeof(UINT16),"directory marker");
  h.finish(directory);
  require(cb.GetVolumeInfo(fs,&volume)==STATUS_SUCCESS&&volume.FreeSize<volume.TotalSize,"native quota not charged");
  h.close_owner();
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(h.fixture.image()));
  const auto recovered_basic=view.basic_metadata(inode);
  require(view.read("Data/renamed.bin",0,5000)==short_data&&view.stat("Data/renamed.bin").inode==inode&&
      recovered_basic&&recovered_basic->creation_time==stamp,"independent CPU readback after writer retirement");
}
void disposition(StoreCompressionMode mode) {
  Harness h(mode);auto& cb=h.cb();auto* fs=&h.fs;
  auto initial=h.make(L"\\target");h.write(initial,nw::tail());h.finish(initial);
  auto a=h.open(L"\\target"),b=h.open(L"\\target");
  require(cb.SetDelete(fs,a,nullptr,TRUE)==STATUS_SHARING_VIOLATION&&
      !h.adapter->snapshot().inode_state(2).delete_pending&&!h.adapter->stats().delete_requests&&
      h.adapter->stats().contexts==2&&!h.adapter->requires_offline(),
      "unconfirmed peer deletion must reject without changing durable intent/ownership");
  // The engine deliberately does not infer the second Open's adoption from
  // the first handle's SetDelete. Exercise real SDK I/O on BOTH handles, then
  // retain the complete original Windows-global cancellation oracle below.
  h.read(a,nw::tail());h.read(b,nw::tail());
  require(cb.SetDelete(fs,a,nullptr,TRUE)==STATUS_SUCCESS&&cb.SetDelete(fs,b,nullptr,TRUE)==STATUS_SUCCESS&&
      cb.SetDelete(fs,a,nullptr,FALSE)==STATUS_SUCCESS&&!h.adapter->snapshot().inode_state(2).delete_pending&&
      !h.adapter->stats().delete_requests,"Windows cancellation must clear global pending state");
  h.finish(a);h.finish(b);require(!h.missing("target"),"global cancellation removed file");
  // Cancelled prepare/initial option has no Cleanup, no deletion intent.
  a=h.open(L"\\target",FILE_DELETE_ON_CLOSE);cb.Close(fs,a);
  require(!h.adapter->requires_offline()&&!h.adapter->stats().contexts&&!h.missing("target"),"cancelled initial Open changed inode");
  a=h.open(L"\\target",FILE_DELETE_ON_CLOSE);h.finish(a,FspCleanupDelete);
  require(h.missing("target"),"adopted initial Cleanup did not delete");
  auto fresh=h.make(L"\\replace");h.write(fresh,nw::tail());h.finish(fresh);
  a=h.open(L"\\replace",4U<<24);cb.Close(fs,a);
  require(h.adapter->snapshot().read("replace",0,100)==nw::tail(),"cancelled overwrite preparation changed content");
  a=h.open(L"\\replace",4U<<24);
  FSP_FSCTL_FILE_INFO info{};
  require(cb.Overwrite(fs,a,FILE_ATTRIBUTE_ARCHIVE,FALSE,4096,&info)==STATUS_SUCCESS&&
      info.FileSize==0&&info.AllocationSize==4096,"actual Open/Overwrite two-stage storage result");
  h.write(a,nw::tail());h.finish(a);
  const auto stale=a;auto next=h.open(L"\\replace");
  require(next!=stale&&cb.GetFileInfo(fs,stale,&info)==STATUS_INVALID_HANDLE&&!h.adapter->requires_offline(),
      "stale adapter token rebound/poisoned healthy session");h.finish(next);
  h.close_owner();
}
void capacity_and_threads() {
  Harness h(StoreCompressionMode::automatic);auto initial=h.make(L"\\read");h.write(initial,nw::tail());h.finish(initial);
  std::array<PVOID,NativeFileSession::max_contexts> handles{};
  for(auto& handle:handles)handle=h.open(L"\\read");
  PVOID denied=reinterpret_cast<PVOID>(1);FSP_FSCTL_FILE_INFO info{};std::memset(&info,0x55,sizeof(info));
  require(h.cb().Create(&h.fs,const_cast<PWSTR>(L"\\overflow"),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,
      FILE_ATTRIBUTE_NORMAL,h.security.data(),0,&denied,&info)==STATUS_INSUFFICIENT_RESOURCES&&
      !denied&&!info.IndexNumber&&h.missing("overflow")&&!h.adapter->requires_offline(),
      "context-capacity failure mutated namespace/exposed output");
  std::atomic<bool> okay{true};
  auto read=[&](PVOID context) {
    for(unsigned i=0;i<4;++i) {
      std::array<std::byte,17> bytes{};ULONG count=0;
      if(h.cb().Read(&h.fs,context,bytes.data(),0,17,&count)!=STATUS_SUCCESS||count!=17||
          !std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==std::byte{0x5b};}))okay.store(false);
    }
  };
  {std::jthread a(read,handles[0]),b(read,handles[1]);a.join();b.join();}
  require(okay.load(),"bounded concurrent callbacks lost bytes");
  for(auto handle:handles)h.finish(handle);
  require(!h.adapter->stats().contexts,"bounded callback table not retired");h.close_owner();
}
void void_failure() {
  Harness h(StoreCompressionMode::raw);auto context=h.make(L"\\retained");
  h.cb().Cleanup(&h.fs,context,nullptr,0x80000000U);
  require(h.adapter->requires_offline()&&h.adapter->stats().fenced&&h.adapter->stats().contexts==1,
      "unsupported VOID Cleanup must retain context and request offline");
  h.cb().Close(&h.fs,context);
  FSP_FSCTL_FILE_INFO info{};ULONG count=99;std::array<std::byte,17> bytes{};
  require(h.adapter->stats().contexts==1&&h.cb().Write(&h.fs,context,bytes.data(),0,17,FALSE,FALSE,&count,&info)==
      STATUS_IO_DEVICE_ERROR&&!count&&!info.IndexNumber,"fenced callback retried/released/exposed accepted output");
  h.fs.UserContext=nullptr;h.adapter.reset();
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(h.fixture.image()));view.verify_all();
  require(view.stat("retained").logical_bytes==0,"fenced owner destruction lost acknowledged creation");
}
}
int main(){try {
  nw::hosted();
  // Warm up identical operations before counting covered OS HANDLE resources.
  {Harness warm(StoreCompressionMode::raw);warm.close_owner();}
  {std::jthread warm_thread([]{});warm_thread.join();}
  DWORD before=0,after=0;require(GetProcessHandleCount(GetCurrentProcess(),&before)!=FALSE,"handle baseline");
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}){contents(mode);disposition(mode);}
  capacity_and_threads();void_failure();
  require(GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE&&before==after,"adapter OS handles leaked");
  std::cout<<"Windows writable-adapter evidence: {\"schema\":1,\"checks\":"<<checks<<",\"dispatches\":"<<dispatches
      <<",\"handle_before\":"<<before<<",\"handle_after\":"<<after
      <<",\"mounted\":false,\"gpu\":false,\"complete\":true}\n";return 0;
}catch(const std::exception& e){std::cerr<<"Writable adapter failure: "<<e.what()<<"\n";return 1;}}
