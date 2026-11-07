#pragma once
// Fixed-width host/device task ABI. No runtime dependency.
namespace flitzfs::gpu_detail {
using u32 = unsigned int;
using u8 = unsigned char;
static_assert(sizeof(u32) == 4 && sizeof(u8) == 1, "GPU ABI needs 32/8-bit integers");
inline constexpr u32 abi_version = 1;
inline constexpr u32 max_block_bytes = 65536;
inline constexpr u32 max_batch_bytes = 262144;
inline constexpr u32 max_tasks = 72; // ceil(total/4096) + at most seven partial blocks
inline constexpr u32 segment_bytes = 4096;
inline constexpr u32 pending = 0xffffffffU;
struct Task {
  u32 input_offset, input_bytes, output_offset, decoded_bytes;
  u32 segment_offset, segment_length, kind, fill;
};
static_assert(sizeof(Task) == 32, "GPU task layout changed");
enum Status : u32 { success=0, bad_extent=1, bad_kind=2, bad_table=3, truncated=4, bad_code=5 };

} // namespace flitzfs::gpu_detail
