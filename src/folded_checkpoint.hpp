#pragma once

#include "flitzfs/object_store.hpp"
#include "record_digest.hpp"

namespace flitzfs::detail {

inline constexpr std::uint64_t kFoldedMirrorRoots=2*kStoreCommitUnit;

// The complete mirrored image keeps its root area and uses paired units
// thereafter. Both addresses in each pair map to one folded unit.
constexpr std::uint64_t folded_mirror_offset(std::uint64_t offset,std::uint64_t unit) noexcept {
  if(offset<kFoldedMirrorRoots)return offset;
  const auto relative=offset-kFoldedMirrorRoots;
  return kFoldedMirrorRoots+(relative/(2*unit))*unit+relative%unit;
}
constexpr std::uint64_t folded_mirror_extent(std::uint64_t mirrored_end) noexcept {
  return kFoldedMirrorRoots+(mirrored_end-kFoldedMirrorRoots)/2;
}

struct FoldedCheckpoint {
  std::uint64_t virtual_end=0,generation=0;
  std::uint32_t unit=0;
  Digest roots{};
};

} // namespace flitzfs::detail
