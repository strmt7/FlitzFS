#include "hip_module_verification.hpp"
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <algorithm>
#include <array>
#include <new>

namespace flitzfs::hip_detail {
namespace {
using Exit = HipWorkerExit;
void require(bool condition, Exit code, const char* message) {
  if (!condition) throw HipWorkerFailure(code, message);
}
bool valid_basename(std::wstring_view name) noexcept {
  if (name.size() < 5 || name.size() > 128 || !name.ends_with(L".dll") ||
      name.front() < L'a' || name.front() > L'z') return false;
  for (const auto character : name.substr(0, name.size() - 4))
    if (!((character >= L'a' && character <= L'z') ||
          (character >= L'0' && character <= L'9') || character == L'_')) return false;
  return true;
}
bool cached_trust(const std::filesystem::path& path, decltype(&WinVerifyTrust) verify) noexcept {
  WINTRUST_FILE_INFO file_info{};
  file_info.cbStruct = sizeof(file_info);
  file_info.pcwszFilePath = path.c_str();
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file_info;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const auto status = verify(nullptr, &action, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  const auto closed = verify(nullptr, &action, &trust);
  // WinVerifyTrust returns LONG, not HRESULT: only zero indicates success.
  return status == ERROR_SUCCESS && closed == ERROR_SUCCESS;
}
}
bool verify_cached_signature(const std::filesystem::path& path, decltype(&WinVerifyTrust) verify) noexcept {
  return verify && cached_trust(path, verify);
}
bool valid_sha256(std::string_view expected) noexcept {
  return expected.size() == 64 && std::all_of(expected.begin(), expected.end(), [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}
struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Hash {
  BCRYPT_HASH_HANDLE handle = nullptr;
  ~Hash() { if (handle) BCryptDestroyHash(handle); }
};

bool matches_sha256(const ReadOnlyFile& file, std::string_view expected) {
  if (!valid_sha256(expected)) return false;
  Algorithm algorithm;
  require(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
      Exit::runtime_unavailable, "SHA256 provider unavailable");
  ULONG length = 0, returned = 0;
  require(BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH,
              reinterpret_cast<PUCHAR>(&length), sizeof(length), &returned, 0) >= 0 &&
          returned == sizeof(length) && length && length <= 65536,
      Exit::runtime_unavailable, "SHA256 object size");
  std::vector<UCHAR> object(length);
  Hash hash;
  require(BCryptCreateHash(algorithm.handle, &hash.handle, object.data(), length, nullptr, 0, 0) >= 0,
      Exit::runtime_unavailable, "SHA256 context");
  std::array<UCHAR,65536> bytes{};
  for (std::uint64_t offset = 0; offset < file.size();) {
    const auto count = static_cast<ULONG>((std::min)(file.size() - offset,
        static_cast<std::uint64_t>(bytes.size())));
    file.read_exact_at(offset, bytes.data(), count);
    require(BCryptHashData(hash.handle, bytes.data(), count, 0) >= 0,
        Exit::runtime_unavailable, "SHA256 input");
    offset += count;
  }
  std::array<UCHAR,32> digest{};
  require(BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
      Exit::runtime_unavailable, "SHA256 final");
  constexpr std::string_view hex = "0123456789abcdef";
  if (expected.size() != 2 * digest.size()) return false;
  for (std::size_t i = 0; i < digest.size(); ++i) {
    if (expected[2*i] != hex[digest[i] >> 4] || expected[2*i+1] != hex[digest[i] & 15])
      return false;
  }
  return true;
}

VerifiedSystemModule::VerifiedSystemModule(std::wstring_view basename, std::string_view expected) {
  try {
    require(valid_basename(basename) && valid_sha256(expected), Exit::invalid_request,
        "invalid system module identity");
    std::array<wchar_t,32768> value{};
    const auto length = GetSystemDirectoryW(value.data(), static_cast<UINT>(value.size()));
    require(length && length < value.size(), Exit::runtime_unavailable, "system directory unavailable");
    const std::filesystem::path directory(std::wstring_view(value.data(), length));
    const auto drive = directory.root_name().native();
    require(directory.is_absolute() && drive.size() == 2 && drive[1] == L':',
        Exit::runtime_unavailable, "system module must be on a local drive");
    auto parent = directory.root_path();
    directories_.emplace_back(parent);
    for (const auto& component : directory.relative_path()) {
      require(component != L"." && component != L".." && directories_.size() < 32,
          Exit::runtime_unavailable, "system directory path depth");
      parent /= component;
      directories_.emplace_back(parent);
    }
    path_ = directory / std::filesystem::path(basename);
    file_.emplace(ReadOnlyFile::open(path_));
    require(file_->size() && file_->size() <= 32ULL * 1024 * 1024,
        Exit::runtime_unavailable, "system module size budget");
    require(matches_sha256(*file_, expected), Exit::integrity_failure, "system module byte identity");
    require(verify_cached_signature(path_, &WinVerifyTrust), Exit::runtime_unavailable, "system module trust unavailable");
  } catch (const HipWorkerFailure&) { throw; }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception&) {
      throw HipWorkerFailure(Exit::runtime_unavailable, "system module cannot be verified");
    }
}
}
