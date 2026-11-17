#include "winfsp_session.hpp"
#include "memory_volume_factory.hpp"
#include "flitzfs/container.hpp"
#include "flitzfs/native_session.hpp"
#include "native_windows_fixture.hpp"
#include "windows_app_budget.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct LegacyFiles {
  fs::path input, archive;
  bool owns_input=false, owns_archive=false;
  explicit LegacyFiles(const fs::path& root):input(root/L"input.bin"),archive(root/L"input.flfs"){}
  LegacyFiles(const LegacyFiles&)=delete;
  LegacyFiles& operator=(const LegacyFiles&)=delete;
  ~LegacyFiles() {
    std::error_code ignored;
    if(owns_input)fs::remove(input,ignored);
    if(owns_archive)fs::remove(archive,ignored);
  }
  void create() {
    const auto file=CreateFileW(input.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"new session input fixture required");
    owns_input=true;
    std::array<char,8192> bytes{};
    bytes.fill('a');
    DWORD written=0;
    const auto wrote=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);
    const auto closed=CloseHandle(file);
    require(wrote && closed && written==bytes.size(),"session input fixture write");
    (void)flitzfs::pack_new(input,archive);
    owns_archive=true;
  }
};
enum class Failure { none, preflight, create, create_partial, create_null_success, start, mount,
                     stop_during_start, stop_during_mount };
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
  s.invariant = s.invariant && parameters->ReadOnlyVolume &&
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
  file_system->Interface->DispatcherStopped(file_system, TRUE);
}
VOID unmount(FSP_FILE_SYSTEM* file_system) {
  auto& s = *active; s.record('r'); s.invariant = s.invariant && file_system == &s.fs;
}
VOID destroy(FSP_FILE_SYSTEM* file_system) {
  auto& s = *active; s.record('d'); s.invariant = s.invariant && file_system == &s.fs;
  s.fs.UserContext = nullptr;
}
flitzfs::WinFspSessionApi api() { return {preflight, create, start, mount, stop, unmount, destroy,cpu_origin}; }
flitzfs::ReadOnlyMountPlan plan() { return {L"C:\\FlitzFS-fixture\\mount", 15, true}; }

template<class MakeSession>
void exercise_lifecycle(const MakeSession& make_session, std::wstring path, std::uint64_t inode=0,
    std::span<const std::byte> payload={}) {
    Provider provider;
    active = &provider;
    bool missing_api = false;
    try { auto session=make_session({}); }
    catch (const std::invalid_argument&) { missing_api = true; }
    require(missing_api && provider.count == 0, "incomplete provider accepted");
    {
      auto incomplete=api();incomplete.originating_process_id=nullptr;
      bool refused=false;
      try{auto session=make_session(incomplete);}
      catch(const std::invalid_argument&){refused=true;}
      require(refused&&provider.count==0,"missing origin provider reached lifecycle operations");
    }
    {
      auto session=make_session(api());
      auto invalid = plan(); invalid.allow_driver_start = false;
      require(session.start(invalid) == STATUS_INVALID_PARAMETER && provider.count == 0,
          "unconsented plan called a provider");
      invalid = plan(); invalid.directory = L"R:";
      require(session.start(invalid) == STATUS_INVALID_PARAMETER && provider.count == 0,
          "drive-letter mount accepted");
      require(session.start(plan()) == STATUS_SUCCESS && provider.trace() == "pcsm", "startup order");
      require(session.state() == flitzfs::SessionState::running && !session.dispatcher_stopped(), "running state");
      provider.origin=GetCurrentProcessId();
      FSP_FSCTL_FILE_INFO denied_info{};PVOID denied_context=reinterpret_cast<PVOID>(1);
      require(provider.fs.Interface->Open(&provider.fs,path.data(),0,
          FILE_GENERIC_READ,&denied_context,&denied_info)==STATUS_ACCESS_DENIED&&!denied_context&&
          session.self_origin_rejected(),"session lost self-origin admission");
      provider.origin=0;
      if(inode) {
        FSP_FSCTL_FILE_INFO info{};PVOID context=nullptr;
        require(provider.fs.Interface->Open(&provider.fs,path.data(),0,FILE_GENERIC_READ,&context,&info)==
            STATUS_SUCCESS && info.IndexNumber==inode,"session did not expose the committed inode");
        std::vector<std::byte> bytes(payload.size());
        ULONG count=0;
        require(provider.fs.Interface->Read(&provider.fs,context,bytes.data(),0,
            static_cast<ULONG>(bytes.size()),&count)==STATUS_SUCCESS && count==bytes.size() &&
            std::equal(bytes.begin(),bytes.end(),payload.begin()),"session readback differs from source");
        provider.fs.Interface->Close(&provider.fs,context);
      }
      require(session.start(plan()) == STATUS_INVALID_DEVICE_STATE && provider.trace() == "pcsm", "double start");
      session.stop();
      require(provider.trace() == "pcsmtrd" && provider.callback_alive_during_stop && provider.invariant,
          "join before mount/adapter destruction");
      require(session.dispatcher_stopped() && !session.dispatcher_stopped_unexpectedly(), "normal stop notification");
      require(session.state() == flitzfs::SessionState::stopped, "stopped state");
      session.stop();
      require(session.start(plan()) == STATUS_INVALID_DEVICE_STATE && provider.trace() == "pcsmtrd", "restart or double cleanup");
    }
    require(provider.trace() == "pcsmtrd", "destructor repeated cleanup");
    struct Case { Failure failure; NTSTATUS expected; const char* trace; bool stopped; };
    const Case cases[] = {
      {Failure::preflight, STATUS_ACCESS_DENIED, "p", false},
      {Failure::create, STATUS_INSUFFICIENT_RESOURCES, "pc", false},
      {Failure::create_partial, STATUS_INSUFFICIENT_RESOURCES, "pcd", false},
      {Failure::create_null_success, STATUS_INTERNAL_ERROR, "pc", false},
      {Failure::start, STATUS_INSUFFICIENT_RESOURCES, "pcstd", true},
      {Failure::mount, STATUS_OBJECT_NAME_COLLISION, "pcs mtd", true},
      {Failure::stop_during_start, STATUS_DEVICE_NOT_READY, "pcstd", true},
      {Failure::stop_during_mount, STATUS_DEVICE_NOT_READY, "pcsmtrd", true},
    };
    for (const auto& item : cases) {
      provider = {}; provider.failure = item.failure;
      std::string expected(item.trace); expected.erase(std::remove(expected.begin(), expected.end(), ' '), expected.end());
      {
        auto session=make_session(api());
        require(session.start(plan()) == item.expected && session.state() == flitzfs::SessionState::failed, "failure status/state");
        require(provider.trace() == expected && provider.invariant, "rollback order");
        require(!item.stopped || provider.callback_alive_during_stop, "adapter died before stop");
        if (item.failure == Failure::stop_during_start || item.failure == Failure::stop_during_mount)
          require(session.dispatcher_stopped_unexpectedly(), "unexpected stop reason overwritten");
        require(session.start(plan()) == STATUS_INVALID_DEVICE_STATE, "failed session retried");
        session.stop();
      }
      require(provider.trace() == expected, "failed destructor cleanup repeated");
    }
    provider = {};
    {
      auto session=make_session(api());
      require(session.start(plan()) == STATUS_SUCCESS, "destructor test start");
      provider.fs.Interface->DispatcherStopped(&provider.fs, FALSE);
      require(session.dispatcher_stopped_unexpectedly(), "external stop not observed");
    }
    require(provider.trace() == "pcsmtrd" && provider.callback_alive_during_stop, "destructor failed to join");
    active=nullptr;
}

void memory_lifecycle() {
  using namespace flitzfs;
  const auto payload=native_windows_fixture::data();
  {
    auto backing=MemoryVolumeBacking::create();
    std::uint64_t inode=0;
    {
      auto writer=NativeFileSession::create_paged_volume(backing.head,backing.volume_identity,backing.image,
          backing.image_identity,{backing.layout.image_capacity},{StoreCompressionMode::automatic,3,false});
      writer.create_entry("Data",true);
      inode=writer.create_entry("Data/sample.bin").created_inodes.at(0);
      const auto file=writer.open("Data/sample.bin",{true,true,false},NativeOpenKind::file);
      writer.write(file,0,payload);writer.flush(file);writer.cleanup(file,false);writer.close(file);
    }
    exercise_lifecycle([&](WinFspSessionApi injected) {
      return WinFspReadOnlySession(backing.open_read_only(),injected);
    },L"\\Data\\sample.bin",inode,payload);
  }
  require(MemoryVolumeDomain::cleanup_healthy(),"memory lifecycle backing cleanup failed");
}

void file_lifecycle() {
  using namespace flitzfs;
  namespace nw=native_windows_fixture;
  nw::Fixture fixture;
  LegacyFiles files(fixture.root);
  files.create();
  const std::vector<CatalogSource> sources{{"sample.bin",files.archive}};
  exercise_lifecycle([&](WinFspSessionApi injected) {
    return WinFspReadOnlySession(sources,injected);
  },L"\\sample.bin");
  const auto expected=nw::make_image(fixture.image(),false);
  exercise_lifecycle([&](WinFspSessionApi injected) {
    return WinFspReadOnlySession(ReadOnlyProjection::open_native_file(fixture.image(),nw::identity()),injected);
  },L"\\Data\\sample.bin",expected.sample_inode,nw::data());

  Provider provider;
  active=&provider;
  bool wrong_identity=false;auto other=nw::identity();other[1]=std::byte{1};
  try { WinFspReadOnlySession session(ReadOnlyProjection::open_native_file(fixture.image(),other),api()); }
  catch (const std::exception&) { wrong_identity=true; }
  require(wrong_identity && !provider.count,"wrong native identity reached driver provider");
  bool corrupt_rejected=false;
  {
    std::fstream stream(files.archive,std::ios::binary | std::ios::in | std::ios::out);
    stream.put('!');
    require(bool(stream),"legacy malformed fixture write");
  }
  try { WinFspReadOnlySession session(sources,api()); }
  catch (const std::exception&) { corrupt_rejected=true; }
  require(corrupt_rejected && !provider.count,"corrupt input reached lifecycle provider");
  active=nullptr;
}
} // namespace

int main(int argc,char** argv) {
  try {
    flitzfs::app_fixture::Budget budget;
    if(argc==1) memory_lifecycle();
    else if(argc==2 && std::string_view(argv[1])=="--file") file_lifecycle();
    else throw std::invalid_argument("unknown session fixture mode");
    std::cout<<"Read-only lifecycle, rollback, and retirement checks passed using injected callbacks.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
