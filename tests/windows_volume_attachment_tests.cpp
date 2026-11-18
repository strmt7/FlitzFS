#include "volume_controller.hpp"
#include "volume_app_winfsp.hpp"
#include "windows_app_budget.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace flitzfs;
namespace {
void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

struct Provider {
  FSP_FILE_SYSTEM filesystem{};
  std::atomic<unsigned> started{0},stopped{0},unmounted{0},deleted{0};
  std::atomic<bool> collision{true};
  static Provider* current;

  static NTSTATUS preflight(PWSTR,PWSTR path) {
    if(!valid_mount_directory(path))return STATUS_INVALID_PARAMETER;
    return GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_FILE_NOT_FOUND?
        STATUS_SUCCESS:STATUS_OBJECT_NAME_COLLISION;
  }
  static NTSTATUS create(PWSTR,const FSP_FSCTL_VOLUME_PARAMS* params,
      const FSP_FILE_SYSTEM_INTERFACE* table,FSP_FILE_SYSTEM** output) {
    current->filesystem={};
    current->filesystem.Version=static_cast<UINT16>(sizeof(FSP_FILE_SYSTEM));
    current->filesystem.UmFileContextIsUserContext2=params->UmFileContextIsUserContext2;
    current->filesystem.Interface=table;
    *output=&current->filesystem;
    return STATUS_SUCCESS;
  }
  static NTSTATUS start(FSP_FILE_SYSTEM*,ULONG) {++current->started;return STATUS_SUCCESS;}
  static NTSTATUS mount(FSP_FILE_SYSTEM*,PWSTR) {
    return current->collision?STATUS_OBJECT_NAME_COLLISION:STATUS_SUCCESS;
  }
  static VOID stop(FSP_FILE_SYSTEM* fs) {
    ++current->stopped;
    fs->Interface->DispatcherStopped(fs,TRUE);
  }
  static VOID unmount(FSP_FILE_SYSTEM*) {++current->unmounted;}
  static VOID destroy(FSP_FILE_SYSTEM* fs) {++current->deleted;fs->UserContext=nullptr;}
  static UINT32 origin() {return 0;}
  static WinFspSessionApi load() {
    return {preflight,create,start,mount,stop,unmount,destroy,origin};
  }
};
Provider* Provider::current=nullptr;

struct Window {
  HWND handle=CreateWindowExW(0,L"STATIC",L"FlitzFS attachment test",0,0,0,0,0,HWND_MESSAGE,
      nullptr,GetModuleHandleW(nullptr),nullptr);
  Window(){require(handle!=nullptr,"Cannot create test message window");}
  ~Window(){DestroyWindow(handle);}
};

void close_file(FSP_FILE_SYSTEM& fs,PVOID context) {
  FSP_FSCTL_TRANSACT_REQ request;
  FSP_FSCTL_TRANSACT_RSP response;
  std::memset(&request,0,sizeof(request));
  std::memset(&response,0,sizeof(response));
  request.Kind=FspFsctlTransactCleanupKind;
  request.Req.Cleanup.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpCleanup(&fs,&request,&response)==STATUS_SUCCESS,"Attachment Cleanup");
  request.Kind=FspFsctlTransactCloseKind;
  request.Req.Close.UserContext2=reinterpret_cast<UINT64>(context);
  require(FspFileSystemOpClose(&fs,&request,&response)==STATUS_SUCCESS,"Attachment Close");
}

constexpr std::wstring_view test_name=L"\\retained.txt";
constexpr std::string_view contents="Acknowledged attachment bytes";
void make_file(Provider& provider) {
  auto& fs=provider.filesystem;
  const auto* callbacks=fs.Interface;
  wchar_t root[]=L"\\";
  auto name=std::wstring(test_name);
  SIZE_T size=0;
  UINT32 attributes=0;
  require(callbacks->GetSecurityByName(&fs,root,&attributes,nullptr,&size)==STATUS_BUFFER_OVERFLOW &&
      size && size<65536,"Attachment root ACL size");
  std::vector<std::byte> security(size);
  require(callbacks->GetSecurityByName(&fs,root,&attributes,security.data(),&size)==STATUS_SUCCESS,
      "Attachment root ACL");
  FSP_FSCTL_FILE_INFO metadata{};
  PVOID context=nullptr;
  require(callbacks->Create(&fs,name.data(),FILE_NON_DIRECTORY_FILE,FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,
      security.data(),0,&context,&metadata)==STATUS_SUCCESS && context,"Attachment Create");
  auto data=std::string(contents);
  ULONG written=0;
  require(callbacks->Write(&fs,context,data.data(),0,static_cast<ULONG>(data.size()),FALSE,FALSE,&written,
      &metadata)==STATUS_SUCCESS && written==data.size(),"Attachment Write");
  require(callbacks->Flush(&fs,context,&metadata)==STATUS_SUCCESS,"Attachment Flush");
  close_file(fs,context);
}

void verify_file(Provider& provider) {
  auto& fs=provider.filesystem;
  PVOID context=nullptr;
  auto name=std::wstring(test_name);
  FSP_FSCTL_FILE_INFO metadata{};
  constexpr auto open=FILE_NON_DIRECTORY_FILE|(1U<<24);
  require(fs.Interface->Open(&fs,name.data(),open,FILE_READ_DATA,&context,&metadata)==STATUS_SUCCESS &&
      context && metadata.FileSize==contents.size(),"Remounted attachment file metadata differs");
  std::string read(contents.size(),'\0');
  ULONG count=0;
  require(fs.Interface->Read(&fs,context,read.data(),0,static_cast<ULONG>(read.size()),&count)==STATUS_SUCCESS &&
      count==read.size() && read==contents,"Remounted attachment file bytes differ");
  close_file(fs,context);
}

template<class Predicate>
VolumeControllerView await(VolumeController& controller,Predicate predicate) {
  const auto deadline=GetTickCount64()+7000;
  for(;;) {
    const auto view=controller.view();
    if(predicate(view))return view;
    require(!view.stopped && GetTickCount64()<deadline,"Attachment controller did not publish expected state");
    Sleep(5);
  }
}

void lifecycle() {
  Provider provider;
  Provider::current=&provider;
  Window window;
  VolumeController controller(window.handle,make_winfsp_memory_attachment(Provider::load));
  const auto submit=[&](VolumeCommandKind kind){require(controller.submit({kind,{},{}}),"Attachment command refused");};
  const auto wait_state=[&](VolumeAttachmentState expected) {
    return await(controller,[&](const auto& view){return view.idle() && view.attachment.state==expected;});
  };

  submit(VolumeCommandKind::create);
  await(controller,[](const auto& view){return view.idle() && view.volume.ready;});
  require(!controller.submit({VolumeCommandKind::attach,{},{}}) && provider.started==0,
      "Preview allowed a second memory owner");
  submit(VolumeCommandKind::discard);
  await(controller,[](const auto& view){return view.idle() && !view.has_volume();});

  submit(VolumeCommandKind::attach);
  const auto failed=wait_state(VolumeAttachmentState::detached);
  require(failed.has_volume() && failed.attachment.status==STATUS_OBJECT_NAME_COLLISION &&
      provider.started==1 && provider.stopped==1 && provider.deleted==1 && provider.unmounted==0,
      "Collision failed to retain the memory owner");
  provider.collision=false;
  submit(VolumeCommandKind::attach);
  const auto first=wait_state(VolumeAttachmentState::attached).attachment.location;
  require(!first.empty() && !controller.stop(false) &&
      !controller.submit({VolumeCommandKind::discard,{},{}}),"Attached memory could be dropped implicitly");
  make_file(provider);

  submit(VolumeCommandKind::detach);
  require(wait_state(VolumeAttachmentState::detached).has_volume() && !controller.stop(false),
      "Detach failed to retain memory or protect exit");
  submit(VolumeCommandKind::attach);
  const auto next=wait_state(VolumeAttachmentState::attached).attachment.location;
  require(!next.empty() && first!=next,"Reattachment reused the former mount target");
  verify_file(provider);
  provider.filesystem.Interface->DispatcherStopped(&provider.filesystem,FALSE);
  wait_state(VolumeAttachmentState::detached);

  submit(VolumeCommandKind::discard);
  require(!wait_state(VolumeAttachmentState::empty).has_volume(),"Explicit discard left the attachment active");
  require(controller.stop(false),"Empty application could not exit");
  await(controller,[](const auto& view){return view.stopped;});
  require(provider.started==3 && provider.stopped==3 && provider.unmounted==2 && provider.deleted==3,
      "WinFsp operation ownership was unbalanced");
  require(MemoryVolumeDomain::cleanup_healthy(),"Attachment backing cleanup failed");
  std::cout<<"attachment_lifecycle collision_retained=1 fresh_targets=2 cpu_readback=1 "
      <<"spontaneous_stop_observed=1 joined_provider_sessions="<<provider.deleted.load()<<'\n';
}
}

int main() {
  try {
    app_fixture::Budget budget;
    require(!GetModuleHandleW(L"winfsp-x64.dll"),"WinFsp SDK loaded before explicit runtime binding");
    require(load_volume_app_winfsp().complete() && GetModuleHandleW(L"winfsp-x64.dll"),
        "App loader did not bind its verified WinFsp SDK");
    std::cout<<"attachment_sdk_binding delay_loaded=1 verified_exports=1\n";
    lifecycle();
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
