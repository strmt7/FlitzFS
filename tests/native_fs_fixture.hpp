#pragma once
#include "flitzfs/native_filesystem.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <mutex>
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
inline Bytes cold_test_payload(unsigned salt=0,unsigned extent_salt_stride=0) {
  std::vector<Bytes> vocabulary(173);
  Bytes result;
  result.reserve(24*kNativeExtentBytes);
  for(unsigned i=0;i<24;++i) {
    if(!i || extent_salt_stride) {
      const auto extent_salt=salt+i*extent_salt_stride;
      for(unsigned word=0;word<vocabulary.size();++word)
        vocabulary[word]=data(11+word*29%75,82003+word*37+extent_salt);
    }
    const auto choices=data(kNativeExtentBytes,180451+i*5);
    const auto end=result.size()+kNativeExtentBytes;
    for(std::size_t j=0;result.size()<end;++j) {
      const auto key=std::to_integer<unsigned>(choices[j%choices.size()]);
      const auto& part=vocabulary[key%vocabulary.size()];
      const auto take=(std::min)(part.size(),end-result.size());
      result.insert(result.end(),part.begin(),part.begin()+static_cast<std::ptrdiff_t>(take));
      if(result.size()<end)result.push_back(choices[(j*47+19)%choices.size()]);
    }
  }
  return result;
}
inline void add_little_endian(std::span<std::byte> word,std::uint64_t value) {
  for(auto& byte:word) {
    const auto sum=std::to_integer<unsigned>(byte)+static_cast<unsigned>(value&255);
    byte=static_cast<std::byte>(sum&255);
    value=(value>>8)+(sum>>8);
  }
}
inline Bytes calibrated_extent(std::span<const std::byte> base,unsigned member) {
  Bytes bytes(base.begin(),base.end());
  if(!member)return bytes;
  for(std::size_t at=0;at<bytes.size();at+=4) {
    auto change=std::uint64_t{0x31ac05d9}*member;
    // A reproducible calibration shift with occasional positive and negative
    // residuals outside the sampled intervals.
    if(at%4096>=64)change+=(at/4+member)%3-std::uint64_t{1};
    add_little_endian(std::span(bytes).subspan(at,4),change);
  }
  return bytes;
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
  std::uint64_t read_bytes_at_flush=0;
  std::uint64_t write_bytes=0;std::size_t flushes=0;
  std::uint64_t capacity=kStoreMaxPhysicalBytes;
  static constexpr auto no_read_fault=(std::numeric_limits<std::uint64_t>::max)();
  mutable std::uint64_t corrupt_read_offset=no_read_fault,corrupt_read_number=0,corrupt_followup_offset=no_read_fault;
  mutable std::uint64_t matching_reads=0,corrupted_read_start=no_read_fault;
  unsigned failure=0;
  std::uint32_t failure_unit=65536; // Explicit model, default old behavior.
  std::uint64_t extent() const override{return live.size();}
  StoreIoContract contract() const noexcept override{return {failure_unit,true};}
  void admit_write(std::uint64_t projected) override{if(projected>capacity)throw std::runtime_error("RAM admission");}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{
    if(at>live.size() || out.size()>live.size()-at)throw std::runtime_error("RAM read");
    read_bytes+=out.size();++read_calls;
    std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),out.size(),out.begin());
    // Count every read covering the selected byte, irrespective of the
    // caller's batching. Corrupt just that byte on the selected observation.
    if(corrupt_read_offset>=at && corrupt_read_offset-at<out.size() && ++matching_reads==corrupt_read_number) {
      corrupted_read_start=at;
      out[static_cast<std::size_t>(corrupt_read_offset-at)]^=std::byte{1};
      if(corrupt_followup_offset!=no_read_fault) {
        corrupt_read_offset=corrupt_followup_offset;
        corrupt_followup_offset=no_read_fault;
        ++corrupt_read_number;
      }
    }
  }
  static void copy(Bytes& to,std::uint64_t at,std::span<const std::byte> in,
      std::uint64_t maximum=kStoreMaxPhysicalBytes) {
    if(at>maximum || in.size()>maximum-at)throw std::runtime_error("RAM cap");
    if(at+in.size()>to.size())to.resize(static_cast<std::size_t>(at+in.size()));
    std::copy(in.begin(),in.end(),to.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override {
    const bool fail=calls++==fail_at;++writes;write_bytes+=in.size();
    if(fail) {
      if(failure==1){copy(live,at,in.first(in.size()/2),capacity);copy(stable,at,in.first(in.size()/2),capacity);}
      if(failure==2) {
        const auto unit=at/failure_unit*failure_unit;
        if(stable.size()<unit+failure_unit)stable.resize(static_cast<std::size_t>(unit+failure_unit));
        std::fill_n(stable.begin()+static_cast<std::ptrdiff_t>(unit),failure_unit,std::byte{0});live=stable;
      }
      throw std::runtime_error("injected native write failure");
    }
    copy(live,at,in,capacity);
  }
  void flush() override {
    read_bytes_at_flush=read_bytes;
    ++flushes;const bool fail=calls++==fail_at;if(fail && failure==0)throw std::runtime_error("injected native flush failure");
    stable=live;if(fail)throw std::runtime_error("injected native post-flush failure");
  }
};
// All access to the modeled bytes goes through this provider during a test
// with concurrent I/O. The fixture may inspect the model after readers join.
class ConcurrentRam:public StoreByteIo {
 public:
  explicit ConcurrentRam(std::shared_ptr<Ram> ram):ram_(std::move(ram)) {}
  std::uint64_t extent() const override{std::lock_guard lock(mutex_);return ram_->extent();}
  StoreIoContract contract() const noexcept override{return ram_->contract();}
  bool concurrent_reads_supported() const noexcept override{return true;}
  void admit_write(std::uint64_t end) override{std::lock_guard lock(mutex_);ram_->admit_write(end);}
  void write_exact(std::uint64_t offset,std::span<const std::byte> bytes) override {
    std::lock_guard lock(mutex_);ram_->write_exact(offset,bytes);
  }
  void flush() override{std::lock_guard lock(mutex_);ram_->flush();}
  void read_exact(std::uint64_t offset,std::span<std::byte> bytes) const override {
    std::lock_guard lock(mutex_);ram_->read_exact(offset,bytes);
  }

 private:
  std::shared_ptr<Ram> ram_;
  mutable std::mutex mutex_;
};

inline std::shared_ptr<Ram> image(Bytes bytes,std::uint32_t failure_unit=65536) {
  auto ram=std::make_shared<Ram>();ram->failure_unit=failure_unit;
  ram->live=bytes;ram->stable=std::move(bytes);return ram;
}
inline std::vector<NativeFileMutation> baseline() {
  return {file_create("A",true),file_create("A/file.bin"),file_write("A/file.bin",0,data(17,1)),
          file_create("target.bin"),file_write("target.bin",0,data(31,2)),
          file_create("stable.bin"),file_write("stable.bin",0,data(31,18))};
}
inline std::vector<NativeFileMutation> update() {
  return {file_rename("A","B"),file_write("B/file.bin",65532,data(77,3)),file_truncate("B/file.bin",65545),
          file_rename("B/file.bin","target.bin",true),file_remove("B"),
          file_write("stable.bin",0,Bytes(31)),file_write("stable.bin",0,data(31,18))};
}
// Find this fixture's unique padded payload by verified contents, not a
// hardcoded object ID or physical offset. Only bounded synthetic objects exist.
inline StoreObjectInfo stable_record(const ObjectStoreSnapshot& objects) {
  Bytes expected(kNativeExtentBytes);const auto bytes=data(31,18);
  std::copy(bytes.begin(),bytes.end(),expected.begin());
  StoreObjectInfo result;bool found=false;
  for(const auto& object:objects.list())if(object.id!=1 && object.logical_bytes==kNativeExtentBytes && objects.read(object.id)==expected) {
    if(found)throw std::runtime_error("ambiguous stable extent fixture");
    result=object;found=true;
  }
  if(!found)throw std::runtime_error("missing stable extent fixture");
  return result;
}
inline Bytes final_bytes() {
  Bytes out(65545);const auto old=data(17,1),addition=data(77,3);
  std::copy(old.begin(),old.end(),out.begin());std::copy_n(addition.begin(),13,out.begin()+65532);return out;
}
inline void expected(const NativeFilesystemSnapshot& snapshot,bool updated,bool continued=false) {
  const auto entries=snapshot.list();
  if(entries.size()!=(updated?2U:3U)+(continued?1U:0U))throw std::runtime_error("partial native namespace");
  if(updated) {
    if(snapshot.read("target.bin",0,100000)!=final_bytes())throw std::runtime_error("native replaced content mismatch");
    for(const auto* path:{"A","B"}) {
      bool missing=false;try{(void)snapshot.stat(path);}catch(const NativeFsError& e){missing=e.code()==NativeFsErrorCode::not_found;}
      if(!missing)throw std::runtime_error("native old directory leaked");
    }
  } else if(snapshot.read("A/file.bin",0,100000)!=data(17,1) || snapshot.read("target.bin",0,100000)!=data(31,2))
    throw std::runtime_error("native acknowledged bytes changed");
  if(snapshot.read("stable.bin",0,100)!=data(31,18) || snapshot.stat("stable.bin").logical_bytes!=31 ||
      snapshot.stat("stable.bin").modified_sequence!=(updated?3U:2U))
    throw std::runtime_error("native elided extent bytes/metadata changed");
  if(continued && snapshot.read("continued",0,4000)!=data(3073,91))throw std::runtime_error("native continuation bytes");
}
}
