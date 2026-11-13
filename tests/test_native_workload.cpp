#include "native_fs_fixture.hpp"
#include <iostream>
#include <limits>
namespace {
using namespace flitzfs;
using namespace native_fixture;
unsigned checks=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void rejects(F&& f,const char* why) {
  ++checks;try{f();}catch(const NativeFsError&){return;}throw std::runtime_error(why);
}
NativeFilesystemWriter fresh(const std::shared_ptr<Ram>& ram,StoreCompressionMode mode) {
  return NativeFilesystemWriter::initialize(ObjectStoreWriter::create(ram,identity(),{mode,3,false}));
}
NativeFilesystemSnapshot reopen(const Bytes& bytes) {
  return NativeFilesystemSnapshot::open(ObjectStoreSnapshot::open(image(bytes)));
}
std::uint64_t count(const NativeFilesystemSnapshot& view) {
  const auto info=view.workload_info();require(info.enabled,"workload capability absent");
  return info.committed_input_bytes;
}
Bytes baseline(StoreCompressionMode mode,bool enabled) {
  auto ram=std::make_shared<Ram>();
  {
    auto fs=fresh(ram,mode);file_apply(fs,file_create("keep"));
    file_apply(fs,file_write("keep",0,data(7,1)));
    if(enabled) {
      fs.enable_workload_accounting();
      file_apply(fs,file_write("keep",0,data(7,1)));
    }
  }
  return ram->stable;
}
void accounting(StoreCompressionMode mode) {
  auto ram=std::make_shared<Ram>();auto fs=fresh(ram,mode);
  file_apply(fs,file_create("keep"));file_apply(fs,file_write("keep",0,data(7,1)));
  const auto legacy=ram->stable;
  require(!fs.snapshot().workload_info().enabled&&!reopen(legacy).workload_info().enabled,
      "legacy writes silently enabled accounting");
  auto old=fs.snapshot();fs.enable_workload_accounting();
  require(count(fs.snapshot())==0&&fs.snapshot().read("keep",0,100)==data(7,1)&&
      ram->stable!=legacy&&!old.workload_info().enabled,"activation credited history or changed data");
  const auto reads=ram->read_calls;
  require(fs.workload_info()==NativeWorkloadInfo{true,0}&&ram->read_calls==reads,
      "writer workload diagnostic queried backing");
  const auto before=ram->live;const auto calls=ram->calls;
  const auto seq=fs.snapshot().sequence();fs.enable_workload_accounting();
  require(ram->live==before&&ram->calls==calls&&fs.snapshot().sequence()==seq,
      "repeated enable wrote or reset accounting");
  file_apply(fs,file_write("keep",0,data(7,1)));require(count(fs.snapshot())==7,"identical input missing");
  file_apply(fs,file_write("keep",0,data(7,1)));require(count(fs.snapshot())==14,"repeated input missing");
  file_apply(fs,file_write("keep",0,Bytes(5)));require(count(fs.snapshot())==19,"zero input missing");
  file_apply(fs,file_truncate("keep",100));require(count(fs.snapshot())==19,"sparse growth minted input");
  const auto inode=fs.snapshot().stat("keep").inode;
  require(fs.write_inode(inode,0,data(3,9),{true,false}).bytes==3&&count(fs.snapshot())==22,
      "append actual input mismatch");
  require(fs.write_inode(inode,102,data(4,11),{false,true}).bytes==1&&count(fs.snapshot())==23,
      "constrained input counted unaccepted bytes");
  require(fs.write_inode(inode,UINT64_MAX,data(4,11),{false,true}).bytes==0&&
      count(fs.snapshot())==23,"zero constrained write minted input");
  file_apply(fs,file_write("keep",0,{}));require(count(fs.snapshot())==23,"empty write minted input");
  fs.set_basic_metadata(inode,{});fs.reserve_inode_result(inode,4096,{});
  fs.flush();require(count(fs.snapshot())==23,"metadata, reservation or flush minted input");
  const std::array<NativeFileMutation,2> writes{
    file_write("keep",0,data(3,12)),file_write("keep",0,data(4,13))};
  fs.apply(writes);require(count(fs.snapshot())==30,"ordered batch input mismatch");
  const auto stable=ram->stable;const auto io=ram->calls;
  const std::array<NativeFileMutation,2> rejected{
    file_write("keep",0,data(17,14)),file_create("keep")};
  rejects([&]{fs.apply(rejected);},"invalid final batch operation accepted");
  require(ram->stable==stable&&ram->calls==io&&count(fs.snapshot())==30&&!fs.poisoned(),
      "rejected batch changed persisted input");
  const auto snapshot=fs.snapshot();const auto bytes=snapshot.read("keep",0,200);
  {auto retired=std::move(fs);
    require(!fs.workload_info().enabled&&retired.workload_info()==NativeWorkloadInfo{true,30},
        "moved writer exposed/lost workload");} // Retire its writer before copying.
  auto moved=std::move(old);require(!old.workload_info().enabled&&!moved.workload_info().enabled,
      "moved legacy snapshot exposed accounting");
  for(unsigned rewrite=0;rewrite<2;++rewrite) {
    auto target=std::make_shared<Ram>();auto id=identity();id[2]=std::byte(rewrite+1);
    const auto original=ram->stable;
    auto copy=rewrite?NativeFilesystemWriter::recompress(snapshot,target,id,{mode,9,false}):
        NativeFilesystemWriter::compact(snapshot,target,id,{mode,3,false});
    require(count(copy.snapshot())==30&&copy.snapshot().read("keep",0,200)==bytes&&
        ram->stable==original,"image rewrite reset/minted input or changed source");
    const auto copy_bytes=target->stable;
    require(count(reopen(copy_bytes))==30,"independent CPU copy reopen lost input");
  }
  auto resumed_io=image(stable);const auto observed=reopen(stable);
  auto resumed=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(resumed_io,identity(),
      observed.storage_info().generation,{mode,3,false}));
  require(count(resumed.snapshot())==30,"resume reset input");
  resumed.enable_workload_accounting();require(count(resumed.snapshot())==30,"resume/re-enable reset input");
  file_apply(resumed,file_write("keep",0,data(2,15)));
  require(count(reopen(resumed_io->stable))==32,"continued input not independently recoverable");
}
void interrupted(StoreCompressionMode mode,bool enabling) {
  const auto original=baseline(mode,!enabling);
  const auto exercise=[&](Ram& ram,NativeFilesystemWriter& fs){
    ram.calls=0;
    if(enabling)fs.enable_workload_accounting();
    else file_apply(fs,file_write("keep",0,data(11,5)));
  };
  auto probe=image(original);std::size_t steps=0;
  {
    const auto view=reopen(original);
    auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(probe,identity(),
        view.storage_info().generation,{mode,3,false}));
    exercise(*probe,fs);steps=probe->calls;
  }
  // This fixture changes at most two records (header + payload each).
  // Replicated publication has three <=64-KiB zero-padding ranges written
  // in 4-KiB chunks, two indexes, two roots and three flushes.
  constexpr auto max_calls=2*2+3*(kStoreCommitUnit/4096)+2+2+3;
  std::cout<<"Observed workload publication calls: "<<steps<<"; bound="<<max_calls<<"\n";
  require(steps>0&&steps<=max_calls,"publication cut count outside derived fixture bound");
  for(unsigned failure=0;failure<3;++failure)for(std::size_t cut=0;cut<steps;++cut) {
    auto ram=image(original);
    {
      const auto view=reopen(original);
      auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(ram,identity(),
          view.storage_info().generation,{mode,3,false}));
      ram->fail_at=cut;ram->failure=failure;
      rejects([&]{exercise(*ram,fs);},"publication fault not reached");
      const auto reads=ram->read_calls,calls=ram->calls;
      require(!fs.workload_info().enabled&&ram->read_calls==reads&&ram->calls==calls,
          "poisoned writer diagnostic exposed authority or did I/O");
    }
    const auto recovered=reopen(ram->stable);
    if(enabling) {
      require(recovered.read("keep",0,100)==data(7,1)&&
          (!recovered.workload_info().enabled||count(recovered)==0),
          "interrupted activation credited history or lost acknowledged data");
    } else {
      const auto bytes=recovered.read("keep",0,100);
      require((bytes==data(7,1)&&count(recovered)==7)||
          (bytes==data(11,5)&&count(recovered)==18),
          "recovery mixed write bytes and accounting");
    }
    const auto info=recovered.storage_info();auto continued=image(ram->stable);
    auto fs=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(continued,identity(),
        info.generation,{mode,3,false}));
    fs.enable_workload_accounting();const auto start=count(fs.snapshot());
    file_apply(fs,file_write("keep",0,data(2,19)));
    require(count(reopen(continued->stable))==start+2,"repaired continuation reset accounting");
  }
  std::cout<<"Workload fault cuts: "<<steps<<" boundaries x3 modes; activation="<<enabling<<"\n";
}
void forged_metadata() {
  const auto original=baseline(StoreCompressionMode::raw,true);
  for(unsigned variant=0;variant<4;++variant) {
    auto ram=image(original);
    {
      const auto snapshot=ObjectStoreSnapshot::open(ram);auto metadata=snapshot.read(1);
      if(variant==0)for(unsigned i=0;i<8;++i)metadata[56+i]=std::byte{255};
      if(variant==1){metadata[7]=std::byte{'5'};metadata[8]=std::byte{5};}
      if(variant==2){metadata[7]=std::byte{'7'};metadata[8]=std::byte{7};}
      if(variant==3)metadata[7]=std::byte{'5'};
      auto writer=ObjectStoreWriter::resume(ram,identity(),snapshot.info().generation,
          {StoreCompressionMode::raw,3,false});
      const StoreMutation changed{1,false,std::move(metadata)};writer.commit_replicated({&changed,1});
    }
    if(variant) {
      rejects([&]{(void)reopen(ram->stable);},"intact invalid/unsupported accounting framing accepted");
      continue;
    }
    const auto observed=reopen(ram->stable);
    require(count(observed)==UINT64_MAX,"saturated counter not representable");
    auto writer=NativeFilesystemWriter::resume(ObjectStoreWriter::resume(ram,identity(),
        observed.storage_info().generation,{StoreCompressionMode::raw,3,false}));
    file_apply(writer,file_write("keep",0,data(3,31)));
    require(count(reopen(ram->stable))==UINT64_MAX&&
        reopen(ram->stable).read("keep",0,3)==data(3,31),
        "counter saturation wrapped, reset or blocked file writes");
  }
}
}
int main() {
  try {
    for(const auto mode:{StoreCompressionMode::raw,StoreCompressionMode::automatic}) {
      accounting(mode);interrupted(mode,true);interrupted(mode,false);
    }
    forged_metadata();
    std::cout<<"PASS: "<<checks<<" persistent workload assertions; CPU RAM persistence model only.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"Workload qualification: "<<e.what()<<"\n";return 1;}
}
