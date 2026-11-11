#include "native_fs_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <type_traits>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0,cases=0;
constexpr NativeFileAccess all{true,true,true};
constexpr std::uint64_t stamp=133800000000000099ULL;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
template<class F>void rejects_session(F&& fn,NativeSessionErrorCode code,const char* why) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}throw std::runtime_error(why);
}
NativeBasicMetadata basic(){return {11,22,33,44,2};}
NativeAutomaticMetadata effects(){return {stamp,true,true,true,true};}
NativeBasicMetadata updated(){return {11,stamp,stamp,stamp,34};}
// The selected backend contract is synchronous. Once the final real barrier
// completes, deny EVERY backing operation (except the noexcept contract).
// This proves the success path does not query storage to construct FileInfo.
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();
  bool sealed=false;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  mutable unsigned blocked=0;
  void allowed() const {if(sealed){++blocked;throw std::runtime_error("post-publication backend unavailable");}}
  std::uint64_t extent() const override {allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t size) override{allowed();ram->admit_write(size);}
  void read_exact(std::uint64_t at,std::span<std::byte> bytes) const override{allowed();ram->read_exact(at,bytes);}
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override{allowed();ram->write_exact(at,bytes);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
  void arm(){seal_at=ram->flushes+3;}
};
NativeFilesystemWriter fresh(std::shared_ptr<Sealed> io,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false}));
}
enum class Operation{write,append,constrained,truncate,same_eof,identical,sparse};
const char* name(Operation op) {
  switch(op){
    case Operation::write:return "write";case Operation::append:return "append";
    case Operation::constrained:return "constrained";case Operation::truncate:return "truncate";
    case Operation::same_eof:return "same-eof";case Operation::identical:return "identical";
    case Operation::sparse:return "sparse";
  }
  return "invalid";
}
void sealed_result(StoreCompressionMode mode,Operation op) {
  auto io=std::make_shared<Sealed>();auto writer=fresh(io,mode);
  const std::array<NativeFileMutation,2> setup{{file_create("f"),file_write("f",0,data(65530,91))}};
  const NativeBasicMutation value{0,1,basic()};writer.apply_with_basic(setup,{&value,1});
  auto session=NativeFileSession::create(std::move(writer));
  const auto handle=session.open("f",all),other=session.open("f",all);
  // Exercise a detached, cleaned context that is still valid for paging I/O.
  session.set_delete(handle,true);session.cleanup(handle,true);
  auto old=session.snapshot();const auto sequence=old.sequence();
  const auto old_bytes=old.read_inode(2,0,100000);
  auto bytes=data(65530,91);std::uint64_t offset=0;std::uint32_t count=0;
  io->arm();NativeInodeMutationCommit result;
  switch(op) {
    case Operation::write:{
      const auto patch=data(17,92);std::copy(patch.begin(),patch.end(),bytes.begin()+7);
      offset=7;count=17;result=session.write_result(handle,offset,patch,{},effects());break;
    }
    case Operation::append:{
      const auto extra=data(29,93);offset=bytes.size();count=29;
      bytes.insert(bytes.end(),extra.begin(),extra.end());
      result=session.write_result(handle,UINT64_MAX,extra,{true,false},effects());break;
    }
    case Operation::constrained:{
      const auto patch=data(100,94);offset=bytes.size()-7;count=7;
      std::copy_n(patch.begin(),7,bytes.end()-7);
      result=session.write_result(handle,offset,patch,{false,true},effects());break;
    }
    case Operation::truncate:
      bytes.resize(13);result=session.truncate_result(handle,13,effects());break;
    case Operation::same_eof:
      result=session.truncate_result(handle,bytes.size(),effects());break;
    case Operation::identical:
      count=23;result=session.write_result(handle,0,std::span<const std::byte>(bytes).first(count),{},effects());break;
    case Operation::sparse:
      result=session.truncate_result(handle,kNativeMaxFileBytes,effects());break;
  }
  const auto size=op==Operation::sparse?kNativeMaxFileBytes:bytes.size();
  require(io->sealed && !io->blocked,"mutation result performed I/O after final publication barrier");
  require(result.bytes==count && result.offset==offset && result.transaction.sequence==sequence+1 &&
      result.info.inode==2 && result.info.logical_bytes==size &&
      result.info.modified_sequence==result.transaction.sequence && result.info.basic==updated() &&
      !result.info.directory && !result.info.delete_pending && result.info.unlinked,
      "commit-bound facts differ from actual requested data/basic/lifetime");
  require(session.stats().contexts==2 && session.stats().cleaned==1 &&
      old_bytes==data(65530,91) && old.basic_metadata(2)==basic() &&
      old.stat_inode(2).logical_bytes==65530,
      "result publication changed ownership or old acknowledged snapshot");
  // The old snapshot's payload backend is intentionally unavailable now.
  // Reopen the NEW committed bytes through a separate independent backend.
  auto oracle=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable)));
  const auto info=oracle.stat_inode(2);const auto state=oracle.inode_state(2);
  require(info.inode==result.info.inode && info.logical_bytes==result.info.logical_bytes &&
      info.modified_sequence==result.info.modified_sequence && info.directory==result.info.directory &&
      oracle.basic_metadata(2)==result.info.basic && state.unlinked==result.info.unlinked &&
      state.delete_pending==result.info.delete_pending &&
      oracle.storage_info().generation==result.transaction.storage.generation,
      "returned facts do not match independently recovered commit");
  if(op==Operation::sparse) {
    require(oracle.read_inode(2,kNativeMaxFileBytes-37,37)==Bytes(37) && io->ram->live.size()<4U*1024*1024,
        "sparse result allocated logical EOF or exposed nonzero tail");
  } else require(oracle.read_inode(2,0,100000)==bytes,"returned mutation lost requested data");
  // Prove the denial is active, and a later failing query cannot retroactively
  // change the already returned value or release either owned context.
  rejects([&]{(void)session.basic_metadata(handle);},NativeFsErrorCode::storage_failure,
      "sealed backend query unexpectedly succeeded");
  require(io->blocked>0 && session.poisoned() && session.stats().contexts==2 &&
      result.info.basic==updated() && result.info.logical_bytes==size,
      "subsequent I/O failure changed returned facts/context ownership");
  const auto blocked=io->blocked;
  rejects_session([&]{session.truncate_result(other,0);},NativeSessionErrorCode::terminal,
      "fenced result operation retried storage");
  require(io->blocked==blocked,"fenced mutation-result operation reached sealed backend");
  ++cases;
  std::cout<<"Native result seal: {\"schema\":1,\"mode\":\""
      <<(mode==StoreCompressionMode::raw?"raw":"automatic")<<"\",\"operation\":\""<<name(op)
      <<"\",\"post_barrier_io\":0,\"complete\":true}\n";
}
void noops_and_absence(StoreCompressionMode mode) {
  auto io=std::make_shared<Sealed>();auto writer=fresh(io,mode);
  const auto op=file_create("legacy");writer.apply({&op,1});writer.write_inode(2,0,data(23,95));
  auto old=writer.snapshot();const auto calls=io->ram->calls,barriers=io->ram->flushes;
  const auto sequence=old.sequence(),modified=old.stat_inode(2).modified_sequence;
  for(const auto options:{NativeInodeWriteOptions{false,true},NativeInodeWriteOptions{true,true}}) {
    const auto result=writer.write_inode_result(2,UINT64_MAX,data(7,96),options,effects());
    require(!result.bytes && result.info.inode==2 && result.info.logical_bytes==23 &&
        result.info.modified_sequence==modified && !result.info.basic &&
        !result.info.directory && !result.info.unlinked && !result.info.delete_pending &&
        result.transaction.sequence==sequence && io->ram->calls==calls && io->ram->flushes==barriers,
        "no-op mutation result invented times, advanced sequence or published");
  }
  const auto empty=writer.write_inode_result(2,0,{});
  const auto same=writer.truncate_inode_result(2,23);
  require(!empty.bytes && !same.bytes && !empty.info.basic && !same.info.basic &&
      empty.transaction.sequence==sequence && same.transaction.sequence==sequence &&
      empty.info.modified_sequence==modified && same.info.modified_sequence==modified &&
      io->ram->calls==calls,"empty/same-EOF result did not retain legacy facts");
  const auto reads=io->ram->read_calls;
  rejects([&]{writer.write_inode_result(2,0,data(1,97),{},{UINT64_MAX,false,true,false,false});},
      NativeFsErrorCode::invalid_operation,"result API persisted invalid clock");
  rejects([&]{writer.truncate_inode_result(2,1,{1,true,false,false,false});},
      NativeFsErrorCode::invalid_operation,"result API accepted unused clock");
  require(io->ram->read_calls==reads && io->ram->calls==calls && !writer.poisoned(),
      "invalid result input reached backing or fenced healthy writer");
  auto session=NativeFileSession::create(std::move(writer));
  const auto reader=session.open("legacy",{true,false,false});
  const auto before_reads=io->ram->read_calls,before_calls=io->ram->calls;
  rejects_session([&]{session.write_result(reader,0,data(1,97));},NativeSessionErrorCode::access_denied,
      "read-only context got mutation result access");
  rejects_session([&]{session.truncate_result(reader,0);},NativeSessionErrorCode::access_denied,
      "read-only context got result truncate access");
  require(io->ram->read_calls==before_reads && io->ram->calls==before_calls,
      "denied result grant reached I/O");
  session.cleanup(reader,false);session.close(reader);const auto closed_calls=io->ram->calls;
  rejects_session([&]{session.write_result(reader,0,data(1,98));},NativeSessionErrorCode::invalid_handle,
      "closed result context resurrected");
  require(io->ram->calls==closed_calls,"stale result context performed writes");
}
}
int main(){try {
  static_assert(std::is_nothrow_copy_assignable_v<NativeInodeCommitInfo>);
  static_assert(std::is_nothrow_move_constructible_v<NativeInodeMutationCommit>);
  static_assert(sizeof(NativeInodeCommitInfo)<=80);
  for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    for(const auto op:{Operation::write,Operation::append,Operation::constrained,Operation::truncate,
        Operation::same_eof,Operation::identical,Operation::sparse})sealed_result(mode,op);
    noops_and_absence(mode);
  }
  require(cases==14,"sealed result cases omitted");
  std::cout<<"Native result evidence: {\"schema\":1,\"sealed_cases\":"<<cases
      <<",\"checks\":"<<checks<<",\"complete\":true}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"Native result failure: "<<e.what()<<"\n";return 1;}}
