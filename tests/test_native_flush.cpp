#include "flitzfs/native_session.hpp"
#include "native_fs_fixture.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <new>

namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void rejects(F&& action,const char* why) {
  ++checks;try{action();}catch(const std::exception&){return;}throw std::runtime_error(why);
}
template<class F>void rejects_fs(F&& action,NativeFsErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeFsError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
template<class F>void rejects_session(F&& action,NativeSessionErrorCode code,const char* why) {
  ++checks;try{action();}catch(const NativeSessionError& error){if(error.code()==code)return;throw;}
  throw std::runtime_error(why);
}
std::uint64_t get(const Bytes& bytes,std::size_t at,unsigned width=8) {
  std::uint64_t value=0;
  for(unsigned i=0;i<width;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes.at(at+i)))<<(8*i);
  return value;
}
std::uint64_t record_offset(const Ram& ram,std::uint64_t id) {
  const auto root=get(ram.live,16)>get(ram.live,kStoreCommitUnit+16)?0U:kStoreCommitUnit;
  const auto manifest=get(ram.live,static_cast<std::size_t>(root+32));
  for(std::uint64_t i=0;i<get(ram.live,static_cast<std::size_t>(manifest+12),4);++i) {
    const auto at=static_cast<std::size_t>(manifest+64+64*i);
    if(get(ram.live,at)==id)return get(ram.live,at+16);
  }
  throw std::runtime_error("synthetic record missing");
}
bool same(const StoreCommitInfo& a,const StoreCommitInfo& b) {
  return a.generation==b.generation && a.physical_end==b.physical_end &&
      a.logical_bytes==b.logical_bytes && a.objects==b.objects &&
      a.degraded_root==b.degraded_root && a.stored_payload_bytes==b.stored_payload_bytes &&
      a.checkpoint_generation==b.checkpoint_generation &&
      a.replicated_checkpoint==b.replicated_checkpoint && a.degraded_checkpoint==b.degraded_checkpoint;
}
class Observed final:public StoreByteIo {
 public:
  std::shared_ptr<Ram> ram=std::make_shared<Ram>();
  mutable std::size_t reads=0,extents=0;
  std::size_t barriers=0,read_failure=(std::numeric_limits<std::size_t>::max)();
  std::size_t extent_failure=(std::numeric_limits<std::size_t>::max)();
  bool allocation_failure=false,after=false,post_allocation=false;
  std::function<void()> at_barrier,after_barrier;
  StoreIoContract contract() const noexcept override{return ram->contract();}
  std::uint64_t extent() const override {
    if(extents++==extent_failure)throw std::runtime_error("injected extent observation failure");
    return ram->extent();
  }
  void admit_write(std::uint64_t bytes) override{ram->admit_write(bytes);}
  void read_exact(std::uint64_t at,std::span<std::byte> out) const override {
    if((after && post_allocation) || reads++==read_failure) {
      if(allocation_failure || post_allocation)throw std::bad_alloc();
      throw std::runtime_error("injected metadata observation failure");
    }
    ram->read_exact(at,out);
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> bytes) override{ram->write_exact(at,bytes);}
  void flush() override {
    ++barriers;if(at_barrier)at_barrier();
    ram->flush();after=true;
    if(after_barrier)after_barrier();
  }
  void observe() {
    reads=extents=barriers=0;after=false;
    read_failure=extent_failure=(std::numeric_limits<std::size_t>::max)();
    allocation_failure=post_allocation=false;at_barrier={};after_barrier={};
  }
};
ObjectStoreWriter generic(std::shared_ptr<Observed> io,StoreCompressionMode mode,bool replicated) {
  auto writer=ObjectStoreWriter::create(io,identity(),{mode,6,false});
  const std::array<StoreMutation,2> changes{{{1,false,data(73,171)},{2,false,data(91,172)}}};
  if(replicated)writer.commit_replicated(changes);else writer.commit(changes);
  return writer;
}
NativeFileSession native(std::shared_ptr<Observed> io,StoreCompressionMode mode) {
  auto writer=NativeFilesystemWriter::initialize(ObjectStoreWriter::create(io,identity(),{mode,6,false}));
  const std::array<NativeFileMutation,4> setup{{file_create("f"),file_write("f",0,data(73,173)),
      file_create("keep"),file_write("keep",0,data(91,174))}};
  writer.apply(setup);return NativeFileSession::create(std::move(writer));
}
void unchanged_barrier(StoreCompressionMode mode,bool replicated) {
  auto io=std::make_shared<Observed>();auto writer=generic(io,mode,replicated);
  const auto old=writer.snapshot();const auto info=old.info();const auto bytes=io->ram->live;
  const auto writes=io->ram->writes,flushes=io->ram->flushes;
  io->observe();const auto result=writer.flush_checkpoint();
  require(same(result,info) && io->barriers==1 && io->ram->flushes==flushes+1 &&
      io->ram->writes==writes && io->ram->live==bytes && io->ram->stable==bytes && !writer.poisoned(),
      "checkpoint flush was a no-op, mutated bytes/generation, or lost metadata");
  require(old.read(1)==data(73,171) && writer.snapshot().read(2)==data(91,172),
      "checkpoint flush changed retained/live contents");
  // Success cannot rely on a single hardcoded metadata shape.
  writer.flush_checkpoint();require(io->barriers==2 && same(writer.snapshot().info(),info),"repeated real barrier changed state");
}
void native_lifetimes(StoreCompressionMode mode) {
  auto io=std::make_shared<Observed>();auto session=native(io,mode);
  const NativeFileAccess access{true,true,true};auto a=session.open("f",access),b=session.open("f",access);
  session.set_delete(a,true);session.cleanup(a,true);const auto suffix=data(13,175);
  session.write(b,UINT64_MAX,suffix,{true,false});
  const auto before=session.snapshot();const auto stats=session.stats();const auto bytes=io->ram->live;
  io->observe();const auto writes=io->ram->writes;
  const auto result=session.flush(a);
  require(result.sequence==before.sequence() && result.created_inodes.empty() &&
      same(result.storage,before.storage_info()) && io->barriers==1 && io->ram->writes==writes &&
      io->ram->live==bytes && session.stats().contexts==stats.contexts &&
      session.stats().cleaned==stats.cleaned && session.info(b).unlinked,
      "file flush retired/advanced a cleaned live inode");
  session.flush();require(io->barriers==2 && session.stats().contexts==2,"volume flush skipped barrier or changed contexts");
  session.close(a);session.cleanup(b,false);session.close(b);
  const auto barriers=io->barriers,reads=io->ram->read_calls;
  rejects_session([&]{session.flush(a);},NativeSessionErrorCode::invalid_handle,"closed context flush reached backing");
  require(io->barriers==barriers && io->ram->read_calls==reads,"invalid file flush performed I/O");
  session.flush();require(io->barriers==barriers+1 && !session.snapshot().needs_session_recovery(),
      "flush after clean retirement failed");
}
void barrier_failure(StoreCompressionMode mode,unsigned failure) {
  auto io=std::make_shared<Observed>();auto session=native(io,mode);const auto handle=session.open("f",{true,true,true});
  const auto snapshot=session.snapshot();const auto info=snapshot.storage_info();const auto bytes=io->ram->live;
  io->observe();io->ram->fail_at=io->ram->calls;io->ram->failure=failure;
  rejects_fs([&]{session.flush(handle);},NativeFsErrorCode::storage_failure,"backend flush error hidden");
  require(session.poisoned() && session.stats().contexts==1 && !session.stats().cleaned &&
      io->barriers==1 && io->ram->live==bytes,"flush failure changed context/acknowledged bytes or did not fence");
  const auto barriers=io->barriers,reads=io->ram->read_calls;
  rejects_session([&]{session.flush();},NativeSessionErrorCode::terminal,"failed session retried barrier");
  require(io->barriers==barriers && io->ram->read_calls==reads,"fenced flush performed I/O");
  auto recovered=NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(io->ram->stable)));
  require(same(recovered.storage_info(),info) && recovered.read("f",0,100)==data(73,173) &&
      recovered.read("keep",0,100)==data(91,174),"flush failure lost an earlier acknowledgement");
}
void observation_failures(StoreCompressionMode mode) {
  std::size_t read_steps=0,extent_steps=0;
  {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);io->observe();
    writer.flush_checkpoint();read_steps=io->reads;extent_steps=io->extents;
  }
  require(read_steps>0 && read_steps<=16 && extent_steps>0 && extent_steps<=16,"flush observation trace cap");
  for(bool extent:{false,true})for(std::size_t step=0;step<(extent?extent_steps:read_steps);++step) {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);
    const auto bytes=io->ram->live;io->observe();
    if(extent)io->extent_failure=step;else io->read_failure=step;
    rejects([&]{writer.flush_checkpoint();},"metadata observation failure accepted");
    require(writer.poisoned() && io->barriers<=1 && io->ram->live==bytes,
        "observation failure changed bytes, retried or failed to fence");
    const auto before=io->barriers;
    rejects([&]{writer.flush_checkpoint();},"poisoned metadata observer retried");
    require(io->barriers==before,"poisoned observer called backend");
    require(ObjectStoreSnapshot::open(image(io->ram->stable)).read(2)==data(91,172),
        "metadata observation failure lost earlier acknowledged data");
  }
}
void degradation_and_during_barrier(StoreCompressionMode mode) {
  for(unsigned unit=0;unit<4;++unit)for(bool during:{false,true}) {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);
    const auto info=writer.snapshot().info();const auto root=static_cast<std::size_t>((info.generation%2)*kStoreCommitUnit);
    const std::array<std::uint64_t,4> offsets{{0,kStoreCommitUnit,get(io->ram->live,root+32),get(io->ram->live,root+96)}};
    const auto damage=[&] {
      std::fill_n(io->ram->live.begin()+static_cast<std::ptrdiff_t>(offsets[unit]),
          static_cast<std::size_t>(kStoreCommitUnit),std::byte{0});
    };
    io->observe();if(during)io->at_barrier=damage;else damage();
    const auto writes=io->ram->writes;
    rejects([&]{writer.flush_checkpoint();},"degraded checkpoint flush accepted");
    require(writer.poisoned() && io->barriers==(during?1U:0U) && io->ram->writes==writes,
        "degraded flush repaired metadata, wrote bytes or skipped fencing");
    auto recovered=ObjectStoreSnapshot::open(image(io->ram->live));
    require(recovered.info().degraded_checkpoint && recovered.read(1)==data(73,171) &&
        recovered.read(2)==data(91,172),"flush degradation damaged acknowledged payloads");
  }
  {
    auto io=std::make_shared<Observed>();auto session=native(io,mode);auto handle=session.open("f");
    io->observe();io->at_barrier=[] {throw 7;};
    rejects_fs([&]{session.flush(handle);},NativeFsErrorCode::storage_failure,"unknown backend exception escaped native flush domain");
    require(session.poisoned() && io->barriers==1 && session.stats().contexts==1,
        "unknown backend failure did not retain/fence context");
  }
  // Raw extent must remain stable across the barrier; uncertain tails are retained.
  {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);const auto size=io->ram->live.size();
    io->observe();io->at_barrier=[&]{io->ram->live.resize(size+17);};
    rejects([&]{writer.flush_checkpoint();},"changed raw extent accepted after barrier");
    require(writer.poisoned() && io->ram->live.size()==size+17,"changed tail was truncated or ignored");
  }
  // The native layer verifies its namespace AFTER the real barrier too.
  {
    auto io=std::make_shared<Observed>();auto session=native(io,mode);auto handle=session.open("f");
    const auto offset=record_offset(*io->ram,1);
    io->observe();io->after_barrier=[&]{io->ram->live.at(static_cast<std::size_t>(offset+128))^=std::byte{1};};
    rejects_fs([&]{session.flush(handle);},NativeFsErrorCode::storage_failure,"post-barrier namespace corruption accepted");
    require(session.poisoned() && io->barriers==1 && session.stats().contexts==1,
        "namespace flush corruption lost context/fence");
  }
}
void allocation_and_tails(StoreCompressionMode mode) {
  {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);io->observe();
    io->read_failure=0;io->allocation_failure=true;
    rejects([&]{writer.flush_checkpoint();},"preflight allocation failure accepted");
    require(!writer.poisoned() && !io->barriers,"preflight allocation pressure fenced a healthy unentered barrier");
    io->observe();writer.flush_checkpoint();require(io->barriers==1,"healthy retry after preflight pressure failed");
  }
  {
    auto io=std::make_shared<Observed>();auto writer=generic(io,mode,true);io->observe();
    io->post_allocation=true;
    rejects([&]{writer.flush_checkpoint();},"post-barrier allocation failure accepted");
    require(writer.poisoned() && io->barriers==1,"unverified barrier completion did not fence");
  }
  {
    auto io=std::make_shared<Observed>();auto session=native(io,mode);io->observe();
    io->read_failure=0;io->allocation_failure=true;
    rejects_fs([&]{session.flush();},NativeFsErrorCode::limit,"native preflight allocation error hidden");
    require(!session.poisoned() && !io->barriers,"native preflight pressure entered/fenced barrier");
    io->observe();session.flush();const auto read_steps=io->reads;
    require(read_steps>=12 && read_steps<=20,"native flush metadata trace outside cap");
    io->observe();io->read_failure=read_steps-1;io->allocation_failure=true;
    rejects_fs([&]{session.flush();},NativeFsErrorCode::storage_failure,"native post-barrier allocation error hidden");
    require(session.poisoned() && io->barriers==1,"native unverified completion remained usable");
  }
  auto source=std::make_shared<Observed>();Bytes acknowledged;std::uint64_t generation=0;
  {auto writer=generic(source,mode,true);acknowledged=source->ram->stable;generation=writer.snapshot().info().generation;}
  auto io=std::make_shared<Observed>();io->ram=image(acknowledged);io->ram->live.resize(acknowledged.size()+17);
  io->ram->stable=io->ram->live;
  auto writer=ObjectStoreWriter::resume(io,identity(),generation,{mode,6,false});
  const auto tail=io->ram->live;io->observe();writer.flush_checkpoint();
  require(io->barriers==1 && io->ram->live==tail,"flush erased/rewrote admitted uncertain tail");
  auto moved=std::move(writer);const auto before=io->barriers;
  rejects([&]{writer.flush_checkpoint();},"moved-from writer flushed");
  require(io->barriers==before,"moved-from flush reached backend");
  moved.flush_checkpoint();
}
}
int main(){try {
  {
    auto io=std::make_shared<Observed>();auto writer=ObjectStoreWriter::create(io,identity());
    const auto bytes=io->ram->live;io->observe();const auto result=writer.flush_checkpoint();
    require(result.generation==0 && result.objects==0 && io->barriers==1 && io->ram->live==bytes,
        "empty generation-zero checkpoint could not flush unchanged");
  }
  for(auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
    unchanged_barrier(mode,false);unchanged_barrier(mode,true);native_lifetimes(mode);
    for(unsigned failure=0;failure<3;++failure)barrier_failure(mode,failure);
    observation_failures(mode);degradation_and_during_barrier(mode);allocation_and_tails(mode);
  }
  std::cout<<"Native flush evidence: {\"schema\":1,\"checks\":"<<checks
      <<",\"complete\":true}\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"native flush failure after "<<checks<<": "<<error.what()<<'\n';return 1;}}
