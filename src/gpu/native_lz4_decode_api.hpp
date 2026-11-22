#pragma once
#include "native_lz4_decode_layout.hpp"
#include <type_traits>

namespace flitzfs::native_lz4_decode {
using Kernel=void (*)(u8*,u64,Layout);
struct KernelApi {
  u32 size_bytes,abi,layout_bytes,record_bytes;
  Kernel wave32,wave64;
};
static_assert(std::is_standard_layout_v<KernelApi> && std::is_trivially_copyable_v<KernelApi>);
inline bool compatible(const KernelApi& value) noexcept {
  return value.size_bytes==sizeof(KernelApi) && value.abi==version &&
      value.layout_bytes==sizeof(Layout) && value.record_bytes==sizeof(Record) &&
      value.wave32 && value.wave64;
}
using KernelApiEntry=const KernelApi* (*)() noexcept;
}
