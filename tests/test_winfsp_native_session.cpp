#include "winfsp_session.hpp"
#include "winfsp_native.hpp"
#include "native_windows_fixture.hpp"
#include "native_fs_fixture.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <array>
#include <algorithm>
#include <iostream>
namespace {
namespace nw=native_windows_fixture;
unsigned checks=0;
void require(bool okay,const char* why){++checks;if(!okay)throw std::runtime_error(why);}
enum class Failure { none, preflight, create, create_partial, create_null_success, start, mount,
                     stop_during_start, stop_during_mount, stop_without_notification };
struct Provider {
  FSP_FILE_SYSTEM fs{};
  Failure failure = Failure::none;
  std::array<char, 32> calls{};
  std::size_t count = 0;
  bool invariant = true;
  bool callback_alive_during_stop = false;
  UINT32 origin=0;
  void record(char c) noexcept {
    if (count < calls.size()) calls[count++] = c; else invariant = false;
  }
  std::string trace() const { return {calls.data(), count}; }
};
Provider* active = nullptr;
// Explicit synthetic origin for the injected CPU-only lifecycle provider.
UINT32 cpu_origin(){return active->origin;}
NTSTATUS preflight(PWSTR device, PWSTR path) {
  auto& s = *active; s.record('p');
  s.invariant = s.invariant && std::wstring_view(device) == L"WinFsp.Disk" &&
      std::wstring_view(path) == L"C:\\FlitzFS-fixture\\mount";
  return s.failure == Failure::preflight ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}
NTSTATUS create(PWSTR, const FSP_FSCTL_VOLUME_PARAMS* parameters,
    const FSP_FILE_SYSTEM_INTERFACE* callbacks, FSP_FILE_SYSTEM** result) {
  auto& s = *active; s.record('c');
  s.invariant = s.invariant && !parameters->ReadOnlyVolume &&
      parameters->UmFileContextIsUserContext2 && parameters->IrpCapacity == 100;
  *result = nullptr;
  if (s.failure == Failure::create) return STATUS_INSUFFICIENT_RESOURCES;
  if (s.failure == Failure::create_null_success) return STATUS_SUCCESS;
  s.fs.Interface = callbacks;
  *result = &s.fs;
  return s.failure == Failure::create_partial ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
}
NTSTATUS start(FSP_FILE_SYSTEM* file_system, ULONG threads) {
  auto& s = *active; s.record('s');
  s.invariant = s.invariant && file_system == &s.fs && file_system->UserContext && threads == 2;
  if (s.failure == Failure::stop_during_start) file_system->Interface->DispatcherStopped(file_system, FALSE);
  return s.failure == Failure::start ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS;
}
NTSTATUS mount(FSP_FILE_SYSTEM* file_system, PWSTR path) {
  auto& s = *active; s.record('m');
  s.invariant = s.invariant && file_system == &s.fs &&
      std::wstring_view(path) == L"C:\\FlitzFS-fixture\\mount";
  if (s.failure == Failure::stop_during_mount) file_system->Interface->DispatcherStopped(file_system, FALSE);
  return s.failure == Failure::mount ? STATUS_OBJECT_NAME_COLLISION : STATUS_SUCCESS;
}
VOID stop(FSP_FILE_SYSTEM* file_system) {
  auto& s = *active; s.record('t');
  FSP_FSCTL_VOLUME_INFO info{};
  s.callback_alive_during_stop = file_system->Interface->GetVolumeInfo(file_system, &info) == STATUS_SUCCESS;
  // It is safe to deliver a duplicate notification; the first stop reason wins.
  if(s.failure!=Failure::stop_without_notification)file_system->Interface->DispatcherStopped(file_system, TRUE);
}
VOID unmount(FSP_FILE_SYSTEM* file_system) {
  auto& s = *active; s.record('r'); s.invariant = s.invariant && file_system == &s.fs;
}
VOID destroy(FSP_FILE_SYSTEM* file_system) {
  auto& s = *active; s.record('d'); s.invariant = s.invariant && file_system == &s.fs;
  s.fs.UserContext = nullptr;
}
// Only the mock callback test can pause this synthetic RAM read. The
// timeout bounds the callback even if the controller cannot release it.
class PausedReadStore final:public flitzfs::StoreByteIo {
 public:
  PausedReadStore():release_(resume_.get_future()) {}
  std::future<void> pause_next_read() {
    armed_=true;
    return entered_.get_future();
  }
  void release_read(){resume_.set_value();}
  std::uint64_t extent() const override{return storage_.extent();}
  flitzfs::StoreIoContract contract() const noexcept override{return storage_.contract();}
  void admit_write(std::uint64_t size) override{storage_.admit_write(size);}
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override{storage_.write_exact(at,bytes);}
  void flush() override{storage_.flush();}
  void read_exact(std::uint64_t at,std::span<std::byte> bytes) const override {
    if(armed_.exchange(false)) {
      entered_.set_value();
      if(release_.wait_for(std::chrono::seconds(4))!=std::future_status::ready)
        throw std::runtime_error("mock foreground callback did not resume");
    }
    storage_.read_exact(at,bytes);
  }
 private:
  native_fixture::Ram storage_;
  mutable std::atomic<bool> armed_{false};
  mutable std::promise<void> entered_;
  std::promise<void> resume_;
  mutable std::future<void> release_;
};
flitzfs::WinFspSessionApi api() { return {preflight, create, start, mount, stop, unmount, destroy,cpu_origin}; }
flitzfs::NativeWriteMountPlan plan() { return {L"C:\\FlitzFS-fixture\\mount", 15, true, true}; }
}
int main(){try {
  using namespace flitzfs;nw::hosted();Provider provider;active=&provider;
  const auto create_adapter=[](nw::Fixture& fixture) {
    return WinFspNativeWritable::create_new_hosted(fixture.image(),nw::identity());
  };

  {
    nw::Fixture f;auto incomplete=api();incomplete.originating_process_id=nullptr;
    bool refused=false;
    try{WinFspNativeSession session(create_adapter(f),incomplete);}
    catch(const std::invalid_argument&){refused=true;}
    require(refused&&!provider.count,"missing native origin provider reached lifecycle operations");
  }
  {
    nw::Fixture f;WinFspNativeSession session(create_adapter(f),api());
    require(session.start(plan())==STATUS_SUCCESS,"native origin fixture startup");
    provider.origin=GetCurrentProcessId();
    PVOID context=reinterpret_cast<PVOID>(1);FSP_FSCTL_FILE_INFO info{};
    require(provider.fs.Interface->Open(&provider.fs,const_cast<PWSTR>(L"\\"),FILE_DIRECTORY_FILE,
        FILE_GENERIC_READ,&context,&info)==STATUS_ACCESS_DENIED&&!context&&session.self_origin_rejected(),
        "native session lost self-origin admission");
    provider.origin=0;session.stop();
    require(session.self_origin_rejected()&&session.state()==SessionState::stopped&&
        !session.requires_offline()&&!session.stats().contexts,"retired native session lost guard evidence");
  }
  provider={};
  {
    nw::Fixture f;bool rejected=false;
    try{WinFspNativeSession session(create_adapter(f),{});}catch(const std::invalid_argument&){rejected=true;}
    require(rejected&&!provider.count,"incomplete writable provider accepted");
    // Invalid construction must also release the exclusive writer handle.
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();
  }
  {
    nw::Fixture f;WinFspNativeSession session(create_adapter(f),api());
    auto invalid=plan();invalid.allow_native_writes=false;
    require(session.start(invalid)==STATUS_INVALID_PARAMETER&&!provider.count,"missing native-write opt-in reached provider");
    invalid=plan();invalid.allow_driver_start=false;
    require(session.start(invalid)==STATUS_INVALID_PARAMETER&&!provider.count,"missing driver opt-in reached provider");
    invalid=plan();invalid.directory=L"R:";
    require(session.start(invalid)==STATUS_INVALID_PARAMETER&&!provider.count,"drive-letter writable mount accepted");
    require(session.start(plan())==STATUS_SUCCESS&&provider.trace()=="pcsm"&&
        session.state()==SessionState::running,"writable startup order");
    require(session.start(plan())==STATUS_INVALID_DEVICE_STATE&&provider.trace()=="pcsm","writable double start");
    session.stop();
    require(provider.trace()=="pcsmtrd"&&provider.callback_alive_during_stop&&provider.invariant&&
        session.state()==SessionState::stopped&&session.dispatcher_stopped()&&!session.requires_offline()&&!session.dispatcher_stopped_unexpectedly(),
        "writable stop did not join before releasing callback owner");
    require(!session.stats().contexts&&!session.stats().fenced,"retired writable diagnostics lost");
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();
    session.stop();require(provider.trace()=="pcsmtrd","writable stop repeated release");
  }
  struct Case{Failure failure;NTSTATUS status;const char* trace;bool joined;};
  const Case cases[]={
    {Failure::preflight,STATUS_ACCESS_DENIED,"p",false},
    {Failure::create,STATUS_INSUFFICIENT_RESOURCES,"pc",false},
    {Failure::create_partial,STATUS_INSUFFICIENT_RESOURCES,"pcd",false},
    {Failure::create_null_success,STATUS_INTERNAL_ERROR,"pc",false},
    {Failure::start,STATUS_INSUFFICIENT_RESOURCES,"pcstd",true},
    {Failure::mount,STATUS_OBJECT_NAME_COLLISION,"pcsmtd",true},
    {Failure::stop_during_start,STATUS_DEVICE_NOT_READY,"pcstd",true},
    {Failure::stop_during_mount,STATUS_DEVICE_NOT_READY,"pcsmtrd",true}
  };
  unsigned retired=1;
  for(const auto& item:cases) {
    provider={};provider.failure=item.failure;nw::Fixture f;
    {
      WinFspNativeSession session(create_adapter(f),api());
      require(session.start(plan())==item.status&&session.state()==SessionState::failed,"writable rollback status/state");
      require(provider.trace()==item.trace&&provider.invariant&&(!item.joined||provider.callback_alive_during_stop),
          "writable rollback resource order");
      if(item.failure==Failure::stop_during_start||item.failure==Failure::stop_during_mount)
        require(session.dispatcher_stopped()&&session.dispatcher_stopped_unexpectedly(),
            "failed startup discarded first unexpected dispatcher stop");
      require(session.start(plan())==STATUS_INVALID_DEVICE_STATE,"failed writable session restarted");
      session.stop();
      auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();++retired;
    }
    require(provider.trace()==item.trace,"failed writable destructor repeated cleanup");
  }
  provider={};
  {
    nw::Fixture f;
    {
      WinFspNativeSession session(create_adapter(f),api());require(session.start(plan())==STATUS_SUCCESS,"destructor startup");
      provider.fs.Interface->DispatcherStopped(&provider.fs,FALSE);
      require(session.dispatcher_stopped_unexpectedly(),"external stop reason lost");
      session.stop();
      require(session.state()==SessionState::failed&&session.dispatcher_stopped_unexpectedly(),"normal stop erased prior failure");
      auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();++retired;
    }
    require(provider.trace()=="pcsmtrd"&&provider.callback_alive_during_stop,"destructor failed callback retirement");
  }
  provider={};
  {
    nw::Fixture f;WinFspNativeSession session(create_adapter(f),api());
    require(session.start(plan())==STATUS_SUCCESS,"fenced-session startup");
    std::array<std::byte,4096> descriptor{};SIZE_T bytes=descriptor.size();UINT32 attributes=0;
    require(provider.fs.Interface->GetSecurityByName(&provider.fs,const_cast<PWSTR>(L"\\"),&attributes,
        descriptor.data(),&bytes)==STATUS_SUCCESS,"fenced-session root security");
    PVOID context=nullptr;FSP_FSCTL_FILE_INFO info{};
    require(provider.fs.Interface->Create(&provider.fs,const_cast<PWSTR>(L"\\retained"),FILE_NON_DIRECTORY_FILE,
        FILE_ALL_ACCESS,FILE_ATTRIBUTE_NORMAL,descriptor.data(),0,&context,&info)==STATUS_SUCCESS,
        "fenced-session creation");
    provider.fs.Interface->Cleanup(&provider.fs,context,nullptr,0x80000000U);
    require(session.requires_offline()&&session.stats().contexts==1&&session.stats().fenced,
        "VOID failure not visible to external controller");
    // This is a controller call after Cleanup returned, never a callback join.
    session.stop();
    require(provider.trace()=="pcsmtrd"&&provider.invariant&&session.state()==SessionState::failed&&
        session.requires_offline()&&session.stats().contexts==1&&session.stats().fenced,
        "offlining erased failed retained-context outcome or teardown order");
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();++retired;
    require(view.stat("retained").logical_bytes==0,"offlining lost acknowledged creation");
    session.stop();require(provider.trace()=="pcsmtrd","offlining repeated provider cleanup");
  }
  provider={};provider.failure=Failure::stop_without_notification;
  {
    nw::Fixture f;WinFspNativeSession session(create_adapter(f),api());
    require(session.start(plan())==STATUS_SUCCESS,"missing-notification startup");
    session.stop();
    require(provider.trace()=="pcsmtrd"&&provider.callback_alive_during_stop&&session.state()==SessionState::failed&&
        session.requires_offline()&&!session.dispatcher_stopped(),"missing dispatcher notification accepted as clean stop");
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open_file(f.image()));view.verify_all();++retired;
  }
  provider={};
  {
    auto head=std::make_shared<native_fixture::Ram>();
    auto source=std::make_shared<PausedReadStore>();
    auto target=std::make_shared<native_fixture::Ram>();
    auto volume=nw::identity(),image=volume,next=volume;
    image[0]=std::byte{31};next[0]=std::byte{32};
    WinFspNativeSession session(WinFspNativeWritable::create_managed_hosted(head,volume,source,image),api());
    require(session.start(plan())==STATUS_SUCCESS,"idle contention mock startup");
    auto entered=source->pause_next_read();
    NTSTATUS callback_status=STATUS_UNSUCCESSFUL;
    std::jthread foreground([&]{
      FSP_FSCTL_VOLUME_INFO info{};
      callback_status=provider.fs.Interface->GetVolumeInfo(&provider.fs,&info);
    });
    const bool waiting=entered.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    HostedIdleMaintenanceResult idle{HostedIdleMaintenanceState::stale};
    if(waiting)idle=session.begin_idle_recompression_hosted(target,next);
    source->release_read();
    foreground.join();
    require(waiting&&idle.state==HostedIdleMaintenanceState::busy&&
        callback_status==STATUS_SUCCESS&&target->live.empty(),
        "idle admission did work while a foreground callback held control");
    require(session.begin_idle_recompression_hosted(target,next).state==HostedIdleMaintenanceState::begun,
        "foreground contention consumed the later idle opportunity");
    session.stop();
    require(!session.requires_offline()&&!session.stats().contexts,"stopping with idle candidate leaked authority");
  }
  require(retired==12,"writable lifecycle retirement case omitted");
  std::cout<<"Windows writable-lifecycle evidence: {\"schema\":1,\"checks\":"<<checks<<",\"retired_writers\":"<<retired
      <<",\"real_provider_calls\":0,\"mounted\":false,\"complete\":true}\n";return 0;
}catch(const std::exception& e){std::cerr<<"Writable lifecycle failure: "<<e.what()<<"\n";return 1;}}
