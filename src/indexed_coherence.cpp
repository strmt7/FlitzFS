#include "flitzfs/indexed_coherence.hpp"
#include "optimization_feature_wire.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

namespace flitzfs {
namespace {
constexpr std::uint64_t root_object=1;
constexpr std::size_t posting_header_bytes=16;
constexpr auto feature_bytes=detail::kOptimizationGroupFeatureBytes;
constexpr auto posting_bytes=posting_header_bytes+feature_bytes;
constexpr std::size_t feature_identity_bytes=64,word_bytes=8;
constexpr auto feature_role_offset=feature_bytes-word_bytes;
constexpr auto projected_posting_bytes=posting_header_bytes+feature_identity_bytes+2*word_bytes;
constexpr std::uint8_t alternate_schema=4;
constexpr std::array<std::string_view,4> root_signatures{"FLTZCOR1","FLTZCOR2","FLTZCOR3","FLTZCOR4"};
constexpr std::array<std::string_view,4> posting_signatures{"FLTZCOE1","FLTZCOE2","FLTZCOE3","FLTZCOE4"};
constexpr std::uint64_t alternate_domain=1ULL<<62,domain_mask=3ULL<<62,slot_mask=255;
static_assert(feature_role_offset==feature_identity_bytes+word_bytes*kOptimizationGroupBands);
static_assert(projected_posting_bytes<posting_bytes);
constexpr auto raw_storage=StoreCompressionOptions{StoreCompressionMode::raw,1,false};
using Root=std::array<std::byte,32>;
using Bytes=std::vector<std::byte>;

void signature(std::span<std::byte> bytes,std::string_view magic) {
  std::ranges::transform(magic,bytes.begin(),[](char value){return static_cast<std::byte>(value);});
}
bool signature_matches(std::span<const std::byte> bytes,std::string_view magic) {
  return std::equal(magic.begin(),magic.end(),bytes.begin(),
      [](char expected,std::byte actual){return static_cast<std::byte>(expected)==actual;});
}
constexpr std::uint8_t posting_schema(std::uint32_t band) noexcept {
  if(band<kCoherenceMaskedBand)return 1;
  if(band<kCoherenceModularBand)return 2;
  return 3;
}
struct CatalogFormat {
  std::uint8_t schema=1,band_schema=1;
  bool operator==(const CatalogFormat&) const=default;
};
Root root_record(StoreIdentity volume,StoreIdentity image,CatalogFormat format) {
  if(volume==StoreIdentity{} || image==StoreIdentity{} || image==volume)
    throw std::invalid_argument("coherence catalog identities");
  Root bytes{};
  signature(bytes,root_signatures[format.schema-1]);
  std::ranges::copy(image,bytes.begin()+8);
  bytes[24]=static_cast<std::byte>(format.schema);
  if(format.schema==alternate_schema)bytes[25]=static_cast<std::byte>(format.band_schema);
  return bytes;
}
std::uint64_t posting_key(std::uint32_t band,std::uint64_t fingerprint) noexcept {
  std::array<std::byte,9> input{};
  input.front()=static_cast<std::byte>(band);
  for(unsigned i=0;i<8;++i)input[i+1]=static_cast<std::byte>(fingerprint>>(i*8));
  const auto digest=detail::record_digest(input);
  std::uint64_t key=0;
  for(unsigned i=0;i<8;++i)key|=std::uint64_t(std::to_integer<unsigned>(digest[i]))<<(i*8);
  return key | (1ULL<<63); // Separate from low keys reserved for catalog metadata.
}
std::uint64_t fingerprint_of(const IndexedCoherencePosting& posting) noexcept {
  if(posting.band>=kOptimizationGroupBands)return posting.projected_fingerprint;
  return posting.representative.bands[posting.band];
}
std::uint64_t alternate_prefix(std::uint32_t band,std::uint64_t fingerprint) noexcept {
  return (posting_key(band,fingerprint)&~(domain_mask|slot_mask))|alternate_domain;
}
std::uint64_t alternate_key(const IndexedCoherencePosting& posting) noexcept {
  std::array<std::byte,16> extent{};
  for(unsigned i=0;i<8;++i) {
    extent[i]=static_cast<std::byte>(posting.representative.inode>>(i*8));
    extent[8+i]=static_cast<std::byte>(posting.representative.extent_index>>(i*8));
  }
  return alternate_prefix(posting.band,fingerprint_of(posting))|
      std::to_integer<std::uint64_t>(detail::record_digest(extent).front());
}
bool same_bucket(const IndexedCoherencePosting& a,const IndexedCoherencePosting& b) noexcept {
  return a.band==b.band && fingerprint_of(a)==fingerprint_of(b);
}
bool same_extent(const IndexedCoherencePosting& a,const IndexedCoherencePosting& b) noexcept {
  return a.representative.inode==b.representative.inode &&
      a.representative.extent_index==b.representative.extent_index;
}
auto revision_order(const IndexedCoherencePosting& posting) noexcept {
  return std::pair{posting.representative.revision,posting.representative.object};
}
Bytes encode_posting(const IndexedCoherencePosting& posting,bool alternate=false) {
  const auto schema=posting_schema(posting.band);
  const bool projected=schema>1;
  Bytes bytes(projected?projected_posting_bytes:posting_bytes);
  signature(bytes,posting_signatures[(alternate?alternate_schema:schema)-1]);
  bytes[8]=static_cast<std::byte>(posting.band);
  auto body=std::span(bytes).subspan(posting_header_bytes);
  if(!projected) {
    detail::encode_optimization_group_feature(body.first<feature_bytes>(),posting.representative);
    return bytes;
  }
  std::array<std::byte,feature_bytes> feature{};
  detail::encode_optimization_group_feature(feature,posting.representative);
  std::ranges::copy(std::span(feature).first<feature_identity_bytes>(),body.begin());
  std::ranges::copy(std::span(feature).subspan<feature_role_offset,word_bytes>(),
      body.begin()+feature_identity_bytes);
  for(unsigned i=0;i<word_bytes;++i)
    body[feature_identity_bytes+word_bytes+i]=static_cast<std::byte>(posting.projected_fingerprint>>(i*8));
  return bytes;
}
IndexedCoherencePosting decode_posting(std::uint64_t key,std::span<const std::byte> bytes,CatalogFormat format) {
  if(bytes.size()<posting_header_bytes)throw std::runtime_error("truncated coherence catalog posting");
  const auto band=std::to_integer<std::uint32_t>(bytes[8]);
  const auto record_schema=posting_schema(band);
  const bool projected=record_schema>1;
  const bool alternate=(key&domain_mask)==alternate_domain;
  if(band>=kCoherenceBandCount || record_schema>format.band_schema ||
      (alternate && format.schema!=alternate_schema) ||
      bytes.size()!=(projected?projected_posting_bytes:posting_bytes) ||
      !signature_matches(bytes,posting_signatures[(alternate?alternate_schema:record_schema)-1]) ||
      !std::ranges::all_of(bytes.subspan(9,7),[](auto value){return value==std::byte{};}))
    throw std::runtime_error("invalid coherence catalog posting");
  const auto body=bytes.subspan(posting_header_bytes);
  std::array<std::byte,feature_bytes> expanded{};
  std::uint64_t projection=0;
  if(projected) {
    std::ranges::copy(body.first<feature_identity_bytes>(),expanded.begin());
    std::ranges::copy(body.subspan<feature_identity_bytes,word_bytes>(),expanded.begin()+feature_role_offset);
    for(unsigned i=0;i<word_bytes;++i)
      projection|=std::uint64_t(std::to_integer<unsigned>(body[feature_identity_bytes+word_bytes+i]))<<(i*8);
  }
  const auto feature=detail::decode_optimization_group_feature(projected?
      std::span<const std::byte,feature_bytes>(expanded):body.first<feature_bytes>());
  if(!feature)throw std::runtime_error("invalid coherence catalog representative");
  const IndexedCoherencePosting posting{band,*feature,projection};
  const auto fingerprint=fingerprint_of(posting);
  if(!fingerprint || (alternate?alternate_key(posting):posting_key(band,fingerprint))!=key)
    throw std::runtime_error("invalid coherence catalog posting binding");
  return posting;
}
std::multimap<std::uint64_t,const IndexedCoherencePosting*> collect_postings(
    std::span<const IndexedCoherencePosting> postings) {
  if(postings.size()>kStoreMaxObjects)throw PagedStoreCapacityError("coherence catalog transaction size");
  std::multimap<std::uint64_t,const IndexedCoherencePosting*> pending;
  for(const auto& posting:postings) {
    if(posting.band>=kCoherenceBandCount ||
        (posting.band<kOptimizationGroupBands && posting.projected_fingerprint) ||
        !detail::valid_optimization_group_feature(posting.representative) ||
        !fingerprint_of(posting))throw std::invalid_argument("coherence catalog posting fields");
    const auto key=posting_key(posting.band,fingerprint_of(posting));
    pending.emplace(key,&posting);
  }
  return pending;
}
std::optional<IndexedCoherencePosting> read_posting(const PagedObjectStoreSnapshot* store,
    std::uint64_t key,CatalogFormat format) {
  if(!store)return {};
  const auto bytes=store->read_if_present(key,posting_bytes);
  if(!bytes)return {};
  return decode_posting(key,*bytes,format);
}
struct PostingPlan {
  std::vector<StoreMutation> primary,alternates;
  std::uint8_t band_schema;
};
PostingPlan plan_postings(std::span<const IndexedCoherencePosting> postings,
    const PagedObjectStoreSnapshot* store,CatalogFormat format) {
  const auto pending=collect_postings(postings);
  PostingPlan plan{{},{},format.band_schema};
  std::map<std::uint64_t,IndexedCoherencePosting> displaced;
  for(auto first=pending.begin();first!=pending.end();) {
    const auto key=first->first;
    const auto end=pending.upper_bound(key);
    const auto& newest=*std::max_element(first,end,[](const auto& a,const auto& b) {
      return revision_order(*a.second)<revision_order(*b.second);
    })->second;
    const auto previous=read_posting(store,key,format);
    if(!previous || revision_order(*previous)<revision_order(newest)) {
      plan.band_schema=(std::max)(plan.band_schema,posting_schema(newest.band));
      plan.primary.push_back({key,false,encode_posting(newest)});
      const auto offer=[&](const IndexedCoherencePosting& posting) {
        if(!same_bucket(posting,newest) || same_extent(posting,newest))return;
        const auto slot=alternate_key(posting);
        const auto [position,inserted]=displaced.try_emplace(slot,posting);
        if(!inserted && revision_order(position->second)<revision_order(posting))position->second=posting;
      };
      if(previous)offer(*previous);
      for(auto observation=first;observation!=end;++observation)
        if(!previous || revision_order(*observation->second)>revision_order(*previous))
          offer(*observation->second);
    }
    first=end;
  }
  for(const auto& [key,alternate]:displaced) {
    const auto previous=format.schema==alternate_schema?read_posting(store,key,format):std::nullopt;
    if(!previous || revision_order(*previous)<revision_order(alternate))
      plan.alternates.push_back({key,false,encode_posting(alternate,true)});
  }
  return plan;
}
template<class Publish>
auto publish_plan(PostingPlan plan,StoreIdentity volume,StoreIdentity image,CatalogFormat previous,
    bool creating,const std::function<bool(std::uint64_t)>& reserve,Publish&& publish) {
  const CatalogFormat primary{previous.schema==alternate_schema?alternate_schema:plan.band_schema,plan.band_schema};
  const CatalogFormat extended{alternate_schema,plan.band_schema};
  const auto needs_root=[&](CatalogFormat format){return creating || format!=previous;};
  bool reservation_attempted=false;
  const std::function<bool(std::uint64_t)> charge=[&](std::uint64_t bytes) {
    reservation_attempted=true;
    return !reserve || reserve(bytes);
  };
  const auto attempt=[&](CatalogFormat format,bool alternates) {
    auto mutations=plan.primary;
    if(needs_root(format)) {
      const auto root=root_record(volume,image,format);
      mutations.push_back({root_object,false,Bytes(root.begin(),root.end())});
    }
    if(alternates) {
      const auto count=(std::min)(plan.alternates.size(),kStoreMaxObjects-mutations.size());
      mutations.insert(mutations.end(),plan.alternates.begin(),plan.alternates.begin()+count);
    }
    if(creating)std::ranges::sort(mutations,{},&StoreMutation::id);
    return publish(mutations,charge);
  };
  if(!plan.alternates.empty() && plan.primary.size()+needs_root(extended)<kStoreMaxObjects) {
    try{return attempt(extended,true);}
    catch(const PagedStoreCapacityError&) {
      // The append planner checks geometry and byte credit before asking
      // for reservation. Only that clean refusal can drop the optional rows.
      if(reservation_attempted)throw;
    }
  }
  return attempt(primary,false);
}
}
IndexedCoherenceSnapshot::IndexedCoherenceSnapshot(
    PagedObjectStoreSnapshot store,StoreIdentity volume,StoreIdentity image)
    :store_(std::move(store)),image_(image) {
  if(store_.identity()!=volume)throw std::runtime_error("foreign coherence catalog volume");
  const auto record=store_.read(root_object,Root{}.size());
  if(record.size()!=Root{}.size())throw std::runtime_error("invalid coherence catalog root size");
  schema_=std::to_integer<std::uint8_t>(record[24]);
  band_schema_=schema_==alternate_schema?std::to_integer<std::uint8_t>(record[25]):schema_;
  if(!schema_ || schema_>root_signatures.size() || !band_schema_ || band_schema_>=alternate_schema ||
      !std::ranges::equal(root_record(volume,image,{schema_,band_schema_}),record))
    throw std::runtime_error("foreign or unsupported coherence catalog root");
}
IndexedCoherenceSnapshot IndexedCoherenceSnapshot::open(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits) {
  return IndexedCoherenceSnapshot(PagedObjectStoreSnapshot::open_impl(std::move(backing),limits,true),volume,image);
}
std::optional<OptimizationGroupFeature> IndexedCoherenceSnapshot::find(
    std::uint32_t band,std::uint64_t fingerprint) const {
  if(band>=kCoherenceBandCount)throw std::invalid_argument("coherence catalog band");
  if(!fingerprint || posting_schema(band)>band_schema_)return {};
  const auto key=posting_key(band,fingerprint);
  const auto posting=read_posting(&store_,key,{schema_,band_schema_});
  if(!posting || posting->band!=band || fingerprint_of(*posting)!=fingerprint)return {};
  return posting->representative;
}
std::optional<IndexedCoherenceAlternate> IndexedCoherenceSnapshot::next_alternate(
    std::uint32_t band,std::uint64_t fingerprint,std::uint64_t after) const {
  if(band>=kCoherenceBandCount)throw std::invalid_argument("coherence catalog band");
  if(!has_alternates() || !fingerprint || posting_schema(band)>band_schema_)return {};
  const auto prefix=alternate_prefix(band,fingerprint);
  if(after && (after&~slot_mask)!=prefix)throw std::invalid_argument("foreign coherence alternate cursor");
  const auto row=store_.read_first_after(after?after:prefix-1,prefix|slot_mask,posting_bytes);
  if(!row)return {};
  const auto posting=decode_posting(row->first,row->second,{schema_,band_schema_});
  IndexedCoherenceAlternate result{row->first,{}};
  if(posting.band==band && fingerprint_of(posting)==fingerprint)result.representative=posting.representative;
  return result;
}

IndexedCoherenceWriter::IndexedCoherenceWriter(
    PagedObjectStoreWriter store,StoreIdentity volume,StoreIdentity image)
    :store_(std::move(store)),volume_(volume),image_(image) {}
IndexedCoherenceWriter IndexedCoherenceWriter::create(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits) {
  return create_reserved(std::move(backing),volume,image,limits,{},UINT64_MAX,{});
}
IndexedCoherenceWriter IndexedCoherenceWriter::create_reserved(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits,std::span<const IndexedCoherencePosting> postings,
    std::uint64_t maximum_write_bytes,const std::function<bool(std::uint64_t)>& reserve) {
  auto store=publish_plan(plan_postings(postings,nullptr,{}),volume,image,{},true,reserve,
      [&](std::span<const StoreMutation> mutations,const auto& charge) {
        return PagedObjectStoreWriter::create_impl(backing,volume,limits,raw_storage,false,
            mutations,maximum_write_bytes,charge);
      });
  return IndexedCoherenceWriter(std::move(store),volume,image);
}
IndexedCoherenceWriter IndexedCoherenceWriter::resume(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,std::uint64_t expected_generation,PagedStoreLimits limits) {
  const auto observed=IndexedCoherenceSnapshot::open(backing,volume,image,limits);
  if(observed.info().generation!=expected_generation)throw std::runtime_error("stale coherence catalog generation");
  auto store=PagedObjectStoreWriter::resume(std::move(backing),volume,expected_generation,limits,raw_storage);
  return IndexedCoherenceWriter(std::move(store),volume,image);
}
IndexedCoherenceSnapshot IndexedCoherenceWriter::snapshot() const {
  if(store_.poisoned())throw std::runtime_error("coherence catalog writer unavailable");
  return IndexedCoherenceSnapshot(store_.snapshot_impl(true),volume_,image_);
}
PagedStoreWriteResult IndexedCoherenceWriter::publish(
    std::span<const IndexedCoherencePosting> postings,std::uint64_t maximum_write_bytes) {
  return publish_reserved(postings,maximum_write_bytes,{});
}
PagedStoreWriteResult IndexedCoherenceWriter::publish_reserved(
    std::span<const IndexedCoherencePosting> postings,std::uint64_t maximum_write_bytes,
    const std::function<bool(std::uint64_t)>& reserve) {
  const auto previous=snapshot();
  const CatalogFormat format{previous.schema_,previous.band_schema_};
  auto plan=plan_postings(postings,&previous.store_,format);
  if(plan.primary.empty())return {previous.info(),0};
  return publish_plan(std::move(plan),volume_,image_,format,false,reserve,
      [&](std::span<const StoreMutation> mutations,const auto& charge) {
        return store_.commit_impl(mutations,kStoreMaxObjects,kStoreMaxTransactionBytes,maximum_write_bytes,charge);
      });
}
PagedObjectStoreWriter::Rewrite IndexedCoherenceWriter::prepare_compaction(std::shared_ptr<StoreByteIo> target,
    std::function<bool(const OptimizationGroupFeature&)> keep,std::function<bool(std::uint64_t)> charge) const {
  const auto current=snapshot();
  const CatalogFormat format{current.schema_,current.band_schema_};
  const auto root=root_record(volume_,image_,format);
  return store_.prepare_rebuild_retaining(std::move(target),raw_storage,{posting_bytes,
      [root,format,keep=std::move(keep)](std::uint64_t key,std::span<const std::byte> bytes) {
        if(key!=root_object)return keep(decode_posting(key,bytes,format).representative);
        if(!std::ranges::equal(root,bytes))throw std::runtime_error("coherence compaction root differs");
        return true;
      },std::move(charge)});
}
IndexedCoherenceStorage::IndexedCoherenceStorage(PagedStoreLimits limits,Reserve reserve)
    :limits_(limits),reserve_(std::move(reserve)) {
  if(!limits_.maximum_physical_bytes || !reserve_)throw std::invalid_argument("coherence storage reservation");
}
}
