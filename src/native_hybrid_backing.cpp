#include "flitzfs/native_hybrid_backing.hpp"
#include "flitzfs/native_session.hpp"
#include "folded_checkpoint.hpp"
#include "protected_stripe_image.hpp"
#include "store_read_scope.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace flitzfs {
namespace {
using detail::Digest;
using detail::ProtectedStripeImage;
using Bytes=std::vector<std::byte>;
constexpr std::uint64_t roots=detail::kFoldedMirrorRoots;
static_assert(kNativeHybridTailPrefixBytes==2*roots);
constexpr std::size_t root_size=detail::kPagedStoreCheckpointWriteBytes/2;
constexpr std::uint64_t maximum_io=static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
constexpr std::string_view binding_magic="FLTXHB01",compact_binding_magic="FLTXHB02";
using BindingRecord=std::array<std::byte,128>;

[[noreturn]] void fail(const char* reason) {throw std::runtime_error(reason);}

struct Binding {
  std::uint64_t unit=0,base_end=0,base_occupied=0;
  StoreIdentity identity{};
  Digest frozen_roots{};
  std::uint64_t virtual_limit=0;
  std::uint64_t slot_bytes=0;
  bool operator==(const Binding&) const=default;

  std::uint64_t folded_extent() const noexcept {return detail::folded_mirror_extent(base_end);}
  std::uint64_t tail_prefix() const noexcept {return 4*slot_bytes;}
  bool compact() const noexcept {return slot_bytes!=kStoreCommitUnit;}
};

constexpr std::uint64_t slot_bytes(std::uint64_t unit,NativeHybridControls controls) noexcept {
  return controls==NativeHybridControls::compact?unit:kStoreCommitUnit;
}

std::uint64_t get64(std::span<const std::byte> bytes,std::size_t offset) noexcept {
  std::uint64_t value=0;
  for(unsigned i=0;i<8;++i)value|=std::to_integer<std::uint64_t>(bytes[offset+i])<<(8*i);
  return value;
}
void put64(std::span<std::byte> bytes,std::size_t offset,std::uint64_t value) noexcept {
  for(unsigned i=0;i<8;++i)bytes[offset+i]=static_cast<std::byte>((value>>(8*i))&255);
}
template<std::size_t N>std::array<std::byte,N> field(std::span<const std::byte> bytes,std::size_t offset) noexcept {
  std::array<std::byte,N> result{};
  std::copy_n(bytes.begin()+offset,N,result.begin());
  return result;
}
BindingRecord encode(const Binding& binding) noexcept {
  BindingRecord bytes{};
  const auto magic=binding.compact()?compact_binding_magic:binding_magic;
  std::ranges::copy(std::as_bytes(std::span(magic)),bytes.begin());
  put64(bytes,8,binding.unit);
  put64(bytes,16,binding.base_end);
  put64(bytes,24,binding.base_occupied);
  std::ranges::copy(binding.identity,bytes.begin()+32);
  std::ranges::copy(binding.frozen_roots,bytes.begin()+48);
  put64(bytes,80,binding.virtual_limit);
  std::ranges::copy(detail::record_digest(std::span(bytes).first(96)),bytes.begin()+96);
  return bytes;
}
std::optional<Binding> read_binding(const StoreByteIo& tail,std::uint64_t offset,bool probing=false) {
  BindingRecord bytes{};
  if(offset>tail.extent() || bytes.size()>tail.extent()-offset)return {};
  try {tail.read_exact(offset,bytes);}
  catch(const std::bad_alloc&){throw;}
  catch(const detail::StoreCaptureInterrupted&){throw;}
  catch(const std::exception&){return {};}
  const auto front=std::span(bytes).first(96);
  if(detail::record_digest(front)!=field<32>(bytes,96))return {};
  const auto has=[&](std::string_view magic) {
    return std::ranges::equal(front.first(magic.size()),std::as_bytes(std::span(magic)));
  };
  if(probing && !has("FLTXHB"))return {};
  const bool compact=has(compact_binding_magic),legacy=has(binding_magic);
  const auto unit=get64(bytes,8);
  const auto spacing=compact?unit:kStoreCommitUnit;
  if(probing && (compact || legacy) && offset!=spacing)return {};
  if((!legacy && !compact) || (offset && offset!=spacing) || (compact && unit>=kStoreCommitUnit) || get64(bytes,88))
    fail("unsupported intact hybrid backing binding");
  return Binding{unit,get64(bytes,16),get64(bytes,24),field<16>(bytes,32),
      field<32>(bytes,48),get64(bytes,80),spacing};
}
std::pair<Binding,bool> load_binding(const StoreByteIo& tail) {
  const auto first=read_binding(tail,0);
  std::optional<Binding> second;
  if(first) {
    // A checksummed record must supply a legal peer address before use.
    if(first->slot_bytes<root_size || first->slot_bytes>kStoreCommitUnit || !std::has_single_bit(first->slot_bytes))
      fail("invalid hybrid binding slot");
    second=read_binding(tail,first->slot_bytes);
  }else {
    for(std::uint64_t offset=root_size;offset<=kStoreCommitUnit;offset*=2) {
      if(auto recovered=read_binding(tail,offset,true)) {
        if(second)fail("ambiguous hybrid backing peer");
        second=std::move(recovered);
      }
    }
  }
  if(!first && !second)fail("missing hybrid backing binding");
  if(first && second && *first!=*second)fail("conflicting hybrid backing bindings");
  return {first?*first:*second,!(first && second)};
}
void check_limits(StoreIdentity identity,std::uint64_t virtual_limit,std::uint64_t physical_limit) {
  if(identity==StoreIdentity{} || virtual_limit<=roots || virtual_limit>maximum_io ||
      physical_limit<4*root_size || physical_limit>maximum_io)
    fail("invalid hybrid backing limits");
}
void check_geometry(const Binding& binding) {
  const auto unit=binding.unit;
  if(unit<root_size || unit>kStoreCommitUnit || !std::has_single_bit(unit) ||
      (binding.compact() && binding.slot_bytes!=unit) ||
      binding.base_end<roots+2*unit || binding.base_end>binding.virtual_limit ||
      (binding.base_end-roots)%(2*unit))
    fail("invalid hybrid base geometry");
}
Digest root_binding(const StoreByteIo& source) {
  std::array<std::byte,root_size> first{},second{};
  source.read_exact(0,first);
  source.read_exact(kStoreCommitUnit,second);
  return detail::record_digest(first,second);
}

std::uint64_t check_empty_backings(const std::shared_ptr<StoreByteIo>& workspace,
    const std::shared_ptr<StoreByteIo>& base,const std::shared_ptr<StoreByteIo>& tail) {
  if(!workspace || !base || !tail || workspace==base || workspace==tail || base==tail ||
      workspace->extent() || base->extent() || tail->extent() ||
      !valid_store_io_contract(workspace->contract()) || !valid_store_io_contract(base->contract()) ||
      !valid_store_io_contract(tail->contract()))
    fail("hybrid preparation needs three distinct, qualified empty backings");
  const auto unit=(std::max<std::uint64_t>)(root_size,workspace->contract().write_failure_unit_bytes);
  if(tail->contract().write_failure_unit_bytes>unit)fail("hybrid tail needs the compacted mirror's failure unit");
  return unit;
}

std::uint64_t seal_footprint(StoreIdentity identity,std::uint64_t end,
    std::uint32_t base_unit,std::uint64_t limit,std::uint64_t slot) {
  const auto tail_prefix=4*slot;
  if(limit<=tail_prefix)fail("hybrid seal capacity");
  const auto occupied=ProtectedStripeImage::projected_occupied(identity,detail::folded_mirror_extent(end),
      base_unit,limit-tail_prefix,slot<kStoreCommitUnit);
  if(occupied+tail_prefix>=end)fail("hybrid base and tail do not reduce the protected footprint");
  return occupied;
}

Binding bind_workspace(const detail::FoldedCheckpoint& packed,const StoreByteIo& workspace,StoreIdentity identity,
    std::uint64_t virtual_limit,std::uint64_t unit,std::uint32_t base_unit,std::uint64_t physical_limit,
    std::uint64_t slot) {
  Binding binding{packed.unit,packed.virtual_end,0,identity,packed.roots,virtual_limit,slot};
  check_geometry(binding);
  if(binding.unit!=unit || binding.folded_extent()!=workspace.extent())
    fail("hybrid folded workspace checkpoint differs");
  binding.base_occupied=seal_footprint(identity,binding.base_end,base_unit,physical_limit,slot);
  return binding;
}

void check_staging(std::uint64_t workspace,std::uint64_t base,std::uint64_t tail_prefix,std::uint64_t limit) {
  if(workspace>limit || base+tail_prefix>limit-workspace)
    fail("hybrid staging exceeds the workspace and candidate budget");
}

class TailBootstrap {
 public:
  // Each step issues at most one ordered flush. Keep the prior stage valid
  // until the next opportunity, including on a slow or unavailable backing.
  bool advance(StoreByteIo& tail,const StoreByteIo& sealed,const Binding& binding) {
    const auto contract=tail.contract();
    if(!valid_store_io_contract(contract) || contract.write_failure_unit_bytes>binding.unit)
      fail("hybrid tail contract changed before binding");
    switch(phase_) {
      case Phase::roots: {
        if(tail.extent())fail("hybrid tail is no longer empty");
        const auto slot=binding.slot_bytes;
        tail.admit_write(binding.tail_prefix());
        roots_.resize(2*slot);
        tail.write_exact(0,roots_);
        sealed.read_exact(0,std::span(roots_).first(slot));
        sealed.read_exact(kStoreCommitUnit,std::span(roots_).last(slot));
        tail.write_exact(2*slot,roots_);
        tail.flush();
        phase_=Phase::first;
        return false;
      }
      case Phase::first: {
        Bytes copied(roots_.size());
        tail.read_exact(2*binding.slot_bytes,copied);
        if(copied!=roots_ || tail.extent()!=binding.tail_prefix())fail("hybrid mutable roots differ");
        roots_.clear();
        tail.write_exact(0,encode(binding));
        tail.flush();
        phase_=Phase::second;
        return false;
      }
      case Phase::second:
        tail.write_exact(binding.slot_bytes,encode(binding));
        tail.flush();
        phase_=Phase::ready;
        break;
      case Phase::ready:
        break;
    }
    return true;
  }

 private:
  enum class Phase { roots, first, second, ready };
  Phase phase_=Phase::roots;
  Bytes roots_;
};

} // namespace

struct NativeHybridBacking::Impl {
  struct Lease {
    Lease(std::shared_ptr<StoreByteIo> backing,bool writer):io(std::move(backing)) {
      if(!io || !(writer?io->acquire_writer():io->acquire_exclusive_use()))
        fail("hybrid backing already owned or unavailable");
    }
    ~Lease(){io->release_writer();}
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    std::shared_ptr<StoreByteIo> io;
  };

  Impl(std::shared_ptr<StoreByteIo> base_io,std::shared_ptr<StoreByteIo> tail_io,std::uint64_t cap)
      :writable_tail(valid_store_io_contract(tail_io->contract()) && tail_io->allows_writer()),
       base_owner(std::move(base_io),false),tail(std::move(tail_io),writable_tail),physical_limit(cap) {}

  std::uint64_t tail_extent() const {
    const auto extent=tail.io->extent();
    const auto prefix=binding.tail_prefix();
    if(extent<prefix || extent>physical_limit-binding.base_occupied ||
        extent-prefix>binding.virtual_limit-binding.base_end)
      fail("hybrid writable tail outside admitted bounds");
    return extent;
  }
  std::uint64_t tail_offset(std::uint64_t at) const noexcept {
    return binding.tail_prefix()+(at-binding.base_end);
  }
  std::shared_ptr<ProtectedStripeImage> open_bound_base(const Binding& expected) const {
    if(physical_limit<=expected.tail_prefix())fail("hybrid backing capacity");
    auto checked=ProtectedStripeImage::open(base_owner.io,physical_limit-expected.tail_prefix());
    if(checked->identity()!=expected.identity || checked->extent()!=expected.folded_extent() ||
        checked->occupied()!=expected.base_occupied || root_binding(*checked)!=expected.frozen_roots)
      fail("hybrid base does not match the checked binding");
    return checked;
  }

  const bool writable_tail;
  Lease base_owner,tail;
  std::shared_ptr<ProtectedStripeImage> base;
  const std::uint64_t physical_limit;
  Binding binding;
  mutable std::atomic<bool> degraded{false};
};

NativeHybridBacking::NativeHybridBacking(std::unique_ptr<Impl> state) noexcept:impl_(std::move(state)) {}
NativeHybridBacking::~NativeHybridBacking()=default;

std::uint64_t NativeHybridBacking::workspace_capacity(StoreIdentity identity,StoreIoContract storage,
    std::uint64_t packed_end_bound,std::uint64_t staging_bytes,std::uint64_t private_workspace_bytes) {
  if(!valid_store_io_contract(storage) || packed_end_bound<=roots || packed_end_bound>maximum_io ||
      staging_bytes>maximum_io)return 0;
  const auto alignment=(std::max<std::uint64_t>)(root_size,storage.write_failure_unit_bytes);
  const auto tail_prefix=4*alignment;
  if(staging_bytes<=tail_prefix)return 0;
  const auto budget=staging_bytes-tail_prefix;
  // A file workspace overlaps its candidate's allocation budget. A
  // separately owned workspace can use its own admitted capacity.
  const auto workspace_limit=private_workspace_bytes?private_workspace_bytes:budget/2;
  std::uint64_t low=0,high=(std::min)(detail::folded_mirror_extent(packed_end_bound),workspace_limit)/alignment;
  const auto base_bytes=[&](std::uint64_t workspace) {
    return ProtectedStripeImage::projected_occupied(identity,workspace,storage.write_failure_unit_bytes,maximum_io,true);
  };
  while(low<high) {
    const auto middle=low+(high-low+1)/2;
    const auto workspace=middle*alignment;
    const auto available=private_workspace_bytes?budget:budget-workspace;
    if(base_bytes(workspace)<=available)low=middle;
    else high=middle-1;
  }
  const auto profitable=[&](std::uint64_t workspace) {
    return workspace>roots && base_bytes(workspace)+tail_prefix<roots+2*(workspace-roots);
  };
  if(profitable(low*alignment))return low*alignment;
  if(!low)return 0;
  // A partial stripe can price worse than the preceding complete stripe.
  const auto complete=((low-1)/detail::kProtectedStripeDataUnits)*detail::kProtectedStripeDataUnits*alignment;
  return profitable(complete)?complete:0;
}

std::array<std::shared_ptr<StoreByteIo>,2> NativeHybridBacking::components() const noexcept {
  return {impl_->base_owner.io,impl_->tail.io};
}
NativePairedSealedRange NativeHybridBacking::sealed_range() const noexcept {
  const auto& b=impl_->binding;
  return {b.base_occupied,b.base_end,static_cast<std::uint32_t>(b.slot_bytes)};
}

std::shared_ptr<NativeHybridBacking> NativeHybridBacking::open(std::shared_ptr<StoreByteIo> base,
    std::shared_ptr<StoreByteIo> tail,StoreIdentity expected,std::uint64_t virtual_limit,std::uint64_t physical_limit) {
  check_limits(expected,virtual_limit,physical_limit);
  if(!base || !tail || base==tail || tail->extent()<4*root_size || tail->extent()>physical_limit)
    fail("hybrid image needs a bounded tail and a distinct base");
  auto state=std::make_unique<Impl>(std::move(base),std::move(tail),physical_limit);
  auto [binding,degraded]=load_binding(*state->tail.io);
  if(binding.identity!=expected || binding.virtual_limit!=virtual_limit)
    fail("hybrid image identity or capacity differs");
  check_geometry(binding);
  if(state->writable_tail && state->tail.io->contract().write_failure_unit_bytes>binding.unit)
    fail("hybrid tail failure unit exceeds the indexed geometry");
  state->base=state->open_bound_base(binding);
  state->binding=binding;
  state->degraded.store(degraded,std::memory_order_relaxed);
  (void)state->tail_extent();
  return std::shared_ptr<NativeHybridBacking>(new NativeHybridBacking(std::move(state)));
}

std::shared_ptr<NativeHybridBacking> NativeHybridBacking::create(const NativeFilesystemSnapshot& source,
    std::shared_ptr<StoreByteIo> workspace,std::shared_ptr<StoreByteIo> base,std::shared_ptr<StoreByteIo> tail,
    StoreIdentity identity,std::uint64_t physical_limit,StoreCompressionOptions compression,
    NativeHybridControls controls) {
  const auto source_storage=source.image_storage();
  check_limits(identity,source_storage.maximum_physical_bytes,physical_limit);
  if(source_storage.kind!=NativeImageKind::indexed_paged || source.needs_session_recovery() ||
      identity==source.identity())
    fail("hybrid creation needs a closed indexed source and a new identity");
  const auto unit=check_empty_backings(workspace,base,tail);
  const auto slot=slot_bytes(unit,controls);
  const auto packed=source.build_folded_workspace(workspace,identity,compression);
  const auto binding=bind_workspace(packed,*workspace,identity,source_storage.maximum_physical_bytes,unit,
      base->contract().write_failure_unit_bytes,physical_limit,slot);
  const auto sealed=ProtectedStripeImage::seal(workspace,base,identity,physical_limit-binding.tail_prefix(),
      binding.compact());
  if(sealed->occupied()!=binding.base_occupied || root_binding(*sealed)!=binding.frozen_roots)
    fail("hybrid checked base differs from the staged checkpoint");
  {
    const Impl::Lease tail_owner(tail,true);
    TailBootstrap bootstrap;
    while(!bootstrap.advance(*tail,*sealed,binding)) {}
  }
  auto result=open(std::move(base),std::move(tail),identity,binding.virtual_limit,physical_limit);
  auto checked=NativeFilesystemSnapshot::open_image(result,source_storage);
  checked.verify_all();
  if(!result->allows_writer() || checked.storage_info().degraded_checkpoint ||
      checked.storage_info().generation!=packed.generation)
    fail("hybrid checkpoint did not verify after publication");
  return result;
}

struct NativeHybridBacking::Build::Impl {
  Impl(PagedObjectStoreWriter::Rewrite rewrite,std::shared_ptr<StoreByteIo> workspace,
      StoreIoContract base_contract,NativeImageStorage storage,StoreIdentity identity,std::uint64_t unit,
      std::uint64_t candidate_limit,std::uint64_t staging_limit,bool owned_file_target,std::uint64_t control_slot)
      :rewrite(std::move(rewrite)),workspace(std::move(workspace)),base_contract(base_contract),
       storage(storage),identity(identity),unit(unit),candidate_limit(candidate_limit),staging_limit(staging_limit),
       control_slot(control_slot),owned_file_target(owned_file_target) {}

  void begin_seal() {
    seal.emplace(ProtectedStripeImage::begin_seal(workspace,base,identity,candidate_limit-binding.tail_prefix(),
        binding.compact()));
    phase=BuildPhase::sealing;
  }

  void begin_binding() {
    tail_owner.emplace(tail,true);
    phase=BuildPhase::binding;
  }

  bool advance(const NativeFilesystemWriter& source) {
    if(!source.matches_paged_recompression(rewrite))fail("hybrid preparation source checkpoint changed");
    switch(phase) {
      case BuildPhase::workspace: {
        detail::FoldedCheckpoint packed;
        if(!source.advance_folded_recompression(rewrite,packed))return false;
        binding=bind_workspace(packed,*workspace,identity,storage.maximum_physical_bytes,unit,
            base_contract.write_failure_unit_bytes,candidate_limit,control_slot);
        check_staging(binding.folded_extent(),binding.base_occupied,binding.tail_prefix(),staging_limit);
        generation=packed.generation;
        phase=BuildPhase::awaiting_base;
        if(base)begin_seal();
        return false;
      }
      case BuildPhase::awaiting_base:
      case BuildPhase::awaiting_tail:
        return false;
      case BuildPhase::sealing:
        if(!seal->advance())return false;
        sealed=seal->finish();
        seal.reset();
        if(sealed->occupied()!=binding.base_occupied || root_binding(*sealed)!=binding.frozen_roots)
          fail("hybrid checked base differs from the staged checkpoint");
        base_lease.emplace(base,false);
        workspace.reset();
        phase=BuildPhase::awaiting_tail;
        if(tail)begin_binding();
        return false;
      case BuildPhase::binding: {
        if(!bootstrap.advance(*tail,*sealed,binding))return false;
        tail_owner.reset();
        base_lease.reset();
        auto reopened=NativeHybridBacking::open(base,tail,identity,binding.virtual_limit,candidate_limit);
        auto checked=NativeFilesystemSnapshot::open_image(reopened,storage);
        if(!reopened->allows_writer() || checked.storage_info().degraded_checkpoint ||
            checked.storage_info().generation!=generation)
          fail("hybrid checkpoint did not verify after binding");
        verified.emplace(std::move(checked));
        result=std::move(reopened);
        phase=BuildPhase::ready;
        break;
      }
      case BuildPhase::ready:
        break;
      case BuildPhase::stopped:
        fail("hybrid preparation is unavailable");
    }
    return true;
  }

  PagedObjectStoreWriter::Rewrite rewrite;
  std::shared_ptr<StoreByteIo> workspace,base,tail;
  StoreIoContract base_contract;
  NativeImageStorage storage;
  StoreIdentity identity;
  std::uint64_t unit,candidate_limit,staging_limit,control_slot;
  const bool owned_file_target;
  BuildPhase phase=BuildPhase::workspace;
  Binding binding;
  std::uint64_t generation=0;
  std::optional<ProtectedStripeImage::SealWork> seal;
  std::shared_ptr<ProtectedStripeImage> sealed;
  std::optional<NativeHybridBacking::Impl::Lease> base_lease,tail_owner;
  TailBootstrap bootstrap;
  std::shared_ptr<NativeHybridBacking> result;
  std::optional<NativeFilesystemSnapshot> verified;
};

NativeHybridBacking::Build NativeHybridBacking::prepare(const NativeFileSession& session,
    std::shared_ptr<StoreByteIo> workspace,std::shared_ptr<StoreByteIo> base,std::shared_ptr<StoreByteIo> tail,
    StoreIdentity identity,std::uint64_t candidate_limit,std::uint64_t staging_limit,
    StoreCompressionOptions compression,NativeHybridControls controls) {
  const auto& owner=session.ready();
  session.admit_replacement(owner,workspace,identity);
  session.admit_replacement(owner,base,identity);
  session.admit_replacement(owner,tail,identity);
  (void)check_empty_backings(workspace,base,tail);
  auto work=prepare(session,std::move(workspace),base->contract(),identity,candidate_limit,staging_limit,compression,
      controls);
  work.impl_->base=std::move(base);
  work.impl_->tail=std::move(tail);
  return work;
}

NativeHybridBacking::Build NativeHybridBacking::prepare(const NativeFileSession& session,
    std::shared_ptr<StoreByteIo> workspace,StoreIoContract base_contract,StoreIdentity identity,
    std::uint64_t candidate_limit,std::uint64_t staging_limit,StoreCompressionOptions compression,
    NativeHybridControls controls) {
  return prepare_impl(session,std::move(workspace),base_contract,identity,candidate_limit,staging_limit,
      compression,false,controls);
}
NativeHybridBacking::Build NativeHybridBacking::prepare_impl(const NativeFileSession& session,
    std::shared_ptr<StoreByteIo> workspace,StoreIoContract base_contract,StoreIdentity identity,
    std::uint64_t candidate_limit,std::uint64_t staging_limit,StoreCompressionOptions compression,
    bool owned_file_target,NativeHybridControls controls) {
  const auto& writer=session.hybrid_source(owned_file_target);
  session.admit_replacement(session.ready(),workspace,identity,owned_file_target);
  if(workspace->extent() || !valid_store_io_contract(workspace->contract()) || !valid_store_io_contract(base_contract))
    fail("hybrid preparation requires an empty workspace and qualified backing contracts");
  const auto source=writer.snapshot();
  const auto storage=source.image_storage();
  check_limits(identity,storage.maximum_physical_bytes,candidate_limit);
  const auto unit=(std::max<std::uint64_t>)(root_size,workspace->contract().write_failure_unit_bytes);
  const auto slot=slot_bytes(unit,controls);
  if(storage.kind!=NativeImageKind::indexed_paged || identity==source.identity() || staging_limit<4*slot)
    fail("invalid managed source or hybrid staging budget");
  const auto base_unit=base_contract.write_failure_unit_bytes;
  auto admission=[identity,base_unit,candidate_limit,staging_limit,slot](std::uint64_t end) {
    check_staging(detail::folded_mirror_extent(end),seal_footprint(identity,end,base_unit,candidate_limit,slot),
        4*slot,staging_limit);
  };
  auto rewrite=writer.prepare_folded_recompression(workspace,identity,compression,admission);
  return Build(std::make_unique<Build::Impl>(std::move(rewrite),std::move(workspace),base_contract,
      storage,identity,unit,candidate_limit,staging_limit,owned_file_target,slot));
}

NativeHybridBacking::Build::Build(std::unique_ptr<Impl> state) noexcept:impl_(std::move(state)) {}
NativeHybridBacking::Build::Build(Build&&) noexcept=default;
NativeHybridBacking::Build& NativeHybridBacking::Build::operator=(Build&&) noexcept=default;
NativeHybridBacking::Build::~Build()=default;
NativeHybridBacking::BuildPhase NativeHybridBacking::Build::phase() const noexcept {
  return impl_?impl_->phase:BuildPhase::stopped;
}
bool NativeHybridBacking::Build::source_current(const NativeFileSession& session) const noexcept {
  if(!impl_)return false;
  try {return session.hybrid_source(impl_->owned_file_target).matches_paged_recompression(impl_->rewrite);}
  catch(const std::exception&){return false;}
}
bool NativeHybridBacking::Build::can_continue_preparation() const noexcept {
  if(phase()==BuildPhase::sealing)return impl_->seal->can_continue_preparation();
  if(phase()!=BuildPhase::workspace)return false;
  using Phase=PagedRewritePhase;
  const auto next=impl_->rewrite.progress().phase;
  return next==Phase::verifying_source || next==Phase::copying || next==Phase::building_index ||
      next==Phase::verifying_target || next==Phase::verifying_publication;
}
bool NativeHybridBacking::Build::advance(const NativeFileSession& session) {
  if(!impl_)fail("hybrid preparation is unavailable");
  try {return impl_->advance(session.hybrid_source(impl_->owned_file_target));}
  catch(...){cancel();throw;}
}
std::uint64_t NativeHybridBacking::Build::required_store_bytes() const noexcept {
  if(phase()==BuildPhase::awaiting_base)return impl_->binding.base_occupied;
  if(phase()==BuildPhase::awaiting_tail) {
    const auto& b=impl_->binding;
    return (std::min)(impl_->candidate_limit-b.base_occupied,b.tail_prefix()+(b.virtual_limit-b.base_end));
  }
  return 0;
}
void NativeHybridBacking::Build::provide_store(const NativeFileSession& session,std::shared_ptr<StoreByteIo> store) {
  const auto needed=phase();
  if(needed!=BuildPhase::awaiting_base && needed!=BuildPhase::awaiting_tail)
    fail("hybrid preparation is not waiting for a store");
  try {
    if(!source_current(session))
      fail("hybrid preparation source checkpoint changed");
    session.admit_replacement(session.ready(),store,impl_->identity,impl_->owned_file_target);
    if(store==impl_->workspace || store==impl_->base || store->extent() ||
        !valid_store_io_contract(store->contract()))
      fail("hybrid preparation needs a distinct, qualified empty store");
    const auto unit=store->contract().write_failure_unit_bytes;
    if(needed==BuildPhase::awaiting_base) {
      if(unit!=impl_->base_contract.write_failure_unit_bytes)
        fail("hybrid base contract differs from the admitted geometry");
      impl_->base=std::move(store);
      impl_->begin_seal();
    }else {
      if(unit>impl_->binding.unit)fail("hybrid tail needs the compacted mirror's failure unit");
      impl_->tail=std::move(store);
      impl_->begin_binding();
    }
  }catch(...){cancel();throw;}
}
std::shared_ptr<NativeHybridBacking> NativeHybridBacking::Build::finish(const NativeFileSession& session) {
  if(phase()!=BuildPhase::ready)fail("hybrid preparation is incomplete");
  try {
    session.hybrid_source(impl_->owned_file_target).verify_folded_recompression_source(impl_->rewrite);
    auto result=std::move(impl_->result);
    cancel();
    return result;
  }catch(...){cancel();throw;}
}
bool NativeHybridBacking::Build::publish(NativeFileSession& session,
    std::shared_ptr<NativeHybridBacking>& replacement) {
  if(phase()!=BuildPhase::ready || replacement)fail("live hybrid selection needs ready work and an empty replacement slot");
  try {
    const auto file=impl_->owned_file_target;
    session.hybrid_source(file).verify_folded_recompression_source(impl_->rewrite);
    auto checked=std::move(*impl_->verified);
    replacement=std::move(impl_->result);
    cancel();
    if(session.publish_hybrid_replacement(replacement,checked,file))return true;
    replacement.reset();
    return false;
  }catch(...) {
    cancel();
    if(!session.poisoned())replacement.reset();
    throw;
  }
}
void NativeHybridBacking::Build::cancel() noexcept {impl_.reset();}

std::uint64_t NativeHybridBacking::occupied() const {
  return impl_->binding.base_occupied+impl_->tail_extent();
}
std::uint64_t NativeHybridBacking::extent() const {
  return impl_->binding.base_end+(impl_->tail_extent()-impl_->binding.tail_prefix());
}
void NativeHybridBacking::read_exact(std::uint64_t at,std::span<std::byte> output) const {
  detail::StoreCaptureReadScope::require_active();
  const auto& state=*impl_;
  const auto& binding=state.binding;
  if(at>binding.virtual_limit || output.size()>binding.virtual_limit-at)fail("hybrid read bounds");
  while(at<roots && !output.empty()) {
    const auto at_slot=at%kStoreCommitUnit;
    const auto count=(std::min<std::uint64_t>)(output.size(),kStoreCommitUnit-at_slot);
    const auto retained=at_slot<binding.slot_bytes?(std::min)(count,binding.slot_bytes-at_slot):0;
    if(retained) {
      const auto offset=(2+at/kStoreCommitUnit)*binding.slot_bytes+at_slot;
      state.tail.io->read_exact(offset,output.first(retained));
    }
    std::ranges::fill(output.subspan(retained,count-retained),std::byte{});
    output=output.subspan(count);
    at+=count;
  }
  while(!output.empty() && at<binding.base_end) {
    const auto count=(std::min<std::uint64_t>)(output.size(),binding.unit-at%binding.unit);
    const auto folded=detail::folded_mirror_offset(at,binding.unit);
    try {state.base->read_exact(folded,output.first(count));}
    catch(const std::bad_alloc&){throw;}
    catch(const detail::StoreCaptureInterrupted&){throw;}
    catch(const std::exception&){state.degraded.store(true,std::memory_order_release);throw;}
    output=output.subspan(count);
    at+=count;
  }
  if(!output.empty())state.tail.io->read_exact(state.tail_offset(at),output);
}
void NativeHybridBacking::write_exact(std::uint64_t at,std::span<const std::byte> input) {
  if(!has_writer() || !allows_writer())fail("hybrid backing has no healthy writer");
  const auto& state=*impl_;
  const auto& binding=state.binding;
  if(at<roots && input.size()<=roots-at) {
    if(binding.compact()) {
      const auto at_slot=at%kStoreCommitUnit;
      if(at_slot>=binding.slot_bytes || input.size()>binding.slot_bytes-at_slot)
        fail("hybrid write exceeds a mutable root slot");
      state.tail.io->write_exact((2+at/kStoreCommitUnit)*binding.slot_bytes+at_slot,input);
    }else state.tail.io->write_exact(roots+at,input);
    return;
  }
  if(at<binding.base_end || at>binding.virtual_limit || input.size()>binding.virtual_limit-at)
    fail("hybrid writes cannot overlap the sealed base");
  const auto offset=state.tail_offset(at);
  const auto available=state.physical_limit-binding.base_occupied;
  if(offset>available || input.size()>available-offset)fail("hybrid write exceeds physical backing budget");
  state.tail.io->write_exact(offset,input);
}
void NativeHybridBacking::admit_write(std::uint64_t projected) {
  const auto& state=*impl_;
  if(!has_writer() || !allows_writer() || projected<extent() || projected>state.binding.virtual_limit)
    fail("hybrid write admission refused");
  const auto projected_tail=state.tail_offset(projected);
  if(projected_tail>state.physical_limit-state.binding.base_occupied)fail("hybrid physical backing budget");
  state.tail.io->admit_write(projected_tail);
}
void NativeHybridBacking::flush() {
  if(!allows_writer())fail("hybrid backing requires recovery before flush");
  impl_->tail.io->flush();
}
StoreIoContract NativeHybridBacking::contract() const noexcept {
  const auto tail=impl_->tail.io->contract();
  const auto base=impl_->base_owner.io->contract();
  if(!impl_->writable_tail || !impl_->tail.io->allows_writer() || !valid_store_io_contract(tail) ||
      !valid_store_io_contract(base) || tail.write_failure_unit_bytes>impl_->binding.unit)
    return {};
  return {tail.write_failure_unit_bytes,true,false};
}
bool NativeHybridBacking::concurrent_reads_supported() const noexcept {
  return impl_->base->concurrent_reads_supported() && impl_->tail.io->concurrent_reads_supported();
}
bool NativeHybridBacking::backing_degraded() const noexcept {
  return impl_->degraded.load(std::memory_order_acquire) || impl_->base->degraded() ||
      impl_->base_owner.io->backing_degraded() || impl_->tail.io->backing_degraded();
}
bool NativeHybridBacking::allows_writer() const noexcept {
  return !backing_degraded() && valid_store_io_contract(contract());
}
void NativeHybridBacking::verify_prepared_binding() const {
  const auto& state=*impl_;
  const auto [observed,degraded]=load_binding(*state.tail.io);
  if(degraded || observed!=state.binding || state.tail_extent()!=observed.tail_prefix() ||
      state.open_bound_base(observed)->degraded())
    fail("prepared hybrid binding changed before selection");
}
void NativeHybridBacking::verify_backing() const {
  auto& state=*impl_;
  try {
    const auto [observed,degraded]=load_binding(*state.tail.io);
    if(observed!=state.binding)fail("hybrid backing binding changed");
    if(degraded)state.degraded.store(true,std::memory_order_release);
    (void)state.tail_extent();
    state.base->verify_all();
    if(root_binding(*state.base)!=state.binding.frozen_roots)fail("hybrid frozen roots changed");
  }catch(const std::bad_alloc&){throw;}
  catch(const detail::StoreCaptureInterrupted&){throw;}
  catch(const std::exception&){state.degraded.store(true,std::memory_order_release);throw;}
}

} // namespace flitzfs
