#include "flitzfs/windows_path.hpp"
#include <cstddef>
#include <initializer_list>
#include <string_view>

namespace flitzfs {
namespace {
bool separator(wchar_t c) noexcept { return c == L'/' || c == L'\\'; }
wchar_t upper(wchar_t c) noexcept {
  return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}
bool equal_ascii(std::wstring_view value, std::wstring_view expected) noexcept {
  if (value.size() != expected.size()) return false;
  for (std::size_t i = 0; i < value.size(); ++i) if (upper(value[i]) != expected[i]) return false;
  return true;
}
bool reserved(std::wstring_view component) noexcept {
  auto stem = component.substr(0, component.find(L'.'));
  while (!stem.empty() && stem.back() == L' ') stem.remove_suffix(1);
  for (const auto* name : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$", L"CLOCK$"})
    if (equal_ascii(stem, name)) return true;
  if (stem.size() != 4 || (!equal_ascii(stem.substr(0, 3), L"COM") &&
                           !equal_ascii(stem.substr(0, 3), L"LPT"))) return false;
  const auto digit = stem[3];
  return (digit >= L'0' && digit <= L'9') || digit == L'\u00b9' ||
      digit == L'\u00b2' || digit == L'\u00b3';
}
} // namespace
bool ordinary_windows_file_path(std::wstring_view path) noexcept {
  if (path.empty() || path.size() > 32767 || separator(path.back())) return false;
  std::size_t start = 0;
  if (path.size() >= 2 && path[1] == L':') {
    const auto drive = upper(path[0]);
    if (drive < L'A' || drive > L'Z' || path.size() < 4 || !separator(path[2])) return false;
    start = 3;
  } else if (separator(path[0])) {
    // Reject UNC/device/extended prefixes, including mixed slash spellings.
    if (path.size() < 2 || separator(path[1])) return false;
    start = 1;
  }
  while (start < path.size()) {
    auto stop = start;
    for (; stop < path.size() && !separator(path[stop]); ++stop) {
      const auto c = path[stop];
      if (c < 0x20 || c == L':' || c == L'"' || c == L'<' || c == L'>' ||
          c == L'|' || c == L'?' || c == L'*') return false;
    }
    const auto part = path.substr(start, stop - start);
    if (part.empty() || part.size() > 255) return false;
    if (part == L"." || part == L"..") {
      if (stop == path.size()) return false;
    } else if (part.back() == L'.' || part.back() == L' ' || reserved(part)) return false;
    start = stop + 1;
  }
  return true;
}
} // namespace flitzfs
