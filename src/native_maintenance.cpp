#include "flitzfs/native_maintenance.hpp"
#include "flitzfs/native_session.hpp"
#include "native_group_codec.hpp"
#include "native_maintenance_source.hpp"
#include "record_digest.hpp"
#include <algorithm>
#include <array>
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
std::uint64_t fraction(std::uint64_t total,std::uint64_t ppm) noexcept {
  return total/million*ppm+total%million*ppm/million;
}
std::uint64_t add(std::uint64_t a,std::uint64_t b) noexcept {
  return b>(std::numeric_limits<std::uint64_t>::max)()-a?(std::numeric_limits<std::uint64_t>::max)():a+b;
}
std::uint64_t word(std::span<const std::byte> bytes) noexcept {
  std::uint64_t result=0;for(unsigned i=0;i<8;++i)result|=std::uint64_t(std::to_integer<unsigned>(bytes[i]))<<(8*i);return result;
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
struct NativeGroupMaintenance::Impl {
  struct Feature {
    NativeContentToken token;
    Digest content{};
    std::array<std::uint64_t,positional_bands+detail::kNativeGroupShiftFeatures> bands{};
  };
  using Band=std::pair<unsigned,std::uint64_t>;
  std::weak_ptr<detail::NativeMaintenanceSource> source;
  std::shared_ptr<const void> reservation;
  NativeMaintenancePolicy policy;NativeMaintenanceProgress state;
  Phase phase=Phase::idle;StoreIdentity image{};NativeExtentSelection cursor{};
  NativeExtentSelection indexed_work_base{};
  bool first=true,stopped=false;unsigned pending_history=0;
  std::uint64_t scan_sequence=0,completed_sequence=0;
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
  std::set<Digest> attempted;std::deque<Digest> attempted_order;
  std::optional<NativeFilesystemWriter::SharedBaseWork> work;
  Digest work_key{};std::size_t work_members=0;
  Digest prospective_key{};
  std::vector<std::uint64_t> work_ids;
  std::uint64_t last_observation=0,last_capture=0,tokens=1000000,debt=0,predicted=1000000;
  explicit Impl(std::weak_ptr<detail::NativeMaintenanceSource> s,NativeMaintenancePolicy p):source(std::move(s)),policy(p){}
  void reset_index() noexcept {
    work.reset();features.clear();buckets.clear();attempted.clear();attempted_order.clear();candidate_order.clear();
    candidate_index=band_offset=0;completed_sequence=0;phase=Phase::idle;state.owned_payload_bytes=0;pending_history=0;work_ids.clear();
    representatives.clear();refresh_base.reset();extent_scan_exhausted=false;
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
  NativeMaintenanceProgress defer(Defer reason,bool release=false) noexcept {
    if(release)reset_index();state.phase=Phase::deferred;state.deferral=reason;
    state.owned_payload_bytes=work?work->progress().owned_payload_bytes:0;return state;
  }
  void begin_scan(NativeFilesystemWriter& writer) noexcept {
    scan_sequence=writer.maintenance_content_sequence();cursor={};first=true;seen.fill(false);phase=Phase::scanning;
    extent_scan_exhausted=false;
  }
  void complete_scan(NativeFilesystemWriter& writer) noexcept {
    if(writer.maintenance_content_sequence()!=scan_sequence)begin_scan(writer);
    else {completed_sequence=scan_sequence;phase=Phase::idle;}
  }
  Feature measure(NativeFilesystemWriter& writer,NativeContentToken token) {
    const auto bytes=writer.maintenance_read(token);
    Feature feature;
    feature.token=token;feature.content=detail::record_digest(bytes);
    constexpr auto width=kNativeExtentBytes/positional_bands;
    for(unsigned i=0;i<positional_bands;++i)
      feature.bands[i]=word(detail::record_digest(std::span<const std::byte>(bytes).subspan(i*width,width)));
    if(writer.maintenance_indexed()) {
      const auto shifted=detail::native_group_shift_features(bytes);
      std::copy(shifted.begin(),shifted.end(),feature.bands.begin()+positional_bands);
    }
    ++state.sampled_extents;
    return feature;
  }
  void rank_buckets(bool indexed) {
    static const auto zero_band=word(detail::record_digest(std::array<std::byte,kNativeExtentBytes/positional_bands>{}));
    buckets.clear();
    for(const auto& [id,feature]:features) {
      if(indexed && feature.token.grouped && !feature.token.exact_base)continue;
      const auto count=indexed?feature.bands.size():positional_bands;
      for(unsigned i=0;i<count;++i) {
        const auto empty=i<positional_bands?zero_band:0;
        if(!indexed || feature.bands[i]!=empty)buckets[{i,feature.bands[i]}].push_back(id);
      }
    }
    if(indexed)for(const auto& [id,feature]:representatives) {
      if(features.contains(id))continue;
      for(unsigned i=0;i<feature.bands.size();++i) {
        const auto bucket=buckets.find({i,feature.bands[i]});
        if(bucket!=buckets.end())bucket->second.push_back(id);
      }
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
  void retain_indexed_window(std::size_t maximum) {
    for(const auto& [id,feature]:features) {
      if(!feature.token.grouped || feature.token.exact_base)representatives.insert_or_assign(id,feature);
      else representatives.erase(id);
    }
    while(representatives.size()>maximum) {
      const auto disposable=std::ranges::find_if(representatives,[](const auto& item){return !item.second.token.exact_base;});
      representatives.erase(disposable==representatives.end()?representatives.begin():disposable);
    }
    features.clear();buckets.clear();candidate_order.clear();
  }
  void propose_indexed_group(NativeFilesystemWriter& writer,std::size_t maximum_members) {
    const auto& ids=buckets.at(candidate_order[candidate_index++]);
    const auto reusable=std::ranges::find_if(ids,[&](auto id){return indexed_feature(id).token.exact_base;});
    const auto prior=std::ranges::find_if(ids,[&](auto id){return !features.contains(id);});
    const auto representative=reusable!=ids.end()?*reusable:prior!=ids.end()?*prior:ids.front();
    std::vector<NativeContentToken> expected{indexed_feature(representative).token};
    expected.reserve(maximum_members);
    for(const auto id:ids) {
      if(expected.size()==maximum_members)break;
      const auto found=features.find(id);
      if(id!=representative && found!=features.end() && !found->second.token.grouped)expected.push_back(found->second.token);
    }
    if(expected.size()<2)return;
    std::ranges::sort(expected,{},&NativeContentToken::object);
    std::vector<NativeExtentSelection> selected;
    std::vector<std::byte> key;
    selected.reserve(expected.size());key.reserve(16+48*expected.size());
    append(key,2);append(key,representative);
    for(const auto& token:expected) {
      selected.push_back(token.extent);
      append(key,token.object);append(key,token.revision);
      const auto& content=indexed_feature(token.object).content;
      key.insert(key.end(),content.begin(),content.end());
    }
    work_key=detail::record_digest(key);
    if(attempted.contains(work_key))return;
    work_ids.clear();work_ids.reserve(expected.size());
    for(const auto& token:expected)work_ids.push_back(token.object);
    const auto base=indexed_feature(representative).token.extent;
    remember(work_key);
    work.emplace(writer.prepare_shared_base(base,selected,expected));
    indexed_work_base=base;work_members=expected.size();
    ++state.attempted_groups;phase=Phase::preparing;
  }
  void step_indexed(NativeFilesystemWriter& writer,std::size_t maximum_members) {
    // The budget limits one trial and one scan window. Existing anchors can
    // accept further windows without rewriting earlier members.
    const auto maximum=(std::min)(maximum_members,kNativeMaxDirtyExtents);
    if(refresh_base) {
      const auto token=writer.maintenance_token(*refresh_base);
      refresh_base.reset();
      if(token && token->object) {
        auto feature=measure(writer,*token);
        if(feature.token.exact_base)representatives.insert_or_assign(token->object,std::move(feature));
      }
      rank_buckets(true);
      return;
    }
    if(phase==Phase::scanning) {
      const auto token=writer.maintenance_next(cursor,first);
      if(token) {
        cursor=token->extent;first=false;
        if(token->object) {
          const auto known=representatives.find(token->object);
          if(known!=representatives.end() && known->second.token.revision==token->revision) {
            features.insert_or_assign(token->object,known->second);++state.reused_features;
          } else features.insert_or_assign(token->object,measure(writer,*token));
        }
        if(features.size()>=maximum)rank_buckets(true);
      } else {extent_scan_exhausted=true;rank_buckets(true);}
      return;
    }
    if(candidate_index<candidate_order.size()){propose_indexed_group(writer,maximum);return;}
    retain_indexed_window(maximum);
    if(extent_scan_exhausted)complete_scan(writer);
    else phase=Phase::scanning;
  }
  void step(NativeFilesystemWriter& writer,std::size_t maximum_members) {
    if(work) {
      if(phase==Phase::preparing) {
        if(writer.advance_shared_base(*work))phase=Phase::publishing;
      } else if(writer.maintenance_indexed()) {
        const auto previous_end=writer.maintenance_physical_end();
        const auto result=writer.finish_shared_base(*work);
        if(result.applied) {
          account_group(result,previous_end);
          for(const auto id:work_ids){features.erase(id);representatives.erase(id);}
          refresh_base=indexed_work_base;
        } else ++state.declined_groups;
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
      state.owned_payload_bytes=work?work->progress().owned_payload_bytes:0;return;
    }
    if(phase==Phase::idle) {
      if(completed_sequence==writer.maintenance_content_sequence())return;
      begin_scan(writer);return;
    }
    if(writer.maintenance_indexed()){step_indexed(writer,maximum_members);return;}
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
  if(!impl_)return {Phase::failed};auto state=impl_->state;
  if(impl_->source.expired())state.phase=Phase::failed;return state;
}
bool NativeGroupMaintenance::active_group() const noexcept {
  return impl_ && impl_->work.has_value();
}
bool NativeGroupMaintenance::pending() const noexcept {
  if(!impl_ || impl_->stopped)return false;
  const auto source=impl_->source.lock();
  if(!source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned())return false;
  const auto& writer=*source->writer;
  return impl_->phase!=Phase::idle || (writer.maintenance_has_candidates() &&
      (impl_->image!=writer.maintenance_identity() || impl_->completed_sequence!=writer.maintenance_content_sequence()));
}
void NativeGroupMaintenance::cancel() noexcept {
  if(impl_){impl_->reset_index();impl_->stopped=true;impl_->state.phase=Phase::failed;impl_->source.reset();impl_->reservation.reset();}
}
NativeMaintenanceProgress NativeGroupMaintenance::advance(const NativeMaintenanceObservation& observed,std::uint64_t now_ms) {
  if(!impl_)return {Phase::failed};auto& p=*impl_;
  const auto source=p.source.lock();
  if(p.stopped || !source || !source->writer || !source->terminal || *source->terminal || source->writer->poisoned()) {
    p.reset_index();p.state.phase=Phase::failed;return p.state;
  }
  auto& writer=*source->writer;const auto image=writer.maintenance_identity();
  if(p.image!=image){p.reset_index();p.image=image;}
  if(!observed.sequence || observed.sequence<=p.last_observation)return p.defer(Defer::observation);
  p.last_observation=observed.sequence;
  const auto resources=detail::maintenance_resources(observed,now_ms,p.policy);
  if(!resources || observed.captured_ms<p.last_capture)return p.defer(Defer::observation,true);
  const auto elapsed=p.last_capture?(std::min)(observed.captured_ms-p.last_capture,observed.cpu_window_ms):0;
  p.last_capture=observed.captured_ms;
  const auto allowance=resources->memory_allowance;
  const auto scratch=fixed_scratch+(writer.maintenance_indexed()?detail::kNativeGroupAnchorIndexBytes:0);
  const auto maximum_members=allowance>scratch?
      (std::min)(std::uint64_t(kNativeMaxExtents),(allowance-scratch)/member_scratch):0;
  if(maximum_members<2 || (p.work && p.work_members>maximum_members))return p.defer(Defer::memory,true);
  if(!writer.maintenance_has_candidates())p.reset_index();
  if(!writer.maintenance_has_candidates() ||
      (!p.work && p.phase==Phase::idle && p.completed_sequence==writer.maintenance_content_sequence())) {
    p.state.phase=Phase::idle;p.state.deferral=Defer::none;p.state.next_opportunity_delay_ms=0;return p.state;
  }
  if(observed.foreground_pending)return p.defer(Defer::foreground);
  if(resources->cpu_busy)return p.defer(Defer::cpu);
  if(!p.work && writer.maintenance_busy())return p.defer(Defer::other_maintenance);
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
    p.step(writer,static_cast<std::size_t>(maximum_members));
    p.state.phase=p.phase;p.state.deferral=Defer::none;
  }catch(const std::bad_alloc&){p.reset_index();p.state.phase=Phase::deferred;p.state.deferral=Defer::memory;}
  catch(const NativeFsError& error) {
    p.forget_pending();
    const bool was_indexed_group=writer.maintenance_indexed() && p.work.has_value();
    p.work.reset();p.state.owned_payload_bytes=0;
    if(!writer.poisoned() && error.code()==NativeFsErrorCode::limit) {
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
  return p.state;
}
}
