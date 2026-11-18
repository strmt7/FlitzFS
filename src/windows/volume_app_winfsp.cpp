#include "volume_app_winfsp.hpp"
#include "volume_directory_pin.hpp"
#include "flitzfs/read_only_file.hpp"
#include <bcrypt.h>
#include <delayimp.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
constexpr char library_name[]="winfsp-x64.dll";
constexpr wchar_t wide_library_name[]=L"winfsp-x64.dll";
constexpr std::string_view expected_digest=FLITZFS_VOLUME_APP_WINFSP_SHA256;
constexpr std::uint64_t expected_size=FLITZFS_VOLUME_APP_WINFSP_BYTES;
static_assert(expected_digest.size()==64 && expected_size<=(std::numeric_limits<ULONG>::max)());
// The delay import table and all mounted sessions borrow this module until
// process exit. On import failure, partially resolved addresses stay valid.
HMODULE qualified_module=nullptr;
bool imports_resolved=false;

FARPROC WINAPI qualified_delay_load(unsigned notice,PDelayLoadInfo details) {
  if(notice!=dliNotePreLoadLibrary || std::strcmp(details->szDll,library_name)!=0)return nullptr;
  if(!qualified_module) {
    RaiseException(VcppException(ERROR_SEVERITY_ERROR,ERROR_MOD_NOT_FOUND),EXCEPTION_NONCONTINUABLE,0,nullptr);
  }
  return reinterpret_cast<FARPROC>(qualified_module);
}

int delay_import_filter(DWORD code) noexcept {
  return code==VcppException(ERROR_SEVERITY_ERROR,ERROR_MOD_NOT_FOUND) ||
      code==VcppException(ERROR_SEVERITY_ERROR,ERROR_PROC_NOT_FOUND)?
      EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH;
}

HRESULT resolve_imports() noexcept {
  __try {return __HrLoadAllImportsForDll(library_name);}
  __except(delay_import_filter(GetExceptionCode())) {return E_NOINTERFACE;}
}

bool matches_sdk(const flitzfs::ReadOnlyFile& file) {
  if(file.size()!=expected_size)return false;
  std::vector<UCHAR> bytes(static_cast<std::size_t>(expected_size));
  file.read_exact_at(0,bytes.data(),bytes.size());
  std::array<UCHAR,32> digest{};
  if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()),
      digest.data(),static_cast<ULONG>(digest.size()))<0)
    throw std::runtime_error("Cannot verify the WinFsp runtime");
  constexpr char hex[]="0123456789abcdef";
  for(std::size_t i=0;i<digest.size();++i) {
    if(expected_digest[2*i]!=hex[digest[i]>>4] || expected_digest[2*i+1]!=hex[digest[i]&15U])return false;
  }
  return true;
}

}

extern "C" const PfnDliHook __pfnDliNotifyHook2=qualified_delay_load;

namespace flitzfs {
WinFspSessionApi load_volume_app_winfsp() {
  if(!qualified_module) {
    std::wstring executable(32768,L'\0');
    const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    if(!length || length>=executable.size())throw std::runtime_error("Cannot locate the FlitzFS application");
    executable.resize(length);
    const auto path=std::filesystem::path(executable).parent_path()/wide_library_name;
    const auto parents=flitzfs::pin_volume_path_parents(path.native());
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)
      throw std::runtime_error("The WinFsp runtime is unavailable beside FlitzFS");
    const auto file=flitzfs::ReadOnlyFile::open(path);
    if(!matches_sdk(file))throw std::runtime_error("The WinFsp runtime differs from this FlitzFS build");
    DWORD previous=0;
    if(!SetThreadErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX|SEM_NOGPFAULTERRORBOX,&previous))
      throw std::runtime_error("Cannot prepare unattended WinFsp runtime loading");
    qualified_module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetThreadErrorMode(previous,nullptr);
    if(!qualified_module)throw std::runtime_error("The verified WinFsp runtime could not load");
    imports_resolved=SUCCEEDED(resolve_imports());
  }
  if(!imports_resolved)throw std::runtime_error("The WinFsp runtime lacks the required interface");
  return linked_winfsp_session_api();
}

std::unique_ptr<VolumeAttachment> make_volume_app_attachment() {
  return make_winfsp_memory_attachment(load_volume_app_winfsp);
}
}
