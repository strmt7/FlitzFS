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
constexpr auto band_count=kOptimizationGroupBands+kOptimizationXorProjections;
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
Root root_record(StoreIdentity volume,StoreIdentity image,bool projected_keys) {
  if(volume==StoreIdentity{} || image==StoreIdentity{} || image==volume)
    throw std::invalid_argument("coherence catalog identities");
  Root bytes{};
  signature(bytes,projected_keys?"FLTZCOR2":"FLTZCOR1");
  std::ranges::copy(image,bytes.begin()+8);
  bytes[24]=projected_keys?std::byte{2}:std::byte{1};
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
auto revision_order(const IndexedCoherencePosting& posting) noexcept {
  return std::pair{posting.representative.revision,posting.representative.object};
}
Bytes encode_posting(const IndexedCoherencePosting& posting) {
  const bool projected=posting.band>=kOptimizationGroupBands;
  Bytes bytes(projected?projected_posting_bytes:posting_bytes);
  signature(bytes,projected?"FLTZCOE2":"FLTZCOE1");
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
IndexedCoherencePosting decode_posting(std::uint64_t key,std::span<const std::byte> bytes,bool projected_keys) {
  const bool legacy=bytes.size()==posting_bytes && signature_matches(bytes,"FLTZCOE1");
  const bool projected=projected_keys && bytes.size()==projected_posting_bytes && signature_matches(bytes,"FLTZCOE2");
  if((!legacy && !projected) ||
      !std::ranges::all_of(bytes.subspan(9,7),[](auto value){return value==std::byte{};}))
    throw std::runtime_error("invalid coherence catalog posting");
  const auto band=std::to_integer<std::uint32_t>(bytes[8]);
  if(band>=band_count || projected!=(band>=kOptimizationGroupBands))
    throw std::runtime_error("invalid coherence catalog band");
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
  if(!fingerprint || posting_key(band,fingerprint)!=key)
    throw std::runtime_error("invalid coherence catalog posting binding");
  return posting;
}
std::map<std::uint64_t,const IndexedCoherencePosting*> collect_postings(
    std::span<const IndexedCoherencePosting> postings) {
  if(postings.size()>kStoreMaxObjects)throw PagedStoreCapacityError("coherence catalog transaction size");
  std::map<std::uint64_t,const IndexedCoherencePosting*> pending;
  for(const auto& posting:postings) {
    if(posting.band>=band_count ||
        (posting.band<kOptimizationGroupBands && posting.projected_fingerprint) ||
        !detail::valid_optimization_group_feature(posting.representative) ||
        !fingerprint_of(posting))throw std::invalid_argument("coherence catalog posting fields");
    const auto key=posting_key(posting.band,fingerprint_of(posting));
    const auto [position,inserted]=pending.emplace(key,&posting);
    if(!inserted && revision_order(*position->second)<revision_order(posting))position->second=&posting;
  }
  return pending;
}
}
IndexedCoherenceSnapshot::IndexedCoherenceSnapshot(
    PagedObjectStoreSnapshot store,StoreIdentity volume,StoreIdentity image)
    :store_(std::move(store)),image_(image) {
  if(store_.identity()!=volume)throw std::runtime_error("foreign coherence catalog volume");
  const auto expected=root_record(volume,image,false);
  const auto record=store_.read(root_object,expected.size());
  if(std::ranges::equal(expected,record))return;
  if(!std::ranges::equal(root_record(volume,image,true),record))
    throw std::runtime_error("foreign or unsupported coherence catalog root");
  projected_keys_=true;
}
IndexedCoherenceSnapshot IndexedCoherenceSnapshot::open(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits) {
  return IndexedCoherenceSnapshot(PagedObjectStoreSnapshot::open(std::move(backing),limits),volume,image);
}
std::optional<OptimizationGroupFeature> IndexedCoherenceSnapshot::find(
    std::uint32_t band,std::uint64_t fingerprint) const {
  if(band>=band_count)throw std::invalid_argument("coherence catalog band");
  if(!fingerprint || (!projected_keys_ && band>=kOptimizationGroupBands))return {};
  const auto key=posting_key(band,fingerprint);
  const auto bytes=store_.read_if_present(key,posting_bytes);
  if(!bytes)return {};
  const auto posting=decode_posting(key,*bytes,projected_keys_);
  if(posting.band!=band || fingerprint_of(posting)!=fingerprint)return {};
  return posting.representative;
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
  const auto pending=collect_postings(postings);
  const auto projected=std::ranges::any_of(pending,[](const auto& entry) {
    return entry.second->projected_fingerprint!=0;
  });
  const auto root=root_record(volume,image,projected);
  std::vector<StoreMutation> mutations;
  mutations.reserve(pending.size()+1);
  mutations.push_back({root_object,false,Bytes(root.begin(),root.end())});
  for(const auto& [key,posting]:pending)mutations.push_back({key,false,encode_posting(*posting)});
  auto store=PagedObjectStoreWriter::create_impl(std::move(backing),volume,limits,raw_storage,false,
      mutations,maximum_write_bytes,reserve);
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
  return IndexedCoherenceSnapshot(store_.snapshot(),volume_,image_);
}
PagedStoreWriteResult IndexedCoherenceWriter::publish(
    std::span<const IndexedCoherencePosting> postings,std::uint64_t maximum_write_bytes) {
  return publish_reserved(postings,maximum_write_bytes,{});
}
PagedStoreWriteResult IndexedCoherenceWriter::publish_reserved(
    std::span<const IndexedCoherencePosting> postings,std::uint64_t maximum_write_bytes,
    const std::function<bool(std::uint64_t)>& reserve) {
  const auto pending=collect_postings(postings);
  auto previous=snapshot();
  std::vector<StoreMutation> mutations;
  mutations.reserve(pending.size());
  bool projected=false;
  for(const auto& [key,posting]:pending) {
    if(const auto bytes=previous.store_.read_if_present(key,posting_bytes)) {
      const auto existing=decode_posting(key,*bytes,previous.has_projected_keys());
      if(revision_order(existing)>=revision_order(*posting))continue;
    }
    projected|=posting->projected_fingerprint!=0;
    mutations.push_back({key,false,encode_posting(*posting)});
  }
  if(mutations.empty())return {previous.info(),0};
  if(projected && !previous.has_projected_keys()) {
    const auto root=root_record(volume_,image_,true);
    mutations.push_back({root_object,false,Bytes(root.begin(),root.end())});
  }
  return store_.commit_impl(mutations,kStoreMaxObjects,kStoreMaxTransactionBytes,maximum_write_bytes,reserve);
}
PagedObjectStoreWriter::Rewrite IndexedCoherenceWriter::prepare_compaction(std::shared_ptr<StoreByteIo> target,
    std::function<bool(const OptimizationGroupFeature&)> keep,std::function<bool(std::uint64_t)> charge) const {
  const auto projected=snapshot().has_projected_keys();
  const auto root=root_record(volume_,image_,projected);
  return store_.prepare_rebuild_retaining(std::move(target),raw_storage,{posting_bytes,
      [root,projected,keep=std::move(keep)](std::uint64_t key,std::span<const std::byte> bytes) {
        if(key!=root_object)return keep(decode_posting(key,bytes,projected).representative);
        if(!std::ranges::equal(root,bytes))throw std::runtime_error("coherence compaction root differs");
        return true;
      },std::move(charge)});
}
IndexedCoherenceStorage::IndexedCoherenceStorage(PagedStoreLimits limits,Reserve reserve)
    :limits_(limits),reserve_(std::move(reserve)) {
  if(!limits_.maximum_physical_bytes || !reserve_)throw std::invalid_argument("coherence storage reservation");
}
}
