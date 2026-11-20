#include "flitzfs/optimization_state.hpp"
#include "flitzfs/native_filesystem.hpp"
#include "native_name_index.hpp"
#include "optimization_feature_wire.hpp"
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
constexpr std::size_t scan_at=benchmark_at+kOptimizationMaxBenchmarks*32,feature_at=scan_at+64;
constexpr auto feature_size=detail::kOptimizationGroupFeatureBytes;
constexpr std::size_t projection_size=8+8*kOptimizationXorProjections;
constexpr unsigned scrub_capability=6;
constexpr std::size_t scrub_profile_size=56;
constexpr std::size_t scrub_at=profile_at+kOptimizationMaxProfiles*scrub_profile_size,scrub_size=48;
static_assert(scrub_at+scrub_size<=benchmark_at);
constexpr std::array slot_magic{"FLTOP001","FLTOP002","FLTOP003","FLTOP004","FLTOP005","FLTOP006"};
constexpr std::size_t legacy_scan_capacity=63;
static_assert(kOptimizationGroupRepresentatives==(digest_at-feature_at)/feature_size);
[[noreturn]] void fail(const char* text){throw std::runtime_error(text);}
void require(bool value,const char* text){if(!value)fail(text);}
void put(std::span<std::byte> b,std::size_t at,std::uint64_t value,unsigned count=8) {
  for(unsigned i=0;i<count;++i)b[at+i]=static_cast<std::byte>(value>>(8*i));
}
std::uint64_t get(std::span<const std::byte> b,std::size_t at,unsigned count=8) {
  std::uint64_t value=0;for(unsigned i=0;i<count;++i)
    value|=std::uint64_t(std::to_integer<unsigned>(b[at+i]))<<(8*i);
  return value;
}
bool zeros(std::span<const std::byte> b){return std::all_of(b.begin(),b.end(),[](auto x){return x==std::byte{};});}
bool extent_coordinate(std::uint64_t inode,std::uint64_t block) noexcept {
  return inode>1 && inode<detail::kNativeIndexedInodeLimit && block<kNativeMaxFileBytes/kNativeExtentBytes;
}
bool valid_scan(const OptimizationGroupScan& scan) noexcept {
  if(scan.image==StoreIdentity{} || !scan.namespace_sequence || !extent_coordinate(scan.inode,scan.extent_index) ||
      (scan.complete&&scan.revisit) || scan.representatives.size()>kOptimizationGroupRepresentatives ||
      (!scan.xor_sampling && !scan.projections.empty()) ||
      scan.projections.size()>detail::optimization_group_projection_capacity(scan.representatives.size()))return false;
  std::size_t next_representative=0;
  for(const auto& projection:scan.projections) {
    if(projection.representative<next_representative ||
        projection.representative>=scan.representatives.size())return false;
    next_representative=projection.representative+1;
  }
  auto previous=detail::kNativeIndexedDataObject;
  for(const auto& item:scan.representatives) {
    if(item.object<=previous || !detail::valid_optimization_group_feature(item))return false;
    previous=item.object;
  }
  return true;
}
void encode_scan(Bytes& b,const OptimizationGroupScan& scan) {
  std::copy(scan.image.begin(),scan.image.end(),b.begin()+scan_at);
  put(b,scan_at+16,scan.namespace_sequence);
  put(b,scan_at+24,scan.inode);
  put(b,scan_at+32,scan.extent_index);
  put(b,scan_at+40,(scan.complete?1U:0U)|(scan.revisit?2U:0U),4);
  put(b,scan_at+44,scan.xor_sampling?2:1,4);
  put(b,scan_at+48,scan.representatives.size(),4);
  put(b,scan_at+52,scan.projections.size(),4);
  for(std::size_t i=0;i<scan.representatives.size();++i) {
    const auto offset=feature_at+i*feature_size;
    detail::encode_optimization_group_feature(
        std::span<std::byte,feature_size>(b.data()+offset,feature_size),scan.representatives[i]);
  }
  auto offset=feature_at+scan.representatives.size()*feature_size;
  for(const auto& projection:scan.projections) {
    put(b,offset,projection.representative,4);
    for(std::size_t i=0;i<projection.signatures.size();++i)
      put(b,offset+8+8*i,projection.signatures[i]);
    offset+=projection_size;
  }
}
std::optional<OptimizationGroupScan> decode_scan(std::span<const std::byte> b,std::uint64_t version) {
  const auto flags=get(b,scan_at+40,4),count=get(b,scan_at+48,4),projections=get(b,scan_at+52,4);
  const bool xor_sampling=version==5;
  const auto capacity=version==3?legacy_scan_capacity:kOptimizationGroupRepresentatives;
  if(flags>2 || get(b,scan_at+44,4)!=(xor_sampling?2U:1U) || count>capacity ||
      (!xor_sampling && projections) || !zeros(b.subspan(scan_at+56,8)) ||
      projections>detail::optimization_group_projection_capacity(static_cast<std::size_t>(count)))return {};
  const auto projection_at=feature_at+static_cast<std::size_t>(count)*feature_size;
  const auto end=projection_at+static_cast<std::size_t>(projections)*projection_size;
  if(!zeros(b.subspan(end,digest_at-end)))return {};
  OptimizationGroupScan scan;
  std::copy_n(b.begin()+scan_at,16,scan.image.begin());
  scan.namespace_sequence=get(b,scan_at+16);scan.inode=get(b,scan_at+24);scan.extent_index=get(b,scan_at+32);
  scan.complete=flags==1;scan.revisit=flags==2;scan.xor_sampling=xor_sampling;
  scan.representatives.reserve(static_cast<std::size_t>(count));
  for(std::size_t i=0;i<count;++i) {
    const auto offset=feature_at+i*feature_size;
    const auto item=detail::decode_optimization_group_feature(
        std::span<const std::byte,feature_size>(b.data()+offset,feature_size));
    if(!item)return {};
    scan.representatives.push_back(*item);
  }
  scan.projections.reserve(static_cast<std::size_t>(projections));
  for(auto offset=projection_at;offset<end;offset+=projection_size) {
    if(!zeros(b.subspan(offset+4,4)))return {};
    OptimizationGroupProjection projection;
    projection.representative=static_cast<std::uint32_t>(get(b,offset,4));
    for(std::size_t i=0;i<projection.signatures.size();++i)
      projection.signatures[i]=get(b,offset+8+8*i);
    scan.projections.push_back(projection);
  }
  if(!valid_scan(scan))return {};
  return scan;
}
bool valid(const OptimizationState& state) {
  if(state.profiles.size()>kOptimizationMaxProfiles||state.benchmarks.size()>kOptimizationMaxBenchmarks)return false;
  if(state.group_scan && (!state.settled_input_bytes || !valid_scan(*state.group_scan)))return false;
  if(const auto& scrub=state.payload_scrub) {
    const auto& position=scrub->position;
    if(!state.settled_input_bytes || !scrub->pass_started_utc || position.image==StoreIdentity{} ||
        !position.generation || !position.after || position.after>=position.through)return false;
  }
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
  const auto& scan=snapshot.state.group_scan;
  unsigned version=budgeted?2:1;
  if(scan)version=scan->xor_sampling?5:(scan->representatives.size()>legacy_scan_capacity?4:3);
  const auto& scrub=snapshot.state.payload_scrub;
  if(scrub)version=scrub_capability;
  const auto profile_size=scrub?scrub_profile_size:64;
  std::memcpy(b.data(),slot_magic[version-1],8);
  put(b,8,version,4);put(b,12,slot,4);put(b,16,snapshot.sequence);
  if(budgeted){put(b,52,1,4);put(b,56,*snapshot.state.settled_input_bytes);}
  std::copy(snapshot.volume.begin(),snapshot.volume.end(),b.begin()+24);
  put(b,40,1,4);put(b,44,snapshot.state.profiles.size(),4);put(b,48,snapshot.state.benchmarks.size(),4);
  for(std::size_t i=0;i<snapshot.state.profiles.size();++i) {
    const auto& p=snapshot.state.profiles[i];const auto at=profile_at+i*profile_size;
    put(b,at,p.workload_key);put(b,at+8,p.strategy,4);put(b,at+12,p.observations,4);
    put(b,at+16,p.logical_bytes);put(b,at+24,p.stored_bytes);put(b,at+32,p.cpu_ns);
    put(b,at+40,p.gpu_ns);put(b,at+48,p.confidence_millionths,4);
  }
  for(std::size_t i=0;i<snapshot.state.benchmarks.size();++i) {
    const auto& v=snapshot.state.benchmarks[i];const auto at=benchmark_at+i*32;
    put(b,at,v.workload_key);put(b,at+8,v.strategy,4);put(b,at+12,v.logical_bytes,4);
    put(b,at+16,v.stored_bytes,4);put(b,at+20,v.backend,4);put(b,at+24,v.elapsed_ns);
  }
  if(scan)encode_scan(b,*scan);
  if(scrub) {
    const auto& position=scrub->position;
    std::copy(position.image.begin(),position.image.end(),b.begin()+scrub_at);
    put(b,scrub_at+16,position.generation);put(b,scrub_at+24,position.after);
    put(b,scrub_at+32,position.through);put(b,scrub_at+40,scrub->pass_started_utc);
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
  if(version<1 || version>slot_magic.size() || std::memcmp(b.data(),slot_magic[version-1],8) ||
      get(b,40,4)!=1)return {SlotCode::unsupported,{}};
  OptimizationSnapshot out;out.code=OptimizationReadCode::available;out.sequence=get(b,16);
  std::copy_n(b.begin()+24,16,out.volume.begin());
  if(out.volume!=volume)return {SlotCode::foreign,{}};
  const auto profiles=get(b,44,4),benchmarks=get(b,48,4);
  if(get(b,12,4)!=slot||!out.sequence||profiles>kOptimizationMaxProfiles||
      benchmarks>kOptimizationMaxBenchmarks||
      (version==1?!zeros(b.subspan(52,12)):get(b,52,4)!=1))return {SlotCode::invalid,{}};
  if(version>=2)out.state.settled_input_bytes=get(b,56);
  out.state.profiles.reserve(static_cast<std::size_t>(profiles));
  out.state.benchmarks.reserve(static_cast<std::size_t>(benchmarks));
  const auto profile_size=version==scrub_capability?scrub_profile_size:64;
  for(std::size_t i=0;i<kOptimizationMaxProfiles;++i) {
    const auto at=profile_at+i*profile_size;
    if(i>=profiles){if(!zeros(b.subspan(at,profile_size)))return {SlotCode::invalid,{}};continue;}
    OptimizationProfile p;p.workload_key=get(b,at);p.strategy=static_cast<std::uint32_t>(get(b,at+8,4));
    p.observations=static_cast<std::uint32_t>(get(b,at+12,4));p.logical_bytes=get(b,at+16);
    p.stored_bytes=get(b,at+24);p.cpu_ns=get(b,at+32);p.gpu_ns=get(b,at+40);
    p.confidence_millionths=static_cast<std::uint32_t>(get(b,at+48,4));
    if(!zeros(b.subspan(at+52,profile_size-52)))return {SlotCode::invalid,{}};
    out.state.profiles.push_back(p);
  }
  for(std::size_t i=0;i<kOptimizationMaxBenchmarks;++i) {
    const auto at=benchmark_at+i*32;
    if(i>=benchmarks){if(!zeros(b.subspan(at,32)))return {SlotCode::invalid,{}};continue;}
    OptimizationBenchmark v;v.workload_key=get(b,at);v.strategy=static_cast<std::uint32_t>(get(b,at+8,4));
    v.logical_bytes=static_cast<std::uint32_t>(get(b,at+12,4));v.stored_bytes=static_cast<std::uint32_t>(get(b,at+16,4));
    v.backend=static_cast<std::uint32_t>(get(b,at+20,4));v.elapsed_ns=get(b,at+24);out.state.benchmarks.push_back(v);
  }
  if(version==scrub_capability) {
    if(!zeros(b.subspan(scrub_at+scrub_size,benchmark_at-scrub_at-scrub_size)))return {SlotCode::invalid,{}};
    auto& scrub=out.state.payload_scrub.emplace();
    std::copy_n(b.begin()+scrub_at,16,scrub.position.image.begin());
    scrub.position.generation=get(b,scrub_at+16);scrub.position.after=get(b,scrub_at+24);
    scrub.position.through=get(b,scrub_at+32);scrub.pass_started_utc=get(b,scrub_at+40);
    if(!zeros(b.subspan(scan_at,digest_at-scan_at))) {
      out.state.group_scan=decode_scan(b,get(b,scan_at+44,4)==2?5:4);
      if(!out.state.group_scan)return {SlotCode::invalid,{}};
    }
  }else if(version>=3) {
    out.state.group_scan=decode_scan(b,version);
    if(!out.state.group_scan)return {SlotCode::invalid,{}};
  }else if(!zeros(b.subspan(scan_at,digest_at-scan_at)))return {SlotCode::invalid,{}};
  if(!valid(out.state))return {SlotCode::invalid,{}};
  return {SlotCode::valid,std::move(out)};
}
OptimizationSnapshot no_state(OptimizationReadCode code) {OptimizationSnapshot out;out.code=code;return out;}
bool same_policy(const OptimizationState& a,const OptimizationState& b) {
  return a.profiles==b.profiles && a.benchmarks==b.benchmarks && a.group_scan==b.group_scan;
}
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
  const auto& fresh=slots[newer].snapshot.state;
  const auto& prior=slots[older].snapshot.state;
  if(fresh.settled_input_bytes) {
    const auto next=*fresh.settled_input_bytes,previous=*prior.settled_input_bytes;
    // Read-funded verification can advance only the cursor and forfeit input credit.
    const bool scrub_only=fresh.payload_scrub && fresh.payload_scrub!=prior.payload_scrub && same_policy(fresh,prior);
    if(next<previous || (next-previous<kOptimizationAreaBytes && !scrub_only))
      return {no_state(OptimizationReadCode::invalid),0};
  }
  auto out=std::move(slots[newer].snapshot);out.degraded=true;return {std::move(out),newer};
}
void contract(const StoreByteIo& io) {
  require(valid_store_io_contract(io.contract()),"optimization area needs qualified persistence/failure units");
}
}
std::size_t detail::optimization_group_projection_capacity(std::size_t representatives) noexcept {
  if(representatives>kOptimizationGroupRepresentatives)return 0;
  return (std::min)(representatives,(digest_at-feature_at-representatives*feature_size)/projection_size);
}
bool detail::valid_optimization_group_feature(const OptimizationGroupFeature& feature) noexcept {
  return feature.object>kNativeIndexedDataObject && feature.object<2*kNativeIndexedDataObject &&
      feature.revision && extent_coordinate(feature.inode,feature.extent_index);
}
void detail::encode_optimization_group_feature(std::span<std::byte,kOptimizationGroupFeatureBytes> bytes,
    const OptimizationGroupFeature& feature) {
  put(bytes,0,feature.inode);
  put(bytes,8,feature.extent_index);
  put(bytes,16,feature.object);
  put(bytes,24,feature.revision);
  std::ranges::copy(feature.content,bytes.begin()+32);
  for(std::size_t j=0;j<feature.bands.size();++j)put(bytes,64+j*8,feature.bands[j]);
  put(bytes,160,feature.exact_base);
}
std::optional<OptimizationGroupFeature> detail::decode_optimization_group_feature(
    std::span<const std::byte,kOptimizationGroupFeatureBytes> bytes) {
  const auto base=get(bytes,160);
  if(base>1)return {};
  OptimizationGroupFeature feature;
  feature.inode=get(bytes,0);
  feature.extent_index=get(bytes,8);
  feature.object=get(bytes,16);
  feature.revision=get(bytes,24);
  std::copy_n(bytes.begin()+32,32,feature.content.begin());
  for(std::size_t j=0;j<feature.bands.size();++j)feature.bands[j]=get(bytes,64+j*8);
  feature.exact_base=base==1;
  if(!valid_optimization_group_feature(feature))return {};
  return feature;
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
  void check_pending_repair() const {
    const auto current=load(*io,selected.volume);
    require(current.snapshot.code==OptimizationReadCode::available&&current.snapshot.degraded&&
        current.snapshot.sequence==selected.sequence&&current.snapshot.state==selected.state&&
        1-current.slot==repair_first,"pending budget repair changed outside owner");
  }
  void pair(const OptimizationSnapshot& next,unsigned first=0,bool work_funded=false) {
    require(work_funded||remaining,"optimization publication allowance exhausted");
    // Prepare both bounded records and complete admission BEFORE mutation.
    const auto a=encode(next,first),b=encode(next,1-first);
    io->admit_write(kOptimizationAreaBytes);
    if(!work_funded)--remaining;
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
    p.check_pending_repair();
    p.pair(next,p.repair_first);p.repair_pending=false;
  }catch(...){p.poisoned=true;throw;}
  return BudgetResult::published;
}
OptimizationStateWriter::BudgetResult OptimizationStateWriter::publish_budgeted(
    const OptimizationState& state,std::uint64_t input,bool automatic_scan,std::uint64_t additional_write_bytes) {
  require(impl_&&!impl_->poisoned&&!impl_->repair_pending&&impl_->selected.state.settled_input_bytes,
      "no ready budgeted writer");
  auto& p=*impl_;
  require(valid(state)&&state.settled_input_bytes==p.selected.state.settled_input_bytes,
      "caller cannot change budget settlement");
  require(!automatic_scan||state.group_scan.has_value(),"automatic publication needs a scan checkpoint");
  require(automatic_scan||!additional_write_bytes,"only a scan can reserve catalog writes");
  if(same_policy(state,p.selected.state) && !additional_write_bytes) {
    try{p.check();}catch(...){p.poisoned=true;throw;}
    return BudgetResult::unchanged;
  }
  const auto settled=*p.selected.state.settled_input_bytes;
  const auto base_cost=automatic_scan?kOptimizationGroupScanInputBytes:kOptimizationAreaBytes;
  if((!automatic_scan&&!p.remaining)||input<settled||input-settled<base_cost ||
      additional_write_bytes>(input-settled-base_cost)/kOptimizationGroupScanInputPerWriteByte)
    return BudgetResult::deferred;
  const auto cost=base_cost+additional_write_bytes*kOptimizationGroupScanInputPerWriteByte;
  require(p.selected.sequence!=(std::numeric_limits<std::uint64_t>::max)(),"optimization sequence exhausted");
  OptimizationSnapshot next;next.code=OptimizationReadCode::available;next.volume=p.selected.volume;
  next.sequence=p.selected.sequence+1;next.state=state;
  next.state.settled_input_bytes=settled+cost;
  next.state.payload_scrub=p.selected.state.payload_scrub;
  p.io->admit_write(kOptimizationAreaBytes);
  try{p.check();p.pair(next,0,automatic_scan);}catch(...){p.poisoned=true;throw;}
  return BudgetResult::published;
}
std::optional<OptimizationPayloadScrub> OptimizationStateWriter::payload_scrub_checkpoint() const noexcept {
  return poisoned()?std::nullopt:impl_->selected.state.payload_scrub;
}
bool OptimizationStateWriter::publish_payload_scrub(const OptimizationPayloadScrub& progress) {
  require(!poisoned()&&impl_->selected.state.settled_input_bytes,"no funded advisory owner for scrub");
  auto& p=*impl_;
  if(p.selected.state.payload_scrub==progress)return false;
  require(p.selected.sequence!=(std::numeric_limits<std::uint64_t>::max)(),"optimization sequence exhausted");
  auto next=p.selected;++next.sequence;next.degraded=false;
  next.state.payload_scrub=progress;
  if(p.repair_pending)next.state.settled_input_bytes=(std::max)(*next.state.settled_input_bytes,*p.repair_input_floor);
  require(valid(next.state),"invalid payload scrub checkpoint");
  p.io->admit_write(kOptimizationAreaBytes);
  try {
    if(p.repair_pending)p.check_pending_repair();else p.check();
    p.pair(next,p.repair_pending?p.repair_first:0,true);
    p.repair_pending=false;p.repair_input_floor.reset();
  }catch(...){p.poisoned=true;throw;}
  return true;
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
