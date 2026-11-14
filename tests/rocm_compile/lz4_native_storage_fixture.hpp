#pragma once
#include "../locked_ram_fixture.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "../../src/store_codec.hpp"
#include <array>
#include <optional>
namespace lz4_native_fixture {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
inline void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
inline Bytes source(){
  Bytes prefix(8192),plain(65536);std::uint64_t x=0x679a210fed44c3abULL;
  for(auto& b:prefix){x^=x<<13;x^=x>>7;x^=x<<17;b=static_cast<std::byte>(x>>21);}
  for(std::size_t i=0;i<plain.size();++i)plain[i]=prefix[i%prefix.size()];return plain;
}
inline std::uint64_t integer(std::span<const std::byte> bytes,std::size_t at,unsigned width){
  check(width && width<=8 && at<=bytes.size() && width<=bytes.size()-at,"native record integer bounds");
  std::uint64_t result=0;
  for(unsigned i=0;i<width;++i)result|=std::uint64_t(std::to_integer<unsigned>(bytes[at+i]))<<(8*i);
  return result;
}
class Volume final {
 public:
  Volume(){
    check(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,"native fixture entered dirty lock state");
    store_=std::make_shared<locked_ram_fixture::Store>(2U*1024*1024);
    StoreIdentity identity{};identity[0]=std::byte{0x57};identity[1]=std::byte{0x3a};
    writer_.emplace(NativeFilesystemWriter::initialize(ObjectStoreWriter::create(store_,identity,
        {StoreCompressionMode::fast,6,false})));
    NativeFileMutation create;create.operation=NativeFileOperation::create_file;create.path="candidate.bin";
    writer_->apply({&create,1});inode_=writer_->snapshot().stat("candidate.bin").inode;
    writer_->write_inode_storage_result(inode_,0,sentinel_);
    old_.emplace(writer_->snapshot());store_->verify();
  }
  bool publish(std::span<const std::byte> plain,std::span<const std::byte> candidate){
    check(plain.size()==65536,"one native synthetic extent required");
    const NativeLz4ExtentCandidate hint{0,candidate};
    const auto committed=writer_->write_inode_storage_with_lz4(inode_,0,plain,{&hint,1});
    check(committed.bytes==plain.size() && committed.offset==0 && committed.state.info.inode==inode_,
        "native commit-bound write result");
    auto objects=ObjectStoreSnapshot::open(store_);
    const auto info=objects.info();image_bytes=store_->extent();active_payload_bytes=info.stored_payload_bytes;
    auto independent=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(store_));
    check(independent.read("candidate.bin",0,65536)==Bytes(plain.begin(),plain.end()) &&
        old_->read("candidate.bin",0,4)==Bytes(sentinel_.begin(),sentinel_.end()),
        "native current/pinned source readback differs");
    independent.verify_all();store_->verify();
    if(candidate.empty() || candidate.size()>=plain.size())return false;
    // Test-only inspection AFTER full public CPU verification, against the
    // same immutable locked image. No owner path or raw-device access exists.
    std::uint64_t object_id=0;
    for(const auto& object:objects.list())if(object.logical_bytes==plain.size() &&
        objects.read(object.id)==Bytes(plain.begin(),plain.end())){
      check(!object_id,"ambiguous native extent record");object_id=object.id;
    }
    check(object_id!=0,"native data object missing");
    std::array<std::byte,4096> root{};
    store_->read_exact((info.generation%2)*kStoreCommitUnit,root);
    check(integer(root,16,8)==info.generation,"native inspected generation changed");
    const auto count=integer(root,44,4),index_bytes=integer(root,40,4);
    check(count && count<=kStoreMaxObjects && index_bytes==64+64*count,"native index shape");
    Bytes index(static_cast<std::size_t>(index_bytes));store_->read_exact(integer(root,32,8),index);
    for(std::size_t i=0;i<count;++i){
      const auto at=64+64*i;if(integer(index,at,8)!=object_id)continue;
      std::array<std::byte,128> record{};const auto offset=integer(index,at+16,8);
      store_->read_exact(offset,record);
      if(integer(record,12,4)!=4 || integer(record,32,4)!=plain.size() ||
          integer(record,36,4)!=candidate.size())return false;
      Bytes payload(candidate.size());store_->read_exact(offset+128,payload);
      return std::equal(payload.begin(),payload.end(),candidate.begin(),candidate.end());
    }
    throw std::runtime_error("native inspected object absent");
  }
  bool publish_uniform(std::span<const std::byte> plain,std::span<const NativeLz4ExtentCandidate> candidates){
    check(plain.size()==2U*65536 && (candidates.empty() || candidates.size()==2),
        "two-extent native calibration shape");
    const auto committed=writer_->write_inode_storage_with_lz4(inode_,0,plain,candidates);
    check(committed.bytes==plain.size() && committed.offset==0 && committed.state.info.inode==inode_,
        "uniform native commit-bound result");
    auto objects=ObjectStoreSnapshot::open(store_);const auto info=objects.info();
    image_bytes=store_->extent();active_payload_bytes=info.stored_payload_bytes;
    const auto independent=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(store_));
    check(independent.read("candidate.bin",0,static_cast<std::uint32_t>(plain.size()))==Bytes(plain.begin(),plain.end()) &&
        old_->read("candidate.bin",0,4)==Bytes(sentinel_.begin(),sentinel_.end()),
        "uniform native current/prior CPU recovery");
    independent.verify_all();store_->verify();
    if(candidates.empty())return false;
    // Both extents contain identical plaintext. The native writer may retain
    // separate immutable objects or deduplicate them. Verify EVERY matching
    // data object, rather than assuming either allocation behavior.
    std::vector<std::uint64_t> object_ids;
    for(const auto& object:objects.list())if(object.logical_bytes==65536 &&
        objects.read(object.id)==Bytes(plain.begin(),plain.begin()+65536)){
      object_ids.push_back(object.id);
    }
    check(!object_ids.empty() && object_ids.size()<=2,"uniform native data object inventory");
    std::array<std::byte,4096> root{};store_->read_exact((info.generation%2)*kStoreCommitUnit,root);
    check(integer(root,16,8)==info.generation,"uniform native generation changed");
    const auto count=integer(root,44,4),index_bytes=integer(root,40,4);
    check(count && count<=kStoreMaxObjects && index_bytes==64+64*count,"uniform native index shape");
    Bytes index(static_cast<std::size_t>(index_bytes));store_->read_exact(integer(root,32,8),index);
    for(const auto object_id:object_ids){
      bool located=false,exact=false;
      for(std::size_t i=0;i<count;++i){
        const auto at=64+64*i;if(integer(index,at,8)!=object_id)continue;
        check(!located,"uniform native duplicate index identity");located=true;
        std::array<std::byte,128> record{};const auto offset=integer(index,at+16,8);
        store_->read_exact(offset,record);
        if(integer(record,12,4)!=4 || integer(record,32,4)!=65536)continue;
        const auto size=integer(record,36,4);if(!size || size>65536)continue;
        Bytes payload(static_cast<std::size_t>(size));store_->read_exact(offset+128,payload);
        for(const auto& hint:candidates)if(hint.extent_index<2 && hint.payload.size()==payload.size() &&
            std::equal(hint.payload.begin(),hint.payload.end(),payload.begin()))exact=true;
      }
      check(located,"uniform native inspected object absent");
      if(!exact)return false;
    }
    return true;
  }
  void close(){
    old_.reset();writer_.reset();store_.reset();
    check(locked_ram_fixture::locked_bytes==0 && locked_ram_fixture::cleanup_ok,"native locked image retirement");
    working_.restore();closed=true;
  }
  std::uint64_t image_bytes=0,active_payload_bytes=0;
  bool closed=false;
 private:
  locked_ram_fixture::WorkingSet working_;
  std::shared_ptr<locked_ram_fixture::Store> store_;
  std::optional<NativeFilesystemWriter> writer_;
  std::optional<NativeFilesystemSnapshot> old_;
  std::uint64_t inode_=0;
  const std::array<std::byte,4> sentinel_{std::byte{1},std::byte{0x53},std::byte{0x7a},std::byte{9}};
};
}
