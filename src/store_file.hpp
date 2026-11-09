#pragma once
#include "flitzfs/object_store.hpp"
namespace flitzfs::detail {
std::shared_ptr<StoreByteIo> new_store_file(const std::filesystem::path&,StoreIoContract);
std::shared_ptr<StoreByteIo> read_store_file(const std::filesystem::path&);
}
