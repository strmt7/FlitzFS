#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace flitzfs::detail {
// Indexed name profile 1 fits the WinFsp request's UTF-16 path, including
// the leading separator and terminating NUL. The stored strings are UTF-8.
inline constexpr std::uint32_t kIndexedOrdinalNames=1;
inline constexpr std::size_t kIndexedNameUnits=255;
inline constexpr std::size_t kIndexedPathUnits=1022;
inline constexpr std::size_t kIndexedNameBytes=3*kIndexedNameUnits;
inline constexpr std::size_t kIndexedPathDepth=16;

struct IndexedPathName {
  std::string key;
  std::size_t utf16_units=0;
  bool extended=false;
};

IndexedPathName indexed_path_name(std::string_view,bool allow_root=false);
std::string indexed_component_key(std::string_view);
std::size_t indexed_utf16_length(std::string_view);
// Versioned ordinal equivalence used by the persistent index.
char32_t indexed_ordinal_key(char32_t) noexcept;
}
