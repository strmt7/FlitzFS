#pragma once
#include <string_view>

namespace flitzfs {
// Pure lexical check for host Windows file paths, NOT virtual catalog names.
// No file/device is opened. Parent reparse points and concurrent path mutation
// are outside this check; callers must still validate the actual opened handle.
bool ordinary_windows_file_path(std::wstring_view path) noexcept;
} // namespace flitzfs
