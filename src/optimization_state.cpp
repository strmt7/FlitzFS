#include "flitzfs/optimization_state.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
namespace flitzfs {
namespace {
using Bytes=std::vector<std::byte>;
constexpr std::size_t digest_at=kStoreCommitUnit-32,profile_at=64,benchmark_at=64+32*64;
constexpr char magic[]="FLTOP001",budget_magic[]="FLTOP002";
[[noreturn]] void fail(const char* text){throw std::runtime_error(text);}
void require(bool value,const char* text){if(!value)fail(text);}
void put(Bytes& b,std::size_t at,std::uint64_t value,unsigned count=8) {
  for(unsigned i=0;i<count;++i)b[at+i]=static_cast<std::byte>(value>>(8*i));
}
std::uint64_t get(std::span<const std::byte> b,std::size_t at,unsigned count=8) {
  std::uint64_t value=0;for(unsigned i=0;i<count;++i)
    value|=std::uint64_t(std::to_integer<unsigned>(b[at+i]))<<(8*i);
  return value;
}
bool zeros(std::span<const std::byte> b){return std::all_of(b.begin(),b.end(),[](auto x){return x==std::byte{};});}
bool valid(const OptimizationState& state) {
  if(state.profiles.size()>kOptimizationMaxProfiles||state.benchmarks.size()>kOptimizationMaxBenchmarks)return false;
  std::pair<std::uint64_t,std::uint32_t> previous{};
  for(const auto& p:state.profiles) {
    const auto key=std::pair{p.workload_key,p.strategy};
    if(!p.workload_key||!p.strategy||p.strategy>4||key<=previous||!p.observations||
        p.observations>1000000||!p.logical_bytes||p.logical_bytes>std::uint64_t(p.observations)*65536||
        p.stored_bytes>p.logical_bytes||p.confidence_millionths>1000000)return false;
    previous=key;
  }
  for(const auto& b:state.benchmarks)
    if(!b.workload_key||!b.strategy||b.strategy>4||!b.logical_bytes||b.logical_bytes>65536||
        b.stored_bytes>b.logical_bytes||!b.elapsed_ns||(b.backend!=1&&b.backend!=2))return false;
  return true;
}
Bytes encode(const OptimizationSnapshot& snapshot,unsigned slot) {
  require(snapshot.volume!=StoreIdentity{}&&snapshot.sequence&&slot<2&&valid(snapshot.state),
      "invalid optimization state");
  Bytes b(kStoreCommitUnit);
  const bool budgeted=snapshot.state.settled_input_bytes.has_value();
  std::memcpy(b.data(),budgeted?budget_magic:magic,8);put(b,8,budgeted?2:1,4);put(b,12,slot,4);put(b,16,snapshot.sequence);
  if(budgeted){put(b,52,1,4);put(b,56,*snapshot.state.settled_input_bytes);}
  std::copy(snapshot.volume.begin(),snapshot.volume.end(),b.begin()+24);
  put(b,40,1,4);put(b,44,snapshot.state.profiles.size(),4);put(b,48,snapshot.state.benchmarks.size(),4);
  for(std::size_t i=0;i<snapshot.state.profiles.size();++i) {
    const auto& p=snapshot.state.profiles[i];const auto at=profile_at+i*64;
    put(b,at,p.workload_key);put(b,at+8,p.strategy,4);put(b,at+12,p.observations,4);
    put(b,at+16,p.logical_bytes);put(b,at+24,p.stored_bytes);put(b,at+32,p.cpu_ns);
    put(b,at+40,p.gpu_ns);put(b,at+48,p.confidence_millionths,4);
  }
  for(std::size_t i=0;i<snapshot.state.benchmarks.size();++i) {
    const auto& v=snapshot.state.benchmarks[i];const auto at=benchmark_at+i*32;
    put(b,at,v.workload_key);put(b,at+8,v.strategy,4);put(b,at+12,v.logical_bytes,4);
    put(b,at+16,v.stored_bytes,4);put(b,at+20,v.backend,4);put(b,at+24,v.elapsed_ns);
  }
  const auto digest=detail::record_digest(std::span<const std::byte>(b).first(digest_at));
  std::copy(digest.begin(),digest.end(),b.begin()+digest_at);return b;
}
enum class SlotCode { corrupt,valid,invalid,unsupported,foreign };
struct Slot {SlotCode code=SlotCode::corrupt;OptimizationSnapshot snapshot;};
Slot decode(std::span<const std::byte> b,unsigned slot,StoreIdentity volume) {
  const auto digest=detail::record_digest(b.first(digest_at));
  if(!std::equal(digest.begin(),digest.end(),b.begin()+digest_at))return {};
  const auto version=get(b,8,4);
  if((version!=1&&version!=2)||std::memcmp(b.data(),version==2?budget_magic:magic,8)||
      get(b,40,4)!=1)return {SlotCode::unsupported,{}};
  OptimizationSnapshot out;out.code=OptimizationReadCode::available;out.sequence=get(b,16);
  std::copy_n(b.begin()+24,16,out.volume.begin());
  if(out.volume!=volume)return {SlotCode::foreign,{}};
  const auto profiles=get(b,44,4),benchmarks=get(b,48,4);
  if(get(b,12,4)!=slot||!out.sequence||profiles>kOptimizationMaxProfiles||
      benchmarks>kOptimizationMaxBenchmarks||
      (version==1?!zeros(b.subspan(52,12)):get(b,52,4)!=1))return {SlotCode::invalid,{}};
  if(version==2)out.state.settled_input_bytes=get(b,56);
  out.state.profiles.reserve(static_cast<std::size_t>(profiles));
  out.state.benchmarks.reserve(static_cast<std::size_t>(benchmarks));
  for(std::size_t i=0;i<kOptimizationMaxProfiles;++i) {
    const auto at=profile_at+i*64;
    if(i>=profiles){if(!zeros(b.subspan(at,64)))return {SlotCode::invalid,{}};continue;}
    OptimizationProfile p;p.workload_key=get(b,at);p.strategy=static_cast<std::uint32_t>(get(b,at+8,4));
    p.observations=static_cast<std::uint32_t>(get(b,at+12,4));p.logical_bytes=get(b,at+16);
    p.stored_bytes=get(b,at+24);p.cpu_ns=get(b,at+32);p.gpu_ns=get(b,at+40);
    p.confidence_millionths=static_cast<std::uint32_t>(get(b,at+48,4));
    if(!zeros(b.subspan(at+52,12)))return {SlotCode::invalid,{}};
    out.state.profiles.push_back(p);
  }
  for(std::size_t i=0;i<kOptimizationMaxBenchmarks;++i) {
    const auto at=benchmark_at+i*32;
    if(i>=benchmarks){if(!zeros(b.subspan(at,32)))return {SlotCode::invalid,{}};continue;}
    OptimizationBenchmark v;v.workload_key=get(b,at);v.strategy=static_cast<std::uint32_t>(get(b,at+8,4));
    v.logical_bytes=static_cast<std::uint32_t>(get(b,at+12,4));v.stored_bytes=static_cast<std::uint32_t>(get(b,at+16,4));
    v.backend=static_cast<std::uint32_t>(get(b,at+20,4));v.elapsed_ns=get(b,at+24);out.state.benchmarks.push_back(v);
  }
  if(!zeros(b.subspan(benchmark_at+kOptimizationMaxBenchmarks*32,
        digest_at-benchmark_at-kOptimizationMaxBenchmarks*32))||!valid(out.state))return {SlotCode::invalid,{}};
  return {SlotCode::valid,std::move(out)};
}
OptimizationSnapshot no_state(OptimizationReadCode code) {OptimizationSnapshot out;out.code=code;return out;}
struct Loaded {OptimizationSnapshot snapshot;unsigned slot=0;};
Loaded load(const StoreByteIo& io,StoreIdentity volume) {
  require(volume!=StoreIdentity{},"missing optimization volume identity");
  const auto extent=io.extent();
  if(!extent)return {};
  if(extent!=kOptimizationAreaBytes)return {no_state(OptimizationReadCode::invalid),0};
  Bytes scratch(kStoreCommitUnit);Slot slots[2];
  for(unsigned i=0;i<2;++i){io.read_exact(i*kStoreCommitUnit,scratch);slots[i]=decode(scratch,i,volume);}
  for(const auto& s:slots) {
    if(s.code==SlotCode::invalid)return {no_state(OptimizationReadCode::invalid),0};
    if(s.code==SlotCode::unsupported)return {no_state(OptimizationReadCode::unsupported),0};
    if(s.code==SlotCode::foreign)return {no_state(OptimizationReadCode::foreign_volume),0};
  }
  const bool a=slots[0].code==SlotCode::valid,b=slots[1].code==SlotCode::valid;
  if(!a&&!b)return {no_state(OptimizationReadCode::invalid),0};
  if(!a||!b){const unsigned chosen=a?0:1;auto out=std::move(slots[chosen].snapshot);out.degraded=true;return {std::move(out),chosen};}
  auto& x=slots[0].snapshot;auto& y=slots[1].snapshot;
  if(bool(x.state.settled_input_bytes)!=bool(y.state.settled_input_bytes))
    return {no_state(OptimizationReadCode::invalid),0};
  if(x.sequence==y.sequence) {
    if(x.state!=y.state)return {no_state(OptimizationReadCode::invalid),0};
    return {std::move(y),1};
  }
  const unsigned newer=x.sequence>y.sequence?0:1,older=1-newer;
  if(slots[newer].snapshot.sequence-slots[older].snapshot.sequence!=1)
    return {no_state(OptimizationReadCode::invalid),0};
  const auto& fresh=slots[newer].snapshot.state.settled_input_bytes;
  const auto& prior=slots[older].snapshot.state.settled_input_bytes;
  if(fresh&&(*fresh<*prior||*fresh-*prior<kOptimizationAreaBytes))
    return {no_state(OptimizationReadCode::invalid),0};
  auto out=std::move(slots[newer].snapshot);out.degraded=true;return {std::move(out),newer};
}
void contract(const StoreByteIo& io) {
  require(valid_store_io_contract(io.contract()),"optimization area needs qualified persistence/failure units");
}
}
OptimizationSnapshot read_optimization_state(const StoreByteIo& io,StoreIdentity volume) {
  require(volume!=StoreIdentity{},"missing optimization volume identity");
  try{return load(io,volume).snapshot;}
  catch(const std::bad_alloc&){throw;}
  catch(const std::exception&){return no_state(OptimizationReadCode::io_failure);}
}
struct OptimizationStateWriter::Impl {
  std::shared_ptr<StoreByteIo> io;
  OptimizationSnapshot selected;
  unsigned remaining=0;
  bool leased=false,poisoned=false,repair_pending=false;
  unsigned repair_first=0;
  std::optional<std::uint64_t> repair_input_floor;
  Impl(std::shared_ptr<StoreByteIo> backing,unsigned limit):io(std::move(backing)),remaining(limit) {
    require(io&&limit&&limit<=4,"invalid optimization backend/publication allowance");
    require(io->acquire_writer(),"optimization backend already has a writer");leased=true;
  }
  ~Impl(){if(leased)io->release_writer();}
  void check() const {
    require(!poisoned,"optimization writer is terminal");
    const auto current=load(*io,selected.volume).snapshot;
    require(current.code==OptimizationReadCode::available&&!current.degraded&&
        current.sequence==selected.sequence&&current.state==selected.state,"optimization state changed outside owner");
  }
  void pair(const OptimizationSnapshot& next,unsigned first=0) {
    require(remaining,"optimization publication allowance exhausted");
    // Prepare both bounded records and complete admission BEFORE mutation.
    const auto a=encode(next,first),b=encode(next,1-first);
    io->admit_write(kOptimizationAreaBytes);--remaining;
    io->write_exact(first*kStoreCommitUnit,a);io->flush();
    io->write_exact((1-first)*kStoreCommitUnit,b);io->flush();
    auto observed=load(*io,next.volume).snapshot;
    require(observed.code==OptimizationReadCode::available&&!observed.degraded&&
        observed.sequence==next.sequence&&observed.state==next.state,"optimization publication readback");
    static_assert(std::is_nothrow_move_assignable_v<OptimizationSnapshot>);
    selected=std::move(observed);
  }
};
OptimizationStateWriter::OptimizationStateWriter(std::unique_ptr<Impl> p) noexcept:impl_(std::move(p)){}
OptimizationStateWriter::OptimizationStateWriter(OptimizationStateWriter&&) noexcept=default;
OptimizationStateWriter& OptimizationStateWriter::operator=(OptimizationStateWriter&&) noexcept=default;
OptimizationStateWriter::~OptimizationStateWriter()=default;
OptimizationStateWriter OptimizationStateWriter::create(std::shared_ptr<StoreByteIo> io,StoreIdentity volume,unsigned limit) {
  return create_state(std::move(io),volume,{},limit);
}
OptimizationStateWriter OptimizationStateWriter::create_budgeted(std::shared_ptr<StoreByteIo> io,
    StoreIdentity volume,std::uint64_t settled,unsigned limit) {
  OptimizationState state;state.settled_input_bytes=settled;
  return create_state(std::move(io),volume,std::move(state),limit);
}
OptimizationStateWriter OptimizationStateWriter::create_state(std::shared_ptr<StoreByteIo> io,
    StoreIdentity volume,OptimizationState state,unsigned limit) {
  require(volume!=StoreIdentity{},"missing optimization volume identity");
  auto p=std::make_unique<Impl>(std::move(io),limit);contract(*p->io);
  require(!p->io->extent(),"optimization creation requires a new empty backend");
  p->io->admit_write(kOptimizationAreaBytes);
  OptimizationSnapshot initial;initial.code=OptimizationReadCode::available;initial.volume=volume;initial.sequence=1;
  initial.state=std::move(state);
  const Bytes empty(kStoreCommitUnit);
  p->io->write_exact(0,empty);p->io->write_exact(kStoreCommitUnit,empty);p->pair(initial);
  return OptimizationStateWriter(std::move(p));
}
OptimizationStateWriter OptimizationStateWriter::resume(std::shared_ptr<StoreByteIo> io,StoreIdentity volume,
    std::uint64_t sequence,unsigned limit) {
  require(volume!=StoreIdentity{}&&sequence,"missing optimization volume/sequence lease");
  auto p=std::make_unique<Impl>(std::move(io),limit);contract(*p->io);
  auto observed=load(*p->io,volume);
  require(observed.snapshot.code==OptimizationReadCode::available&&observed.snapshot.sequence==sequence&&
      !observed.snapshot.state.settled_input_bytes,
      "legacy resume requires exact current unbudgeted state");
  p->selected=observed.snapshot;p->io->admit_write(kOptimizationAreaBytes);
  // Fence the observed survivor before repair; preserve it by writing peer first.
  p->io->flush();
  const auto fenced=load(*p->io,volume);
  require(fenced.snapshot.code==OptimizationReadCode::available&&fenced.snapshot.sequence==sequence&&
      fenced.snapshot.state==observed.snapshot.state&&fenced.slot==observed.slot&&
      fenced.snapshot.degraded==observed.snapshot.degraded,"optimization state changed during resume");
  if(observed.snapshot.degraded)p->pair(observed.snapshot,1-observed.slot);
  else p->check();
  return OptimizationStateWriter(std::move(p));
}
void OptimizationStateWriter::publish(const OptimizationState& state) {
  require(impl_&&!impl_->poisoned,"optimization writer is terminal");
  require(valid(state),"invalid optimization profile/history");
  auto& p=*impl_;
  if(state==p.selected.state) {
    try {p.check();}catch(...){p.poisoned=true;throw;}
    return; // Identical current advice consumes no writes, sequence or allowance.
  }
  require(!p.selected.state.settled_input_bytes&&!state.settled_input_bytes,
      "budgeted state requires controlled publication");
  require(p.remaining,"optimization publication allowance exhausted");
  require(p.selected.sequence!=(std::numeric_limits<std::uint64_t>::max)(),"optimization sequence exhausted");
  OptimizationSnapshot next;next.code=OptimizationReadCode::available;next.volume=p.selected.volume;
  next.sequence=p.selected.sequence+1;next.state=state;
  // Rejected arguments, exhausted allowance and resource preflight do no writes.
  p.io->admit_write(kOptimizationAreaBytes);
  try {p.check();p.pair(next);}
  catch(...){p.poisoned=true;throw;}
}
OptimizationStateWriter OptimizationStateWriter::resume_budgeted(std::shared_ptr<StoreByteIo> io,
    StoreIdentity volume,std::uint64_t sequence,std::uint64_t input,unsigned limit) {
  require(volume!=StoreIdentity{}&&sequence,"missing optimization volume/sequence lease");
  auto p=std::make_unique<Impl>(std::move(io),limit);contract(*p->io);
  auto observed=load(*p->io,volume);
  require(observed.snapshot.code==OptimizationReadCode::available&&observed.snapshot.sequence==sequence&&
      observed.snapshot.state.settled_input_bytes&&*observed.snapshot.state.settled_input_bytes<=input,
      "budgeted resume requires exact current funded state");
  p->selected=observed.snapshot;
  p->repair_pending=observed.snapshot.degraded;p->repair_first=1-observed.slot;
  if(p->repair_pending)p->repair_input_floor=input; // Damaged state forfeits old unspent entitlement.
  p->io->admit_write(kOptimizationAreaBytes);p->io->flush();
  const auto fenced=load(*p->io,volume);
  require(fenced.snapshot.code==OptimizationReadCode::available&&fenced.snapshot.sequence==sequence&&
      fenced.snapshot.state==observed.snapshot.state&&fenced.slot==observed.slot&&
      fenced.snapshot.degraded==observed.snapshot.degraded,"budgeted state changed during resume");
  OptimizationStateWriter result(std::move(p));
  if(result.budget_repair_pending())(void)result.repair_budgeted(input);
  return result;
}
OptimizationStateWriter::BudgetResult OptimizationStateWriter::repair_budgeted(std::uint64_t input) {
  require(impl_&&!impl_->poisoned&&impl_->selected.state.settled_input_bytes,"no budgeted writer");
  auto& p=*impl_;if(!p.repair_pending)return BudgetResult::unchanged;
  const auto settled=(std::max)(*p.selected.state.settled_input_bytes,p.repair_input_floor.value_or(0));
  if(!p.remaining||input<settled||input-settled<kOptimizationAreaBytes)return BudgetResult::deferred;
  require(p.selected.sequence!=(std::numeric_limits<std::uint64_t>::max)(),"optimization sequence exhausted");
  auto next=p.selected;++next.sequence;next.degraded=false;
  next.state.settled_input_bytes=settled+kOptimizationAreaBytes;
  p.io->admit_write(kOptimizationAreaBytes);
  try {
    const auto current=load(*p.io,next.volume);
    require(current.snapshot.code==OptimizationReadCode::available&&current.snapshot.degraded&&
        current.snapshot.sequence==p.selected.sequence&&current.snapshot.state==p.selected.state&&
        1-current.slot==p.repair_first,"pending budget repair changed outside owner");
    p.pair(next,p.repair_first);p.repair_pending=false;
  }catch(...){p.poisoned=true;throw;}
  return BudgetResult::published;
}
OptimizationStateWriter::BudgetResult OptimizationStateWriter::publish_budgeted(
    const OptimizationState& state,std::uint64_t input) {
  require(impl_&&!impl_->poisoned&&!impl_->repair_pending&&impl_->selected.state.settled_input_bytes,
      "no ready budgeted writer");
  auto& p=*impl_;
  require(valid(state)&&state.settled_input_bytes==p.selected.state.settled_input_bytes,
      "caller cannot change budget settlement");
  if(state==p.selected.state) {
    try{p.check();}catch(...){p.poisoned=true;throw;}
    return BudgetResult::unchanged;
  }
  const auto settled=*p.selected.state.settled_input_bytes;
  if(!p.remaining||input<settled||input-settled<kOptimizationAreaBytes)return BudgetResult::deferred;
  require(p.selected.sequence!=(std::numeric_limits<std::uint64_t>::max)(),"optimization sequence exhausted");
  OptimizationSnapshot next;next.code=OptimizationReadCode::available;next.volume=p.selected.volume;
  next.sequence=p.selected.sequence+1;next.state=state;
  next.state.settled_input_bytes=settled+kOptimizationAreaBytes;
  p.io->admit_write(kOptimizationAreaBytes);
  try{p.check();p.pair(next);}catch(...){p.poisoned=true;throw;}
  return BudgetResult::published;
}
bool OptimizationStateWriter::budget_repair_pending() const noexcept {
  return impl_&&!impl_->poisoned&&impl_->repair_pending;
}
std::optional<std::uint64_t> OptimizationStateWriter::budget_repair_floor() const noexcept {
  return impl_&&!impl_->poisoned&&impl_->repair_pending?impl_->repair_input_floor:std::nullopt;
}
std::optional<std::uint64_t> OptimizationStateWriter::budget_settlement() const noexcept {
  return impl_&&!impl_->poisoned?impl_->selected.state.settled_input_bytes:std::nullopt;
}
OptimizationSnapshot OptimizationStateWriter::snapshot() const {
  require(impl_&&!impl_->poisoned&&!impl_->repair_pending,"optimization writer is unavailable");return impl_->selected;
}
std::uint64_t OptimizationStateWriter::sequence() const noexcept {
  return impl_&&!impl_->poisoned&&!impl_->repair_pending?impl_->selected.sequence:0;
}
unsigned OptimizationStateWriter::remaining_publications() const noexcept{return impl_?impl_->remaining:0;}
bool OptimizationStateWriter::poisoned() const noexcept{return !impl_||impl_->poisoned;}
} // namespace flitzfs
