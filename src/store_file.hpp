#pragma once
#include "flitzfs/object_store.hpp"
namespace flitzfs::detail {
std::shared_ptr<StoreByteIo> new_store_file(const std::filesystem::path&,StoreIoContract);
std::shared_ptr<StoreByteIo> read_store_file(const std::filesystem::path&);
// Test-only owned-fixture adapter; requires a disposable hosted VM at runtime.
// No production factory, CLI route, or permission to modify user files.
std::shared_ptr<StoreByteIo> reopen_store_file_for_hosted_test(const std::filesystem::path&,StoreIoContract);
}
