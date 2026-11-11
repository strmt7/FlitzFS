#include "record_digest.hpp"
#include "blake3.h"
namespace flitzfs::detail {
Digest record_digest(std::span<const std::byte> first,std::span<const std::byte> second) noexcept {
  static_assert(BLAKE3_OUT_LEN == 32);
  blake3_hasher state;
  blake3_hasher_init(&state);
  if(!first.empty()) blake3_hasher_update(&state,first.data(),first.size());
  if(!second.empty()) blake3_hasher_update(&state,second.data(),second.size());
  Digest result{};
  blake3_hasher_finalize(&state,reinterpret_cast<std::uint8_t*>(result.data()),result.size());
  return result;
}
}
