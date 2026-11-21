#pragma once
#include "volume_attachment.hpp"
#include "winfsp_session.hpp"
#include <filesystem>
#include <variant>

namespace flitzfs {
// The application selects its qualified runtime. SDK tests supply the same ABI
// operations without starting a driver or creating a mount.
std::unique_ptr<VolumeAttachment> make_winfsp_memory_attachment(WinFspSessionApi (*load_api)());

struct WinFspVolumeFileSelection {
  struct WindowsFile {
    std::filesystem::path path;
    StoreIoContract qualification;
  };
  struct Create {
    StoreIdentity image;
    PagedStoreLimits limits;
  };
  // The caller supplies a qualified backing. An existing file can select its
  // identity from its checked head while its exclusive lease is held.
  // Create is consumed once an exclusive backing has been acquired.
  std::variant<std::shared_ptr<StoreByteIo>,WindowsFile> backing;
  StoreIdentity volume;
  std::optional<Create> create;
};

std::unique_ptr<VolumeAttachment> make_winfsp_volume_file_attachment(WinFspSessionApi (*load_api)(),
    WinFspVolumeFileSelection);
}
