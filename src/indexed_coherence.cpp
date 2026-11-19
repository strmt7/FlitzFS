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
Root root_record(StoreIdentity volume,StoreIdentity image) {
  if(volume==StoreIdentity{} || image==StoreIdentity{} || image==volume)
    throw std::invalid_argument("coherence catalog identities");
  Root bytes{};
  signature(bytes,"FLTZCOR1");
  std::ranges::copy(image,bytes.begin()+8);
  bytes[24]=std::byte{1}; // Aligned and shifted feature schema.
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
  return posting.representative.bands[posting.band];
}
auto revision_order(const IndexedCoherencePosting& posting) noexcept {
  return std::pair{posting.representative.revision,posting.representative.object};
}
Bytes encode_posting(const IndexedCoherencePosting& posting) {
  Bytes bytes(posting_bytes);
  signature(bytes,"FLTZCOE1");
  bytes[8]=static_cast<std::byte>(posting.band);
  detail::encode_optimization_group_feature(
      std::span<std::byte,feature_bytes>(bytes.data()+posting_header_bytes,feature_bytes),
      posting.representative);
  return bytes;
}
IndexedCoherencePosting decode_posting(std::uint64_t key,std::span<const std::byte> bytes) {
  if(bytes.size()!=posting_bytes || !signature_matches(bytes,"FLTZCOE1") ||
      !std::ranges::all_of(bytes.subspan(9,7),[](auto value){return value==std::byte{};}))
    throw std::runtime_error("invalid coherence catalog posting");
  const auto band=std::to_integer<std::uint32_t>(bytes[8]);
  const auto feature=detail::decode_optimization_group_feature(
      std::span<const std::byte,feature_bytes>(bytes.data()+posting_header_bytes,feature_bytes));
  if(!feature || band>=feature->bands.size() || !feature->bands[band] ||
      posting_key(band,feature->bands[band])!=key)
    throw std::runtime_error("invalid coherence catalog posting binding");
  return {band,*feature};
}
std::map<std::uint64_t,const IndexedCoherencePosting*> collect_postings(
    std::span<const IndexedCoherencePosting> postings) {
  if(postings.size()>kStoreMaxObjects)throw PagedStoreCapacityError("coherence catalog transaction size");
  std::map<std::uint64_t,const IndexedCoherencePosting*> pending;
  for(const auto& posting:postings) {
    if(posting.band>=kOptimizationGroupBands || !detail::valid_optimization_group_feature(posting.representative) ||
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
  const auto expected=root_record(volume,image);
  if(store_.identity()!=volume)throw std::runtime_error("foreign coherence catalog volume");
  const auto record=store_.read(root_object,expected.size());
  if(!std::ranges::equal(expected,record))throw std::runtime_error("foreign or unsupported coherence catalog root");
}
IndexedCoherenceSnapshot IndexedCoherenceSnapshot::open(std::shared_ptr<StoreByteIo> backing,
    StoreIdentity volume,StoreIdentity image,PagedStoreLimits limits) {
  return IndexedCoherenceSnapshot(PagedObjectStoreSnapshot::open(std::move(backing),limits),volume,image);
}
std::optional<OptimizationGroupFeature> IndexedCoherenceSnapshot::find(
    std::uint32_t band,std::uint64_t fingerprint) const {
  if(band>=kOptimizationGroupBands)throw std::invalid_argument("coherence catalog band");
  if(!fingerprint)return {};
  const auto key=posting_key(band,fingerprint);
  const auto bytes=store_.read_if_present(key,posting_bytes);
  if(!bytes)return {};
  const auto posting=decode_posting(key,*bytes);
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
  const auto root=root_record(volume,image);
  const auto pending=collect_postings(postings);
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
  for(const auto& [key,posting]:pending) {
    if(const auto bytes=previous.store_.read_if_present(key,posting_bytes)) {
      const auto existing=decode_posting(key,*bytes);
      if(revision_order(existing)>=revision_order(*posting))continue;
    }
    mutations.push_back({key,false,encode_posting(*posting)});
  }
  if(mutations.empty())return {previous.info(),0};
  return store_.commit_impl(mutations,kStoreMaxObjects,kStoreMaxTransactionBytes,maximum_write_bytes,reserve);
}
IndexedCoherenceStorage::IndexedCoherenceStorage(PagedStoreLimits limits,Reserve reserve)
    :limits_(limits),reserve_(std::move(reserve)) {
  if(!limits_.maximum_physical_bytes || !reserve_)throw std::invalid_argument("coherence storage reservation");
}
}
