#include "volume_app_winfsp.hpp"
#include "volume_directory_pin.hpp"
#include "flitzfs/read_only_file.hpp"
#ifdef FLITZFS_EXPLORER_FILE_BACKING
#include "memory_volume_factory.hpp"
#include "../store_file.hpp"
#include "flitzfs/native_volume_file.hpp"
#include <algorithm>
#endif
#include <bcrypt.h>
#include <delayimp.h>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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
std::filesystem::path qualified_path;

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

std::filesystem::path bundled_runtime_path() {
  std::wstring executable(32768,L'\0');
  const auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
  if(!length || length>=executable.size())throw std::runtime_error("Cannot locate the FlitzFS application");
  executable.resize(length);
  return std::filesystem::path(executable).parent_path()/wide_library_name;
}

class VerifiedRuntime {
 public:
  VerifiedRuntime(std::filesystem::path path,const char* unavailable,const char* mismatch):
      path_(std::move(path)),parents_(flitzfs::pin_volume_path_parents(path_.native())),
      file_(open_runtime(path_,unavailable)) {
    if(!matches_sdk(file_))throw std::runtime_error(mismatch);
  }

  const std::filesystem::path& path() const noexcept {return path_;}

 private:
  static flitzfs::ReadOnlyFile open_runtime(const std::filesystem::path& path,const char* unavailable) {
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)throw std::runtime_error(unavailable);
    return flitzfs::ReadOnlyFile::open(path);
  }

  std::filesystem::path path_;
  std::vector<flitzfs::VolumeDirectoryPin> parents_;
  flitzfs::ReadOnlyFile file_;
};

VerifiedRuntime bundled_runtime() {
  return {bundled_runtime_path(),"The WinFsp runtime is unavailable beside FlitzFS",
      "The WinFsp runtime differs from this FlitzFS build"};
}

std::filesystem::path registered_runtime_path() {
  // Use the vendor's registered installation root. Its bin directory can be
  // a junction to the currently selected side-by-side deployment.
  wchar_t root[MAX_PATH]{};
  DWORD bytes=sizeof(root);
  if(RegGetValueW(HKEY_LOCAL_MACHINE,L"" FSP_FSCTL_PRODUCT_FULL_REGKEY,L"InstallDir",
      RRF_RT_REG_SZ,nullptr,root,&bytes)!=ERROR_SUCCESS)
    throw std::runtime_error("WinFsp is not registered for Explorer attachment");
  const auto bin=std::filesystem::path(root)/L"bin";
  if(!bin.is_absolute() || !flitzfs::ordinary_windows_file_path(bin.native()))
    throw std::runtime_error("The registered WinFsp installation directory is unsuitable");
  const auto directory=CreateFileW(bin.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
      nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
  if(directory==INVALID_HANDLE_VALUE)
    throw std::runtime_error("The registered WinFsp runtime directory is unavailable");
  wchar_t resolved[MAX_PATH+4]{};
  const auto length=GetFinalPathNameByHandleW(directory,resolved,static_cast<DWORD>(std::size(resolved)),
      FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  CloseHandle(directory);
  constexpr std::wstring_view prefix=LR"(\\?\)";
  if(!length || length>=std::size(resolved) || !std::wstring_view(resolved,length).starts_with(prefix))
    throw std::runtime_error("The registered WinFsp runtime directory could not be resolved");
  const auto path=std::filesystem::path(std::wstring_view(resolved,length).substr(prefix.size()))/
      wide_library_name;
  // WinFsp's side-by-side lookup resolves the DLL path in a MAX_PATH buffer.
  if(!flitzfs::ordinary_windows_file_path(path.native()) || path.native().size()>=MAX_PATH)
    throw std::runtime_error("The registered WinFsp runtime path is unsuitable");
  return path;
}

flitzfs::WinFspSessionApi load_qualified_runtime(const VerifiedRuntime& runtime) {
  if(!qualified_module) {
    qualified_path=runtime.path();
    DWORD previous=0;
    if(!SetThreadErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX|SEM_NOGPFAULTERRORBOX,&previous))
      throw std::runtime_error("Cannot prepare unattended WinFsp runtime loading");
    qualified_module=LoadLibraryExW(qualified_path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetThreadErrorMode(previous,nullptr);
    if(!qualified_module)throw std::runtime_error("The verified WinFsp runtime could not load");
    imports_resolved=SUCCEEDED(resolve_imports());
  } else if(qualified_path!=runtime.path()) {
    throw std::runtime_error("The WinFsp runtime location changed. Restart FlitzFS before attaching");
  }
  if(!imports_resolved)throw std::runtime_error("The WinFsp runtime lacks the required interface");
  return flitzfs::linked_winfsp_session_api();
}

}

extern "C" const PfnDliHook __pfnDliNotifyHook2=qualified_delay_load;

namespace flitzfs {
WinFspSessionApi load_volume_app_winfsp() {
  return load_qualified_runtime(bundled_runtime());
}

WinFspSessionApi load_volume_app_mount_winfsp() {
  const auto bundled=bundled_runtime();
  const VerifiedRuntime registered(registered_runtime_path(),"The registered WinFsp runtime is unavailable",
      "The registered WinFsp runtime differs from this FlitzFS build. Use a matching FlitzFS bundle");
  const auto api=load_qualified_runtime(registered);
  const auto deployment=registered.path().parent_path().parent_path();
  const auto identity=FspSxsIdent();
  if(!identity || !*identity || _wcsicmp(deployment.parent_path().filename().c_str(),L"SxS")!=0 ||
      _wcsicmp(deployment.filename().c_str(),(std::wstring(L"sxs.")+identity).c_str())!=0)
    throw std::runtime_error("The registered WinFsp runtime has no matching side-by-side driver identity");

  UINT32 runtime_version=0,driver_version=0;
  if(!NT_SUCCESS(FspVersion(&runtime_version)) || !NT_SUCCESS(FspFsctlServiceVersion(&driver_version)) ||
      runtime_version!=driver_version)
    throw std::runtime_error("The registered WinFsp driver version could not be matched to its runtime");
  return api;
}

std::unique_ptr<VolumeAttachment> make_volume_app_attachment() {
  return make_winfsp_memory_attachment(load_volume_app_mount_winfsp);
}

#ifdef FLITZFS_EXPLORER_FILE_BACKING
std::unique_ptr<VolumeAttachment> make_volume_app_file_attachment(const VolumeFileRequest& request,
    WinFspSessionApi (*load_api)()) {
  auto path=request.path;
  path.make_preferred();
  if(!path.is_absolute() || path!=path.lexically_normal() || !ordinary_windows_file_path(path.native()))
    throw std::invalid_argument("Choose an ordinary local volume file");

  if(request.action!=VolumeFileAction::create_new && request.action!=VolumeFileAction::open_existing)
    throw std::invalid_argument("Choose how to open the volume file");
  const auto parents=pin_volume_path_parents(path.native());
  auto destination=detail::volume_file_destination(path);
  const auto& geometry=destination.geometry;
  if(!geometry.allocation_unit_bytes || kStoreCommitUnit%geometry.allocation_unit_bytes)
    throw std::runtime_error("The selected backing requires qualified NTFS allocation geometry");
  StoreIoContract contract{static_cast<std::uint32_t>(kStoreCommitUnit),true,
      geometry.isolated_sparse_ranges,true};
  WinFspVolumeFileSelection selection{};
  if(request.action==VolumeFileAction::create_new) {
    const auto extent=(std::min)(destination.available_growth,geometry.maximum_extent);
    auto limits=NativeVolumeFile::paired_image_limits(extent,true);
    std::optional<std::uint64_t> dense_physical_limit;
    if(limits) {
      dense_physical_limit=extent;
      contract.isolated_range_reclamation=false;
    } else if(contract.isolated_range_reclamation) {
      limits=NativeVolumeFile::paired_image_limits(extent,false);
      contract.isolated_tail_truncation=false;
    }
    if(!limits)
      throw std::runtime_error("The target is too small for an indexed volume");
    selection.volume=fresh_volume_identity();
    selection.create=WinFspVolumeFileSelection::Create{fresh_volume_identity(),*limits,dense_physical_limit};
  }
  selection.backing=WinFspVolumeFileSelection::WindowsFile{path,contract,std::move(destination.volume_root)};
  return make_winfsp_volume_file_attachment(load_api,std::move(selection));
}
#endif
}
