#include "hip_module_verification.hpp"
#include <array>
#include <softpub.h>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace flitzfs;
unsigned checks = 0;
void require(bool condition, const char* why) {
  ++checks; if (!condition) throw std::runtime_error(why);
}
template<class Function>
void rejects(Function&& call, HipWorkerExit expected) {
  try { call(); throw std::runtime_error("metadata unexpectedly accepted"); }
  catch (const HipWorkerFailure& failure) { require(failure.code == expected, "wrong metadata failure"); }
}

LONG verify_status = 0, close_status = 0;
unsigned signature_calls = 0;
bool signature_flags = true;
LONG WINAPI fake_verify(HWND ui, GUID* action, LPVOID raw) {
  const GUID expected_action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  auto* trust = static_cast<WINTRUST_DATA*>(raw);
  if (!trust || !action) { signature_flags = false; return 1; }
  signature_flags = signature_flags && !ui && action &&
      std::memcmp(action, &expected_action, sizeof(GUID)) == 0 && trust &&
      trust->cbStruct == sizeof(*trust) && trust->dwUIChoice == WTD_UI_NONE &&
      trust->fdwRevocationChecks == WTD_REVOKE_NONE && trust->dwUnionChoice == WTD_CHOICE_FILE &&
      trust->dwProvFlags == (WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT) &&
      trust->pFile && trust->pFile->cbStruct == sizeof(WINTRUST_FILE_INFO) &&
      trust->pFile->pcwszFilePath &&
      std::wstring_view(trust->pFile->pcwszFilePath) == L"flitzfs_signature_policy_fixture.dll";
  ++signature_calls;
  if (signature_calls == 1) {
    signature_flags = signature_flags && trust->dwStateAction == WTD_STATEACTION_VERIFY;
    trust->hWVTStateData = reinterpret_cast<HANDLE>(std::uintptr_t{1});
    return verify_status;
  }
  signature_flags = signature_flags && signature_calls == 2 &&
      trust->dwStateAction == WTD_STATEACTION_CLOSE &&
      trust->hWVTStateData == reinterpret_cast<HANDLE>(std::uintptr_t{1});
  trust->hWVTStateData = nullptr;
  return close_status;
}
void signature_policy() {
  const std::filesystem::path path(L"flitzfs_signature_policy_fixture.dll");
  for (const LONG status : {0L, 1L, 5L, -1L}) {
    verify_status = status; close_status = 0; signature_calls = 0; signature_flags = true;
    require(hip_detail::verify_cached_signature(path, fake_verify) == (status == 0),
        "nonzero LONG trust status was accepted");
    require(signature_calls == 2 && signature_flags, "signature state/flags were not preserved");
  }
  verify_status = 0; close_status = 5; signature_calls = 0; signature_flags = true;
  require(!hip_detail::verify_cached_signature(path, fake_verify) && signature_calls == 2 &&
      signature_flags, "signature close failure was accepted");
  require(!hip_detail::verify_cached_signature(path, nullptr), "null trust callback accepted");
}

void unloaded() {
  require(!GetModuleHandleA(FLITZFS_REQUIRED_HIP_RUNTIME_DLL_NAME), "AMD subject module was loaded");
}
}
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 2 && std::wstring_view(argv[1]).size() == 64, "explicit kernel32 digest required");
    std::string expected;
    for (const auto character : std::wstring_view(argv[1])) {
      require(character <= 127, "non-ASCII digest");
      expected.push_back(static_cast<char>(character));
    }
    require(hip_detail::valid_sha256(expected), "invalid digest");
    unloaded();
    signature_policy();
    // Windows' own already-loaded kernel32 provides real signed metadata.
    // No subject is loaded by the verifier and no system file is modified.
    for (unsigned i = 0; i < 2; ++i) {
      const hip_detail::VerifiedSystemModule verified(L"kernel32.dll", expected);
      require(verified.size() && verified.size() <= 32ULL*1024*1024 &&
          verified.path().filename() == L"kernel32.dll", "metadata identity/size");
    }
    DWORD baseline = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &baseline), "metadata handle baseline");
    for (unsigned i = 0; i < 4; ++i) {
      const hip_detail::VerifiedSystemModule verified(L"kernel32.dll", expected);
      require(verified.size() > 0, "empty accepted module");
    }
    for (const auto name : {L"", L"../kernel32.dll", L"kernel32.dll:stream", L"C:\\kernel32.dll",
                           L"kernel32.dll ", L"kernel32.dll/child", L"KERNEL32.dll"}) {
      rejects([&] { const hip_detail::VerifiedSystemModule value(name, expected); },
          HipWorkerExit::invalid_request);
    }
    for (const auto digest : {std::string{}, std::string(63,'0'), std::string(65,'0'), std::string(64,'G')}) {
      rejects([&] { const hip_detail::VerifiedSystemModule value(L"kernel32.dll", digest); },
          HipWorkerExit::invalid_request);
    }
    rejects([&] { const hip_detail::VerifiedSystemModule value(
        L"flitzfs_absent_metadata_8a6142a1.dll", expected); }, HipWorkerExit::runtime_unavailable);
    auto mismatched = expected; mismatched[0] = expected[0] == '0' ? '1' : '0';
    rejects([&] { const hip_detail::VerifiedSystemModule value(L"kernel32.dll", mismatched); },
        HipWorkerExit::integrity_failure);
    unloaded();
    DWORD final = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &final), "metadata final handles");
    require(final <= baseline, "metadata handles accumulated");
    std::cout << "System module metadata: {\"checks\":" << checks <<
        ",\"handle_before\":" << baseline << ",\"handle_after\":" << final <<
        ",\"hip_runtime_loaded\":false,\"gpu_executed\":false,\"complete\":true}\n";
    return 0;
  } catch (const std::exception& failure) {
    std::cerr << "System module metadata failed: " << failure.what() << '\n'; return 1;
  }
}
