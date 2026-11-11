#include "native_fs_fixture.hpp"
#include "native_security_fixture.hpp"
#include "flitzfs/native_session.hpp"
#include <iostream>
#include <optional>
#include <string_view>
namespace {
using namespace flitzfs;
using namespace native_fixture;
namespace sec=native_security_fixture;
unsigned checks=0,sealed_cases=0;
constexpr std::uint64_t stamp=133800000000000127ULL;
constexpr NativeFileAccess all{true,true,true};
NativeBasicMetadata tuple(){return {11,22,33,44,2};}
NativeAutomaticMetadata effects(){return {stamp,true,true,true,true};}
NativeBasicMetadata updated(){return {11,stamp,stamp,stamp,0x22};}
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& fn,NativeFsErrorCode code) {
  ++checks;try{fn();}catch(const NativeFsError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected native reservation rejection");
}
template<class F>void denies(F&& fn,NativeSessionErrorCode code) {
  ++checks;try{fn();}catch(const NativeSessionError& e){if(e.code()==code)return;throw;}
  throw std::runtime_error("expected owned reservation rejection");
}
NativeFilesystemWriter fresh(std::shared_ptr<Ram> ram,StoreCompressionMode mode,bool compact) {
  if(compact)ram->failure_unit=4096;
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,6,false},{compact}));
}
void seed(NativeFilesystemWriter& writer,std::size_t size=7001) {
  const std::array<NativeFileMutation,4> ops{{file_create("target"),file_write("target",0,data(size,71)),
      file_create("keep"),file_write("keep",0,data(37,72))}};
  const NativeBasicMutation basic{0,1,tuple()};
  const NativeSecurityMutation security{0,1,sec::descriptor()};
  writer.apply_with_metadata(ops,{&basic,1},{&security,1});
}
void keep(const NativeFilesystemSnapshot& view) {
  require(view.read("keep",0,100)==data(37,72) && view.stat("keep").inode==3,
      "reservation changed unrelated acknowledged bytes");
}
void basics(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto old=writer.snapshot();const auto historical=ObjectStoreSnapshot::open(ram).read(1);
  require(sec::get(historical,8)==4 && !old.allocation_info(2).reserved_bytes,
      "legacy allocation absence or encoding changed");
  const auto committed=writer.reserve_inode_result(2,16384,effects());
  require(committed.state.reserved_bytes==16384 && committed.state.info.logical_bytes==7001 &&
      committed.state.info.basic==updated() && committed.transaction.sequence==old.sequence()+1,
      "reservation result did not capture atomic basic/EOF state");
  require(!old.allocation_info(2).reserved_bytes && old.basic_metadata(2)==tuple() &&
      old.read("target",0,10000)==data(7001,71),"reservation changed old snapshot");
  require(sec::get(ObjectStoreSnapshot::open(ram).read(1),8)==5 &&
      writer.inspect_allocation_usage().charged_extents==2 &&
      writer.inspect_allocation_usage().live_extents==2,"format/charge did not admit reserved capacity");
  const auto count=ram->calls;
  const auto noop=writer.reserve_inode_result(2,16384,effects());
  require(ram->calls==count && noop.transaction.sequence==committed.transaction.sequence &&
      noop.state.reserved_bytes==16384,"identical reservation published");
  writer.truncate_inode(2,17);
  require(writer.inspect_allocation(2).reserved_bytes==16384,"ordinary EOF shrink released reservation");
  writer.truncate_inode(2,7001);auto zeroed=data(7001,71);std::fill(zeroed.begin()+17,zeroed.end(),std::byte{});
  require(writer.read_inode(2,0,10000)==zeroed,"reserved shrink/regrowth leaked discarded tail");
  const auto shrink=writer.reserve_inode_result(2,4096);
  require(shrink.state.info.logical_bytes==4096 && shrink.state.reserved_bytes==4096,
      "allocation shrink did not truncate EOF");
  writer.truncate_inode(2,65537);
  require(writer.inspect_allocation(2).reserved_bytes==69632 &&
      writer.inspect_allocation_usage().charged_extents==3,"EOF growth did not extend aligned reservation");
  writer.write_inode(2,131072,data(23,73));
  require(writer.inspect_allocation(2).reserved_bytes==135168 &&
      writer.inspect_allocation_usage().charged_extents==4,"write growth did not extend reservation");
  file_apply(writer,file_rename("target","renamed"));
  writer.set_security_descriptor(2,sec::descriptor(2));
  writer.set_basic_metadata(2,tuple());
  writer.overwrite_inode_result(2,{stamp,2,false});
  require(sec::get(ObjectStoreSnapshot::open(ram).read(1),8)==5 &&
      writer.inspect_allocation(2).reserved_bytes==135168 && !writer.inspect_allocation(2).info.logical_bytes &&
      writer.inspect_security_descriptor(2)==sec::descriptor(2),"metadata/overwrite downgraded/lost allocation");
  writer.reserve_inode_result(2,0);
  require(writer.inspect_allocation(2).reserved_bytes==0 &&
      writer.inspect_allocation_usage().charged_extents==1,"zero reservation confused absence or failed to release");
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(ram->stable,ram->failure_unit)));
  require(reopened.allocation_info(2).reserved_bytes==0 && !reopened.allocation_info(1).reserved_bytes &&
      reopened.security_descriptor(2)==sec::descriptor(2),"independent allocation reopen failed");keep(reopened);
}
void capacity_and_admission() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);
  const auto before=ram->live;const auto calls=ram->calls,reads=ram->read_calls;
  for(auto bytes:{std::uint64_t{1},std::uint64_t{4095},kNativeAllocationCapacity+4096,UINT64_MAX})
    rejects([&]{writer.reserve_inode_result(2,bytes);},NativeFsErrorCode::invalid_operation);
  rejects([&]{writer.reserve_inode_result(2,8192,{1,false,false,false,false});},NativeFsErrorCode::invalid_operation);
  require(ram->live==before && ram->calls==calls && ram->read_calls==reads && !writer.poisoned(),
      "pure allocation admission touched backing");
  rejects([&]{writer.reserve_inode_result(1,0);},NativeFsErrorCode::is_directory);
  writer.reserve_inode_result(2,kNativeAllocationCapacity-kNativeExtentBytes);
  require(writer.inspect_allocation_usage().charged_extents==kNativeMaxExtents,
      "mixed reserved and legacy extents were not charged together");
  auto count=ram->calls;
  rejects([&]{writer.reserve_inode_result(2,kNativeAllocationCapacity);},NativeFsErrorCode::limit);
  rejects([&]{writer.write_inode(3,kNativeExtentBytes,data(1,74));},NativeFsErrorCode::limit);
  require(ram->calls==count && !writer.poisoned(),"capacity rejection wrote or poisoned healthy owner");
  writer.reserve_inode_result(2,8192);
  count=ram->calls;
  rejects([&]{writer.truncate_inode(2,kNativeAllocationCapacity+1);},NativeFsErrorCode::limit);
  rejects([&]{writer.write_inode(2,kNativeAllocationCapacity,data(1,75));},NativeFsErrorCode::limit);
  require(ram->calls==count && !writer.poisoned(),"tracked oversized growth wrote/poisoned");
  file_apply(writer,file_create("sparse"));writer.truncate_inode(4,kNativeMaxFileBytes);
  count=ram->calls;
  rejects([&]{writer.reserve_inode_result(4,kNativeAllocationCapacity);},NativeFsErrorCode::limit);
  // A reservation smaller than EOF intentionally truncates it; exercise that
  // separately rather than treating large legacy sparse EOF as reserved.
  require(ram->calls==count && !writer.poisoned(),"oversubscribed reservation admission mutated source");
  const auto sparse=writer.reserve_inode_result(4,4096);
  require(sparse.state.info.logical_bytes==4096 && sparse.state.reserved_bytes==4096 &&
      writer.read_inode(4,0,8192)==Bytes(4096),"legacy sparse reservation did not atomically shrink safely");
}
void namespace_capacity() {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,StoreCompressionMode::raw,false);
  for(const auto* path:{"a","b","c","d"})file_apply(writer,file_create(path));
  auto large=sec::descriptor();large.resize(kNativeMaxSecurityBytes);
  for(unsigned id=2;id<=4;++id)writer.set_security_descriptor(id,large);
  auto last=sec::descriptor();last.resize(15820);writer.set_security_descriptor(5,last);
  const auto bytes=ram->live;const auto count=ram->calls;
  rejects([&]{writer.reserve_inode_result(2,0);},NativeFsErrorCode::limit);
  require(ram->live==bytes && ram->calls==count && !writer.poisoned() &&
      !writer.inspect_allocation(2).reserved_bytes,"format upgrade exceeded shared 64-KiB metadata cap");
  writer.set_security_descriptor(5,sec::descriptor());writer.reserve_inode_result(2,0);
  require(sec::get(ObjectStoreSnapshot::open(ram).read(1),8)==5,"metadata relief did not permit reservation");
}
void lifecycle(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer);
  auto session=NativeFileSession::create(std::move(writer));
  auto readonly=session.open("target",{true,false,false});
  auto denied_calls=ram->calls;const auto denied_reads=ram->read_calls;
  denies([&]{session.reserve_result(readonly,8192);},NativeSessionErrorCode::access_denied);
  require(ram->calls==denied_calls && ram->read_calls==denied_reads,"missing write grant performed allocation I/O");
  auto h=session.open("target",all);
  session.reserve_result(h,131072,effects());
  session.rename(h,"renamed");session.set_delete(h,true);session.cleanup(h,true);
  session.reserve_result(h,4096);
  require(session.allocation_info(h).info.unlinked && session.allocation_info(h).reserved_bytes==4096 &&
      session.read(h,0,8192)==data(4096,71),"detached reservation lost identity/bytes");
  session.close(h);
  require(session.allocation_usage().charged_extents==2,"nonfinal Close released detached reservation");
  session.cleanup(readonly,false);session.close(readonly);
  require(session.allocation_usage().charged_extents==1 && !session.stats().contexts,
      "last Close did not retire detached allocation");
  denies([&]{session.reserve_result(h,0);},NativeSessionErrorCode::invalid_handle);
  auto prepared=session.prepare_open("keep",all,NativeOpenKind::file,{},{});
  denies([&]{session.reserve_result(prepared,0);},NativeSessionErrorCode::invalid_options);
  require(!session.allocation_info(prepared).reserved_bytes,"prepared query invented allocation");
  session.close(prepared);
}
void copied(StoreCompressionMode mode,bool compact) {
  auto ram=std::make_shared<Ram>();std::optional<NativeFilesystemWriter> owner(fresh(ram,mode,compact));seed(*owner);
  owner->reserve_inode_result(2,131072);
  auto view=owner->snapshot();const auto source=ram->live;owner.reset();
  auto target=std::make_shared<Ram>();target->failure_unit=ram->failure_unit;auto id=identity();id[3]=std::byte{19};
  auto copy=NativeFilesystemWriter::compact(view,target,id,{mode,6,false},{compact});
  require(ram->live==source && copy.inspect_allocation(2).reserved_bytes==131072 &&
      copy.inspect_allocation_usage().charged_extents==3,"compaction lost reservations/changed source");keep(copy.snapshot());
  auto reopened=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(target->stable,target->failure_unit)));
  require(reopened.identity()==id && reopened.allocation_info(2).reserved_bytes==131072,
      "allocation new-identity reopen failed");
}
void malformed() {
  auto ram=std::make_shared<Ram>();
  {auto writer=fresh(ram,StoreCompressionMode::raw,false);seed(writer);writer.reserve_inode_result(2,16384);}
  const auto original=ObjectStoreSnapshot::open(ram).read(1);const auto generation=ObjectStoreSnapshot::open(ram).info().generation;
  constexpr std::size_t root=64,target=root+104;
  const auto corrupt=[&](auto mutation) {
    auto clone=image(ram->stable);auto bytes=original;mutation(bytes);
    {auto objects=ObjectStoreWriter::resume(clone,identity(),generation);
      const StoreMutation edit{1,false,std::move(bytes)};objects.commit_replicated({&edit,1});}
    rejects([&]{NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(clone));},NativeFsErrorCode::corrupt);
  };
  corrupt([](auto& b){b[7]=std::byte{'4'};});
  corrupt([](auto& b){sec::put(b,8,4);});
  corrupt([&](auto& b){sec::put(b,target+20,sec::get(b,target+20)&~16U);});
  corrupt([&](auto& b){sec::put(b,target+96,8193,8);});
  corrupt([&](auto& b){sec::put(b,target+96,4096,8);});
  corrupt([&](auto& b){sec::put(b,target+96,kNativeAllocationCapacity+4096,8);});
  corrupt([&](auto& b){sec::put(b,root+20,16);});
  corrupt([&](auto& b){sec::put(b,root+96,4096,8);});
  corrupt([&](auto& b){sec::put(b,target+92,1);});
  corrupt([&](auto& b){sec::put(b,target+20,sec::get(b,target+20)|32);});
  // Exactly-capacity target plus the independently live keep extent is invalid.
  corrupt([&](auto& b){sec::put(b,target+96,kNativeAllocationCapacity,8);});
}
class Sealed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();bool sealed=false;mutable unsigned blocked=0;
  std::size_t seal_at=(std::numeric_limits<std::size_t>::max)();
  void allowed() const{if(sealed){++blocked;throw std::runtime_error("post-publication unavailable");}}
  std::uint64_t extent() const override{allowed();return ram->extent();}
  StoreIoContract contract() const noexcept override{return ram->contract();}
  void admit_write(std::uint64_t n) override{allowed();ram->admit_write(n);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override{allowed();ram->read_exact(at,out);}
  void write_exact(std::uint64_t at,std::span<const std::byte> in) override{allowed();ram->write_exact(at,in);}
  void flush() override{allowed();ram->flush();if(ram->flushes==seal_at)sealed=true;}
};
void sealed(StoreCompressionMode mode,bool compact,bool shrink) {
  auto io=std::make_shared<Sealed>();if(compact)io->ram->failure_unit=4096;
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false},{compact}));seed(writer);
  if(shrink)writer.reserve_inode_result(2,16384);
  auto session=NativeFileSession::create(std::move(writer));auto h=session.open("target",all);
  io->seal_at=io->ram->flushes+3;const auto result=session.reserve_result(h,shrink?4096:16384,effects());
  require(io->sealed && !io->blocked && result.state.reserved_bytes==(shrink?4096:16384) &&
      result.state.info.logical_bytes==(shrink?4096:7001) && result.state.info.basic==updated(),
      "reservation result queried backing after third barrier");
  auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable,io->ram->failure_unit)));
  require(view.allocation_info(2).reserved_bytes==result.state.reserved_bytes &&
      view.read_inode(2,0,10000)==data(shrink?4096:7001,71),"sealed allocation result differs from recovery");keep(view);
  rejects([&]{session.allocation_info(h);},NativeFsErrorCode::storage_failure);
  const auto blocked=io->blocked;
  denies([&]{session.close(h);},NativeSessionErrorCode::terminal);
  require(io->blocked==blocked && session.stats().contexts==1,"fenced allocation released context/retried");++sealed_cases;
}
enum class Phase {reserve,shrink,write_grow,eof_grow};
const char* name(Phase p){return p==Phase::reserve?"reserve":p==Phase::shrink?"shrink":p==Phase::write_grow?"write-grow":"eof-grow";}
std::size_t initial_size(Phase p){return p==Phase::shrink?131145:7001;}
std::optional<std::uint64_t> allocation(Phase p,bool changed) {
  if(p==Phase::reserve)return changed?std::optional<std::uint64_t>{16384}:std::nullopt;
  if(p==Phase::shrink)return changed?8192:196608;
  return changed?73728:8192;
}
Bytes expected(Phase p,bool changed) {
  auto bytes=data(initial_size(p),71);
  if(changed && p==Phase::shrink)bytes.resize(8192);
  if(changed && (p==Phase::write_grow || p==Phase::eof_grow))bytes.resize(70073);
  if(changed && p==Phase::write_grow){const auto extra=data(73,76);std::copy(extra.begin(),extra.end(),bytes.begin()+70000);}
  return bytes;
}
struct Case {std::shared_ptr<Ram> ram;NativeFileSession session;NativeFileHandle handle;};
Case setup(StoreCompressionMode mode,bool compact,Phase p) {
  auto ram=std::make_shared<Ram>();auto writer=fresh(ram,mode,compact);seed(writer,initial_size(p));
  if(p!=Phase::reserve)writer.reserve_inode_result(2,*allocation(p,false));
  auto session=NativeFileSession::create(std::move(writer));auto h=session.open("target",all);
  return {ram,std::move(session),h};
}
void action(Case& c,Phase p) {
  if(p==Phase::write_grow)c.session.write_result(c.handle,70000,data(73,76),{},effects());
  else if(p==Phase::eof_grow)c.session.truncate_result(c.handle,70073,effects());
  else c.session.reserve_result(c.handle,p==Phase::reserve?16384:8192,effects());
}
void closure(const NativeFilesystemSnapshot& view,Phase p,bool changed) {
  keep(view);const auto bytes=expected(p,changed);const auto state=view.allocation_info(2);
  require(state.reserved_bytes==allocation(p,changed) && state.info.logical_bytes==bytes.size() &&
      state.info.basic==(changed?updated():tuple()) && view.security_descriptor(2)==sec::descriptor() &&
      view.read_inode(2,0,200000)==bytes,"allocation failure mixed reservation/EOF/data/basic/security");
}
void faults(StoreCompressionMode mode,bool compact,Phase p) {
  std::size_t steps=0;{auto c=setup(mode,compact,p);c.ram->calls=0;action(c,p);steps=c.ram->calls;}
  require(steps>3 && steps<100,"reservation trace outside bounds");std::size_t cases=0;
  for(unsigned fault=0;fault<3;++fault)for(std::size_t step=0;step<steps;++step) {
    ++cases;std::shared_ptr<Ram> ram;std::uint64_t sequence=0;
    {auto c=setup(mode,compact,p);ram=c.ram;auto old=c.session.snapshot();sequence=old.sequence();
      const auto before=c.session.stats();ram->calls=0;ram->fail_at=step;ram->failure=fault;
      rejects([&]{action(c,p);},NativeFsErrorCode::storage_failure);
      const auto after=c.session.stats();
      require(c.session.poisoned() && before.contexts==after.contexts && before.cleaned==after.cleaned &&
          before.latched_inodes==after.latched_inodes,"failed reservation released/changed owned context");
      const auto calls=ram->calls,reads=ram->read_calls;
      denies([&]{c.session.close(c.handle);},NativeSessionErrorCode::terminal);
      require(ram->calls==calls && ram->read_calls==reads,"fenced allocation Close retried");closure(old,p,false);}
    auto recovered=image(ram->stable,ram->failure_unit);
    auto view=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(recovered));
    require(view.sequence()==sequence || view.sequence()==sequence+1,"reservation recovery sequence rolled back/fabricated");
    closure(view,p,view.sequence()==sequence+1);
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(recovered,identity(),
        view.storage_info().generation,{mode,6,false}));
    const std::array<NativeFileMutation,2> continuation{{file_create("continued"),file_write("continued",0,data(17,77))}};
    writer.apply(continuation);keep(writer.snapshot());
    require(writer.snapshot().read("continued",0,100)==data(17,77) &&
        writer.inspect_allocation(2).reserved_bytes==view.allocation_info(2).reserved_bytes,
        "independent CPU continuation lost reserved state");
  }
  require(cases==3*steps,"reservation fault point omitted");
  std::cout<<"Native allocation evidence: {\"schema\":1,\"phase\":\""<<name(p)
      <<"\",\"mode\":\""<<(mode==StoreCompressionMode::raw?"raw":"automatic")
      <<"\",\"layout\":\""<<(compact?"compact":"fixed")<<"\",\"io_steps\":"<<steps
      <<",\"fault_cases\":"<<cases<<",\"complete\":true}\n";
}
}
int main(int argc,char** argv){try {
  if(argc!=2)throw std::runtime_error("one allocation group required");const std::string_view group=argv[1];
  if(group=="--invariants") {
    capacity_and_admission();namespace_capacity();malformed();
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})for(bool compact:{false,true}) {
      basics(mode,compact);lifecycle(mode,compact);copied(mode,compact);
      for(bool shrink:{false,true})sealed(mode,compact,shrink);
    }
    require(sealed_cases==8,"reservation sealed case omitted");
    std::cout<<"Native allocation invariants: {\"schema\":1,\"checks\":"<<checks
        <<",\"sealed_cases\":"<<sealed_cases<<",\"complete\":true}\n";
  } else {
    const auto p=group=="--reserve-faults"?Phase::reserve:group=="--shrink-faults"?Phase::shrink:
        group=="--write-grow-faults"?Phase::write_grow:group=="--eof-grow-faults"?Phase::eof_grow:
        throw std::runtime_error("unknown allocation group");
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic})
      for(bool compact:{false,true})faults(mode,compact,p);
  }
  return 0;
}catch(const std::exception& e){std::cerr<<"Native allocation failure: "<<e.what()<<"\n";return 1;}}
