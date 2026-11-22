#pragma once

#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/read_only_file.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace native_fixture {

inline std::vector<std::vector<std::byte>> read_packed_corpus(
    const std::filesystem::path& filename,bool complete_files) {
  const auto source=flitzfs::ReadOnlyFile::open(filename);
  const auto input_limit=complete_files?48ULL*1024*1024:4ULL*1024*1024;
  const auto file_limit=complete_files?flitzfs::kNativeMaxReadBytes:flitzfs::kNativeExtentBytes-1;
  if(source.size()<=4 || source.size()>input_limit)
    throw std::invalid_argument("public pack corpus size");
  std::uint64_t offset=0;
  const auto read_size=[&]() {
    std::array<std::byte,4> raw{};
    if(raw.size()>source.size()-offset)throw std::invalid_argument("truncated public pack corpus");
    source.read_exact_at(offset,raw.data(),raw.size());
    offset+=raw.size();
    std::uint32_t value=0;
    for(unsigned i=0;i<raw.size();++i)value|=std::to_integer<std::uint32_t>(raw[i])<<(i*8);
    return value;
  };
  const auto count=read_size();
  if(count<2 || count>192)throw std::invalid_argument("public pack corpus file count");
  std::vector<std::vector<std::byte>> files;
  files.reserve(count);
  for(std::uint32_t i=0;i<count;++i) {
    const auto size=read_size();
    if(!size || size>file_limit || size>source.size()-offset)
      throw std::invalid_argument("public pack corpus file extent");
    auto& file=files.emplace_back(size);
    source.read_exact_at(offset,file.data(),file.size());
    offset+=size;
  }
  if(offset!=source.size())throw std::invalid_argument("public pack corpus has trailing bytes");
  return files;
}

}
