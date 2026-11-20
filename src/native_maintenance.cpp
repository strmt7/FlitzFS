#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/native_session.hpp"
#include "flitzfs/gpu_anchor_match.hpp"
#include "native_group_codec.hpp"
#include "native_maintenance_source.hpp"
#include "optimization_feature_wire.hpp"
#include "record_digest.hpp"
#include "store_codec.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <set>
namespace flitzfs {
namespace {
using Digest=detail::Digest;using Phase=NativeMaintenancePhase;using Defer=NativeMaintenanceDeferral;
constexpr std::uint64_t million=1000000,fixed_scratch=16ULL*kNativeExtentBytes,member_scratch=8ULL*kNativeExtentBytes;
constexpr unsigned positional_bands=8;
constexpr std::size_t xor_reference_tiles=8,xor_planes=8;
using XorReferencePlane=std::array<std::uint64_t,anchor_match::xor_plane_bytes/sizeof(std::uint64_t)>;
using XorReferenceSample=std::array<XorReferencePlane,xor_reference_tiles*xor_planes>;
using XorReferenceOrder=std::array<std::uint64_t,3>;
static_assert(kOptimizationGroupBands==positional_bands+detail::kNativeGroupShiftFeatures);
static_assert(std::tuple_size_v<detail::NativeGroupDeltaSignatures> == kCoherenceModularWidths);
// Charge extra history for its retained entry, bucket membership, and the
// overlapping controller, publisher and verifier copies of checkpoint rows.
constexpr auto representative_scratch=8*(sizeof(OptimizationGroupFeature)+sizeof(OptimizationGroupProjection)+
    sizeof(detail::CoherenceScanKeys)+sizeof(std::optional<XorReferenceSample>));
static_assert(kOptimizationGroupRepresentatives>kNativeMaxDirtyExtents);
struct GroupMemoryBudget {std::size_t members=0,representatives=0;};
GroupMemoryBudget group_memory_budget(const detail::MaintenanceResources& resources,bool indexed,bool sections=false) noexcept {
  const bool prefixed=sections && store_zstd_available();
  const auto scratch=fixed_scratch+(indexed?detail::kNativeGroupAnchorIndexBytes:0)+
      (prefixed?(std::max)(detail::kStorePrefixEncoderMemoryCap,detail::kStoreDecoderMemoryCap):0);
  if(resources.memory_allowance<=scratch)return {};
  const auto available=resources.memory_allowance-scratch;
  // An indexed group can retain both sealed candidates and one section table
  // while the independently budgeted foreground continues.
  const auto member_bytes=member_scratch+(sections?4*kNativeExtentBytes:0)+(prefixed?2*kNativeExtentBytes:0);
  if(available<2*member_bytes)return {};
  const auto extra=indexed?(std::min)(std::uint64_t{kOptimizationGroupRepresentatives-kNativeMaxDirtyExtents},
      (available-2*member_bytes)/representative_scratch):0;
  const auto funded=(available-extra*representative_scratch)/member_bytes;
  // Indexed batches retain several independently bounded transactions.
  // The flat writer still cannot operate beyond its complete namespace.
  const auto limit=indexed?std::uint64_t{(std::numeric_limits<std::size_t>::max)()}:
      std::uint64_t{kNativeMaxExtents};
  const auto members=static_cast<std::size_t>((std::min)(funded,limit));
  return {members,indexed?(std::min)(members,kNativeMaxDirtyExtents)+static_cast<std::size_t>(extra):0};
}
constexpr auto checkpoint_window_samples=kOptimizationGroupScanInputBytes/(2*kNativeExtentBytes);
std::uint64_t fraction(std::uint64_t total,std::uint64_t ppm) noexcept {
  return total/million*ppm+total%million*ppm/million;
}
std::uint64_t add(std::uint64_t a,std::uint64_t b) noexcept {
  return b>(std::numeric_limits<std::uint64_t>::max)()-a?(std::numeric_limits<std::uint64_t>::max)():a+b;
}
std::uint64_t word(std::span<const std::byte> bytes) noexcept {
  std::uint64_t result=0;for(unsigned i=0;i<8;++i)result|=std::uint64_t(std::to_integer<unsigned>(bytes[i]))<<(8*i);return result;
}
OptimizationXorSignatures sample_xor_signatures(std::span<const std::byte> extent) {
  constexpr std::size_t samples=16,sample_bytes=64;
  constexpr auto spacing=kNativeExtentBytes/samples;
  std::array<std::byte,samples*sample_bytes> sampled{},masked{};
  for(std::size_t i=0;i<samples;++i)
    std::ranges::copy(extent.subspan(i*spacing,sample_bytes),sampled.begin()+i*sample_bytes);
  OptimizationXorSignatures signatures{};
  for(unsigned pair=0;pair<signatures.size();++pair) {
    const auto keep=static_cast<std::byte>(255U^(3U<<(pair*2)));
    std::ranges::transform(sampled,masked.begin(),[keep](auto value){return value&keep;});
    if(std::ranges::any_of(masked,[first=masked.front()](auto value){return value!=first;}))
      signatures[pair]=word(detail::record_digest(masked));
  }
  return signatures;
}
XorReferenceSample sample_xor_reference(std::span<const std::byte> extent) {
  XorReferenceSample observations{};
  constexpr auto tile_bytes=anchor_match::xor_tile_bytes;
  constexpr auto spacing=kNativeExtentBytes/(tile_bytes*xor_reference_tiles);
  static_assert(kNativeExtentBytes%(tile_bytes*xor_reference_tiles)==0 && spacing%2==0);
  for(std::size_t i=0;i<xor_reference_tiles;++i) {
    // Sample a complete tile in each stratum, away from the even tiles used
    // in the discovery signature.
    const auto index=i*spacing+2*((i*7)%(spacing/2))+1;
    const auto tile=extent.subspan(index*tile_bytes,tile_bytes);
    for(std::size_t segment=0;segment<anchor_match::xor_plane_bytes;++segment) {
      const auto bytes=word(tile.subspan(segment*8,8));
      for(unsigned bit=0;bit<xor_planes;++bit) {
        const auto plane=anchor_match::xor_plane_bits(bytes,bit);
        observations[i*xor_planes+bit][segment/8]|=std::uint64_t(plane)<<(8*(segment%8));
      }
    }
  }
  return observations;
}
std::uint32_t sampled_xor_cost(const XorReferenceSample& member,const XorReferenceSample& base) noexcept {
  using namespace anchor_match;
  std::uint32_t dense=xor_reference_tiles,sparse=xor_reference_tiles;
  for(std::size_t tile=0;tile<xor_reference_tiles;++tile) {
    std::uint32_t active=0;
    for(std::size_t bit=0;bit<xor_planes;++bit) {
      const auto plane=tile*xor_planes+bit;
      const auto& a=member[plane];const auto& b=base[plane];
      std::uint32_t ones=0;
      for(std::size_t w=0;w<a.size();++w)ones+=static_cast<std::uint32_t>(std::popcount(a[w]^b[w]));
      if(!ones)continue;
      ++active;
      const auto descriptor=sparse_xor_descriptor(ones);
      sparse+=descriptor==sparse_plane_dense?xor_plane_bytes:1+(descriptor&~sparse_plane_inverse);
    }
    dense+=active*xor_plane_bytes;
    sparse+=active!=0; // Sparse tiles carry a second mask when any plane differs.
  }
  return (std::min)(dense,sparse);
}
bool usable_indexed_band(unsigned band,std::uint64_t fingerprint) {
  if(band>=positional_bands)return fingerprint!=0;
  static const auto zero_band=word(detail::record_digest(std::array<std::byte,kNativeExtentBytes/positional_bands>{}));
  return fingerprint!=zero_band;
}
void append(std::vector<std::byte>& bytes,std::uint64_t value) {
  for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<std::byte>(value>>(8*i)));
}
}
std::optional<detail::MaintenanceResources> detail::maintenance_resources(
    const NativeMaintenanceObservation& observed,std::uint64_t now_ms,const NativeMaintenancePolicy& policy) noexcept {
  if(!observed.valid || observed.captured_ms>now_ms || now_ms-observed.captured_ms>policy.maximum_age_ms ||
      !observed.physical_total || observed.physical_available>observed.physical_total ||
      !observed.commit_limit || observed.commit_available>observed.commit_limit || !observed.cpu_total_ticks ||
      observed.cpu_busy_ticks>observed.cpu_total_ticks || !observed.cpu_window_ms ||
      observed.cpu_window_ms>policy.maximum_cpu_window_ms || observed.cpu_window_ms>observed.captured_ms ||
      !policy.memory_share_divisor)return {};
  const auto physical_reserve=observed.physical_total/16,commit_reserve=observed.commit_limit/16;
  const auto physical=observed.physical_available>physical_reserve?observed.physical_available-physical_reserve:0;
  const auto commit=observed.commit_available>commit_reserve?observed.commit_available-commit_reserve:0;
  return MaintenanceResources{(std::min)(physical,commit)/policy.memory_share_divisor,
      observed.cpu_busy_ticks>fraction(observed.cpu_total_ticks,policy.maximum_cpu_busy_ppm)};
}
std::size_t detail::maintenance_group_member_limit(const MaintenanceResources& resources,bool indexed) noexcept {
  return group_memory_budget(resources,indexed).members;
}
std::optional<std::uint32_t> detail::coherence_primary_band(
    const std::array<std::uint64_t,kOptimizationGroupBands>& bands) {
  for(std::uint32_t band=0;band<bands.size();++band)
    if(bands[band] && usable_indexed_band(band,bands[band]))return band;
  return {};
}
struct NativeGroupMaintenance::Impl {
  struct Feature {
    NativeContentToken token;
    Digest content{};
    std::array<std::uint64_t,kOptimizationGroupBands> bands{};
    OptimizationXorSignatures projections{};
    detail::NativeGroupDeltaSignatures deltas{};
    bool projected=false,deltas_sampled=false;
    std::optional<XorReferenceSample> reference_sample;
  };
  struct StagedGroup {
    NativeFilesystemWriter::SharedBaseWork work;
    std::vector<std::uint64_t> ids;
    std::optional<Digest> sampled_key;
    std::size_t member_count,search_members;
  };
  using Band=std::pair<unsigned,std::uint64_t>;
  std::weak_ptr<detail::NativeMaintenanceSource> source;
  std::shared_ptr<const void> reservation;
  NativeMaintenancePolicy policy;NativeMaintenanceProgress state;
  Phase phase=Phase::idle;StoreIdentity image{};NativeExtentSelection cursor{};
  bool first=true,stopped=false;unsigned pending_history=0;
  std::uint64_t scan_sequence=0,completed_sequence=0;
  std::uint64_t checkpoint_samples=0;
  bool checkpoint_loaded=false,checkpoint_due=false,checkpoint_unavailable=false,scan_revisit=false;
  std::array<NativeExtentSelection,kNativeMaxDirtyExtents> updates{};
  std::size_t update_count=0,next_update=0;
  bool updates_need_sweep=false;
  bool prefer_coherence=true;
  std::array<bool,kStoreMaxObjects+1> seen{};
  std::map<std::uint64_t,Feature> features;
  // Indexed discovery retains only a window and a bounded set of prior
  // representatives. Neither owns a namespace snapshot or decoded bytes.
  std::map<std::uint64_t,Feature> representatives;
  bool extent_scan_exhausted=false;
  std::optional<NativeExtentSelection> refresh_base;
  std::map<Band,std::vector<std::uint64_t>> buckets;
  std::vector<Band> candidate_order;
  std::size_t candidate_index=0,band_offset=0;
  std::uint64_t coherence_after=0,coherence_binding=0;
  unsigned coherence_query=0;
  std::set<Digest> attempted;std::deque<Digest> attempted_order;
  std::optional<NativeFilesystemWriter::SharedBaseWork> work;
  std::deque<StagedGroup> staged_groups;
  std::set<std::uint64_t> staged_objects;
  std::set<std::uint64_t> deferred_batch_objects;
  std::size_t staged_extents=0,staged_search_members=0;
  std::size_t staged_scan_extents=0;
  bool work_batched=false,batch_draining=false;
  bool work_sampled_retry=false;
  std::optional<Digest> work_sampled_key;
  Digest work_key{};std::size_t work_members=0;
  Digest prospective_key{};
  std::vector<std::uint64_t> work_ids;
  std::uint64_t last_observation=0,last_capture=0,tokens=1000000,debt=0,predicted=1000000;
  explicit Impl(std::weak_ptr<detail::NativeMaintenanceSource> s,NativeMaintenancePolicy p):source(std::move(s)),policy(p){}
  ~Impl(){cancel_coherence_work();}
  void cancel_coherence_work() noexcept {
    if(const auto owner=source.lock())owner->cancel_coherence_work();
  }
  bool discovery_pending(const NativeFilesystemWriter& writer,const detail::NativeMaintenanceSource& owner,
      StoreIdentity image_id) const noexcept {
    if(owner.group_append_shortfall(image_id,writer.recompression_generation()))return false;
    return active_group() || checkpoint_due || phase!=Phase::idle || (writer.maintenance_has_candidates() &&
        (image!=image_id || completed_sequence!=writer.maintenance_content_sequence() || new_coherence(owner,image_id)));
  }
  void reset_index() noexcept {
    work.reset();staged_groups.clear();staged_objects.clear();deferred_batch_objects.clear();
    staged_extents=staged_search_members=staged_scan_extents=0;
    work_batched=batch_draining=false;
    work_sampled_retry=false;work_sampled_key.reset();
    features.clear();buckets.clear();attempted.clear();attempted_order.clear();candidate_order.clear();
    candidate_index=band_offset=0;completed_sequence=0;phase=Phase::idle;state.owned_payload_bytes=0;pending_history=0;work_ids.clear();
    representatives.clear();refresh_base.reset();extent_scan_exhausted=false;
    coherence_after=coherence_binding=0;
    coherence_query=0;
    checkpoint_loaded=checkpoint_due=false;checkpoint_samples=state.sampled_extents;scan_revisit=false;
    update_count=next_update=0;
    updates_need_sweep=false;
  }
  void remember(const Digest& key) {
    const auto inserted=attempted.insert(key);
    if(inserted.second)try{attempted_order.push_back(key);}catch(...){attempted.erase(inserted.first);throw;}
    if(attempted_order.size()>8*kNativeMaxExtents){attempted.erase(attempted_order.front());attempted_order.pop_front();}
  }
  void forget_pending() noexcept {
    if(pending_history){attempted.erase(work_key);std::erase(attempted_order,work_key);}
    if(pending_history==2){attempted.erase(prospective_key);std::erase(attempted_order,prospective_key);}
    pending_history=0;
  }
  bool active_group() const noexcept{return work.has_value() || !staged_groups.empty();}
  std::size_t group_members() const noexcept{return staged_extents+(work?work_members:0);}
  bool can_advance_cpu_suffix(std::span<const NativeSharedBaseSearchWindow> prefix) const noexcept {
    // Launches hold an immutable leading segment. Draining from the tail
    // preserves its positions until the adviser returns or is cancelled.
    return batch_draining && staged_groups.size()>=prefix.size() &&
        staged_groups.front().work.search_window()==prefix.front() &&
        staged_groups[prefix.size()-1].work.search_window()==prefix.back() &&
        (work || staged_groups.size()>prefix.size());
  }
  std::uint64_t owned_payload() const noexcept {
    auto bytes=work?work->progress().owned_payload_bytes:0;
    for(const auto& group:staged_groups)bytes=add(bytes,group.work.progress().owned_payload_bytes);
    return bytes;
  }
  void emit_batch() noexcept {
    batch_draining=true;
    state.search_batch_ready=true;
    phase=Phase::preparing;
  }
  NativeMaintenanceProgress progress() const noexcept {
    auto result=state;
    if(const auto owner=source.lock()) {
      if(owner->writer)result.append_shortfall_bytes=owner->group_append_shortfall(
          owner->writer->maintenance_identity(),owner->writer->recompression_generation());
      if(owner->coherence)result.coherence_catalog_bytes=owner->coherence->info().physical_end;
      result.coherence_compactions=owner->coherence_compactions;
      result.coherence_discards=owner->coherence_discards;
      result.coherence_compacted_bytes_saved=owner->coherence_compacted_bytes_saved;
    }else result.phase=Phase::failed;
    return result;
  }
  NativeMaintenanceProgress defer(Defer reason,bool release=false) noexcept {
    if(release)reset_index();
    if(reason==Defer::memory)cancel_coherence_work();
    state.phase=Phase::deferred;state.deferral=reason;
    state.owned_payload_bytes=owned_payload();return progress();
  }
  void begin_scan(NativeFilesystemWriter& writer) noexcept {
    update_count=next_update=0;
    updates_need_sweep=false;
    scan_sequence=writer.maintenance_content_sequence();cursor={};first=true;seen.fill(false);phase=Phase::scanning;
    extent_scan_exhausted=false;scan_revisit=false;
  }
  bool begin_update_scan(NativeFilesystemWriter& writer,std::uint64_t previous) noexcept {
    const auto count=writer.maintenance_changes_since(previous,updates);
    if(!count || !*count)return false;
    update_count=*count;
    next_update=0;
    updates_need_sweep=false;
    scan_sequence=writer.maintenance_content_sequence();
    phase=Phase::scanning;
    extent_scan_exhausted=false;
    return true;
  }
  void complete_scan(NativeFilesystemWriter& writer) noexcept {
    if(!scan_revisit && !updates_need_sweep) {
      if(writer.maintenance_content_sequence()==scan_sequence) {
        update_count=next_update=0;
        completed_sequence=scan_sequence;
        phase=Phase::idle;
        return;
      }
      if(begin_update_scan(writer,scan_sequence))return;
    }
    begin_scan(writer);
  }
  bool new_coherence(const detail::NativeMaintenanceSource& owner,StoreIdentity image_id) const noexcept {
    return owner.coherence && owner.coherence->image()==image_id && owner.coherence_binding!=coherence_binding;
  }
  bool restore_scan(NativeFilesystemWriter& writer,const detail::NativeMaintenanceSource& owner,
      std::size_t maximum_representatives,bool new_catalog) {
    checkpoint_loaded=true;
    const auto saved=owner.restore_scan();
    if(!saved || saved->image!=image || saved->namespace_sequence>writer.maintenance_sequence())return false;
    const bool same=saved->namespace_sequence==writer.maintenance_sequence();
    const bool complete=saved->complete && same && !new_catalog && saved->xor_sampling;
    begin_scan(writer);
    if(!saved->complete || complete) {
      scan_revisit=saved->revisit || !same || new_catalog || !saved->xor_sampling;
      cursor={saved->inode,saved->extent_index};first=false;
    }
    // Same-image records can span unrelated commits. The live extent binding
    // must still match before any representative is reused or selected.
    auto projection=saved->projections.begin();
    for(std::size_t row=0;row<saved->representatives.size();++row) {
      const auto& item=saved->representatives[row];
      const NativeContentToken token{{item.inode,item.extent_index},item.object,item.revision,item.revision,
          item.exact_base,item.exact_base};
      Feature feature{token,item.content,item.bands};
      if(projection!=saved->projections.end() && projection->representative==row) {
        feature.projections=projection->signatures;
        feature.projected=true;
        ++projection;
      }
      representatives.emplace(item.object,std::move(feature));
      trim_representatives(maximum_representatives);
    }
    if(complete){completed_sequence=scan_sequence;phase=Phase::idle;}
    ++state.checkpoints_restored;
    return true;
  }
  bool checkpoint_step(NativeFilesystemWriter& writer,const detail::NativeMaintenanceSource& owner,
      std::size_t maximum_representatives,bool new_catalog) {
    // A pending update scan is volatile. Keep the last recoverable sweep
    // checkpoint until its candidate work has settled.
    if(checkpoint_unavailable || update_count)return false;
    try {
      if(!checkpoint_loaded && restore_scan(writer,owner,maximum_representatives,new_catalog))return true;
      if(!checkpoint_due)return false;
      checkpoint_due=false;
      if(!owner.scan_funded())return false;
      OptimizationGroupScan scan;
      scan.image=image;scan.namespace_sequence=writer.maintenance_sequence();
      scan.inode=cursor.inode;scan.extent_index=cursor.extent_index;
      scan.revisit=scan_revisit || scan_sequence!=writer.maintenance_content_sequence();
      scan.complete=phase==Phase::idle&&!scan.revisit;
      scan.xor_sampling=true;
      scan.representatives.reserve(representatives.size());
      std::vector<detail::CoherenceScanKeys> catalog_keys;
      const bool can_publish_catalog=owner.coherence_writer || owner.coherence_storage;
      if(can_publish_catalog)catalog_keys.reserve(representatives.size());
      for(const auto& [id,feature]:representatives) {
        const auto& token=feature.token;
        if(can_publish_catalog && (feature.projected || feature.deltas_sampled))
          catalog_keys.push_back({static_cast<std::uint32_t>(scan.representatives.size()),
              feature.projected?feature.projections:OptimizationXorSignatures{},
              feature.deltas_sampled?feature.deltas:detail::NativeGroupDeltaSignatures{}});
        scan.representatives.push_back({token.extent.inode,token.extent.extent_index,
            id,token.revision,feature.content,feature.bands,token.exact_base});
      }
      const auto projection_capacity=detail::optimization_group_projection_capacity(scan.representatives.size());
      scan.projections.reserve(projection_capacity);
      // Prefer reusable anchors when the fixed slot has limited optional space.
      for(const bool anchor:{true,false}) {
        std::uint32_t row=0;
        for(const auto& [id,feature]:representatives) {
          (void)id;
          if(scan.projections.size()==projection_capacity)break;
          if(feature.projected && feature.token.exact_base==anchor)
            scan.projections.push_back({row,feature.projections});
          ++row;
        }
      }
      std::ranges::sort(scan.projections,{},&OptimizationGroupProjection::representative);
      if(owner.publish_scan(std::move(scan),catalog_keys)) {
        ++state.checkpoints_published;
        checkpoint_samples=state.sampled_extents;
      }
      return true;
    }catch(const std::exception&) {
      // Advisory refusal is independent of the indexed writer and file access.
      checkpoint_due=false;checkpoint_unavailable=true;
      if(writer.poisoned())throw NativeFsError(NativeFsErrorCode::storage_failure,"checkpoint source is no longer readable");
      return false;
    }
  }
  Feature measure(NativeFilesystemWriter& writer,NativeContentToken token,bool published_reference=false) {
    const auto bytes=writer.maintenance_read(token,published_reference);
    Feature feature;
    feature.token=token;
    ++state.sampled_extents;
    if(bytes.empty())return feature;
    feature.content=detail::record_digest(bytes);
    constexpr auto width=kNativeExtentBytes/positional_bands;
    for(unsigned i=0;i<positional_bands;++i)
      feature.bands[i]=word(detail::record_digest(std::span<const std::byte>(bytes).subspan(i*width,width)));
    if(writer.maintenance_indexed()) {
      const auto shifted=detail::native_group_shift_features(bytes);
      std::copy(shifted.begin(),shifted.end(),feature.bands.begin()+positional_bands);
      feature.projections=sample_xor_signatures(bytes);
      feature.deltas=detail::native_group_delta_signatures(bytes);
      feature.projected=feature.deltas_sampled=true;
      if(std::ranges::any_of(feature.projections,[](auto signature){return signature!=0;}))
        feature.reference_sample=sample_xor_reference(bytes);
    }
    return feature;
  }
  void rank_buckets(bool indexed) {
    buckets.clear();
    const auto add_bands=[&](std::uint64_t id,const Feature& feature,bool retained) {
      const auto add_band=[&](Band band) {
        if(!retained)buckets[band].push_back(id);
        else if(const auto bucket=buckets.find(band);bucket!=buckets.end())bucket->second.push_back(id);
      };
      const auto count=indexed?feature.bands.size():positional_bands;
      for(unsigned i=0;i<count;++i) {
        if(!indexed || usable_indexed_band(i,feature.bands[i]))add_band({i,feature.bands[i]});
      }
      if(indexed) {
        for(unsigned i=0;i<feature.projections.size();++i) {
          if(feature.projections[i])add_band({kCoherenceMaskedBand+i,feature.projections[i]});
        }
        for(unsigned i=0;i<feature.deltas.size();++i) {
          if(feature.deltas[i])add_band({kCoherenceModularBand+i,feature.deltas[i]});
        }
      }
    };
    for(const auto& [id,feature]:features) {
      if(indexed && feature.token.grouped && !feature.token.exact_base)continue;
      add_bands(id,feature,false);
    }
    if(indexed)for(const auto& [id,feature]:representatives) {
      if(!features.contains(id))add_bands(id,feature,true);
    }
    candidate_order.clear();
    for(const auto& [band,ids]:buckets)if(ids.size()>=2)candidate_order.push_back(band);
    std::sort(candidate_order.begin(),candidate_order.end(),[&](const auto& a,const auto& b){
      const auto x=buckets.at(a).size(),y=buckets.at(b).size();return x==y?a<b:x>y;
    });
    candidate_index=band_offset=0;phase=Phase::candidates;
  }
  void account_group(const NativeSharedBaseCommit& result,std::uint64_t previous_end) noexcept {
    ++state.published_groups;
    state.minimum_compacted_bytes_saved=add(state.minimum_compacted_bytes_saved,
        result.before_compacted_lower_bound-result.after_compacted_upper_bound);
    if(result.transaction.storage.physical_end>=previous_end)
      state.retained_history_bytes_added=add(state.retained_history_bytes_added,result.transaction.storage.physical_end-previous_end);
  }
  const Feature& indexed_feature(std::uint64_t id) const {
    const auto sampled=features.find(id);
    return sampled!=features.end()?sampled->second:representatives.at(id);
  }
  void trim_representatives(std::size_t maximum) noexcept {
    while(representatives.size()>maximum) {
      const auto disposable=std::ranges::find_if(representatives,[](const auto& item){return !item.second.token.exact_base;});
      representatives.erase(disposable==representatives.end()?representatives.begin():disposable);
    }
  }
  void retain_indexed_window(std::size_t maximum) {
    for(const auto& [id,feature]:features) {
      if(!feature.token.grouped || feature.token.exact_base)representatives.insert_or_assign(id,feature);
      else representatives.erase(id);
    }
    trim_representatives(maximum);
    features.clear();buckets.clear();candidate_order.clear();
    coherence_after=0;coherence_query=0;
  }
  void accept_coherence_candidate(const OptimizationGroupFeature& hit,Band key,
      std::size_t maximum_representatives) {
    if(features.contains(hit.object))return;
    auto known=representatives.find(hit.object);
    const bool added=known==representatives.end();
    if(added) {
      trim_representatives(maximum_representatives-1);
      const NativeContentToken token{{hit.inode,hit.extent_index},hit.object,hit.revision,hit.revision,
          hit.exact_base,hit.exact_base};
      known=representatives.emplace(hit.object,Feature{token,hit.content,hit.bands}).first;
    }
    auto& feature=known->second;
    if(feature.token.revision!=hit.revision || feature.content!=hit.content ||
        feature.token.extent.inode!=hit.inode || feature.token.extent.extent_index!=hit.extent_index ||
        feature.token.exact_base!=hit.exact_base)return;
    if(key.first>=kCoherenceModularBand) {
      auto& signature=feature.deltas[key.first-kCoherenceModularBand];
      if(feature.deltas_sampled || signature==key.second)return;
      signature=key.second;
    }else if(key.first>=kCoherenceMaskedBand) {
      auto& signature=feature.projections[key.first-kCoherenceMaskedBand];
      if(feature.projected || signature==key.second)return;
      signature=key.second;
    }else {
      if(feature.projected || (!added && feature.bands==hit.bands))return;
      feature.bands=hit.bands;
    }
    ++state.coherence_matches;
    // Trial preparation checks the live binding and independently verifies
    // the selected bytes before publication.
    rank_buckets(true);
  }
  bool query_coherence(detail::NativeMaintenanceSource& owner,std::size_t maximum_representatives) {
    const auto& catalog=owner.coherence;
    if(!catalog)return false;
    if(catalog->image()!=image){owner.retire_coherence();return false;}
    constexpr auto modular_query=1+kOptimizationXorProjections;
    const auto queries=catalog->has_modular_keys()?modular_query+kCoherenceModularWidths:
        catalog->has_projected_keys()?modular_query:1;
    for(auto sample=features.lower_bound(coherence_after);sample!=features.end();++sample,coherence_query=0) {
      const auto& feature=sample->second;
      coherence_after=sample->first;
      if(feature.token.grouped)continue;
      while(coherence_query<queries) {
        const auto query=coherence_query++;
        Band key{};
        if(query==0) {
          if(const auto band=detail::coherence_primary_band(feature.bands))key={*band,feature.bands[*band]};
        }else if(query<modular_query) {
          const auto index=query-1;
          key={kCoherenceMaskedBand+index,feature.projections[index]};
        }else {
          const auto index=query-modular_query;
          key={kCoherenceModularBand+static_cast<unsigned>(index),feature.deltas[index]};
        }
        if(!key.second)continue;
        std::optional<OptimizationGroupFeature> hit;
        ++state.coherence_lookups;
        try{hit=catalog->find(key.first,key.second);}
        catch(const std::exception&){owner.retire_coherence();++state.coherence_failures;return true;}
        if(hit)accept_coherence_candidate(*hit,key,maximum_representatives);
        return true; // One point lookup in an admitted opportunity.
      }
    }
    return false;
  }
  XorReferenceOrder choose_xor_reference(std::span<const NativeContentToken> group) const {
    const auto original=group.front().object;
    // With two members the sampled XOR cost is the same in either direction.
    if(group.size()<3)return {original};
    std::vector<const XorReferenceSample*> samples;
    samples.reserve(group.size());
    for(const auto& member:group) {
      const auto& sample=features.at(member.object).reference_sample;
      if(!sample)return {original};
      samples.push_back(&*sample);
    }
    if(std::ranges::all_of(samples,[&](const auto* sample){return *sample==*samples.front();}))return {original};
    using PlaneMember=std::pair<XorReferencePlane,std::size_t>;
    std::vector<PlaneMember> ordered(samples.size());
    std::vector<std::size_t> matches(samples.size());
    for(std::size_t site=0;site<samples.front()->size();++site) {
      const auto first_plane=(*samples.front())[site];
      if(std::ranges::all_of(samples,[site,first_plane](const auto* sample){return (*sample)[site]==first_plane;}))
        continue;
      for(std::size_t i=0;i<samples.size();++i)ordered[i]={(*samples[i])[site],i};
      std::ranges::sort(ordered,{},&PlaneMember::first);
      // A matching tile plane requires zero payload bytes in packed XOR.
      // Members with one equal plane save the same bytes, including
      // a constant self-match. No pairwise extent matrix is needed.
      for(auto first=ordered.begin();first!=ordered.end();) {
        auto end=first+1;
        while(end!=ordered.end() && end->first==first->first)++end;
        const auto count=static_cast<std::size_t>(end-first);
        for(auto item=first;item!=end;++item)matches[item->second]+=count;
        first=end;
      }
    }
    const auto dense=static_cast<std::size_t>(std::ranges::max_element(matches)-matches.begin());
    // The preliminary comparison uses a bounded, stratified probe set even
    // for large groups. Verify the shortlist over every retained sample.
    std::array<const XorReferenceSample*,xor_reference_tiles> probes{};
    const auto count=(std::min)(probes.size(),samples.size());
    for(std::size_t i=0;i<count;++i)probes[i]=samples[i*(samples.size()-1)/(count-1)];
    const auto cost=[&](std::size_t candidate,std::span<const XorReferenceSample* const> peers) {
      std::uint64_t result=0;
      for(const auto* other:peers)result=add(result,sampled_xor_cost(*samples[candidate],*other));
      return result;
    };
    const auto probe_set=std::span(probes).first(count);
    auto preferred=dense;
    auto sampled_best=cost(dense,probe_set);
    for(std::size_t i=0;i<samples.size();++i) {
      const auto value=cost(i,probe_set);
      if(value<sampled_best){preferred=i;sampled_best=value;}
    }
    auto best=dense;
    auto best_cost=cost(dense,samples);
    for(const auto candidate:{preferred,std::size_t{0}}) {
      if(candidate==best)continue;
      const auto value=cost(candidate,samples);
      if(value<best_cost){best=candidate;best_cost=value;}
    }
    XorReferenceOrder result{group[best].object};
    std::size_t next=1;
    for(const auto candidate:{group[dense].object,original})
      if(std::ranges::find(result,candidate)==result.end())result[next++]=candidate;
    return result;
  }
  void propose_indexed_group(NativeFilesystemWriter& writer,std::size_t maximum_members,
      std::size_t remaining_extents,bool collect_batch) {
    const auto band=candidate_order[candidate_index++];
    const auto& ids=buckets.at(band);
    const auto reusable=std::ranges::find_if(ids,[&](auto id){return indexed_feature(id).token.exact_base;});
    const auto prior=std::ranges::find_if(ids,[&](auto id){return !features.contains(id);});
    auto representative=reusable!=ids.end()?*reusable:prior!=ids.end()?*prior:ids.front();
    if(!features.contains(representative)) {
      const auto& token=representatives.at(representative).token;
      const auto live=writer.maintenance_token(token.extent);
      if(!live || live->object!=token.object || live->revision!=token.revision) {
        representatives.erase(representative);
        rank_buckets(true);
        return;
      }
    }
    std::vector<NativeContentToken> expected{indexed_feature(representative).token};
    expected.reserve(maximum_members);
    for(const auto id:ids) {
      if(expected.size()==maximum_members)break;
      const auto found=features.find(id);
      if(id!=representative && found!=features.end() && !found->second.token.grouped)expected.push_back(found->second.token);
    }
    if(expected.size()<2)return;
    if(!staged_objects.empty()) {
      const auto staged=static_cast<std::size_t>(std::ranges::count_if(expected,[&](const auto& token) {
        return staged_objects.contains(token.object);
      }));
      if(staged==expected.size())return;
      if(staged || expected.size()>remaining_extents || std::ranges::any_of(expected,[&](const auto& token) {
        return deferred_batch_objects.contains(token.object);
      })) {
        // Keep the ranking of intersecting candidates. Other candidates can
        // still fill this batch. Revisit the same window after it settles.
        for(const auto& token:expected)deferred_batch_objects.insert(token.object);
        return;
      }
    }
    XorReferenceOrder references{representative};
    const bool masked_reference=reusable==ids.end() && prior==ids.end() &&
        band.first>=kCoherenceMaskedBand && band.first<kCoherenceModularBand;
    if(masked_reference)
      references=choose_xor_reference(expected);
    std::ranges::sort(expected,{},&NativeContentToken::object);
    std::vector<NativeExtentSelection> selected;
    std::vector<std::byte> key;
    selected.reserve(expected.size());key.reserve(16+48*expected.size());
    append(key,2);append(key,0);
    for(const auto& token:expected) {
      selected.push_back(token.extent);
      append(key,token.object);append(key,token.revision);
      const auto& content=indexed_feature(token.object).content;
      key.insert(key.end(),content.begin(),content.end());
    }
    const auto candidate_key=[&](std::byte mode,std::uint64_t anchor) {
      key.front()=mode;
      for(unsigned i=0;i<8;++i)key[8+i]=static_cast<std::byte>(anchor>>(8*i));
      return detail::record_digest(key);
    };
    const bool compare_references=masked_reference && expected.size()>2;
    bool allow_subset=!compare_references;
    std::size_t choice=0;
    work_sampled_key.reset();
    work_sampled_retry=false;
    for(;choice<references.size() && references[choice];++choice) {
      representative=references[choice];
      work_key=candidate_key(std::byte{2},representative);
      if(attempted.contains(work_key))continue;
      if(!choice && compare_references) {
        const auto refined=candidate_key(std::byte{3},representative);
        if(attempted.contains(refined))work_sampled_retry=true;
        else {work_sampled_key=work_key;work_key=refined;}
      }
      break;
    }
    if(choice==references.size() || !references[choice]) {
      if(!compare_references)return;
      representative=references.front();
      work_key=candidate_key(std::byte{4},representative);
      if(attempted.contains(work_key))return;
      // Exhaust the full-group alternatives before retaining a subset. Use
      // the preferred nominee for a single bounded fallback.
      allow_subset=true;
      work_sampled_retry=true;
    } else if(compare_references)--candidate_index;
    work_ids.clear();work_ids.reserve(expected.size());
    for(const auto& token:expected)work_ids.push_back(token.object);
    const auto base=indexed_feature(representative).token.extent;
    remember(work_key);
    work.emplace(writer.prepare_shared_base(base,selected,expected,collect_batch,allow_subset));
    work_members=expected.size();work_batched=collect_batch;
    ++state.attempted_groups;phase=Phase::preparing;
  }
  std::optional<NativeContentToken> next_indexed_sample(NativeFilesystemWriter& writer) {
    if(!update_count)return writer.maintenance_next(cursor,first);
    if(next_update==update_count)return {};
    const auto extent=updates[next_update++];
    return writer.maintenance_token(extent).value_or(NativeContentToken{extent});
  }
  void step_indexed(NativeFilesystemWriter& writer,std::size_t maximum_members,
      std::size_t maximum_representatives,bool collect_batch,detail::NativeMaintenanceSource& owner) {
    // The budget limits one trial and one scan window. Existing anchors can
    // accept further windows without rewriting earlier members.
    const auto maximum=(std::min)(maximum_members,kNativeMaxDirtyExtents);
    if(refresh_base) {
      const auto selected=*refresh_base;
      refresh_base.reset();
      if(selected.inode) {
        const auto token=writer.maintenance_token(selected);
        if(token && token->object) {
          auto feature=measure(writer,*token,true);
          if(feature.token.exact_base)representatives.insert_or_assign(token->object,std::move(feature));
        }
      }
      trim_representatives(maximum_representatives);
      rank_buckets(true);
      return;
    }
    if(phase==Phase::scanning) {
      const auto token=next_indexed_sample(writer);
      if(token) {
        if(!staged_groups.empty())++staged_scan_extents;
        if(!update_count){cursor=token->extent;first=false;}
        if(token->object) {
          const auto known=representatives.find(token->object);
          const bool can_publish_catalog=owner.coherence_writer || owner.coherence_storage;
          if(known!=representatives.end() && known->second.projected && known->second.token.revision==token->revision &&
              (known->second.deltas_sampled || !can_publish_catalog) &&
              known->second.token.extent.inode==token->extent.inode &&
              known->second.token.extent.extent_index==token->extent.extent_index) {
            // Index lookups provide the revision; a verified payload sample
            // also carries the representation's learned group role.
            features.insert_or_assign(token->object,known->second);
            ++state.reused_features;
          } else {
            auto feature=measure(writer,*token);
            if(!feature.token.grouped || feature.token.exact_base)
              features.insert_or_assign(token->object,std::move(feature));
            else {
              features.erase(token->object);
              representatives.erase(token->object);
            }
          }
        }
        if(features.size()>=maximum)rank_buckets(true);
      } else {extent_scan_exhausted=true;rank_buckets(true);}
      return;
    }
    if(candidate_index<candidate_order.size()) {
      propose_indexed_group(writer,maximum,maximum_members-staged_extents,collect_batch);
      return;
    }
    if(!deferred_batch_objects.empty()) {
      emit_batch();
      return;
    }
    if(query_coherence(owner,maximum_representatives))return;
    if(update_count) {
      // Settle submitted groups before deciding whether any updated extent
      // still needs the ordinary sweep to find a distant candidate.
      if(!staged_groups.empty() && !batch_draining) {
        emit_batch();
        return;
      }
      updates_need_sweep=updates_need_sweep ||
          std::ranges::any_of(features,[](const auto& entry){return !entry.second.token.grouped;});
    }
    retain_indexed_window(maximum_representatives);
    if(extent_scan_exhausted && !staged_groups.empty() && !batch_draining) {
      emit_batch();
      return;
    }
    if(extent_scan_exhausted)complete_scan(writer);
    else phase=Phase::scanning;
    const auto samples=state.sampled_extents-checkpoint_samples;
    checkpoint_due=!checkpoint_unavailable && !first && samples &&
        (phase==Phase::idle || samples>=checkpoint_window_samples);
  }
  bool refine_reference(NativeFilesystemWriter& writer,std::size_t maximum_members,bool sections) {
    const bool compare=sections && !work_batched && work_members>=3 && !work_sampled_retry;
    const auto purpose=compare?NativeSharedBaseSearchPurpose::section_comparison:NativeSharedBaseSearchPurpose::advice;
    if((compare || work_sampled_key) && writer.stage_shared_base_search(*work,work_members-1,maximum_members,purpose))
      return true;
    if(!work_sampled_key || !writer.price_shared_base_reference(*work))return false;
    ++state.cpu_reference_pairs;
    return true;
  }
  void step(NativeFilesystemWriter& writer,std::size_t maximum_members,std::size_t maximum_representatives,
      NativeGroupSearchOpportunity search,detail::NativeMaintenanceSource& owner,bool sections) {
    const bool can_batch=search.collect_batch && writer.maintenance_indexed();
    const bool collect_batch=can_batch && search.maximum_members;
    if(!staged_groups.empty() && !batch_draining) {
      if(!collect_batch || staged_search_members>search.maximum_members)batch_draining=true;
      else if(!work && (staged_search_members==search.maximum_members || maximum_members-staged_extents<2 ||
          staged_scan_extents>=search.maximum_members)) {
        emit_batch();
        return;
      }
    }
    if(batch_draining && !work) {
      if(staged_groups.empty()) {
        batch_draining=false;
        deferred_batch_objects.clear();
      }
      if(refresh_base) {
        step_indexed(writer,maximum_members,maximum_representatives,false,owner);
        return;
      }
      if(staged_groups.empty()) {
        rank_buckets(true);
        return;
      } else {
        const bool cpu_suffix=!search.pending_prefix.empty();
        auto& next=cpu_suffix?staged_groups.back():staged_groups.front();
        work.emplace(std::move(next.work));
        for(const auto id:next.ids)staged_objects.erase(id);
        work_ids=std::move(next.ids);
        work_sampled_key=std::move(next.sampled_key);work_sampled_retry=false;
        work_members=next.member_count;work_batched=true;
        staged_extents-=next.member_count;staged_search_members-=next.search_members;
        if(cpu_suffix)staged_groups.pop_back();
        else staged_groups.pop_front();
        if(staged_groups.empty())staged_scan_extents=0;
        phase=Phase::preparing;
      }
    }
    if(work) {
      if(phase==Phase::preparing) {
        auto grant=(std::min)(search.maximum_members,maximum_members);
        if(work_batched)grant=!batch_draining && grant>staged_search_members?
            (std::min)(grant-staged_search_members,work_members-1):0;
        if(work_sampled_retry)grant=0;
        const auto window=work->search_window();
        const auto* reply=work_sampled_retry?nullptr:search.reply;
        if(window && !search.replies.empty()) {
          const auto found=std::ranges::find(search.replies,window,&NativeSharedBaseSearchReply::window);
          if(found!=search.replies.end())reply=&*found;
        }
        if(!reply && writer.stage_shared_base_search(*work,grant,maximum_members)) {
          const auto completed=work->search_window();
          if(completed && work_batched) {
            const auto searched=completed.members().size();
            staged_objects.insert(work_ids.begin(),work_ids.end());
            staged_groups.push_back({std::move(*work),std::move(work_ids),work_sampled_key,work_members,searched});
            staged_extents+=work_members;staged_search_members+=searched;
            work.reset();phase=Phase::candidates;
            if(staged_search_members==search.maximum_members || maximum_members-staged_extents<2)emit_batch();
          } else state.search_window_ready=static_cast<bool>(completed);
        } else if((reply || !refine_reference(writer,maximum_members,sections)) && writer.advance_shared_base(*work,{},reply))
          phase=Phase::publishing;
      } else if(writer.maintenance_indexed()) {
        const auto previous_end=writer.maintenance_physical_end();
        const auto reselected=work->progress().reference_changed;
        const auto result=writer.finish_shared_base(*work);
        if(result.applied) {
          account_group(result,previous_end);
          state.advised_members_published=add(state.advised_members_published,result.advised_members);
          state.prefixed_zstd_members=add(state.prefixed_zstd_members,result.prefixed_zstd_members);
          if(result.section_reference)++state.section_reference_groups;
          else if(reselected)++state.reselected_reference_groups;
          for(const auto id:work_ids) {
            if(std::ranges::binary_search(result.skipped_objects,id))continue;
            features.erase(id);
            representatives.erase(id);
          }
          // Even a base with no visible representative invalidates the
          // candidate ranking that included the replaced member records.
          refresh_base=result.reference;
        } else {
          if(work_sampled_key && !reselected)remember(*work_sampled_key);
          ++state.declined_groups;
        }
        work.reset();work_ids.clear();phase=Phase::candidates;
      } else {
        const auto before_end=writer.maintenance_physical_end();
        // Preallocate both the outcome and the identical newly published
        // group key. No post-commit diagnostic allocation or immediate repeat.
        std::vector<std::byte> next_key;next_key.reserve(8+48*work_ids.size());append(next_key,1);
        const auto next_revision=add(writer.recompression_generation(),2);
        for(const auto id:work_ids){append(next_key,id);append(next_key,next_revision);
          const auto& hash=features.at(id).content;next_key.insert(next_key.end(),hash.begin(),hash.end());}
        prospective_key=detail::record_digest(next_key);
        remember(work_key);pending_history=1;remember(prospective_key);pending_history=2;
        const auto result=writer.finish_shared_base(*work);pending_history=0;
        if(result.applied) {
          account_group(result,before_end);
          // Source physical history remains until separately selected compaction.
          completed_sequence=0;begin_scan(writer);
        } else {attempted.erase(prospective_key);std::erase(attempted_order,prospective_key);++state.declined_groups;phase=Phase::candidates;}
        work.reset();work_ids.clear();
      }
      state.owned_payload_bytes=owned_payload();return;
    }
    if(phase==Phase::idle) {
      if(completed_sequence==writer.maintenance_content_sequence())return;
      if(!begin_update_scan(writer,completed_sequence))begin_scan(writer);
      return;
    }
    if(writer.maintenance_indexed()) {
      step_indexed(writer,maximum_members,maximum_representatives,can_batch && !batch_draining,owner);
      return;
    }
    if(phase==Phase::scanning) {
      const auto next=writer.maintenance_next(cursor,first);
      if(next) {
        cursor=next->extent;first=false;seen[next->object]=true;
        const auto found=features.find(next->object);
        if(found!=features.end() && found->second.token.content_revision==next->content_revision) {
          found->second.token=*next;++state.reused_features;return;
        }
        features[next->object]=measure(writer,*next);return;
      }
      for(auto it=features.begin();it!=features.end();)it=!seen[it->first]?features.erase(it):std::next(it);
      rank_buckets(false);return;
    }
    if(candidate_index==candidate_order.size()){complete_scan(writer);return;}
    const auto& ids=buckets.at(candidate_order[candidate_index]);
    if(ids.size()<2 || band_offset>=ids.size()) {
      ++candidate_index;band_offset=0;return;
    }
    const auto count=(std::min)(maximum_members,ids.size()-band_offset);
    if(count<2){band_offset=ids.size();return;}
    std::vector<NativeExtentSelection> selections;std::vector<std::byte> key;append(key,1);work_ids.clear();
    selections.reserve(count);key.reserve(8+count*48);
    for(std::size_t i=0;i<count;++i) {
      const auto found=features.find(ids[band_offset+i]);
      if(found==features.end() || !writer.maintenance_matches(found->second.token)) {
        completed_sequence=0;phase=Phase::idle;return;
      }
      const auto& feature=found->second;selections.push_back(feature.token.extent);work_ids.push_back(feature.token.object);
      append(key,feature.token.object);append(key,feature.token.revision);
      key.insert(key.end(),feature.content.begin(),feature.content.end());
    }
    band_offset+=count;work_key=detail::record_digest(key);
    if(attempted.contains(work_key))return;
    work.emplace(writer.prepare_shared_base(selections.front(),selections));work_members=count;
    ++state.attempted_groups;phase=Phase::preparing;
  }
};
NativeGroupMaintenance::NativeGroupMaintenance(NativeFileSession& session,NativeMaintenancePolicy policy) {
  if(!policy.maximum_age_ms || policy.maximum_age_ms>1000 || policy.memory_share_divisor<16 ||
      policy.memory_share_divisor>4096 || policy.maximum_cpu_busy_ppm>million ||
      !policy.work_share_ppm || policy.work_share_ppm>50000 || !policy.maximum_cpu_window_ms || policy.maximum_cpu_window_ms>2000)
    throw NativeFsError(NativeFsErrorCode::invalid_operation,"invalid maintenance policy");
  const auto source=session.maintenance_source().lock();
  if(!source || !source->active.expired())throw NativeFsError(NativeFsErrorCode::invalid_operation,"maintenance controller already owned");
  auto p=std::make_unique<Impl>(source,policy);p->reservation=std::make_shared<const unsigned char>(0);
  source->active=p->reservation;impl_=std::move(p);
}
NativeGroupMaintenance::NativeGroupMaintenance(NativeGroupMaintenance&&) noexcept=default;
NativeGroupMaintenance& NativeGroupMaintenance::operator=(NativeGroupMaintenance&&) noexcept=default;
NativeGroupMaintenance::~NativeGroupMaintenance()=default;
NativeMaintenanceProgress NativeGroupMaintenance::progress() const noexcept {
  return impl_?impl_->progress():NativeMaintenanceProgress{Phase::failed};
}
bool NativeGroupMaintenance::active_group() const noexcept {
  return impl_ && impl_->active_group();
}
std::size_t NativeGroupMaintenance::active_group_members() const noexcept {
  return impl_?impl_->group_members():0;
}
bool NativeGroupMaintenance::pending() const noexcept {
  if(!impl_ || impl_->stopped)return false;
  const auto source=impl_->source.lock();
  if(!source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned())return false;
  const auto& writer=*source->writer;
  const auto image=writer.maintenance_identity();
  return impl_->discovery_pending(writer,*source,image) || source->coherence_pending();
}
void NativeGroupMaintenance::release_staged_groups() noexcept {
  if(!active_group())return;
  impl_->reset_index();
  impl_->state.phase=Phase::idle;
  impl_->state.search_window_ready=false;
  impl_->state.search_batch_ready=false;
}
void NativeGroupMaintenance::cancel() noexcept {
  if(impl_){
    impl_->cancel_coherence_work();impl_->reset_index();impl_->stopped=true;
    impl_->state.phase=Phase::failed;impl_->source.reset();impl_->reservation.reset();
  }
}
NativeSharedBaseSearchWindow NativeGroupMaintenance::search_window() const noexcept {
  if(!impl_ || !impl_->work || impl_->state.phase!=Phase::preparing)return {};
  const auto source=impl_->source.lock();
  if(!source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned())return {};
  return impl_->work->search_window();
}
std::vector<NativeSharedBaseSearchWindow> NativeGroupMaintenance::search_batch() const {
  std::vector<NativeSharedBaseSearchWindow> windows;
  if(!impl_ || !impl_->state.search_batch_ready)return windows;
  const auto source=impl_->source.lock();
  if(!source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned())
    return windows;
  windows.reserve(impl_->staged_groups.size());
  for(const auto& group:impl_->staged_groups)
    if(auto window=group.work.search_window())windows.push_back(std::move(window));
  return windows;
}
bool NativeGroupMaintenance::has_retained_search() const noexcept {
  return impl_ && ((impl_->work && impl_->work->search_window()) ||
      std::ranges::any_of(impl_->staged_groups,[](const auto& group) {
        return static_cast<bool>(group.work.search_window());
      }));
}
NativeMaintenanceProgress NativeGroupMaintenance::advance(const NativeMaintenanceObservation& observed,std::uint64_t now_ms,
    NativeGroupSearchOpportunity search) {
  if(!impl_)return {Phase::failed};auto& p=*impl_;
  p.state.search_window_ready=false;
  p.state.search_batch_ready=false;
  const auto source=p.source.lock();
  if(p.stopped || !source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned()) {
    p.reset_index();p.state.phase=Phase::failed;return p.progress();
  }
  auto& writer=*source->writer;const auto image=writer.maintenance_identity();
  if(p.image!=image){p.reset_index();p.image=image;}
  if(!observed.sequence || observed.sequence<=p.last_observation)return p.defer(Defer::observation);
  p.last_observation=observed.sequence;
  const auto resources=detail::maintenance_resources(observed,now_ms,p.policy);
  if(!resources || observed.captured_ms<p.last_capture)return p.defer(Defer::observation,true);
  if(observed.continued && observed.captured_ms!=p.last_capture)return p.defer(Defer::observation);
  const auto elapsed=p.last_capture?(std::min)(observed.captured_ms-p.last_capture,observed.cpu_window_ms):0;
  p.last_capture=observed.captured_ms;
  const bool indexed=writer.maintenance_indexed();
  auto budget=group_memory_budget(*resources,indexed);
  const bool retained_sections=p.work && p.work->progress().section_comparison;
  const bool offer_sections=indexed && !search.maximum_members && !search.collect_batch && !search.reply &&
      search.replies.empty() && p.staged_groups.empty();
  bool sections=false;
  if(retained_sections || offer_sections) {
    const auto variant=group_memory_budget(*resources,indexed,true);
    if(retained_sections || (variant.members>=3 && p.group_members()<=variant.members)) {
      budget=variant;
      sections=true;
    }
  }
  const auto [maximum_members,maximum_representatives]=budget;
  if(maximum_members<2 || p.group_members()>maximum_members)return p.defer(Defer::memory,true);
  if(p.representatives.size()>maximum_representatives) {
    if(p.phase!=Phase::idle)return p.defer(Defer::memory,true);
    p.trim_representatives(maximum_representatives);
  }
  const bool new_catalog=p.new_coherence(*source,image);
  if(!writer.maintenance_has_candidates())p.reset_index();
  const bool discovering=p.discovery_pending(writer,*source,image);
  const bool reclaiming=source->coherence_pending();
  if(!discovering && !reclaiming) {
    if(source->group_append_shortfall(image,writer.recompression_generation()))return p.defer(Defer::capacity);
    p.state.phase=Phase::idle;p.state.deferral=Defer::none;p.state.next_opportunity_delay_ms=0;return p.progress();
  }
  if(observed.foreground_pending)return p.defer(Defer::foreground);
  if(resources->cpu_busy)return p.defer(Defer::cpu);
  if(!search.pending_prefix.empty() && (search.maximum_members || search.reply || !search.replies.empty() ||
      !p.can_advance_cpu_suffix(search.pending_prefix)))return p.defer(Defer::other_maintenance);
  if(!p.active_group() && writer.maintenance_busy())return p.defer(Defer::other_maintenance);
  const bool service_coherence=reclaiming && !p.active_group() && (p.prefer_coherence || !discovering);
  if(observed.continued && (p.phase==Phase::publishing || service_coherence ||
      (indexed && !p.active_group() && p.checkpoint_due)))return p.defer(Defer::observation);
  // Credit only fresh idle observation intervals. Time paces pending work; it
  // never discovers a candidate or overrides contention/integrity evidence.
  const auto elapsed_ns=elapsed>(std::numeric_limits<std::uint64_t>::max)()/million?
      (std::numeric_limits<std::uint64_t>::max)():elapsed*million;
  auto credit=fraction(elapsed_ns,p.policy.work_share_ppm);
  if(credit>=p.debt){credit-=p.debt;p.debt=0;}else {p.debt-=credit;credit=0;}
  const auto burst=(std::max)(std::uint64_t(1000000),add(p.predicted,p.predicted));
  p.tokens=(std::min)(burst,add(p.tokens,credit));p.state.next_opportunity_delay_ms=0;
  if(p.debt || p.tokens<p.predicted) {
    const auto missing=add(p.debt,p.predicted>p.tokens?p.predicted-p.tokens:0);
    p.state.next_opportunity_delay_ms=missing/p.policy.work_share_ppm+(missing%p.policy.work_share_ppm!=0);
    return p.defer(Defer::duty);
  }
  const auto before=std::chrono::steady_clock::now();
  try {
    if(service_coherence) {
      const auto previous=source->coherence_compactions;
      source->advance_coherence();
      if(source->coherence_compactions!=previous && !p.representatives.empty())p.checkpoint_due=true;
      p.prefer_coherence=false;
      p.state.phase=source->coherence_pending()?Phase::preparing:p.phase;
    }else {
      if(new_catalog) {
        p.coherence_binding=source->coherence_binding;
        p.coherence_after=0;
        p.coherence_query=0;
        p.completed_sequence=0;
        if(p.phase!=Phase::idle)p.scan_revisit=true;
      }
      // Until queued groups settle, the previous cursor must remain
      // restartable. The tentative group windows only exist in this process.
      if(!indexed || observed.continued || p.active_group() ||
          !p.checkpoint_step(writer,*source,maximum_representatives,new_catalog))
        p.step(writer,maximum_members,maximum_representatives,search,*source,sections);
      if(p.checkpoint_due && !source->scan_funded())p.checkpoint_due=false;
      p.prefer_coherence=true;
      p.state.phase=p.phase;
    }
    p.state.deferral=Defer::none;
  }catch(const std::bad_alloc&){p.reset_index();p.state.phase=Phase::deferred;p.state.deferral=Defer::memory;}
  catch(const NativeFsError& error) {
    p.forget_pending();
    const bool was_indexed_group=writer.maintenance_indexed() && p.work.has_value();
    p.work.reset();p.state.owned_payload_bytes=p.owned_payload();
    if(!writer.poisoned() && was_indexed_group && error.code()==NativeFsErrorCode::limit &&
        error.append_shortfall_bytes()) {
      // This plan was refused before writes. Release the other staged groups
      // so the existing reclaimer can act, and suppress duplicate work on
      // this exact published image generation.
      source->group_append_refusal={image,writer.recompression_generation(),error.append_shortfall_bytes()};
      ++p.state.declined_groups;
      p.reset_index();
      p.state.phase=Phase::deferred;p.state.deferral=Defer::capacity;
    } else if(!writer.poisoned() && error.code()==NativeFsErrorCode::limit) {
      if(was_indexed_group) {
        p.work_ids.clear();p.phase=Phase::candidates;++p.state.declined_groups;
      } else p.reset_index();
      p.state.phase=Phase::deferred;p.state.deferral=Defer::memory;
    } else if(!writer.poisoned() && (error.code()==NativeFsErrorCode::invalid_operation || error.code()==NativeFsErrorCode::not_found)) {
      p.work_ids.clear();
      if(writer.maintenance_indexed())p.state.phase=p.phase=Phase::candidates;
      else {p.completed_sequence=0;p.phase=Phase::idle;p.state.phase=Phase::scanning;}
    } else {p.reset_index();p.state.phase=Phase::failed;p.stopped=true;}
  }catch(const std::exception&){p.reset_index();p.state.phase=Phase::failed;p.stopped=true;}
  const auto elapsed_work=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-before).count();
  const auto used=elapsed_work>0?static_cast<std::uint64_t>(elapsed_work):1;
  p.state.elapsed_work_ns=add(p.state.elapsed_work_ns,used);
  p.predicted=(std::max)(used,p.predicted-p.predicted/8+used/8);
  if(used>p.tokens){p.debt=add(p.debt,used-p.tokens);p.tokens=0;}else p.tokens-=used;
  return p.progress();
}
}
