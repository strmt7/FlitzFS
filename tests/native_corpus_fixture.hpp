#pragma once

#include "flitzfs/native_filesystem.hpp"
#include "flitzfs/read_only_file.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace native_fixture {

class NamedCorpusFixture final {
 public:
  struct File {
    std::string path;
    std::uint64_t offset,bytes;
  };

  explicit NamedCorpusFixture(const std::filesystem::path& path):input_(flitzfs::ReadOnlyFile::open(path)) {
    if(input_.size()<12 || input_.size()>96ULL*1024*1024)throw std::invalid_argument("named corpus size");
    const auto header=take(0,12);
    const auto marker=std::string_view(reinterpret_cast<const char*>(header.data()),8);
    const auto count=number(header,8);
    if(marker!="FLSTREE1" || !count || count>4096)throw std::invalid_argument("named corpus header");
    files_.reserve(count);
    std::uint64_t offset=12;
    std::set<std::string> names;
    for(std::uint32_t i=0;i<count;++i) {
      const auto entry=take(offset,8);
      offset+=8;
      const auto length=number(entry,0),bytes=number(entry,4);
      if(!length || length>1024 || bytes>1024*1024)throw std::invalid_argument("named corpus record size");
      const auto data=take(offset,length);
      offset+=length;
      if(bytes>input_.size()-offset)throw std::invalid_argument("truncated named corpus file");
      std::string name(reinterpret_cast<const char*>(data.data()),data.size());
      if(name.find_first_of("\\:")!=std::string::npos || name.find('\0')!=std::string::npos || !names.insert(name).second)
        throw std::invalid_argument("invalid named corpus path");
      std::size_t first=0;
      for(;;) {
        const auto end=name.find('/',first);
        const auto component=std::string_view(name).substr(first,end==std::string::npos?end:end-first);
        if(component.empty() || component=="." || component=="..")
          throw std::invalid_argument("invalid named corpus component");
        if(end==std::string::npos)break;
        directories_.insert(name.substr(0,end));
        first=end+1;
      }
      files_.push_back({std::move(name),offset,bytes});
      offset+=bytes;
      logical_bytes_+=bytes;
    }
    if(offset!=input_.size())throw std::invalid_argument("trailing named corpus bytes");
    for(const auto& directory:directories_)
      if(names.contains(directory))throw std::invalid_argument("named corpus directory is also a file");
  }

  const std::vector<File>& files() const noexcept {return files_;}
  const std::set<std::string>& directories() const noexcept {return directories_;}
  std::uint64_t logical_bytes() const noexcept {return logical_bytes_;}
  std::vector<std::byte> read(const File& file) const {return take(file.offset,static_cast<std::size_t>(file.bytes));}

 private:
  static std::uint32_t number(std::span<const std::byte> bytes,std::size_t at) noexcept {
    std::uint32_t value=0;
    for(unsigned i=0;i<4;++i)value|=std::to_integer<std::uint32_t>(bytes[at+i])<<(8*i);
    return value;
  }
  std::vector<std::byte> take(std::uint64_t offset,std::size_t bytes) const {
    if(offset>input_.size() || bytes>input_.size()-offset)throw std::invalid_argument("truncated named corpus");
    std::vector<std::byte> data(bytes);
    if(bytes)input_.read_exact_at(offset,data.data(),data.size());
    return data;
  }
  flitzfs::ReadOnlyFile input_;
  std::vector<File> files_;
  std::set<std::string> directories_;
  std::uint64_t logical_bytes_=0;
};

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
