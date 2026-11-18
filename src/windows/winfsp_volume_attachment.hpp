#pragma once
#include "volume_attachment.hpp"
#include "winfsp_session.hpp"

namespace flitzfs {
// The application selects its qualified runtime. SDK tests supply the same ABI
// operations without starting a driver or creating a mount.
std::unique_ptr<VolumeAttachment> make_winfsp_memory_attachment(WinFspSessionApi (*load_api)());
}
