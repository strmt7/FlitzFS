#pragma once
#include <string_view>

namespace flitzfs {
// Compare already validated ASCII directory names in enumeration order.
// Dot entries precede every ordinary name, even leading spaces/punctuation.
inline bool directory_name_after(std::string_view name, std::string_view marker) noexcept {
  const auto rank = [](std::string_view value) { return value == "." ? 0 : value == ".." ? 1 : 2; };
  if (rank(name) != rank(marker)) return rank(name) > rank(marker);
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  const auto length = name.size() < marker.size() ? name.size() : marker.size();
  for (std::size_t i = 0; i < length; ++i) {
    if (lower(name[i]) != lower(marker[i])) return lower(name[i]) > lower(marker[i]);
  }
  return name.size() > marker.size();
}
} // namespace flitzfs
