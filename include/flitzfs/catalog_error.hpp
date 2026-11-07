#pragma once
#include <stdexcept>

namespace flitzfs {
// Typed failures let OS adapters return statuses without parsing error messages.
enum class CatalogErrorCode { invalid_path, not_found, not_directory, is_directory, limit };
class CatalogError final : public std::runtime_error {
 public:
  CatalogError(CatalogErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}
  CatalogErrorCode code() const noexcept { return code_; }
 private:
  CatalogErrorCode code_;
};
} // namespace flitzfs
