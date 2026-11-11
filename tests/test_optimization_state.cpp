#include "flitzfs/optimization_state.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
namespace {
using namespace flitzfs;
using Bytes=std::vector<std::byte>;
unsigned checks=0;
void require(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
template<class F> void rejects(F&& f,const char* text){
  ++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error(text);
}
StoreIdentity volume(unsigned v=7){StoreIdentity id{};id[0]=static_cast<std::byte>(v);return id;}
class Ram final:public StoreByteIo {
 public:
  Bytes live,stable;
  StoreIoContract declared{65536,true};
  std::size_t calls=0,writes=0,fail_at=(std::numeric_limits<std::size_t>::max)();
  unsigned mode=0;bool pressure=false,read_failure=false;
  std::array<bool,32> dirty{};
  std::uint64_t cap=kOptimizationAreaBytes;
  explicit Ram(std::uint64_t limit=kOptimizationAreaBytes):cap(limit){}
  std::uint64_t extent() const override {return live.size();}
  void read_exact(std::uint64_t at,std::span<std::byte> output) const override {
    if(read_failure||at>live.size()||output.size()>live.size()-at)throw std::runtime_error("RAM read");
    std::copy_n(live.begin()+static_cast<std::ptrdiff_t>(at),output.size(),output.begin());
  }
  void copy(Bytes& to,std::uint64_t at,std::span<const std::byte> input) {
    if(at>cap||input.size()>cap-at)throw std::runtime_error("RAM cap");
    if(to.size()<at+input.size())to.resize(static_cast<std::size_t>(at+input.size()));
    std::copy(input.begin(),input.end(),to.begin()+static_cast<std::ptrdiff_t>(at));
  }
  void write_exact(std::uint64_t at,std::span<const std::byte> input) override {
    const bool broken=calls++==fail_at;++writes;
    if(broken) {
      if(mode==1){copy(live,at,input.first(input.size()/2));copy(stable,at,input.first(input.size()/2));}
      if(mode==2) {
        const auto unit=at/kStoreCommitUnit*kStoreCommitUnit;
        if(stable.size()<unit+kStoreCommitUnit)stable.resize(static_cast<std::size_t>(unit+kStoreCommitUnit));
        std::fill_n(stable.begin()+static_cast<std::ptrdiff_t>(unit),kStoreCommitUnit,std::byte{});
        live=stable;
      }
      throw std::runtime_error("injected write");
    }
    copy(live,at,input);if(input.empty())return;
    const auto first=at/kStoreCommitUnit,last=(at+input.size()-1)/kStoreCommitUnit;
    for(auto unit=first;unit<=last;++unit)dirty.at(static_cast<std::size_t>(unit))=true;
  }
  void flush() override {
    const bool broken=calls++==fail_at;
    if(!broken||mode==1){stable=live;dirty={};}
    else if(mode==2) {
      for(std::size_t i=0;i<dirty.size();++i)if(dirty[i]) {
        const auto at=i*kStoreCommitUnit;
        if(at<live.size())copy(stable,at,std::span<const std::byte>(live).subspan(at,
            (std::min)(static_cast<std::size_t>(kStoreCommitUnit/2),live.size()-at)));
      }
    }
    if(broken)throw std::runtime_error("injected flush");
  }
  void admit_write(std::uint64_t extent) override {
    if(pressure||extent>cap)throw std::runtime_error("RAM pressure");
  }
  StoreIoContract contract() const noexcept override{return declared;}
};
std::shared_ptr<Ram> image(Bytes bytes){auto p=std::make_shared<Ram>();p->live=bytes;p->stable=std::move(bytes);return p;}
OptimizationState policy(unsigned seed=1) {
  OptimizationState s;
  for(unsigned i=0;i<32;++i)s.profiles.push_back({i+1,65536,1000+seed,100+i,200+i,
      1+(i%4),1,500000});
  for(unsigned i=0;i<16;++i)s.benchmarks.push_back({i+1,10000+i,1+(i%4),65536,1000+seed,1+(i%2)});
  return s;
}
OptimizationSnapshot read(const Bytes& bytes){auto io=image(bytes);return read_optimization_state(*io,volume());}
void put(Bytes& b,std::size_t at,std::uint64_t value,unsigned count=8){
  for(unsigned i=0;i<count;++i)b.at(at+i)=static_cast<std::byte>(value>>(8*i));
}
void seal(Bytes& b,unsigned slot) {
  const auto start=slot*kStoreCommitUnit,at=start+kStoreCommitUnit-32;
  const auto hash=detail::record_digest(std::span<const std::byte>(b).subspan(start,kStoreCommitUnit-32));
  std::copy(hash.begin(),hash.end(),b.begin()+static_cast<std::ptrdiff_t>(at));
}
void expect_state(const OptimizationSnapshot& value,std::uint64_t sequence,const OptimizationState& expected) {
  require(value.code==OptimizationReadCode::available&&value.volume==volume()&&
      value.sequence==sequence&&value.state==expected,"recovered advisory state differs");
}
Bytes initial_area() {
  auto io=std::make_shared<Ram>();auto writer=OptimizationStateWriter::create(io,volume(),4);
  expect_state(writer.snapshot(),1,{});
  writer.publish(policy());expect_state(writer.snapshot(),2,policy());
  return io->stable;
}
void admission_and_budget() {
  auto io=std::make_shared<Ram>();
  require(read_optimization_state(*io,volume()).code==OptimizationReadCode::absent,"empty area not absent");
  for(auto c:{StoreIoContract{},StoreIoContract{3,true},StoreIoContract{131072,true},StoreIoContract{512,false}}) {
    io->declared=c;rejects([&]{(void)OptimizationStateWriter::create(io,volume(),4);},"unsafe backend");
    require(io->writes==0,"invalid backend mutated");
  }
  io->declared={65536,true};
  rejects([&]{(void)OptimizationStateWriter::create(io,{});},"zero volume");
  for(unsigned limit:{0U,5U})rejects([&]{(void)OptimizationStateWriter::create(io,volume(),limit);},"allowance limit");
  io->pressure=true;rejects([&]{(void)OptimizationStateWriter::create(io,volume());},"pressure ignored");
  require(io->writes==0,"creation pressure wrote");
  io->pressure=false;auto writer=OptimizationStateWriter::create(io,volume(),4);
  require(io->live.size()==kOptimizationAreaBytes&&writer.remaining_publications()==3,"area/creation allowance");
  const auto before=io->live;const auto calls=io->calls;
  const auto retained=writer.snapshot();writer.publish({});
  require(io->calls==calls&&writer.remaining_publications()==3&&retained.sequence==1,
      "identical advice consumed writes/allowance or changed retained snapshot");
  rejects([&]{(void)OptimizationStateWriter::resume(io,volume(),1);},"competing owner");
  auto invalid=policy();invalid.profiles.push_back(invalid.profiles.back());
  rejects([&]{writer.publish(invalid);},"profile cap");
  invalid=policy();invalid.profiles[1].workload_key=1;invalid.profiles[1].strategy=1;
  rejects([&]{writer.publish(invalid);},"duplicate profile");
  invalid=policy();invalid.profiles[0].stored_bytes=65537;
  rejects([&]{writer.publish(invalid);},"stored/logical bound");
  invalid=policy();invalid.profiles[0].observations=0;
  rejects([&]{writer.publish(invalid);},"zero observations");
  invalid=policy();invalid.profiles[0].confidence_millionths=1000001;
  rejects([&]{writer.publish(invalid);},"confidence bound");
  invalid=policy();invalid.benchmarks.push_back(invalid.benchmarks.back());
  rejects([&]{writer.publish(invalid);},"history cap");
  invalid=policy();invalid.benchmarks[0].backend=3;
  rejects([&]{writer.publish(invalid);},"unknown benchmark backend");
  require(io->calls==calls&&io->live==before&&!writer.poisoned(),"argument rejection mutated/fenced");
  io->pressure=true;rejects([&]{writer.publish(policy());},"write pressure ignored");
  require(io->calls==calls&&!writer.poisoned(),"preflight pressure mutated/fenced");
  io->pressure=false;
  for(unsigned i=1;i<=3;++i)writer.publish(policy(i));
  const auto done=io->calls;
  rejects([&]{writer.publish(policy());},"unbounded publications");
  require(io->calls==done&&!writer.poisoned()&&writer.remaining_publications()==0,"exhausted allowance did I/O");
  auto moved=std::move(writer);require(writer.poisoned()&&!moved.poisoned(),"writer move ownership");
  rejects([&]{writer.publish(policy());},"moved writer operated");
  rejects([&]{(void)OptimizationStateWriter::resume(io,volume(),4);},"moved owner lost lease");
}
void decoding(const Bytes& acknowledged) {
  expect_state(read(acknowledged),2,policy());
  for(unsigned slot=0;slot<2;++slot) {
    auto bad=acknowledged;bad[slot*kStoreCommitUnit+96]^=std::byte{1};
    const auto recovered=read(bad);expect_state(recovered,2,policy());require(recovered.degraded,"bad peer hidden");
  }
  auto both=acknowledged;both[96]^=std::byte{1};both[kStoreCommitUnit+96]^=std::byte{1};
  require(read(both).code==OptimizationReadCode::invalid,"both damaged produced policy");
  for(auto length:{std::size_t{1},std::size_t{65536},std::size_t{131071},std::size_t{131073}}) {
    auto bad=acknowledged;bad.resize(length);require(read(bad).code==OptimizationReadCode::invalid,"wrong area extent");
  }
  struct Mutation {std::size_t at;std::uint64_t value;unsigned count;OptimizationReadCode expected;};
  for(const auto m:std::array{
      Mutation{8,2,4,OptimizationReadCode::unsupported},
      Mutation{40,2,4,OptimizationReadCode::unsupported},
      Mutation{12,1,4,OptimizationReadCode::invalid},
      Mutation{16,0,8,OptimizationReadCode::invalid},
      Mutation{16,9,8,OptimizationReadCode::invalid},
      Mutation{24,19,1,OptimizationReadCode::foreign_volume},
      Mutation{44,0xffffffff,4,OptimizationReadCode::invalid},
      Mutation{44,31,4,OptimizationReadCode::invalid},
      Mutation{64+16,65537,8,OptimizationReadCode::invalid},
      Mutation{48,17,4,OptimizationReadCode::invalid},
      Mutation{52,1,1,OptimizationReadCode::invalid},
      Mutation{64+48,1000001,4,OptimizationReadCode::invalid},
      Mutation{64+32,999999,8,OptimizationReadCode::invalid},
      Mutation{64+52,1,1,OptimizationReadCode::invalid},
      Mutation{64+32*64+20,3,4,OptimizationReadCode::invalid}}) {
    auto bad=acknowledged;put(bad,m.at,m.value,m.count);seal(bad,0);
    const auto state=read(bad);
    require(state.code==m.expected&&state.state.profiles.empty()&&state.sequence==0,
        "intact unsupported/malformed peer rolled back or exposed policy");
  }
  auto exhausted=acknowledged;
  for(unsigned slot=0;slot<2;++slot) {
    put(exhausted,slot*kStoreCommitUnit+16,(std::numeric_limits<std::uint64_t>::max)());seal(exhausted,slot);
  }
  auto exhausted_io=image(exhausted);
  auto exhausted_writer=OptimizationStateWriter::resume(exhausted_io,volume(),
      (std::numeric_limits<std::uint64_t>::max)(),4);
  const auto exhausted_calls=exhausted_io->calls;
  rejects([&]{exhausted_writer.publish(policy(2));},"sequence wrapped");
  require(!exhausted_writer.poisoned()&&exhausted_io->calls==exhausted_calls,
      "sequence exhaustion changed backing/health");
  auto unavailable=image(acknowledged);unavailable->read_failure=true;
  require(read_optimization_state(*unavailable,volume()).code==OptimizationReadCode::io_failure,"I/O failure hidden");
  rejects([&]{(void)read_optimization_state(*unavailable,{});},"missing reader binding");
}
void crash_matrix(const Bytes& acknowledged) {
  auto baseline=std::make_shared<Ram>();
  {auto writer=OptimizationStateWriter::create(baseline,volume(),4);}
  const auto creation_steps=baseline->calls;
  for(std::size_t step=0;step<creation_steps;++step)for(unsigned mode=0;mode<3;++mode) {
    auto io=std::make_shared<Ram>();io->fail_at=step;io->mode=mode;
    rejects([&]{(void)OptimizationStateWriter::create(io,volume(),4);},"creation fault not reached");
    const auto recovered=read(io->stable);
    require(recovered.code==OptimizationReadCode::absent||recovered.code==OptimizationReadCode::invalid||
        (recovered.code==OptimizationReadCode::available&&recovered.sequence==1&&recovered.state==OptimizationState{}),
        "creation fabricated future state");
  }
  for(std::size_t step=0;step<4;++step)for(unsigned mode=0;mode<3;++mode) {
    auto io=image(acknowledged);
    {
    auto writer=OptimizationStateWriter::resume(io,volume(),2,4);
    io->fail_at=io->calls+step;io->mode=mode;
    rejects([&]{writer.publish(policy(2));},"update fault not reached");
    require(writer.poisoned(),"failed writer not terminal");const auto calls=io->calls;
    rejects([&]{writer.publish(policy());},"poisoned update retry");
    rejects([&]{(void)writer.snapshot();},"poisoned cache exposed");
    rejects([&]{(void)OptimizationStateWriter::resume(io,volume(),2);},"poisoned lease released early");
    require(io->calls==calls,"terminal retry did backing writes/barriers");
    }
    const auto recovered=read(io->stable);
    require(recovered.code==OptimizationReadCode::available,"update lost all acknowledged copies");
    if(recovered.sequence==2)expect_state(recovered,2,policy());
    else expect_state(recovered,3,policy(2));
    auto restarted=image(io->stable);
    auto continued=OptimizationStateWriter::resume(restarted,volume(),recovered.sequence,4);
    continued.publish(policy(3));
    expect_state(read(restarted->stable),recovered.sequence+1,policy(3));
  }
  for(unsigned missing=0;missing<2;++missing) {
    auto damaged=acknowledged;damaged[missing*kStoreCommitUnit+96]^=std::byte{1};
    auto io=image(damaged);std::size_t repair_steps=0;
    {auto writer=OptimizationStateWriter::resume(io,volume(),2,4);
      expect_state(writer.snapshot(),2,policy());require(!writer.snapshot().degraded,"peer not repaired");
      require(writer.remaining_publications()==3,"repair did not consume allowance");repair_steps=io->calls;}
    for(std::size_t step=0;step<repair_steps;++step)for(unsigned mode=0;mode<3;++mode) {
      auto broken=image(damaged);broken->fail_at=step;broken->mode=mode;
      rejects([&]{(void)OptimizationStateWriter::resume(broken,volume(),2,4);},"repair fault not reached");
      expect_state(read(broken->stable),2,policy());
    }
  }
}
void user_data_isolation(const Bytes& acknowledged) {
  auto user=std::make_shared<Ram>(2*1024*1024);
  const Bytes payload{std::byte{1},std::byte{2},std::byte{3}};
  {auto writer=ObjectStoreWriter::create(user,volume(99),{StoreCompressionMode::raw});
    const std::array<StoreMutation,1> mutations{StoreMutation{42,false,payload}};
    writer.commit_replicated(mutations);}
  const auto original=user->stable;
  auto advisory=image(acknowledged);advisory->live.assign(kOptimizationAreaBytes,std::byte{0x7f});
  require(read_optimization_state(*advisory,volume()).code==OptimizationReadCode::invalid,"corrupt policy accepted");
  auto independent=std::make_shared<Ram>(2*1024*1024);independent->live=original;
  const auto recovered=ObjectStoreSnapshot::open(independent);
  require(recovered.read(42)==payload&&user->stable==original,"advisory damage changed user data");
  const auto calls=advisory->calls;
  rejects([&]{(void)OptimizationStateWriter::resume(advisory,volume(),2);},"lost area resumed");
  rejects([&]{(void)OptimizationStateWriter::create(advisory,volume());},"damaged area overwritten");
  require(advisory->calls==calls,"invalid area was reset/repaired automatically");
}
}
int main() {
  try {
    admission_and_budget();const auto acknowledged=initial_area();
    decoding(acknowledged);crash_matrix(acknowledged);user_data_isolation(acknowledged);
    std::cout<<"Optimization advisory-state checks: "<<checks
        <<"; fixed RAM area, all creation/update/repair boundaries, no files/GPU/mount.\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
