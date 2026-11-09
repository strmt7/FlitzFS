#pragma once
#include "flitzfs/native_filesystem.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
namespace native_fixture {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
inline StoreIdentity identity(){StoreIdentity id{};id[0]=std::byte{43};id[1]=std::byte{91};return id;}
inline Bytes data(std::size_t n,unsigned seed) {
  Bytes out(n);std::uint64_t x=0x452821e638d01377ULL+seed;
  for(auto& b:out){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>21);}
  return out;
}
inline NativeFileMutation file_create(std::string path,bool directory=false) {
  NativeFileMutation op;op.operation=directory?NativeFileOperation::create_directory:NativeFileOperation::create_file;op.path=std::move(path);return op;
}
inline NativeFileMutation file_write(std::string path,std::uint64_t offset,Bytes data) {
  NativeFileMutation op;op.operation=NativeFileOperation::write;op.path=std::move(path);op.offset=offset;op.data=std::move(data);return op;
}
inline NativeFileMutation file_truncate(std::string path,std::uint64_t size) {
  NativeFileMutation op;op.operation=NativeFileOperation::truncate;op.path=std::move(path);op.size=size;return op;
}
inline NativeFileMutation file_rename(std::string path,std::string target,bool replace=false) {
  NativeFileMutation op;op.operation=NativeFileOperation::rename;op.path=std::move(path);op.target=std::move(target);op.replace=replace;return op;
}
inline NativeFileMutation file_remove(std::string path) {
  NativeFileMutation op;op.operation=NativeFileOperation::remove;op.path=std::move(path);return op;
}
inline NativeFsCommit file_apply(NativeFilesystemWriter& writer,NativeFileMutation op){return writer.apply({&op,1});}
class Ram final:public StoreByteIo {
 public:
  Bytes live,stable;std::size_t calls=0,writes=0,fail_at=(std::numeric_limits<std::size_t>::max)();
  mutable std::uint64_t read_bytes=0,read_calls=0;
  std::uint64_t write_bytes=0;std::size_t flushes=0;
  std::uint64_t corrupt_read_offset=(std::numeric_limits<std::uint64_t>::max)(),corrupt_read_number=0;
  mutable std::uint64_t matching_reads=0;
  unsigned failure=0;
  std::uint64_t extent() const override{return live.size();}
  StoreIoContract contract() const noexcept override{return {65536,true};}
  void admit_write(std::uint64_t projected) override{if(projected>kStoreMaxPhysicalBytes)throw std::runtime_error("RAM admission");}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{
    if(at>live.size() || out.size()>live.size()-at)throw std::runtime_error("RAM read");
    read_bytes+=out.size();++read_calls;
    std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),out.size(),out.begin());
    // Synthetic bad backing bytes on a selected repeated read, never owner data.
    if(at==corrupt_read_offset && ++matching_reads==corrupt_read_number && !out.empty())out[0]^=std::byte{1};
  }
  static void copy(Bytes& to,std::uint64_t at,std::span<const std::byte> in) {
    if(at>kStoreMaxPhysicalBytes || in.size()>kStoreMaxPhysicalBytes-at)throw std::runtime_error("RAM cap");
    if(at+in.size()>to.size())to.resize(static_cast<std::size_t>(at+in.size()));
    std::copy(in.begin(),in.end(),to.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    const bool fail=calls++==fail_at;++writes;write_bytes+=in.size();
    if(fail) {
      if(failure==1){copy(live,at,in.first(in.size()/2));copy(stable,at,in.first(in.size()/2));}
      if(failure==2) {
        const auto unit=at/65536*65536;
        if(stable.size()<unit+65536)stable.resize(static_cast<std::size_t>(unit+65536));
        std::fill_n(stable.begin()+static_cast<std::ptrdiff_t>(unit),65536,std::byte{0});live=stable;
      }
      throw std::runtime_error("injected native write failure");
    }
    copy(live,at,in);
  }
  void flush() override {
    ++flushes;const bool fail=calls++==fail_at;if(fail && failure==0)throw std::runtime_error("injected native flush failure");
    stable=live;if(fail)throw std::runtime_error("injected native post-flush failure");
  }
};
inline std::shared_ptr<Ram> image(Bytes bytes){auto ram=std::make_shared<Ram>();ram->live=bytes;ram->stable=std::move(bytes);return ram;}
inline std::vector<NativeFileMutation> baseline() {
  return {file_create("A",true),file_create("A/file.bin"),file_write("A/file.bin",0,data(17,1)),file_create("target.bin"),file_write("target.bin",0,data(31,2))};
}
inline std::vector<NativeFileMutation> update() {
  return {file_rename("A","B"),file_write("B/file.bin",65532,data(77,3)),file_truncate("B/file.bin",65545),
          file_rename("B/file.bin","target.bin",true),file_remove("B")};
}
inline Bytes final_bytes() {
  Bytes out(65545);const auto old=data(17,1),addition=data(77,3);
  std::copy(old.begin(),old.end(),out.begin());std::copy_n(addition.begin(),13,out.begin()+65532);return out;
}
inline void expected(const NativeFilesystemSnapshot& snapshot,bool updated,bool continued=false) {
  const auto entries=snapshot.list();
  if(entries.size()!=(updated?1U:2U)+(continued?1U:0U))throw std::runtime_error("partial native namespace");
  if(updated) {
    if(snapshot.read("target.bin",0,100000)!=final_bytes())throw std::runtime_error("native replaced content mismatch");
    for(const auto* path:{"A","B"}) {
      bool missing=false;try{(void)snapshot.stat(path);}catch(const NativeFsError& e){missing=e.code()==NativeFsErrorCode::not_found;}
      if(!missing)throw std::runtime_error("native old directory leaked");
    }
  } else if(snapshot.read("A/file.bin",0,100000)!=data(17,1) || snapshot.read("target.bin",0,100000)!=data(31,2))
    throw std::runtime_error("native acknowledged bytes changed");
  if(continued && snapshot.read("continued",0,4000)!=data(3073,91))throw std::runtime_error("native continuation bytes");
}
}
