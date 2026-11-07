#include "winfsp_session.hpp"
#include "flitzfs/container.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Fixture {
  fs::path root;
  Fixture() {
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("flitzfs-session-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(id));
    require(fs::create_directory(root), "new isolated fixture required");
  }
  ~Fixture() {
    std::error_code ignored;
    for (const auto* file : {"input.bin", "input.flfs"}) fs::remove(root / file, ignored);
    fs::remove(root, ignored); // Never recursive.
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
  void record(char c) noexcept {
    if (count < calls.size()) calls[count++] = c; else invariant = false;
  }
  std::string trace() const { return {calls.data(), count}; }
};
Provider* active = nullptr;
NTSTATUS preflight(PWSTR device, PWSTR path) {
  auto& s = *active; s.record('p');
  s.invariant = s.invariant && std::wstring_view(device) == L"WinFsp.Disk" &&
      std::wstring_view(path) == L"C:\\FlitzFS-fixture\\mount";
  return s.failure == Failure::preflight ? STATUS_ACCESS_DENIED : STATUS_SUCCESS;
}
NTSTATUS create(PWSTR, const FSP_FSCTL_VOLUME_PARAMS* parameters,
    const FSP_FILE_SYSTEM_INTERFACE* interface, FSP_FILE_SYSTEM** result) {
  auto& s = *active; s.record('c');
  s.invariant = s.invariant && parameters->ReadOnlyVolume &&
      parameters->UmFileContextIsUserContext2 && parameters->IrpCapacity == 100;
  *result = nullptr;
  if (s.failure == Failure::create) return STATUS_INSUFFICIENT_RESOURCES;
  if (s.failure == Failure::create_null_success) return STATUS_SUCCESS;
  s.fs.Interface = interface;
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
flitzfs::WinFspSessionApi api() { return {preflight, create, start, mount, stop, unmount, destroy}; }
flitzfs::ReadOnlyMountPlan plan() { return {L"C:\\FlitzFS-fixture\\mount", 15, true}; }
}
int main() {
  try {
    Fixture fixture;
    const auto input = fixture.root / "input.bin";
    const auto archive = fixture.root / "input.flfs";
    { std::ofstream stream(input, std::ios::binary); stream << std::string(8192, 'a'); require(bool(stream), "fixture write"); }
    (void)flitzfs::pack_new(input, archive);
    const std::vector<flitzfs::CatalogSource> sources{{"sample.bin", archive}};
    Provider provider;
    active = &provider;
    bool missing_api = false;
    try { flitzfs::WinFspReadOnlySession session(sources, {}); }
    catch (const std::invalid_argument&) { missing_api = true; }
    require(missing_api && provider.count == 0, "incomplete provider accepted");
    {
      flitzfs::WinFspReadOnlySession session(sources, api());
      auto invalid = plan(); invalid.allow_driver_start = false;
      require(session.start(invalid) == STATUS_INVALID_PARAMETER && provider.count == 0,
          "unconsented plan called a provider");
      invalid = plan(); invalid.directory = L"R:";
      require(session.start(invalid) == STATUS_INVALID_PARAMETER && provider.count == 0,
          "drive-letter mount accepted");
      require(session.start(plan()) == STATUS_SUCCESS && provider.trace() == "pcsm", "startup order");
      require(session.state() == flitzfs::SessionState::running && !session.dispatcher_stopped(), "running state");
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
        flitzfs::WinFspReadOnlySession session(sources, api());
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
      flitzfs::WinFspReadOnlySession session(sources, api());
      require(session.start(plan()) == STATUS_SUCCESS, "destructor test start");
      provider.fs.Interface->DispatcherStopped(&provider.fs, FALSE);
      require(session.dispatcher_stopped_unexpectedly(), "external stop not observed");
    }
    require(provider.trace() == "pcsmtrd" && provider.callback_alive_during_stop, "destructor failed to join");
    provider = {};
    bool corrupt_rejected = false;
    { std::fstream stream(archive, std::ios::binary | std::ios::in | std::ios::out); stream.put('!'); }
    try { flitzfs::WinFspReadOnlySession session(sources, api()); }
    catch (const std::exception&) { corrupt_rejected = true; }
    require(corrupt_rejected && provider.count == 0, "corrupt input reached lifecycle provider");
    std::cout << "Native lifecycle injection tests passed; no driver, mount or real dispatcher called.\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
