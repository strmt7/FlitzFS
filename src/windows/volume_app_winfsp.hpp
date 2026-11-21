#pragma once
#include "winfsp_volume_attachment.hpp"

namespace flitzfs {
// Bind the verified user-mode SDK without creating a filesystem or dispatcher.
WinFspSessionApi load_volume_app_winfsp();
std::unique_ptr<VolumeAttachment> make_volume_app_attachment();
#ifdef FLITZFS_EXPLORER_FILE_BACKING
std::unique_ptr<VolumeAttachment> make_volume_app_file_attachment(const VolumeFileRequest&,
    WinFspSessionApi (*load_api)()=load_volume_app_winfsp);
#endif
}
