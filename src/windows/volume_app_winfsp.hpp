#pragma once
#include "winfsp_volume_attachment.hpp"

namespace flitzfs {
// Bind the verified user-mode SDK without creating a filesystem or dispatcher.
WinFspSessionApi load_volume_app_winfsp();
std::unique_ptr<VolumeAttachment> make_volume_app_attachment();
}
