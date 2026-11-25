#include "volume_controller.hpp"
#include "memory_volume_factory.hpp"
#include "volume_app_winfsp.hpp"
#include "windows_app_budget.hpp"
#include "flitzfs/native_volume_file.hpp"
#include "../src/store_file.hpp"
#include "volume_range_fixture.hpp"
#include "winfsp_native.hpp"
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace flitzfs;
namespace {
void require(bool condition,const char* message) {
  if(!condition)throw std::runtime_error(message);
}

void backing_file_geometry() {
  const auto ordinary_ntfs=detail::volume_file_geometry(L"NTFS",true,4096);
  const auto large=detail::volume_file_geometry(L"NTFS",true,2ULL<<20);
  const auto dense_ntfs=detail::volume_file_geometry(L"NTFS",false,65536);
  require(ordinary_ntfs.maximum_extent==(16ULL<<40)-65536 && ordinary_ntfs.isolated_sparse_ranges,
      "The NTFS 4 KiB layout ignored the per-file limit or range alignment");
  require(large.maximum_extent==(8ULL<<50)-65536 && !large.isolated_sparse_ranges,
      "A large NTFS cluster admitted a smaller isolated range");
  require(dense_ntfs.maximum_extent==(256ULL<<40)-65536 && !dense_ntfs.isolated_sparse_ranges,
      "NTFS file extent and sparse capability were coupled");
  const auto fat32=detail::volume_file_geometry(L"FAT32",false,4096);
  const auto exfat=detail::volume_file_geometry(L"exFAT",false,4096);
  const auto unresolved=detail::volume_file_geometry(L"NTFS",true,0);
  const auto overflow=detail::volume_file_geometry(L"NTFS",true,1ULL<<32);
  require(fat32.maximum_extent==(1ULL<<32)-1 && !fat32.isolated_sparse_ranges,
      "FAT32 admitted an unrepresentable backing extent");
  require(!exfat.isolated_sparse_ranges && exfat.maximum_extent==(1ULL<<63)-1 &&
      !unresolved.isolated_sparse_ranges && unresolved.maximum_extent==exfat.maximum_extent &&
      !overflow.isolated_sparse_ranges && overflow.maximum_extent==exfat.maximum_extent,
      "Unknown, dense or overflowing backing geometry admitted isolated ranges");
}

struct Provider {
  FSP_FILE_SYSTEM filesystem{};
  std::atomic<unsigned> started{0},stopped{0},unmounted{0},deleted{0};
  std::atomic<bool> collision{true},confirm_retirement{true},reject_preflight{false};
  static Provider* current;

  static NTSTATUS preflight(PWSTR,PWSTR path) {
    if(current->reject_preflight)return STATUS_ACCESS_DENIED;
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
    if(current->confirm_retirement)fs->Interface->DispatcherStopped(fs,TRUE);
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
    if(view.stopped || GetTickCount64()>=deadline)
      throw std::runtime_error("Attachment controller did not publish expected state. Last state "+
          std::to_string(static_cast<int>(view.attachment.state))+". "+view.message);
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

void verify_backing(const WinFspVolumeFileSelection& file) {
  const auto* path=std::get_if<WinFspVolumeFileSelection::WindowsFile>(&file.backing);
  const auto verified=path?NativeVolumeFile::inspect(path->path,file.volume):
      NativeVolumeFile::inspect(std::get<std::shared_ptr<StoreByteIo>>(file.backing),file.volume);
  const auto recovered=verified.filesystem().read("retained.txt",0,4096);
  const auto expected=std::as_bytes(std::span(contents.data(),contents.size()));
  require(std::ranges::equal(recovered,expected),"Persistent helper close lost acknowledged bytes");
  require(!verified.filesystem().storage_info().degraded_checkpoint,"Persistent helper left degraded replicas");
}

void damage_detached_replica(const WinFspVolumeFileSelection& file) {
  const auto& backing=std::get<std::shared_ptr<StoreByteIo>>(file.backing);
  std::vector<std::byte> bytes(static_cast<std::size_t>(backing->extent()));
  backing->read_exact(0,bytes);
  const auto baseline=NativeVolumeFile::inspect(backing,file.volume);
  constexpr std::uint64_t inode_domain=1ULL<<63;
  const auto inode_record=inode_domain|baseline.filesystem().stat("retained.txt").inode;
  const auto selected_generation=baseline.filesystem().storage_info().generation;
  constexpr std::string_view magic="FLTXDAT1";
  constexpr std::size_t header_size=128;
  std::uint64_t generation=0;
  std::size_t primary=0,peer=0;
  auto scan=bytes.begin()+kNativeVolumeFileImageOffset;
  while(scan!=bytes.end()) {
    const auto hit=std::search(scan,bytes.end(),magic.begin(),magic.end(),
        [](std::byte a,char b){return a==static_cast<std::byte>(b);});
    if(bytes.end()-hit<static_cast<std::ptrdiff_t>(header_size))break;
    scan=hit+magic.size();
    std::uint64_t object=0,revision=0;
    std::memcpy(&object,&hit[16],sizeof(object));
    std::memcpy(&revision,&hit[24],sizeof(revision));
    if(object!=inode_record || !revision || revision<generation || revision>selected_generation)continue;
    const auto offset=static_cast<std::size_t>(hit-bytes.begin());
    if(revision>generation) {
      generation=revision;
      primary=offset;
      peer=0;
    }else if(offset-primary>=backing->contract().write_failure_unit_bytes &&
        std::equal(hit,hit+header_size,bytes.begin()+primary)) {
      peer=offset;
    }
  }
  require(peer!=0,"Detached fixture did not contain a current inode replica pair");
  // Exercise the exact protected record regardless of the foreground codec.
  const auto damage=bytes[peer]^std::byte{1};
  backing->write_exact(peer,std::span(&damage,1));backing->flush();
  const auto expected=std::as_bytes(std::span(contents.data(),contents.size()));
  const auto verified=NativeVolumeFile::inspect(backing,file.volume);
  require(verified.filesystem().storage_info().degraded_checkpoint &&
      std::ranges::equal(verified.filesystem().read("retained.txt",0,4096),expected),
      "Detached fixture did not establish one verified surviving replica");
}

void file_lifecycle(const WinFspVolumeFileSelection& file,bool repair_on_reopen=false) {
  Provider provider;
  Provider::current=&provider;
  Window window;
  VolumeController controller(window.handle,make_winfsp_volume_file_attachment(Provider::load,file));
  const auto submit=[&](VolumeCommandKind kind){require(controller.submit({kind,{},{}}),"Volume file command refused");};
  const auto wait_state=[&](VolumeAttachmentState expected) {
    return await(controller,[&](const auto& view){return view.idle() && view.attachment.state==expected;});
  };

  submit(VolumeCommandKind::attach);
  const auto failed=wait_state(VolumeAttachmentState::detached);
  require(failed.attachment.persistent && failed.can_exit_without_discard() &&
      failed.attachment.status==STATUS_OBJECT_NAME_COLLISION && provider.deleted==1 && provider.unmounted==0,
      "Mount failure did not retain the persistent backing");
  provider.collision=false;
  submit(VolumeCommandKind::attach);
  const auto first=wait_state(VolumeAttachmentState::attached).attachment.location;
  require(!first.empty() && !controller.stop(false) &&
      !controller.submit({VolumeCommandKind::discard,{},{}}),"Closing a mounted file was admitted");
  make_file(provider);

  submit(VolumeCommandKind::detach);
  require(wait_state(VolumeAttachmentState::detached).can_exit_without_discard(),
      "Detached file required destructive discard before exit");
  if(const auto* path=std::get_if<WinFspVolumeFileSelection::WindowsFile>(&file.backing)) {
    bool shared=false;
    try{(void)NativeVolumeFile::inspect(path->path,file.volume);shared=true;}
    catch(const std::exception&){}
    require(!shared,"Detached helper released its exclusive backing prematurely");
  }
  if(repair_on_reopen)damage_detached_replica(file);
  submit(VolumeCommandKind::attach);
  const auto second=wait_state(VolumeAttachmentState::attached).attachment.location;
  require(!second.empty() && second!=first,"File reattachment reused a mount target");
  verify_file(provider);
  provider.filesystem.Interface->DispatcherStopped(&provider.filesystem,FALSE);
  require(wait_state(VolumeAttachmentState::detached).has_volume(),
      "Unsolicited dispatcher stop discarded the persistent backing");

  submit(VolumeCommandKind::discard);
  const auto closed=wait_state(VolumeAttachmentState::empty);
  require(!closed.has_volume() && closed.message=="Volume file closed.",
      "File close did not release the selection");
  verify_backing(file);

  submit(VolumeCommandKind::attach);
  wait_state(VolumeAttachmentState::attached);
  verify_file(provider);
  submit(VolumeCommandKind::detach);
  wait_state(VolumeAttachmentState::detached);
  require(controller.stop(false),"Detached persistent attachment refused ordinary exit");
  await(controller,[](const auto& view){return view.stopped;});
  verify_backing(file);
  require(provider.started==4 && provider.stopped==4 && provider.unmounted==3 && provider.deleted==4,
      "File attachment provider ownership was unbalanced");
  std::cout<<"file_attachment collision_retained=1 fresh_mount_target=1 cpu_reopen=1 "
      <<"ordinary_exit=1 joined_provider_sessions="<<provider.deleted.load()<<'\n';
  if(repair_on_reopen)std::cout<<"file_attachment_startup_repair detached_peer=1 reopened=1 cpu_verified=1\n";
}

void uncertain_file_retirement() {
  constexpr std::uint64_t capacity=8*1024*1024;
  auto domain=MemoryVolumeDomain::create(capacity);
  auto backing=domain.create_store(capacity);
  const WinFspVolumeFileSelection file{backing,fresh_volume_identity(),
      WinFspVolumeFileSelection::Create{fresh_volume_identity(),{capacity-kNativeVolumeFileImageOffset}}};
  Provider provider;
  Provider::current=&provider;
  provider.collision=false;
  Window window;
  {
    VolumeController controller(window.handle,make_winfsp_volume_file_attachment(Provider::load,file));
    require(controller.submit({VolumeCommandKind::attach,{},{}}),"Cannot start retirement fixture");
    await(controller,[](const auto& view){return view.idle() && view.attachment.state==VolumeAttachmentState::attached;});
    make_file(provider);
    provider.confirm_retirement=false;
    require(controller.submit({VolumeCommandKind::detach,{},{}}),"Cannot request retirement");
    const auto blocked=await(controller,[](const auto& view){
      return view.idle() && view.attachment.state==VolumeAttachmentState::blocked;
    });
    require(blocked.attachment.persistent && blocked.has_volume() && !blocked.can_exit_without_discard() &&
        !controller.stop(false) && !controller.stop(true) &&
        !controller.submit({VolumeCommandKind::attach,{},{}}) &&
        !controller.submit({VolumeCommandKind::discard,{},{}}),
        "Unconfirmed dispatcher retirement allowed ownership to change");
  }
  verify_backing(file);
  require(provider.started==1 && provider.stopped==1 && provider.unmounted==1 && provider.deleted==1,
      "Indeterminate-retirement fixture leaked an owner");
  std::cout<<"file_attachment unconfirmed_retirement_refused=1 acknowledged_bytes_retained=1\n";
}

void selected_file_lifecycle() {
  constexpr std::uint64_t capacity=8*1024*1024;
  auto backing=std::make_shared<native_fixture::SparseVolumeRam>(4096);
  backing->io.capacity=2*capacity-kNativeVolumeFileImageOffset;
  const auto identity=fresh_volume_identity();
  const WinFspVolumeFileSelection::Create creation{fresh_volume_identity(),
      {capacity-kNativeVolumeFileImageOffset}};
  const std::filesystem::path path=L"selected.flfs";
  Provider provider;
  Provider::current=&provider;
  Window window;
  VolumeController controller(window.handle,make_winfsp_memory_attachment(Provider::load),
      [&](const VolumeFileRequest& request) {
        require(request.path==path,"File choice changed before reaching the worker");
        const bool create=request.action==VolumeFileAction::create_new;
        return make_winfsp_volume_file_attachment(Provider::load,
            {backing,create?identity:StoreIdentity{},create?std::optional{creation}:std::nullopt});
      });
  const auto state=[&](VolumeAttachmentState expected) {
    return await(controller,[&](const auto& view){return view.idle() && view.attachment.state==expected;});
  };
  const auto select=[&](VolumeFileAction action) {
    return controller.submit({VolumeCommandKind::attach_file,{},{},VolumeFileRequest{action,path}});
  };
  const auto submit=[&](VolumeCommandKind kind) {
    require(controller.submit({kind,{},{}}),"Selected backing command refused");
  };
  require(state(VolumeAttachmentState::empty).can_select_file(),"Empty app cannot select a file");
  require(!controller.submit({VolumeCommandKind::attach_file,{},{}}),"Missing selection was admitted");
  require(select(VolumeFileAction::open_existing),"Empty file validation was not queued");
  require(state(VolumeAttachmentState::empty).can_select_file() && !backing->extent() && provider.started==0,
      "Invalid file could change backing bytes or take attachment ownership");

  provider.reject_preflight=true;
  require(select(VolumeFileAction::create_new),"New selection could not be queued");
  require(state(VolumeAttachmentState::empty).can_select_file() && !backing->extent() && provider.started==0,
      "Preflight refusal modified the file before the mount was checked");

  provider.reject_preflight=false;
  require(select(VolumeFileAction::create_new),"Selected file creation was not queued");
  const auto failed=state(VolumeAttachmentState::detached);
  require(failed.attachment.persistent && failed.attachment.status==STATUS_OBJECT_NAME_COLLISION &&
      !select(VolumeFileAction::open_existing),"Mount collision lost or replaced the selected file");
  provider.collision=false;
  submit(VolumeCommandKind::attach);
  require(state(VolumeAttachmentState::attached).attachment.persistent,"Retry changed to the memory provider");
  make_file(provider);
  submit(VolumeCommandKind::detach);
  state(VolumeAttachmentState::detached);
  submit(VolumeCommandKind::discard);
  require(state(VolumeAttachmentState::empty).can_select_file(),"Close did not release the file selection");
  verify_backing({backing,identity,std::nullopt});

  // A checked selector cannot grant ownership while the image is invalid.
  auto invalid=backing->replay();
  std::fill_n(invalid->io.live.begin()+kNativeVolumeFileImageOffset,kVolumeHeadBytes,std::byte{});
  auto refused=make_winfsp_volume_file_attachment(Provider::load,{invalid,identity,std::nullopt});
  bool invalid_rejected=false;
  try {refused->attach(nullptr);}
  catch(const NativeFsError&) {invalid_rejected=true;}
  require(invalid_rejected && !refused->retains_volume() && invalid->io.calls==0 && provider.started==2,
      "Invalid selected image was retained or touched");

  // Once the CPU audit succeeds, a failed write fence must still retain the selection.
  auto failed_open=make_winfsp_volume_file_attachment(Provider::load,{backing,{},std::nullopt});
  backing->io.fail_at=backing->io.calls;
  const auto unchanged=backing->io.write_bytes;
  bool fence_rejected=false;
  try {failed_open->attach(nullptr);}
  catch(const std::exception&) {fence_rejected=true;}
  require(fence_rejected && failed_open->retains_volume() && provider.started==2 &&
      backing->io.calls==backing->io.fail_at+1 && backing->io.write_bytes==unchanged &&
      failed_open->refresh().state==VolumeAttachmentState::detached,
      "Failed write fence lost validated volume ownership");
  failed_open->discard();

  constexpr StoreCompressionOptions foreground{StoreCompressionMode::automatic,3,false};
  const auto direct_start=backing->io.read_bytes;
  const auto direct_barrier_start=backing->io.flushes;
  WinFspNativeWritable::resume_file_volume(backing,identity,foreground).reset();
  const auto direct_reads=backing->io.read_bytes-direct_start;
  const auto direct_barriers=backing->io.flushes-direct_barrier_start;
  const auto selected_start=backing->io.read_bytes;
  const auto writes=backing->io.write_bytes;
  const auto barriers=backing->io.flushes;
  provider.collision=true;
  require(select(VolumeFileAction::open_existing),"Existing file could not be selected again");
  const auto reopened=state(VolumeAttachmentState::detached);
  require(reopened.attachment.status==STATUS_OBJECT_NAME_COLLISION && reopened.attachment.persistent,
      "Failed first mount of an existing volume did not retain the selection");
  const auto selected_reads=backing->io.read_bytes-selected_start;
  std::cout<<"selected_file_reopen adapter_read_bytes="<<direct_reads<<" helper_read_bytes="<<selected_reads
      <<" startup_flushes="<<backing->io.flushes-barriers<<" startup_write_bytes="<<backing->io.write_bytes-writes
      <<std::endl;
  require(direct_reads && selected_reads==direct_reads && direct_barriers>=2 &&
      backing->io.flushes==barriers+direct_barriers &&
      backing->io.write_bytes==writes,"Helper reopened with a repeated audit, missing fence, or startup write");

  provider.collision=false;
  submit(VolumeCommandKind::attach);
  require(state(VolumeAttachmentState::attached).attachment.persistent,"Checked image identity did not reopen");
  verify_file(provider);
  submit(VolumeCommandKind::detach);
  state(VolumeAttachmentState::detached);
  submit(VolumeCommandKind::discard);
  state(VolumeAttachmentState::empty);
  verify_backing({backing,identity,std::nullopt});
  submit(VolumeCommandKind::attach);
  require(!state(VolumeAttachmentState::attached).attachment.persistent,
      "Memory attach reopened an explicitly closed file");
  submit(VolumeCommandKind::detach);
  state(VolumeAttachmentState::detached);
  submit(VolumeCommandKind::discard);
  state(VolumeAttachmentState::empty);
  require(controller.stop(false),"Empty application refused to exit");
  await(controller,[](const auto& view){return view.stopped;});
  require(provider.started==5 && provider.stopped==5 && provider.unmounted==3 && provider.deleted==5,
      "Selected attachment leaked a provider session");
  std::cout<<"selected_volume_file validated_open=1 preflight_before_writes=1 "
      <<"cpu_reopen=1 memory_after_close=1 joined_provider_sessions="<<provider.deleted.load()<<'\n';
}

void file_lifecycle_in_memory() {
  {
    constexpr std::uint64_t capacity=8*1024*1024;
    auto domain=MemoryVolumeDomain::create(capacity);
    file_lifecycle({domain.create_store(capacity),fresh_volume_identity(),
        WinFspVolumeFileSelection::Create{fresh_volume_identity(),{capacity-kNativeVolumeFileImageOffset}}},true);
  }
  uncertain_file_retirement();
  selected_file_lifecycle();
  require(MemoryVolumeDomain::cleanup_healthy(),"Volume file memory backing cleanup failed");
}

void changed_volume_is_refused(const std::filesystem::path& path) {
  const auto destination=detail::volume_file_destination(path);
  constexpr std::wstring_view different=L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\";
  require(destination.volume_root!=different,"Invalid fixture volume identity");
  Provider provider;
  Provider::current=&provider;
  provider.collision=false;
  constexpr StoreIoContract contract{65536,true};
  for(const bool create:{true,false}) {
    WinFspVolumeFileSelection selection{
        WinFspVolumeFileSelection::WindowsFile{path,contract,std::wstring(different)},fresh_volume_identity(),{}};
    if(create)selection.create=WinFspVolumeFileSelection::Create{fresh_volume_identity(),{8*1024*1024}};
    auto attachment=make_winfsp_volume_file_attachment(Provider::load,std::move(selection));
    bool refused=false;
    try{attachment->attach(nullptr);}
    catch(const std::runtime_error& e){refused=std::string_view(e.what()).ends_with("selected backing volume has changed");}
    require(refused && !attachment->retains_volume() && !std::filesystem::exists(path) && provider.started==0,
        "A changed backing selection reached file open or dispatch");
  }
  std::cout<<"disposable_volume_binding stale_create_refused=1 stale_reopen_refused=1 no_backing_created=1\n";
}

void disposable_file_lifecycle(const std::filesystem::path& base) {
  const auto ticks=std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory=std::filesystem::canonical(base)/("flitzfs-volume-attachment-"+std::to_string(ticks));
  require(std::filesystem::create_directory(directory),"Cannot exclusively create the volume file attachment fixture");
  changed_volume_is_refused(directory/"changed.flfs");
  const auto outside=directory/"outside-extent.flfs";
  const auto geometry=detail::volume_file_destination(outside);
  const auto maximum=geometry.geometry.maximum_extent;
  if(maximum<static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
    bool refused=false;
    try {
      (void)detail::new_volume_file(outside,{65536,true},maximum+1,geometry.volume_root);
    } catch(const std::runtime_error& failure) {
      refused=std::string_view(failure.what()).ends_with("the backing filesystem's maximum file size");
    }
    require(refused && !std::filesystem::exists(outside),
        "An unrepresentable volume reached exclusive creation");
    std::cout<<"disposable_file_extent maximum_bytes="<<maximum<<" refused_before_create=1\n";
  }
  const auto path=directory/"attached.flfs";
  file_lifecycle({WinFspVolumeFileSelection::WindowsFile{path,{65536,true,true}},
      fresh_volume_identity(),WinFspVolumeFileSelection::Create{fresh_volume_identity(),{8*1024*1024}}});
#ifdef FLITZFS_EXPLORER_FILE_BACKING
  const auto selected_path=directory/"selected.flfs";
  Provider provider;
  Provider::current=&provider;
  provider.collision=false;
  Window window;
  VolumeController controller(window.handle,make_winfsp_memory_attachment(Provider::load),
      [](const auto& request){return make_volume_app_file_attachment(request,Provider::load);});
  const auto state=[&](VolumeAttachmentState expected) {
    return await(controller,[&](const auto& view){return view.idle() && view.attachment.state==expected;});
  };
  const auto select=[&](VolumeFileAction action,const std::filesystem::path& selected) {
    require(controller.submit({VolumeCommandKind::attach_file,{},{},VolumeFileRequest{action,selected}}),
        "Cannot queue the disposable file selection");
  };
  const auto release=[&]{
    require(controller.submit({VolumeCommandKind::detach,{},{}}),"Cannot detach the selected file");
    state(VolumeAttachmentState::detached);
    require(controller.submit({VolumeCommandKind::discard,{},{}}),"Cannot close the selected file");
    state(VolumeAttachmentState::empty);
  };
  select(VolumeFileAction::create_new,selected_path);
  require(state(VolumeAttachmentState::attached).attachment.backing_path==selected_path.native(),
      "Selected backing label differs");
  make_file(provider);
  release();
  const auto packed_attributes=GetFileAttributesW(selected_path.c_str());
  const auto sparse_attributes=GetFileAttributesW(path.c_str());
  require(packed_attributes!=INVALID_FILE_ATTRIBUTES && !(packed_attributes&FILE_ATTRIBUTE_SPARSE_FILE) &&
      sparse_attributes!=INVALID_FILE_ATTRIBUTES && (sparse_attributes&FILE_ATTRIBUTE_SPARSE_FILE),
      "The packed and sparse helper controls did not produce distinct physical profiles");
  const auto selected=NativeVolumeFile::inspect(selected_path).info().selection;
  require(selected.placement!=NativeVolumePlacement::external,
      "The selected ordinary file lost the paired layout");
  select(VolumeFileAction::create_new,selected_path);
  require(state(VolumeAttachmentState::empty).can_select_file() && provider.started==1,
      "Exclusive creation consumed an existing volume");
  select(VolumeFileAction::open_existing,selected_path);
  state(VolumeAttachmentState::attached);
  verify_file(provider);
  release();
  select(VolumeFileAction::open_existing,path);
  state(VolumeAttachmentState::attached);
  verify_file(provider);
  release();
  require(controller.stop(false),"Closed persistent controller refused to exit");
  await(controller,[](const auto& view){return view.stopped;});
  verify_backing({WinFspVolumeFileSelection::WindowsFile{selected_path,{65536,true,false,true}},
      selected.volume_identity,std::nullopt});
  require(provider.started==3 && provider.deleted==3,"Selected file provider did not retire");
  std::cout<<"disposable_file_selection exclusive_create=1 existing_bytes_retained=1 cpu_reopen=1"
      <<" packed_create=1 packed_reopen=1 sparse_reopen=1\n";
#endif
}
}

int wmain(int argc,wchar_t** argv) {
  try {
    app_fixture::Budget budget;
    require(!GetModuleHandleW(L"winfsp-x64.dll"),"WinFsp SDK loaded before explicit runtime binding");
    require(load_volume_app_winfsp().complete() && GetModuleHandleW(L"winfsp-x64.dll"),
        "App loader did not bind its verified WinFsp SDK");
    std::cout<<"attachment_sdk_binding delay_loaded=1 verified_exports=1\n";
    if(argc==1) {
      backing_file_geometry();
      lifecycle();
    }
    else if(argc==2 && std::wstring_view(argv[1])==L"--memory-volume-file")file_lifecycle_in_memory();
    else if(argc==3 && std::wstring_view(argv[1])==L"--disposable-volume-file")
      disposable_file_lifecycle(std::filesystem::path(argv[2]));
    else throw std::invalid_argument("Invalid attachment fixture mode");
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';
    return 1;
  }
}
