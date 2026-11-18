#pragma once
#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/uniform_lz4_wire.hpp"

namespace flitzfs::detail {

inline constexpr std::size_t kAnchorLz4BlocksPerMember=4;

// Each member uses two target halves and both halves of the same anchor.
// The caller admits the complete expanded source and worker buffers first.
PreparedUniformLz4 prepare_native_anchor_search(std::span<const std::byte> anchor,
    std::span<const std::byte> members,const UniformLz4Limits&);

// Interpret already verified LZ4 blocks as optional hints into the anchor.
// Empty or unusable replies produce no advice. The indexed writer checks each
// range against its own live bytes before constructing any persistent frame.
std::vector<NativeSharedBaseCopyRange> native_anchor_copy_ranges(
    std::span<const UniformLz4Bytes,kAnchorLz4BlocksPerMember>);

} // namespace flitzfs::detail
