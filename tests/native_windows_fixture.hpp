#pragma once
// Synthetic native-image fixtures ONLY on disposable hosted Windows VMs.
#include "flitzfs/native_filesystem.hpp"
#include <windows.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace native_windows_fixture {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
inline void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline bool environment_is(PCWSTR name,std::wstring_view expected) {
  wchar_t value[64]{};const auto n=GetEnvironmentVariableW(name,value,64);
  return n>0 && n<64 && std::wstring_view(value,n)==expected;
}
inline void hosted() {
  require(environment_is(L"GITHUB_ACTIONS",L"true") &&
      environment_is(L"RUNNER_ENVIRONMENT",L"github-hosted"),
      "native Windows image fixture requires a disposable hosted VM");
}
inline StoreIdentity identity() {
  StoreIdentity result{};result[0]=std::byte{0x4e};result[15]=std::byte{0x57};return result;
}
inline Bytes data() {
  Bytes bytes(192*1024+17);
  for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>((i/31+i%251)%256);
  return bytes;
}
inline Bytes tail() {return Bytes(17,std::byte{0x5b});}
inline NativeFileMutation create(std::string path,bool directory=false) {
  NativeFileMutation m;m.operation=directory?NativeFileOperation::create_directory:
      NativeFileOperation::create_file;m.path=std::move(path);return m;
}
inline NativeFileMutation write(std::string path,std::uint64_t offset,Bytes bytes) {
  NativeFileMutation m;m.operation=NativeFileOperation::write;m.path=std::move(path);
  m.offset=offset;m.data=std::move(bytes);return m;
}
inline NativeFileMutation truncate(std::string path,std::uint64_t size) {
  NativeFileMutation m;m.operation=NativeFileOperation::truncate;m.path=std::move(path);
  m.size=size;return m;
}
struct Expected {
  std::uint64_t sample_inode=0,directory_inode=0,sparse_inode=0,sequence=0;
  StoreCommitInfo storage;
};
inline Expected make_image(const std::filesystem::path& image,bool require_zstd,
    std::uint64_t sparse_bytes=kNativeMaxFileBytes) {
  hosted();require(sparse_bytes>=31 && sparse_bytes<=kNativeMaxFileBytes,"native sparse fixture bounds");
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create_file(
      image,identity(),{4096,true},{StoreCompressionMode::automatic,6,require_zstd}));
  const std::array<NativeFileMutation,9> initial{{
      create("discard"),create("Data",true),create("scratch.bin"),create("Data/other.txt"),
      create("empty.bin"),create("sparse.bin"),write("scratch.bin",0,data()),
      truncate("sparse.bin",sparse_bytes),
      write("sparse.bin",sparse_bytes-tail().size(),tail())}};
  writer.apply(initial);
  NativeFileMutation remove;remove.operation=NativeFileOperation::remove;remove.path="discard";
  NativeFileMutation rename;rename.operation=NativeFileOperation::rename;
  rename.path="scratch.bin";rename.target="Data/sample.bin";
  const std::array<NativeFileMutation,2> final{remove,rename};writer.apply(final);
  auto snapshot=writer.snapshot();snapshot.verify_all();
  Expected result{snapshot.stat("Data/sample.bin").inode,snapshot.stat("Data").inode,
      snapshot.stat("sparse.bin").inode,snapshot.sequence(),snapshot.storage_info()};
  require(result.sample_inode==4 && result.directory_inode==3 &&
      result.sequence==3 && result.storage.replicated_checkpoint &&
      !result.storage.degraded_checkpoint,"native fixture persistent identity/checkpoint");
  require(result.storage.physical_end<=1536*1024,"native fixture image budget");
  return result; // Destroy snapshot/writer and release writable OS handle BEFORE readers.
}
inline Bytes read_image(const std::filesystem::path& image) {
  const auto size=std::filesystem::file_size(image);
  require(size>0 && size<=1536*1024,"native fixture read budget");
  std::ifstream stream(image,std::ios::binary);
  Bytes result(static_cast<std::size_t>(size));
  stream.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(result.size()));
  require(bool(stream),"native fixture image read");return result;
}
struct Fixture {
  std::filesystem::path root;
  Fixture() {
    hosted();wchar_t temporary[512]{};
    const auto n=GetEnvironmentVariableW(L"RUNNER_TEMP",temporary,512);
    require(n && n<512,"owned runner temp unavailable");
    for(unsigned attempt=0;attempt<100 && root.empty();++attempt) {
      auto path=std::filesystem::path(std::wstring(temporary,n))/
          (L"flitzfs-native-view-"+std::to_wstring(GetCurrentProcessId())+L"-"+
           std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(attempt));
      if(std::filesystem::create_directory(path))root=path;
    }
    require(!root.empty(),"new native view fixture required");
  }
  std::filesystem::path image() const{return root/L"image.fltx";}
  ~Fixture() {
    std::error_code ignored;
    for(const auto* leaf:{L"image.fltx",L"damaged.fltx",L"generic.fltx",L"mapped.fltx"})
      std::filesystem::remove(root/leaf,ignored);
    std::filesystem::remove(root,ignored); // Never recursive.
  }
};
}
